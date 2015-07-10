/*
 * Copyright 2010 TU Berlin, Germany (Ruben Merz)
 * Copyright 2010-2014 National ICT Australia Limited (NICTA)
 *
 * This software may be used and distributed solely under the terms of
 * the MIT license (License).  You should find a copy of the License in
 * COPYING or at http://opensource.org/licenses/MIT. By downloading or
 * using this software you accept the terms and the liability disclaimer
 * in the License.
 */
/** \file psql_adapter.c
 * \brief Adapter code for the PostgreSQL database backend.
 */

#define _GNU_SOURCE  /* For NAN */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <libpq-fe.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>

#include "htonll.h"
#include "ocomm/o_log.h"
#include "mem.h"
#include "mstring.h"
#include "guid.h"
#include "json.h"
#include "oml_value.h"
#include "oml_util.h"
#include "database.h"
#include "database_adapter.h"
#include "psql_adapter.h"

/** Used as allocation size of strings to hold integers.
 * XXX: We play it safe as INT64_MAX is 9223372036854775807, that is 19 characters
 * \see psql_insert, psql_table_create
 */
#define MAX_DIGITS 32

static char backend_name[] = "psql";
/* Cannot be static due to the way the server sets its parameters */
char *pg_host = DEFAULT_PG_HOST;
char *pg_port = DEFAULT_PG_PORT;
char *pg_user = DEFAULT_PG_USER;
char *pg_pass = DEFAULT_PG_PASS;
char *pg_conninfo = DEFAULT_PG_CONNINFO;

static int sql_stmt(PsqlDB* self, const char* stmt);

/* Functions needed by the Database struct */
static OmlValueT psql_type_to_oml (const char *s);
static const char* psql_oml_to_type (OmlValueT type);
static ssize_t psql_oml_to_size (OmlValueT type);
static int psql_stmt(Database* db, const char* stmt);
static void psql_release(Database* db);
static int psql_table_create (Database* db, DbTable* table, int shallow);
static int psql_table_free (Database *database, DbTable* table);
static char *psql_prepared_var(Database *db, unsigned int order);
static int psql_insert(Database *db, DbTable *table, int sender_id, int seq_no, double time_stamp, OmlValue *values, int value_count);
static char* psql_get_key_value (Database* database, const char* table, const char* key_column, const char* value_column, const char* key);
static int psql_set_key_value (Database* database, const char* table, const char* key_column, const char* value_column, const char* key, const char* value);
static char* psql_get_metadata (Database* database, const char* key);
static int psql_set_metadata (Database* database, const char* key, const char* value);
static int psql_add_sender_id(Database* database, const char* sender_id);
static char* psql_get_uri(Database *db, char *uri, size_t size);
static TableDescr* psql_get_table_list (Database *database, int *num_tables);

static MString* psql_prepare_conninfo(const char *database, const char *host, const char *port, const char *user, const char *pass, const char *extra_conninfo);
static char* psql_get_sender_id (Database* database, const char* name);
static int psql_set_sender_id (Database* database, const char* name, int id);
static void psql_receive_notice(void *arg, const PGresult *res);

/* Functions to convert scalar data into PostgreSQL's binary format */
static inline int psql_set_long_value(long val, char *paramValue);
static inline int psql_set_double_value(double val, char *paramValue);
static inline int psql_set_int8_value(int8_t val, char *paramValue);
static inline int psql_set_int32_value(int32_t val, char *paramValue);
static inline int psql_set_uint32_value(uint32_t val, char *paramValue);
static inline int psql_set_int64_value(int64_t val, char *paramValue);
static inline int psql_set_uint64_value(uint64_t val, char *paramValue);
static inline int psql_set_guid_value(oml_guid_t val, char *paramValue);
static inline int psql_set_bool_value(int8_t val, char *paramValue);

/** Mapping between OML and PostgreSQL data types
 * \info Sizes taken from http://www.postgresql.org/docs/9.4/static/datatype.html
 * \see psql_type_to_oml, psql_oml_to_type
 */
static db_typemap psql_type_map[] = {
  { OML_DB_PRIMARY_KEY, "SERIAL PRIMARY KEY", 4 }, /* We might need BIGSERIAL at some point. */
  { OML_LONG_VALUE,     "INT4",               4 },
  { OML_DOUBLE_VALUE,   "FLOAT8",             8 }, /* 15 bits of precision, need to use NUMERIC for more, see #1657 */
  { OML_STRING_VALUE,   "TEXT",               0 },
  { OML_BLOB_VALUE,     "BYTEA",              0 },
  { OML_INT32_VALUE,    "INT4",               4 },
  { OML_UINT32_VALUE,   "INT8",               8 }, /* PG doesn't support unsigned types --> promote; INT8 is actually BIGINT... */
  { OML_INT64_VALUE,    "INT8",               8 },
  { OML_UINT64_VALUE,   "BIGINT",             8 }, /* XXX: Same as INT8, so sign is lost... Promote to numeric? See #1921 */
  { OML_GUID_VALUE,     "BIGINT",             8 }, /* XXX: Ditto */
  { OML_BOOL_VALUE,     "BOOLEAN",            1 },
  /* Vector types */
  { OML_VECTOR_DOUBLE_VALUE, "TEXT", 0},
  { OML_VECTOR_INT32_VALUE,  "TEXT", 0},
  { OML_VECTOR_UINT32_VALUE, "TEXT", 0},
  { OML_VECTOR_INT64_VALUE,  "TEXT", 0},
  { OML_VECTOR_UINT64_VALUE, "TEXT", 0},
  { OML_VECTOR_BOOL_VALUE,   "TEXT", 0},
};

/** Convert long into a 32-bit binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \param val long value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared
 */
