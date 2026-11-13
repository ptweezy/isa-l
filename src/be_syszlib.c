/* The zlib that the operating system ships: Apple's on macOS, the distro's on
 * Linux. Compiled without any -I for the from-source zlib. */
#include <zlib.h>

#include "backends.h"

#define ZL_STREAM z_stream
#define ZL_INIT2 inflateInit2
#define ZL_INFLATE inflate
#define ZL_END inflateEnd
#define ZL_CRC32 crc32
#define ZL_ADLER32 adler32
#define ZL_VERSION zlibVersion
#define ZL_SYMBOL be_syszlib
#ifdef __APPLE__
#define ZL_NAME "system zlib (Apple)"
#else
#define ZL_NAME "system zlib (distro)"
#endif

#include "zlib_like.inc"
