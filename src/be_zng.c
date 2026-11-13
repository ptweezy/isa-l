/* zlib-ng in native mode (zng_ prefix). */
#include "zlib-ng.h"

#include "backends.h"

#define ZL_STREAM zng_stream
#define ZL_INIT2 zng_inflateInit2
#define ZL_INFLATE zng_inflate
#define ZL_END zng_inflateEnd
#define ZL_CRC32 zng_crc32
#define ZL_ADLER32 zng_adler32
#define ZL_VERSION zlibng_version
#define ZL_SYMBOL be_zng
#define ZL_NAME "zlib-ng"

#include "zlib_like.inc"
