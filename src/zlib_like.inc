/* Shared body for the three zlib-API backends. The including file defines:
 *   ZL_STREAM, ZL_INIT2, ZL_INFLATE, ZL_END, ZL_CRC32, ZL_ADLER32,
 *   ZL_VERSION, ZL_SYMBOL, ZL_NAME
 */
#include <string.h>

static const char *zl_version(void)
{
	return ZL_VERSION();
}

static size_t zl_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
			 size_t out_cap, int fmt, size_t in_chunk,
			 size_t out_chunk)
{
	ZL_STREAM s;
	size_t in_pos = 0, total = 0;
	int ret;

	memset(&s, 0, sizeof(s));
	if (ZL_INIT2(&s, fmt == FMT_GZIP ? 31 : fmt == FMT_ZLIB ? 15 : -15) != Z_OK)
		return BENCH_ERR;

	if (out_chunk == 0) {
		s.next_in = (void *)in;
		s.avail_in = (unsigned)in_len;
		s.next_out = out;
		s.avail_out = (unsigned)out_cap;
		ret = ZL_INFLATE(&s, Z_FINISH);
		total = out_cap - s.avail_out;
	} else {
		do {
			if (s.avail_in == 0) {
				size_t n = in_len - in_pos;

				if (n > in_chunk)
					n = in_chunk;
				if (n == 0) {
					ret = Z_BUF_ERROR;
					break;
				}
				s.next_in = (void *)(in + in_pos);
				s.avail_in = (unsigned)n;
				in_pos += n;
			}
			s.next_out = out;
			s.avail_out = (unsigned)out_chunk;
			ret = ZL_INFLATE(&s, Z_NO_FLUSH);
			total += out_chunk - s.avail_out;
		} while (ret == Z_OK);
	}
	ZL_END(&s);
	(void)out_cap;
	return ret == Z_STREAM_END ? total : BENCH_ERR;
}

/* The zlib checksum functions take a 32-bit length. */
static uint32_t zl_crc32(const uint8_t *buf, size_t len)
{
	uint32_t c = (uint32_t)ZL_CRC32(0, NULL, 0);

	while (len) {
		unsigned n = len > (1u << 30) ? (1u << 30) : (unsigned)len;

		c = (uint32_t)ZL_CRC32(c, buf, n);
		buf += n;
		len -= n;
	}
	return c;
}

static uint32_t zl_adler32(const uint8_t *buf, size_t len)
{
	uint32_t c = (uint32_t)ZL_ADLER32(0, NULL, 0);

	while (len) {
		unsigned n = len > (1u << 30) ? (1u << 30) : (unsigned)len;

		c = (uint32_t)ZL_ADLER32(c, buf, n);
		buf += n;
		len -= n;
	}
	return c;
}

const backend_t ZL_SYMBOL = {
	ZL_NAME, zl_version, zl_inflate, zl_crc32, zl_adler32,
};
