/*
 * Copyright 2014 National ICT Australia (NICTA)
 *
 * This software may be used and distributed solely under the terms of
 * the MIT license (License).  You should find a copy of the License in
 * COPYING or at http://opensource.org/licenses/MIT. By downloading or
 * using this software you accept the terms and the liability disclaimer
 * in the License.
 */
/**\file zlib_stream.h
 * \brief Interface for the Zlib OmlOutStream.
 * \see OmlOutStream
 */
#include <zlib.h>

#include "oml2/oml_out_stream.h"

typedef struct OmlZlibOutStream {

  /*
   * Fields from OmlOutStream interface
   */

  /** \see OmlOutStream::write, oml_outs_write_f */
  oml_outs_write_f write;
  /** \see OmlOutStream::close, oml_outs_close_f */
  oml_outs_close_f close;

  /** \see OmlOutStream::dest */
  char *dest;

  /*
   * Fields specific to the OmlZlibOutStream
   */

  OmlOutStream* os;              /**< OmlOutStream into which to write result */
  int   header_written;         /**< True if header has been written to file */

} OmlZlibOutStream;

static ssize_t zlib_stream_write(OmlOutStream* hdl, uint8_t* buffer, size_t  length, uint8_t* header, size_t  header_length);

/*
 Local Variables:
 mode: C
 tab-width: 2
 indent-tabs-mode: nil
 vim: sw=2:sts=2:expandtab
*/
