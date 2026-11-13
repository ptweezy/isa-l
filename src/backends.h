/* Common interface for the inflate backends. Each backend lives in its own
 * translation unit so that headers with clashing names (system zlib and a
 * prefixed zlib built from source) never meet. */
#ifndef BACKENDS_H
#define BACKENDS_H

#include <stddef.h>
#include <stdint.h>

#define FMT_RAW 0  /* raw deflate, no checksum */
#define FMT_GZIP 1 /* gzip wrapper, CRC-32 verified */
#define FMT_ZLIB 2 /* zlib wrapper, Adler-32 verified */

#define BENCH_ERR ((size_t)-1)
#define BENCH_UNSUPPORTED ((size_t)-2)

typedef struct {
	const char *name;
	const char *(*version)(void);
	/* Decompress in[0..in_len). If out_chunk is 0, decompress in one call
	 * into out[0..out_cap). Otherwise feed in_chunk bytes at a time and
	 * reuse out[0..out_chunk) for every call, as a streaming reader does.
	 * Returns the number of bytes produced. */
	size_t (*inflate)(const uint8_t *in, size_t in_len, uint8_t *out,
			  size_t out_cap, int fmt, size_t in_chunk,
			  size_t out_chunk);
	uint32_t (*crc32)(const uint8_t *buf, size_t len);   /* may be NULL */
	uint32_t (*adler32)(const uint8_t *buf, size_t len); /* may be NULL */
} backend_t;

extern const backend_t be_syszlib;
extern const backend_t be_madler;
extern const backend_t be_zng;
extern const backend_t be_isal;
extern const backend_t be_isal_stateless;
extern const backend_t be_libdeflate;
#ifdef __APPLE__
extern const backend_t be_apple_compression;
#endif

/* Reference compressor (zlib built from source), so that every platform
 * decompresses bit-identical streams. Returns the compressed size. */
size_t ref_deflate_raw(const uint8_t *in, size_t in_len, uint8_t *out,
		       size_t out_cap, int level);
uint32_t ref_crc32(const uint8_t *buf, size_t len);
uint32_t ref_adler32(const uint8_t *buf, size_t len);

#endif