static inline int
psql_set_long_value(long val, char *paramValue) {
  return psql_set_int32_value((int32_t)val, paramValue);
}

/** Convert double into a double-length binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \todo It would be wiser to use libpqtypes http://libpqtypes.esilo.com/
 *
 * \param val double value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared
 */
static inline int
psql_set_double_value(double val, char *paramValue) {
  /* The length of doubles vary. As we stuff them as ints into a char* array,
   * we need to handle them properly. Nobody's going to help us here...
   * XXX: Note the htonlL if SIZEOF_DOUBLE==8.
   */
#if SIZEOF_DOUBLE==8
  /* We want to interpret the bits as if they were integers without altering them,
   * cast the pointers rather than the variable */
  uint64_t *binval = (uint64_t*)(&val);
  *((uint64_t*)paramValue) = htonll(*binval);

#else
# warning Doubles on architecture where they are not 8 bytes is untested and likely broken
  /* XXX: Assuming 32 bits */
  uint32_t *binval = (uint32_t*)(&val);
  memset(paramValue, 0, 8);
  *((uint32_t*)paramValue) = htonl(*binval);

#endif

  return sizeof(binval);
}

/** Convert int8_t into binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \param val int8_t value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared
 */
static inline int
psql_set_int8_value(int8_t val, char *paramValue) {
  *((int8_t*)paramValue) = val;
  return sizeof((val));
}

/** Convert int32_t into binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \param val int32_t value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared
 */
static inline int
psql_set_int32_value(int32_t val, char *paramValue) {
  *((int32_t*)paramValue) = htonl(val);
  return sizeof((val));
}

/** Convert uint32_t into binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 * \warn uint32_t are stored in BIGINTs, which really are int64_t
 *
 * \param val uint32_t value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared, psql_set_int64_value
 */
static inline int
psql_set_uint32_value(uint32_t val, char *paramValue) {
  /* We want this promoted to a wider type, with the same value, just cast */
  return psql_set_int64_value((int64_t)val, paramValue);
}

/** Convert int64_t into binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \param val int64_t value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared
 */
static inline int
psql_set_int64_value(int64_t val, char *paramValue) {
  *((int64_t*)paramValue) = htonll(val);
  return sizeof((val));
}

/** Convert uint64_t into binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \param val uint64_t value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared, psql_set_int64_value
 */
static inline int
psql_set_uint64_value(uint64_t val, char *paramValue) {
  return psql_set_int64_value((int64_t)val, paramValue);
}

/** Convert GUID into 64 binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \param val uint64_t value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared, psql_set_uint64_value
 */
static inline int
psql_set_guid_value(oml_guid_t val, char *paramValue) {
  return psql_set_uint64_value((uint64_t)val, paramValue);
}

/** Convert uint64_t into binary for insertion into PostgreSQL
 *
 * \warn Remember to update paramLength (with the return value), and paramFormat
 *
 * \param val uint64_t value to convert
 * \param paramValue pointer to memory where the binary should be written
 * \return the size of the binary, to be put in paramLength
 * \see psql_insert, PQexecPrepared, psql_set_int8_value
 */
static inline int
psql_set_bool_value(int8_t val, char *paramValue) {
  return psql_set_int8_value(val, paramValue);
}


/** Prepare the conninfo string to connect to the Postgresql server.
 *
 * \param host server hostname
 * \param port server port or service name
 * \param user username
 * \param pass password
 * \param extra_conninfo additional connection parameters
 * \return a dynamically allocated MString containing the connection information; the caller must take care of freeing it
 *
 * \see mstring_delete, PQconnectdb
 */
static MString*
psql_prepare_conninfo(const char *database, const char *host, const char *port, const char *user, const char *pass, const char *extra_conninfo)
{
  MString *conninfo;
  int portnum;

  portnum = resolve_service(port, 5432);
  conninfo = mstring_create();
  mstring_sprintf (conninfo, "host='%s' port='%d' user='%s' password='%s' dbname='%s' %s",
      host, portnum, user, pass, database, extra_conninfo);

  return conninfo;
}

/** Setup the PostgreSQL backend.
 *
 * \return 0 on success, -1 otherwise
 *
 * \see database_setup_backend
 */
int
psql_backend_setup (void)
{
  MString *str, *conninfo;

  loginfo ("psql: Sending experiment data to PostgreSQL server %s:%s as user '%s'\n",
           pg_host, pg_port, pg_user);

  conninfo = psql_prepare_conninfo("postgres", pg_host, pg_port, pg_user, pg_pass, pg_conninfo);
  PGconn *conn = PQconnectdb (mstring_buf (conninfo));

  if (PQstatus (conn) != CONNECTION_OK) {
    logerror ("psql: Could not connect to PostgreSQL database (conninfo \"%s\"): %s", /* PQerrorMessage strings already have '\n'  */
         mstring_buf(conninfo), PQerrorMessage (conn));
    mstring_delete(conninfo);
    return -1;
  }

  /* oml2-server must be able to create new databases, so check that
     our user has the required role attributes */
  str = mstring_create();
  mstring_sprintf (str, "SELECT rolcreatedb FROM pg_roles WHERE rolname='%s'", pg_user);
  PGresult *res = PQexec (conn, mstring_buf (str));
  mstring_delete(str);
  if (PQresultStatus (res) != PGRES_TUPLES_OK) {
    logerror ("psql: Failed to determine role privileges for role '%s': %s", /* PQerrorMessage strings already have '\n'  */
         pg_user, PQerrorMessage (conn));
    return -1;
  }
  char *has_create = PQgetvalue (res, 0, 0);
  if (strcmp (has_create, "t") == 0)
    logdebug ("psql: User '%s' has CREATE DATABASE privileges\n", pg_user);
  else {
    logerror ("psql: User '%s' does not have required role CREATE DATABASE\n", pg_user);
    return -1;
  }

  mstring_delete(conninfo);
  PQclear (res);
  PQfinish (conn);

  return 0;
}

