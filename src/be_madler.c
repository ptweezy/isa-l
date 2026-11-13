/* zlib built from source with --zprefix, so every symbol is z_-prefixed and
 * can be linked next to the system zlib. Also the reference compressor. */
#include "zlib.h" /* found through -I<zlib source dir>; zconf.h has Z_PREFIX */

#include "backends.h"

#ifndef Z_PREFIX
#error "be_madler.c must be compiled against a zlib configured with --zprefix"
#endif

#define ZL_STREAM z_stream
#define ZL_INIT2 inflateInit2
#define ZL_INFLATE inflate
#define ZL_END inflateEnd
#define ZL_CRC32 crc32
#define ZL_ADLER32 adler32
#define ZL_VERSION zlibVersion
#define ZL_SYMBOL be_madler
#define ZL_NAME "zlib (from source)"

#include "zlib_like.inc"

size_t ref_deflate_raw(const uint8_t *in, size_t in_len, uint8_t *out,
		       size_t out_cap, int level)
{
	z_stream s;
	int ret;

	memset(&s, 0, sizeof(s));
	if (deflateInit2(&s, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) !=
	    Z_OK)
		return BENCH_ERR;
	s.next_in = (void *)in;
	s.avail_in = (unsigned)in_len;
	s.next_out = out;
	s.avail_out = (unsigned)out_cap;
	ret = deflate(&s, Z_FINISH);
	deflateEnd(&s);
	return ret == Z_STREAM_END ? out_cap - s.avail_out : BENCH_ERR;
}

uint32_t ref_crc32(const uint8_t *buf, size_t len)
{
	return zl_crc32(buf, len);
}
