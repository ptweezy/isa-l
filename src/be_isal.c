/* ISA-L igzip: the streaming isal_inflate() and the one-call
 * isal_inflate_stateless(). */
#include <string.h>

#include "crc.h"
#include "igzip_lib.h"

#include "backends.h"

#ifndef ISAL_VERSION_STR
#define ISAL_VERSION_STR "unknown"
#endif

static struct inflate_state st; /* large: keep it off the stack */

static const char *isal_version(void)
{
	return ISAL_VERSION_STR;
}

static size_t isal_run(const uint8_t *in, size_t in_len, uint8_t *out,
		       size_t out_cap, int fmt, size_t in_chunk,
		       size_t out_chunk)
{
	size_t in_pos = 0, total = 0;
	int ret;

	isal_inflate_init(&st);
	st.crc_flag = fmt == FMT_GZIP ? ISAL_GZIP :
		      fmt == FMT_ZLIB ? ISAL_ZLIB : ISAL_DEFLATE;

	if (out_chunk == 0) {
		st.next_in = (uint8_t *)in;
		st.avail_in = (uint32_t)in_len;
		st.next_out = out;
		st.avail_out = (uint32_t)out_cap;
		ret = isal_inflate(&st);
		total = out_cap - st.avail_out;
	} else {
		do {
			if (st.avail_in == 0) {
				size_t n = in_len - in_pos;

				if (n > in_chunk)
					n = in_chunk;
				if (n == 0)
					return BENCH_ERR;
				st.next_in = (uint8_t *)(in + in_pos);
				st.avail_in = (uint32_t)n;
				in_pos += n;
			}
			st.next_out = out;
			st.avail_out = (uint32_t)out_chunk;
			ret = isal_inflate(&st);
			total += out_chunk - st.avail_out;
		} while (ret == ISAL_DECOMP_OK &&
			 st.block_state != ISAL_BLOCK_FINISH);
	}
	if (ret != ISAL_DECOMP_OK || st.block_state != ISAL_BLOCK_FINISH)
		return BENCH_ERR;
	return total;
}

static size_t isal_run_stateless(const uint8_t *in, size_t in_len,
				 uint8_t *out, size_t out_cap, int fmt,
				 size_t in_chunk, size_t out_chunk)
{
	(void)in_chunk;
	if (out_chunk != 0)
		return BENCH_UNSUPPORTED;
	isal_inflate_init(&st);
	st.crc_flag = fmt == FMT_GZIP ? ISAL_GZIP :
		      fmt == FMT_ZLIB ? ISAL_ZLIB : ISAL_DEFLATE;
	st.next_in = (uint8_t *)in;
	st.avail_in = (uint32_t)in_len;
	st.next_out = out;
	st.avail_out = (uint32_t)out_cap;
	if (isal_inflate_stateless(&st) != ISAL_DECOMP_OK)
		return BENCH_ERR;
	return out_cap - st.avail_out;
}

static uint32_t isal_crc(const uint8_t *buf, size_t len)
{
	return crc32_gzip_refl(0, buf, len);
}

static uint32_t isal_adler(const uint8_t *buf, size_t len)
{
	return isal_adler32(1, buf, len);
}

const backend_t be_isal = {
	"ISA-L", isal_version, isal_run, isal_crc, isal_adler,
};

const backend_t be_isal_stateless = {
	"ISA-L (stateless)", isal_version, isal_run_stateless, NULL, NULL,
};
