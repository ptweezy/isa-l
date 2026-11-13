/* Apple's libcompression, COMPRESSION_ZLIB (raw deflate only). macOS only.
 * Measured to see whether the system zlib's speed comes from the same code. */
#ifdef __APPLE__
#include <compression.h>

#include "backends.h"

static const char *ac_version(void)
{
	return "libcompression";
}

static size_t ac_run(const uint8_t *in, size_t in_len, uint8_t *out,
		     size_t out_cap, int fmt, size_t in_chunk,
		     size_t out_chunk)
{
	compression_stream s;
	compression_status st;
	size_t in_pos = 0, total = 0;

	if (fmt != FMT_RAW)
		return BENCH_UNSUPPORTED;
	if (out_chunk == 0) {
		size_t n = compression_decode_buffer(out, out_cap, in, in_len,
						     NULL, COMPRESSION_ZLIB);
		return n ? n : BENCH_ERR;
	}
	if (compression_stream_init(&s, COMPRESSION_STREAM_DECODE,
				    COMPRESSION_ZLIB) != COMPRESSION_STATUS_OK)
		return BENCH_ERR;
	s.src_ptr = in;
	s.src_size = 0;
	do {
		int flags = 0;

		if (s.src_size == 0) {
			size_t n = in_len - in_pos;

			if (n > in_chunk)
				n = in_chunk;
			s.src_ptr = in + in_pos;
			s.src_size = n;
			in_pos += n;
		}
		if (in_pos == in_len)
			flags = COMPRESSION_STREAM_FINALIZE;
		s.dst_ptr = out;
		s.dst_size = out_chunk;
		st = compression_stream_process(&s, flags);
		total += out_chunk - s.dst_size;
	} while (st == COMPRESSION_STATUS_OK);
	compression_stream_destroy(&s);
	return st == COMPRESSION_STATUS_END ? total : BENCH_ERR;
}

const backend_t be_apple_compression = {
	"Apple libcompression", ac_version, ac_run, NULL, NULL,
};
#else
typedef int be_apple_compression_unused;
#endif