/** Mapping from PostgreSQL to OML types.
 * \see db_adapter_type_to_oml
 */
static
OmlValueT psql_type_to_oml (const char *type)
{
  db_typemap *tm = database_db_to_typemap(psql_type_map, LENGTH(psql_type_map), type);
  return tm->type;
}

/** Mapping from OML types to PostgreSQL types.
 * \see db_adapter_oml_to_type
 */
static const char*
psql_oml_to_type (OmlValueT type)
{
  db_typemap *tm = database_oml_to_typemap(psql_type_map, LENGTH(psql_type_map), type);
  return tm->name;
}

/** Mapping from OML types to PostgreSQL storage size.
 * \see db_adapter_oml_to_type
 */
static ssize_t
psql_oml_to_size (OmlValueT type)
{
  db_typemap *tm = database_oml_to_typemap(psql_type_map, LENGTH(psql_type_map), type);
  return tm->size;
}

/** Execute an SQL statement (using PQexec()).
 * \see db_adapter_stmt, PQexec
 */
static int
sql_stmt(PsqlDB* self, const char* stmt)
{
  PGresult   *res;
  logdebug2("psql: Will execute '%s'\n", stmt);
  res = PQexec(self->conn, stmt);

  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    logerror("psql: Error executing '%s': %s", /* PQerrorMessage strings already have '\n'  */
        stmt, PQerrorMessage(self->conn));
    PQclear(res);
    return -1;
  }
  /*
   * Should PQclear PGresult whenever it is no longer needed to avoid memory
   * leaks
   */
  PQclear(res);

  return 0;
}

/** Type-agnostic wrapper for sql_stmt */
static int
psql_stmt(Database* db, const char* stmt)
{
 return sql_stmt((PsqlDB*)db->handle, stmt);
}

/** Create or open an PostgreSQL database
 * \see db_adapter_create
 */
int
psql_create_database(Database* db)
{
  MString *conninfo = NULL;
  MString *str = NULL;
  PGconn  *conn = NULL;
  PGresult *res = NULL;
  int ret = -1;

  loginfo ("psql:%s: Accessing database\n", db->name);

  /*
   * Make a connection to the database server -- check if the
   * requested database exists or not by connecting to the 'postgres'
   * database and querying that.
   */
  conninfo = psql_prepare_conninfo("postgres", pg_host, pg_port, pg_user, pg_pass, pg_conninfo);
  conn = PQconnectdb(mstring_buf (conninfo));

  /* Check to see that the backend connection was successfully made */
  if (PQstatus(conn) != CONNECTION_OK) {
    logerror ("psql: Could not connect to PostgreSQL database (conninfo \"%s\"): %s", /* PQerrorMessage strings already have '\n'  */
         mstring_buf(conninfo), PQerrorMessage (conn));
    goto cleanup_exit;
  }
  PQsetNoticeReceiver(conn, psql_receive_notice, "postgres");

  str = mstring_create();
  mstring_sprintf (str, "SELECT datname from pg_database where datname='%s';", db->name);
  res = PQexec (conn, mstring_buf (str));

  if (PQresultStatus (res) != PGRES_TUPLES_OK) {
    logerror ("psql: Could not get list of existing databases\n");
    goto cleanup_exit;
  }

  /* No result rows means database doesn't exist, so create it instead */
  if (PQntuples (res) == 0) {
    loginfo ("psql:%s: Database does not exist, creating it\n", db->name);
    mstring_set (str, "");
    mstring_sprintf (str, "CREATE DATABASE \"%s\";", db->name);

    res = PQexec (conn, mstring_buf (str));
    if (PQresultStatus (res) != PGRES_COMMAND_OK) {
      logerror ("psql:%s: Could not create database: %s", /* PQerrorMessage strings already have '\n'  */
          db->name, PQerrorMessage (conn));
      goto cleanup_exit;
    }
  }
  mstring_delete(str);
  str = NULL;

  PQfinish (conn);
  mstring_delete(conninfo);

  /* Now that the database should exist, make a connection to the it for real */
  conninfo = psql_prepare_conninfo(db->name, pg_host, pg_port, pg_user, pg_pass, pg_conninfo);
  conn = PQconnectdb(mstring_buf (conninfo));

  /* Check to see that the backend connection was successfully made */
  if (PQstatus(conn) != CONNECTION_OK) {
    logerror ("psql:%s: Could not connect to PostgreSQL database (conninfo \"%s\"): %s",
  /* PQerrorMessage strings already have '\n'  */
        db->name, mstring_buf(conninfo), PQerrorMessage (conn));
    goto cleanup_exit;
  }
  PQsetNoticeReceiver(conn, psql_receive_notice, db->name);
  mstring_delete(conninfo);
  conninfo = NULL;

  PsqlDB* self = (PsqlDB*)oml_malloc(sizeof(PsqlDB));
  self->conn = conn;
  self->last_commit = time (NULL);

  db->backend_name = backend_name;
  db->o2t = psql_oml_to_type;
  db->t2o = psql_type_to_oml;
  db->stmt = psql_stmt;
  db->create = psql_create_database;
  db->release = psql_release;
  db->prepared_var = psql_prepared_var;
  db->table_create = psql_table_create;
  db->table_create_meta = dba_table_create_meta;
  db->table_free = psql_table_free;
  db->insert = psql_insert;
  db->add_sender_id = psql_add_sender_id;
  db->get_metadata = psql_get_metadata;
  db->set_metadata = psql_set_metadata;
  db->get_uri = psql_get_uri;
  db->get_table_list = psql_get_table_list;

  db->handle = self;

  dba_begin_transaction (db);

  /* Everything was successufl, prepare for cleanup */
  ret = 0;

cleanup_exit:
  if (res) { PQclear (res) ; }
  if (ret) { PQfinish (conn); } /* If return !=0, cleanup connection */

  if (str) { mstring_delete (str); }
  if (conninfo) { mstring_delete (conninfo); }
  /* All paths leading to here have conn uninitialised */
  return ret;
}

