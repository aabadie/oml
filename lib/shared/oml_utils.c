/*
 * Copyright 2007-2014 National ICT Australia (NICTA)
 *
 * This software may be used and distributed solely under the terms of
 * the MIT license (License).  You should find a copy of the License in
 * COPYING or at http://opensource.org/licenses/MIT. By downloading or
 * using this software you accept the terms and the liability disclaimer
 * in the License.
 */
/** \file oml_utils.c
 * \brief Various utility functions, mainly strings and memory-buffer related.
 */
#include <stdio.h> // For snprintf
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>

#include "ocomm/o_log.h"
#include "mem.h"
#include "oml_utils.h"
#include "string_utils.h"

/** Dump the contents of a buffer as a string of hex characters
 *
 * \param buf array of data to dump
 * \param len length of buf
 * \return a character string which needs to be oml_free()'d
 *
 * \see oml_malloc, oml_free
 */
char* to_octets(unsigned char *buf, int len)
{
  const int octet_width = 2;
  const int columns = 16;
  len = (len>0xff)?0xff:len; /* Limit the output to something manageable */
  const int rows = len / columns + 2; /* Integer division plus first line */
  /* Each row has 7 non-data characters (numbers and spaces), one more space, and columns*ASCII characters, plus a '\n' */
  const int rowlength = (octet_width * columns + 7 + 1 + columns + 1);
  const int outlength = rows * rowlength + 1;
  char *out = oml_malloc (outlength);
  char strrep[columns + 1];
  int n = 0, i, col, rw=0;

  strrep[columns] = 0;

  if(out) {
    /* Don't forget nil-terminator in snprintf's size */
    n += snprintf(out, outlength - n, "   0 1 2 3  4 5 6 7   8 9 a b  c d e f  0123456789abcdef\n%2x ", rw++);
    for (i = 0; i < len; i++) {
      col = i % columns;

      if (i == 0) {
        while(0); /* Do nothing */

      } else if (col == 0) {
        n += snprintf(&out[n], outlength - n,  " %s\n%2x ", strrep, rw++);

        /* Add some spacing for readability */
      } else if(0 == (col % 8)) {
        n += snprintf(&out[n], outlength - n, "  ");

      } else if (0 == (col % 4)) {
        n += snprintf(&out[n], outlength - n, " ");
      }

      n += snprintf(&out[n], outlength - n, "%02x", (unsigned int)buf[i]);
      if (isprint(buf[i])) {
        strrep[col] = buf[i];
      } else {
        strrep[col] = '.';
      }
    }
    if(col != 0) {
      while(++col<columns) {
        /* Add padding to align ASCII output */
        if (0 == (col % 8)) {
          n += snprintf(&out[n], outlength - n, "    ");
        } else if (0 == (col % 4)) {
          n += snprintf(&out[n], outlength - n, "   ");
        } else {
          n += snprintf(&out[n], outlength - n, "  ");
        }
      }
      strrep[col] = 0;
      n += snprintf(&out[n], outlength - n, " %s", strrep);
    }
  }
  out[outlength - 1] = 0;

  return out;
}

/** Resolve a string containing a service or port into an integer port.
 *
 * Setting default to an impossible value (say, -1) is sufficient to catch resolving errors;
 *
 * \param port string containing the textual service name or port
 * \param defport default port value to return if resolving fails
 * \return the port number as a host-order integer, or the default value if something failed
 * \see getservbyname(3)
 */
int resolve_service(const char *service, int defport)
{
  struct servent *sse;
  char *endptr;
  int portnum = defport, tmpport;

  sse = getservbyname(service, NULL);
  if (sse) {
    portnum = ntohs(sse->s_port);
  } else {
    tmpport = strtol(service, &endptr, 10);
    if (endptr > service)
      portnum = tmpport;
    else
      logwarn("Could not resolve service '%s', defaulting to %d\n", service, portnum);
  }

  return portnum;
}

/** Parse the scheme of an URI and return its type as an +OmlURIType+
 *
 * \param [in] uri the URI to parse
 * \return an +OmlURIType+ indicating the type of the URI
 * \see oml_uri_is_file, oml_uri_is_network
 */
OmlURIType oml_uri_type(const char* uri) {
  int len, ret=OML_URI_UNKNOWN;

  if (!uri) {
    return OML_URI_UNKNOWN;
  }

  len = strlen(uri);

  if(len>=5 && !strncmp(uri, "flush", 5)) {
    ret = OML_URI_FILE_FLUSH;
  } else if(len>=4 && !strncmp(uri, "file", 4)) {
    ret = OML_URI_FILE;
  } else if(len>=3 && !strncmp(uri, "tcp", 3)) {
    ret = OML_URI_TCP;
  } else if(len>=3 && !strncmp(uri, "udp", 3)) {
    ret = OML_URI_UDP;
  }
  return ret;
}

