/* zlib-ng in native mode (zng_ prefix). The build passes -DZNG_NAME with the
 * ref that was built. Two zlib-ng versions cannot share a binary: their
 * internal symbols collide, so a second ref gets its own binary. */
#include "zlib-ng.h"

#include "backends.h"

#ifndef ZNG_NAME
#define ZNG_NAME "zlib-ng"
#endif

#define ZL_STREAM zng_stream
#define ZL_INIT2 zng_inflateInit2
#define ZL_INFLATE zng_inflate
#define ZL_END zng_inflateEnd
#define ZL_CRC32 zng_crc32
#define ZL_ADLER32 zng_adler32
#define ZL_VERSION zlibng_version
#define ZL_SYMBOL be_zng
#define ZL_NAME ZNG_NAME

#include "zlib_like.inc"