/** Release the psql database.
 * \see db_adapter_release
 */
static void
psql_release(Database* db)
{
  PsqlDB* self = (PsqlDB*)db->handle;
  dba_end_transaction (db);
  PQfinish(self->conn);
  oml_free(self);
  db->handle = NULL;
}

/** Create a PostgreSQL database and adapter structures
 * \see db_adapter_create
 */
/* This function is exposed to the rest of the code for backend initialisation */
int
psql_table_create (Database *db, DbTable *table, int shallow)
{
  int i;
  MString *insert = NULL, *insert_name = NULL;
  PsqlDB* psqldb = NULL;
  PGresult *res = NULL;
  PsqlTable* psqltable = NULL;

  logdebug("psql:%s: Creating table '%s' (shallow=%d)\n", db->name, table->schema->name, shallow);

  if (db == NULL) {
    logerror("psql: Tried to create a table in a NULL database\n");
    return -1;
  }
  if (table == NULL) {
    logerror("psql:%s: Tried to create a table from a NULL definition\n", db->name);
    return -1;
  }
  if (table->schema == NULL) {
    logerror("psql:%s: No schema defined for table, cannot create\n", db->name);
    return -1;
  }
  psqldb = (PsqlDB*)db->handle;

  if (!shallow) {
    if (dba_table_create_from_schema(db, table->schema)) {
      logerror("psql:%s: Could not create table '%s': %s", /* PQerrorMessage strings already have '\n' */
          db->name, table->schema->name,
          PQerrorMessage (psqldb->conn));
      goto fail_exit;
    }
  }

  /* Related to #1056. */
  if (table->handle != NULL) {
    logwarn("psql:%s: BUG: Recreating PsqlTable handle for table %s\n",
        table->schema->name);
  }
  psqltable = (PsqlTable*)oml_malloc(sizeof(PsqlTable));
  table->handle = psqltable;

  /* Prepare the insert statement  */
  insert_name = mstring_create();
  mstring_set (insert_name, "OMLInsert-");
  mstring_cat (insert_name, table->schema->name);

  /* Prepare the statement in the database if it doesn't exist yet
   * XXX: We should really only create it if shallow==0; however some
   * tables can get created through dba_table_create_from_* which doesn't
   * initialise the prepared statement; there should be a
   * db_adapter_prepare_insert function provided by the backend, and callable
   * from dba_table_create_from_schema to do the following (in the case of
   * PostgreSQL). See #1056. This might also be the cause of #1268.
   */
  /* This next test might kill the transaction */
  dba_reopen_transaction(db);
  res = PQdescribePrepared(psqldb->conn, mstring_buf (insert_name));
  if(PQresultStatus(res) == PGRES_COMMAND_OK) {
    logdebug("psql:%s: Insertion statement %s already exists\n",
        db->name, mstring_buf (insert_name));
  } else {
    PQclear(res);
    /* This test killed the transaction; start a new one */
    dba_reopen_transaction(db);

    insert = database_make_sql_insert (db, table);
    if (!insert) {
      logerror("psql:%s: Failed to build SQL INSERT statement for table '%s'\n",
          db->name, table->schema->name);
      goto fail_exit;
    }

    logdebug("psql:%s: Preparing statement '%s' (%s)\n", db->name, mstring_buf(insert_name), mstring_buf(insert));
    res = PQprepare(psqldb->conn,
        mstring_buf (insert_name),
        mstring_buf (insert),
        table->schema->nfields + NMETA,
        NULL);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
      logerror("psql:%s: Could not prepare statement: %s", /* PQerrorMessage strings already have '\n' */
          db->name, PQerrorMessage(psqldb->conn));
      PQclear(res);
      goto fail_exit;
    }
  }
  PQclear(res);

  psqltable->insert_stmt = insert_name;
  psqltable->value_count = table->schema->nfields + NMETA;
  psqltable->values = oml_malloc(psqltable->value_count * sizeof(void*));
  for (i=0; i<psqltable->value_count; i++) {
    ssize_t flength = -1;
    if (i<NMETA) {
      /* XXX: The first field in the metadata schema is the tuple ID which we let the DB set automatically */
      flength = psql_oml_to_size(schema_metadata[i+1].type);

    } else {
      flength = psql_oml_to_size(table->schema->fields[i-NMETA].type);
    }

    if (flength<1) {
      flength = MAX_DIGITS;
    }
    psqltable->values[i] = oml_malloc(flength);
    if(psqltable->values[i]) {
      memset(psqltable->values[i], 0, flength);
    } else {
      logerror("psql:%s: Cannot allocate memory for data insertion into table %s:%d\n",
          db->name, table->schema->name, i);
      goto fail_exit;
    }
  }

  if (insert) { mstring_delete (insert); }
  return 0;

fail_exit:
  if (insert) { mstring_delete (insert); }
  if (insert_name) { mstring_delete (insert_name); }
  if (psqltable) {
    for (i=0; i<psqltable->value_count; i++) {
      if (psqltable->values[i]) {
        oml_free(psqltable->values[i]);
      }
    }
    oml_free (psqltable);
  }
  return -1;
}

/** Free a PostgreSQL table
 *
 * Parameter database is ignored in this implementation
 *
 * \see db_adapter_table_free
 */