/** Test OmlURIType as a network URI
 * \param t OmlURIType
 * \return 1 if t is a network type (tcp or udp schemes, 0 otherwise
 * \see oml_uri_type
 */
inline int oml_uri_is_file(OmlURIType t) {
  return t>=OML_URI_FILE && t<=OML_URI_FILE_FLUSH;
}

/** Test OmlURIType as a network URI
 * \param t OmlURIType
 * \return 1 if t is a network type (tcp or udp schemes, 0 otherwise
 */
inline int oml_uri_is_network(OmlURIType t) {
  return t>=OML_URI_TCP && t<=OML_URI_UDP;
}

/** Parse a collection URI of the form [proto:]path[:service].
 *
 * path can be a hostname, an IPv4 address or an IPv6 address within brackets
 * if proto is a network protocol. service is invalid if proto indicates a
 * local file.
 *
 * \param uri string containing the URI to parse
 * \param protocol pointer to be updated to a string containing the selected protocol, to be oml_free()'d by the caller
 * \param path pointer to be updated to a string containing the node name or address, to be oml_free()'d by the caller
 * \param service pointer to be updated to a string  containing the service name or port number, to be oml_free()'d by the caller
 * \return 0 on success, -1 otherwise
 *
 * \see oml_strndup, oml_free, oml_uri_type
 */
int
parse_uri (const char *uri, const char **protocol, const char **path, const char **port)
{
  const int MAX_PARTS = 3;
  char *parts[3] = { NULL, NULL, NULL }, *tmp, *orig;
  size_t lengths[3] = { 0, 0, 0 };
  int is_valid = 1;
  int i;
  OmlURIType uri_type;

  if (uri) {
    uri_type = oml_uri_type(uri);

    orig = parts[2] = oml_strndup (uri, strlen (uri));
    parts[0] = strsep(&parts[2], "[");
    if (parts[2]) {
      i = 0;
      if (*parts[i] != '\0') {
        /* there was something before the brackets, clean it some more */
        tmp = parts[i];
        parts[i] = strsep(&tmp, ":");
        lengths[i] = parts[i] ? strlen (parts[i]) : 0;
        i++;
      } 

      parts[i] = strsep(&parts[2], "]");
      lengths[i] = parts[i] ? strlen (parts[i]) : 0;
      i++;

      parts[i] = parts[2];
      tmp = strsep(&parts[i], ":");
      lengths[i] = parts[i] ? strlen (parts[i]) : 0;

    } else {
      /* restart the parsing */
      parts[2] = parts[0];

      i = 0;

      parts[i] = strsep(&parts[2], ":");
      lengths[i] = parts[i] ? strlen (parts[i]) : 0;
      i++;

      parts[i] = strsep(&parts[2], ":");
      lengths[i] = parts[i] ? strlen (parts[i]) : 0;
      i++;

      lengths[i] = parts[i] ? strlen (parts[i]) : 0;
    }

    /* make sure unused items are clearly so */
    for(++i; i<MAX_PARTS; i++) {
      parts[i] = NULL;
      lengths[i] = 0;
    }

#define trydup(i) (parts[(i)] && lengths[(i)]>0 ? oml_strndup (parts[(i)], lengths[(i)]) : NULL)
    *protocol = *path = *port = NULL;
    if (lengths[0] > 0 && lengths[1] > 0) {
      /* Case 1:  "abc:xyz" or "abc:xyz:123" -- if abc is a transport, use it; otherwise, it's a hostname/path */
      if (oml_uri_is_network(uri_type)) {
        *protocol = trydup (0);
        *path = trydup (1);
        *port = trydup (2);
      } else if (oml_uri_is_file(uri_type)) {
        *protocol = trydup (0);
        *path = trydup (1);
        *port = NULL;
      } else {
        *protocol = NULL;
        *path = trydup (0);
        *port = trydup (1);
      }
    } else if (lengths[0] > 0 && lengths[2] > 0) {
      /* Case 2:  "abc::123" -- not valid, as we can't infer a hostname/path */
      logwarn ("Server URI '%s' is invalid as it does not contain a hostname/path\n", uri);
      is_valid = 0;
    } else if (lengths[0] > 0) {
      *protocol = NULL;
      *path = trydup (0);
      *port = NULL;

      /* Look for potential user errors and issue a warning but proceed as normal */
      if (uri_type != OML_URI_UNKNOWN) {
        logwarn ("Server URI with unknown scheme, assuming 'tcp:%s'\n",
                 *path);
      }
    } else {
      logerror ("Server URI '%s' seems to be empty\n", uri);
      is_valid = 0;
    }
#undef trydup

    oml_free (orig);
  }
  if (is_valid)
    return 0;
  else
    return -1;
}

/*
 Local Variables:
 mode: C
 tab-width: 2
 indent-tabs-mode: nil
 End:
 vim: sw=2:sts=2:expandtab
*/
