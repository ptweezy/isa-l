/* libdeflate: whole-buffer only. */
#include "libdeflate.h"

#include "backends.h"

static struct libdeflate_decompressor *d;

static const char *ld_version(void)
{
	return LIBDEFLATE_VERSION_STRING;
}

static size_t ld_run(const uint8_t *in, size_t in_len, uint8_t *out,
		     size_t out_cap, int fmt, size_t in_chunk,
		     size_t out_chunk)
{
	size_t actual = 0;
	enum libdeflate_result r;

	(void)in_chunk;
	if (out_chunk != 0)
		return BENCH_UNSUPPORTED;
	if (!d)
		d = libdeflate_alloc_decompressor();
	if (fmt == FMT_GZIP)
		r = libdeflate_gzip_decompress(d, in, in_len, out, out_cap,
					       &actual);
	else
		r = libdeflate_deflate_decompress(d, in, in_len, out, out_cap,
						  &actual);
	return r == LIBDEFLATE_SUCCESS ? actual : BENCH_ERR;
}

static uint32_t ld_crc(const uint8_t *buf, size_t len)
{
	return libdeflate_crc32(0, buf, len);
}

static uint32_t ld_adler(const uint8_t *buf, size_t len)
{
	return libdeflate_adler32(1, buf, len);
}

const backend_t be_libdeflate = {
	"libdeflate", ld_version, ld_run, ld_crc, ld_adler,
};