static int
psql_table_free (Database *database, DbTable *table)
{
  int i;
  (void)database;
  PsqlTable *psqltable = (PsqlTable*)table->handle;

  if (psqltable) {
    mstring_delete (psqltable->insert_stmt);

    if (psqltable->values) {
      for (i=0; i<psqltable->value_count; i++) {
        oml_free(psqltable->values[i]);
      }
      oml_free(psqltable->values);
    }

    oml_free (psqltable);
  }
  return 0;
}

/** Return a string suitable for an unbound variable in PostgreSQL.
 * \see db_adapter_prepared_var
 */
static char*
psql_prepared_var(Database *db, unsigned int order)
{
  int nchar = 1 + (int) floor(log10(order+1))+1; /* Get the number of digits */
  char *s = oml_malloc(nchar + 1);

  (void)db;

  if (NULL != s) {
    snprintf(s, nchar + 1, "$%d", order);
  }

  return s;
}

/** Insert value in the PostgreSQL database.
 * \see db_adapter_insert
 */
static int
psql_insert(Database* db, DbTable* table, int sender_id, int seq_no, double time_stamp, OmlValue* values, int value_count)
{
  PsqlDB* psqldb = (PsqlDB*)db->handle;
  PsqlTable* psqltable = (PsqlTable*)table->handle;
  PGresult* res;
  int i;
  double time_stamp_server;
  const char* insert_stmt = mstring_buf (psqltable->insert_stmt);
  unsigned char *escaped_blob;
  size_t len=MAX_DIGITS;

  assert(NMETA+value_count==psqltable->value_count);

  int paramLength[NMETA+value_count];
  int paramFormat[NMETA+value_count];

  i=0;
  paramLength[i] = psql_set_int32_value(sender_id, psqltable->values[i]);
  paramFormat[i] = 1;

  i++;
  paramLength[i] = psql_set_int32_value(seq_no, psqltable->values[i]);
  paramFormat[i] = 1;

  i++;
  paramLength[i] = psql_set_double_value(time_stamp, psqltable->values[i]);
  paramFormat[i] = 1;

  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_stamp_server = tv.tv_sec - db->start_time + 0.000001 * tv.tv_usec;

  if (tv.tv_sec > psqldb->last_commit) {
    if (dba_reopen_transaction (db) == -1) {
      return -1;
    }
    psqldb->last_commit = tv.tv_sec;
  }

  i++;
  paramLength[i] = psql_set_double_value(time_stamp_server, psqltable->values[i]);
  paramFormat[i] = 1;

  OmlValue* v = values;
  struct schema_field *field;
  for (i = NMETA; i < value_count+NMETA; i++, v++) {
    field = &table->schema->fields[i-NMETA];

    if (oml_value_get_type(v) != field->type) {
      logerror("psql:%s: Value %d type mismatch for table '%s'\n", db->name, i-NMETA, table->schema->name);
      return -1;
    }
    paramLength[i] = 0;
    paramFormat[i] = 0;

    switch (field->type) {
      {
        long val = omlc_get_long(*oml_value_get_value(v));
        paramLength[i] = psql_set_long_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
        break;
      }

    case OML_INT32_VALUE:
      {
        int32_t val = omlc_get_int32(*oml_value_get_value(v));
        paramLength[i] = psql_set_int32_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
        break;
      }

    case OML_UINT32_VALUE:
      {
        /* XXX: To keep the sign in the backend, this has been promoted to INT8... */
        uint32_t val = omlc_get_uint32(*oml_value_get_value(v));
        paramLength[i] = psql_set_uint32_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
      }

    case OML_INT64_VALUE:
      {
        int64_t val = omlc_get_int64(*oml_value_get_value(v));
        paramLength[i] = psql_set_int64_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
        break;
      }

    case OML_UINT64_VALUE:
      {
        uint64_t val = omlc_get_uint64(*oml_value_get_value(v));
        paramLength[i] = psql_set_uint64_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
      }

    case OML_DOUBLE_VALUE:
      {
        double val = omlc_get_double(*oml_value_get_value(v));
        paramLength[i] = psql_set_double_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
        break;
      }

    case OML_GUID_VALUE:
      {
        int64_t val = omlc_get_guid(*oml_value_get_value(v));
        paramLength[i] = psql_set_int64_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
        break;
      }

    case OML_BOOL_VALUE:
      snprintf(psqltable->values[i], MAX_DIGITS, "%d", v->value.boolValue ? 1 : 0);
      {
        uint32_t val = omlc_get_bool(*oml_value_get_value(v));
        paramLength[i] = psql_set_bool_value(val, psqltable->values[i]);
        paramFormat[i] = 1;
        break;
      }
      break;

    case OML_STRING_VALUE:
      len=omlc_get_string_length(*oml_value_get_value(v)) + 1;
      if (len > MAX_DIGITS) {
        logdebug2("psql:%s: Reallocating %d bytes for long string\n", db->name, len);
        psqltable->values[i] = oml_realloc(psqltable->values[i], len);
        if (!psqltable->values[i]) {
          logerror("psql:%s: Could not realloc()at memory for string '%s' in field %d of table '%s'\n",
              db->name, omlc_get_string_ptr(*oml_value_get_value(v)), i-NMETA, table->schema->name);
          /* XXX insert the rest anyway */
          return -1;
        }
      }
      snprintf(psqltable->values[i], len, "%s", omlc_get_string_ptr(*oml_value_get_value(v)));
      break;

    case OML_BLOB_VALUE:
      escaped_blob = PQescapeByteaConn(psqldb->conn,
          v->value.blobValue.ptr, v->value.blobValue.length, &len);
      if (!escaped_blob) {
        logerror("psql:%s: Error escaping blob in field %d of table '%s': %s", /* PQerrorMessage strings already have '\n' */
            db->name, i-NMETA, table->schema->name, PQerrorMessage(psqldb->conn));
      }
      if (len > MAX_DIGITS) {
        logdebug2("psql:%s: Reallocating %d bytes for big blob\n", db->name, len);
        psqltable->values[i] = oml_realloc(psqltable->values[i], len);
        if (!psqltable->values[i]) {
          logerror("psql:%s: Could not realloc()at memory for escaped blob in field %d of table '%s'\n",
              db->name, i-NMETA, table->schema->name);
          return -1;
        }
      }
      snprintf(psqltable->values[i], len, "%s", escaped_blob);
      PQfreemem(escaped_blob);
      break;

    case OML_VECTOR_DOUBLE_VALUE:
      vector_double_to_json(v->value.vectorValue.ptr, v->value.vectorValue.nof_elts, &psqltable->values[i]);
      break;

    case OML_VECTOR_INT32_VALUE:
      vector_int32_to_json(v->value.vectorValue.ptr, v->value.vectorValue.nof_elts, &psqltable->values[i]);
      break;

    case OML_VECTOR_UINT32_VALUE:
      vector_uint32_to_json(v->value.vectorValue.ptr, v->value.vectorValue.nof_elts, &psqltable->values[i]);
      break;

    case OML_VECTOR_INT64_VALUE:
      vector_int64_to_json(v->value.vectorValue.ptr, v->value.vectorValue.nof_elts, &psqltable->values[i]);
      break;

    case OML_VECTOR_UINT64_VALUE:
      vector_uint64_to_json(v->value.vectorValue.ptr, v->value.vectorValue.nof_elts, &psqltable->values[i]);
      break;

    case OML_VECTOR_BOOL_VALUE:
      vector_bool_to_json(v->value.vectorValue.ptr, v->value.vectorValue.nof_elts, &psqltable->values[i]);
      break;

    default:
      logerror("psql:%s: Unknown type %d in col '%s' of table '%s'; this is probably a bug\n",
          db->name, field->type, field->name, table->schema->name);
      return -1;
    }
  }
  /* Use stuff from http://www.postgresql.org/docs/current/static/plpgsql-control-structures.html#PLPGSQL-ERROR-TRAPPING */

  res = PQexecPrepared(psqldb->conn, insert_stmt,
                       NMETA+value_count, (const char**)psqltable->values,
                       (int*) &paramLength, (int*) &paramFormat, 0 );

  if (PQresultStatus(res) != PGRES_COMMAND_OK) {
    logerror("psql:%s: INSERT INTO '%s' failed: %s", /* PQerrorMessage strings already have '\n' */
        db->name, table->schema->name, PQerrorMessage(psqldb->conn));
    PQclear(res);
    return -1;
  }
  PQclear(res);

  return 0;
}

/** Do a key-value style select on a database table.
 *
 * FIXME: Not using prepared statements (#168)
 *
 * The caller is responsible for oml_free()ing the returned value when no longer
 * needed.
 *
 * This function does a key lookup on a database table that is set up
 * in key-value style.  The table can have more than two columns, but
 * this function SELECT's two of them and returns the value of the
 * value column.  It checks to make sure that the key returned is the
 * one requested, then returns its corresponding value.
 *
 * This function makes a lot of assumptions about the database and
 * the table:
 *
 * #- the database exists and is open
 * #- the table exists in the database
 * #- there is a column named key_column in the table
 * #- there is a column named value_column in the table
 *
 * The function does not check for any of these conditions, but just
 * assumes they are true.  Be advised.
 *
 * \param database Database to use
 * \param table name of the table to work in
 * \param key_column name of the column to use as key
 * \param value_column name of the column to set the value in
 * \param key key to look for in key_column
 *
 * \return an oml_malloc'd string value for the given key, or NULL
 *
 * \see psql_set_key_value, oml_malloc, oml_free
 */
static char*
psql_get_key_value (Database *database, const char *table, const char *key_column, const char *value_column, const char *key)
{
  if (database == NULL || table == NULL || key_column == NULL ||
      value_column == NULL || key == NULL)
    return NULL;

  PGresult *res;
  PsqlDB *psqldb = (PsqlDB*) database->handle;
  MString *stmt = mstring_create();
  mstring_sprintf (stmt, "SELECT %s FROM %s WHERE %s='%s';",
                   value_column, table, key_column, key);

  res = PQexec (psqldb->conn, mstring_buf (stmt));

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    logerror("psql:%s: Error trying to get %s[%s]; (%s)", /* PQerrorMessage strings already have '\n' */
             database->name, table, key, PQerrorMessage(psqldb->conn));
    goto fail_exit;
  }

  if (PQntuples (res) == 0)
    goto fail_exit;
  if (PQnfields (res) < 1)
    goto fail_exit;

  if (PQntuples (res) > 1)
    logwarn("psql:%s: Key-value lookup for key '%s' in %s(%s, %s) returned more than one possible key.\n",
             database->name, key, table, key_column, value_column);

  char *value = NULL;
  value = PQgetvalue (res, 0, 0);

  if (value != NULL)
    value = oml_strndup (value, strlen (value));

  PQclear (res);
  mstring_delete (stmt);
  return value;

 fail_exit:
  PQclear (res);
  mstring_delete (stmt);
  return NULL;
}

/** Set a value for the given key in the given table
 *
 * FIXME: Not using prepared statements (#168)
 *
 * \param database Database to use
 * \param table name of the table to work in
 * \param key_column name of the column to use as key
 * \param value_column name of the column to set the value in
 * \param key key to look for in key_column
 * \param value value to look for in value_column
 * \return 0 on success, -1 otherwise
 *
 * \see psql_get_key_value
 */
static int
psql_set_key_value (Database *database, const char *table, const char *key_column, const char *value_column, const char *key, const char *value)
{
  PsqlDB *psqldb = (PsqlDB*) database->handle;
  MString *stmt = mstring_create ();
  char *check_value = psql_get_key_value (database, table, key_column, value_column, key);
  if (check_value == NULL) {
    mstring_sprintf (stmt, "INSERT INTO \"%s\" (\"%s\", \"%s\") VALUES ('%s', '%s');",
                     table, key_column, value_column, key, value);
  } else {
    mstring_sprintf (stmt, "UPDATE \"%s\" SET \"%s\"='%s' WHERE \"%s\"='%s';",
                     table, value_column, value, key_column, key);
  }

  if (sql_stmt (psqldb, mstring_buf (stmt))) {
    logwarn("psql:%s: Key-value update failed for %s='%s' in %s(%s, %s) (database error)\n",
            database->name, key, value, table, key_column, value_column);
    return -1;
  }

  return 0;
}

/** Get data from the metadata table
 * \see db_adapter_get_metadata, sq3_get_key_value
 */
static char*
psql_get_metadata (Database *db, const char *key)
{
  return psql_get_key_value (db, "_experiment_metadata", "key", "value", key);
}

/** Set data in the metadata table
 * \see db_adapter_set_metadata, sq3_set_key_value
 */
static int
psql_set_metadata (Database *db, const char *key, const char *value)
{
  return psql_set_key_value (db, "_experiment_metadata", "key", "value", key, value);
}

/** Add a new sender to the database, returning its index.
 * \see db_add_sender_id
 */
static int
psql_add_sender_id(Database *db, const char *sender_id)
{
  PsqlDB *self = (PsqlDB*)db->handle;
  int index = -1;
  char *id_str = psql_get_sender_id (db, sender_id);

  if (id_str) {
    index = atoi (id_str);
    oml_free (id_str);

  } else {
    PGresult *res = PQexec (self->conn, "SELECT MAX(id) FROM _senders;");

    if (PQresultStatus (res) != PGRES_TUPLES_OK) {
      logwarn("psql:%s: Failed to get maximum sender id from database (restarting at 0)<++>: %s", /* PQerrorMessage strings already have '\n'  */
          db->name, PQerrorMessage (self->conn));
      PQclear (res);
      index = 0;
    } else {
      int rows = PQntuples (res);
      if (rows == 0) {
        logwarn("psql:%s: Failed to get maximum sender id from database: empty result; starting at 0\n",
            db->name);
        PQclear (res);
        index = 0;
      } else {
        index = atoi (PQgetvalue (res, 0, 0)) + 1;
        PQclear (res);
      }
    }

    psql_set_sender_id (db, sender_id, index);

  }

  return index;
}

/** Build a URI for this database.
 *
 * URI is of the form postgresql://USER@SERVER:PORT/DATABASE
 *
 * \see db_adapter_get_uri
 */
static char*
psql_get_uri(Database *db, char *uri, size_t size)
{
  int len;
  assert(db);
  assert(uri);
  assert(size>0);
  len = snprintf(uri, size, "postgresql://%s@%s:%d/%s", pg_user, pg_host, resolve_service(pg_port, 5432), db->name);
  if(len < 0 || len >= (int)size) {
    return NULL;
  }
  return uri;
}

/** Get a list of tables in a PostgreSQL database
 * \see db_adapter_get_table_list
 */
static TableDescr*
psql_get_table_list (Database *database, int *num_tables)
{
  PsqlDB *self = database->handle;
  TableDescr *tables = NULL, *t = NULL;
  const char *table_stmt = "SELECT tablename FROM pg_tables WHERE tablename NOT LIKE 'pg%' AND tablename NOT LIKE 'sql%';";
  const char *tablename, *meta;
  const char *ptable_stmt = "OMLGetTableList";
  const char *schema_stmt = "SELECT value FROM _experiment_metadata WHERE key='table_' || $1;"; /* || is a concatenation */
  const char *pschema_stmt = "OMLGetTableSchema";
  const int paramFormats[] = {0};
  PGresult *res = NULL, *schema_res = NULL;
  struct schema *schema = NULL;
  int have_meta = 0;
  int i, nrows;

  /* Get a list of table names */
  logdebug("psql:%s: Preparing statement '%s' (%s)\n", database->name, ptable_stmt, table_stmt);
  res = PQprepare(self->conn, ptable_stmt, table_stmt, 0, NULL);
  if (PQresultStatus (res) != PGRES_COMMAND_OK) {
    logerror("psql:%s: Could not prepare statement %s from '%s': %s", /* PQerrorMessage strings already have '\n'  */
        database->name, ptable_stmt, table_stmt, PQerrorMessage(self->conn));
    goto fail_exit;
  }
  PQclear (res);
  res = NULL;

  /* Check if the _experiment_metadata table exists */
  res = PQexecPrepared (self->conn, ptable_stmt, 0, NULL, 0, NULL, 0);
  if (PQresultStatus (res) != PGRES_TUPLES_OK) {
    logerror("psql:%s: Could not get list of tables with '%s': %s", /* PQerrorMessage strings already have '\n'  */
        database->name, table_stmt, PQerrorMessage(self->conn));
    goto fail_exit;
  }

  nrows = PQntuples (res);
  i = -1;
  do {
    i++; /* Equivalent to sqlite3_step */
    if(i < nrows) { /* Equivalent to an SQLITE_ROW return */
      if (strcmp (PQgetvalue (res, i, 0), "_experiment_metadata") == 0) {
        logdebug("psql:%s: Found table %s\n",
            database->name, PQgetvalue (res, i, 0));
        have_meta = 1;
      }
    }
  } while (i < nrows && !have_meta);

  *num_tables = 0; /* In case !have_meta, we want it to be 0; also, we need it that way for later */

  if(!have_meta) {
    logdebug("psql:%s: _experiment_metadata table not found\n", database->name);
    /* XXX: This is probably a new database, don't exit in error */
    PQclear (res);
    return NULL;
  }

  /* Get schema for all tables */
  logdebug("psql:%s: Preparing statement '%s' (%s)\n", database->name, pschema_stmt, schema_stmt);
  schema_res = PQprepare(self->conn, pschema_stmt, schema_stmt, 1, NULL);
  if (PQresultStatus (schema_res) != PGRES_COMMAND_OK) {
    logerror("psql:%s: Could not prepare statement %s from '%s': %s", /* PQerrorMessage strings already have '\n'  */
        database->name, pschema_stmt, schema_stmt, PQerrorMessage(self->conn));
    goto fail_exit;
  }
  PQclear (schema_res);
  schema_res = NULL;

  i = -1; /* Equivalent to sqlite3_reset; assume nrows is still valid */
  do {
    t = NULL;

    i++; /* Equivalent to sqlite3_step */
    if(i < nrows) { /* Equivalent to an SQLITE_ROW return */
      tablename = PQgetvalue (res, i, 0);

      if(!strcmp (tablename, "_senders")) {
        /* Create a phony entry for the _senders table some
         * server/database.c:database_init() doesn't try to create it */
        t = table_descr_new (tablename, NULL);

      } else {
        /* If it's *not* the _senders table, get its schema from the metadata table */
        logdebug2("psql:%s:%s: Trying to find schema for table %s: %s\n",
            database->name, __FUNCTION__, tablename,
            schema_stmt);

        schema_res = PQexecPrepared (self->conn, pschema_stmt, 1, &tablename, NULL, paramFormats, 0);
        if (PQresultStatus (schema_res) != PGRES_TUPLES_OK) {
          logwarn("psql:%s: Could not get schema for table %s, ignoring it: %s", /* PQerrorMessage strings already have '\n'  */
              database->name, tablename, PQerrorMessage(self->conn));

        } else if (PQntuples(schema_res) <= 0) {
          logwarn("psql:%s: No schema for table %s, ignoring it\n",
              database->name, tablename);

        } else {
          meta = PQgetvalue (schema_res, 0, 0); /* We should only have one result row */
          schema = schema_from_meta(meta);
          PQclear(schema_res);
          schema_res = NULL;

          if (!schema) {
            logwarn("psql:%s: Could not parse schema '%s' (stored in DB) for table %s, ignoring it; "
                "is your database from an oml2-server<2.10?\n",
                database->name, meta, tablename);

          } else {

            t = table_descr_new (tablename, schema);
            if (!t) {
              logerror("psql:%s: Could not create table descrition for table %s\n",
                  database->name, tablename);
              goto fail_exit;
            }
            schema = NULL; /* The pointer has been copied in t (see table_descr_new);
                              we don't want to free it twice in case of error */
          }
        }
      }

      if (t) {
        t->next = tables;
        tables = t;
        (*num_tables)++;
      }
    }
  } while (i < nrows);

  return tables;

fail_exit:
  if (tables) {
    table_descr_list_free(tables);
  }

  if (schema) {
    schema_free(schema);
  }
  if (schema_res) {
   PQclear(schema_res);
  }
  if (res) {
   PQclear(res);
  }

  *num_tables = -1;
  return NULL;
}

/** Get the sender_id for a given name in the _senders table.
 *
 * \param name name of the sender
 * \return the sender ID
 *
 * \see psql_get_key_value
 */
static char*
psql_get_sender_id (Database *database, const char *name)
{
  return psql_get_key_value (database, "_senders", "name", "id", name);
}

/** Set the sender_id for a given name in the _senders table.
 *
 * \param name name of the sender
 * \param id the ID to set
 * \return the sender ID
 *
 * \see psql_set_key_value
 */
static int
psql_set_sender_id (Database *database, const char *name, int id)
{
  MString *mstr = mstring_create();
  mstring_sprintf (mstr, "%d", id);
  int ret = psql_set_key_value (database, "_senders", "name", "id", name, mstring_buf (mstr));
  mstring_delete (mstr);
  return ret;
}

/** Receive notices from PostgreSQL and post them as an OML log message
 * \param arg application-specific state (in our case, the table name)
 * \param res a PGRES_NONFATAL_ERROR PGresult which can be used with PQresultErrorField and PQresultErrorMessage
 */
static void
psql_receive_notice(void *arg, const PGresult *res)
{
  switch(*PQresultErrorField(res, PG_DIAG_SEVERITY)) {
  case 'E': /*RROR*/
  case 'F': /*ATAL*/
  case 'P': /*ANIC*/
    logerror("psql:%s': %s", (char*)arg, PQresultErrorMessage(res));
    break;
  case 'W': /*ARNING*/
    logwarn("psql:%s': %s", (char*)arg, PQresultErrorMessage(res));
    break;
  case 'N': /*OTICE*/
  case 'I': /*NFO*/
    /* Infos and notice from Postgre are not the primary purpose of OML.
     * We only display them as debug messages. */
  case 'L': /*OG*/
  case 'D': /*EBUG*/
    logdebug("psql:%s': %s", (char*)arg, PQresultErrorMessage(res));
    break;
  default:
    logwarn("'psql:%s': Unknown notice: %s", (char*)arg, PQresultErrorMessage(res));
  }
}

/*
 Local Variables:
 mode: C
 tab-width: 2
 indent-tabs-mode: nil
 End:
 vim: sw=2:sts=2:expandtab
*/
