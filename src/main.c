/* Native inflate benchmark: one deflate stream, every decompressor.
 *
 *   bench [--max-mb N] [--rounds N] [--level N] [--dump DIR] FILE...
 *
 * Each FILE is compressed once with a zlib built from source, so every
 * platform decompresses the same bits. Backends are run interleaved, one call
 * per backend per round, and the fastest round is reported. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "backends.h"

#define CHUNK (128 * 1024)

static const backend_t *const backends[] = {
	&be_syszlib,	    &be_madler,	    &be_zng, &be_isal,
	&be_isal_stateless, &be_libdeflate,
#ifdef __APPLE__
	&be_apple_compression,
#endif
};
#define NB (sizeof(backends) / sizeof(backends[0]))

static const struct {
	const char *label;
	int fmt;
	size_t chunk; /* 0: one call into one big buffer */
} configs[] = {
	{ "raw, one call", FMT_RAW, 0 },
	{ "raw, 128 KiB stream", FMT_RAW, CHUNK },
	{ "gzip, one call", FMT_GZIP, 0 },
	{ "gzip, 128 KiB stream", FMT_GZIP, CHUNK },
};
#define NC (sizeof(configs) / sizeof(configs[0]))

static const char *dump_dir; /* --dump DIR: write each reference .gz there */

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void die(const char *msg, const char *arg)
{
	fprintf(stderr, "error: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
	exit(1);
}

static uint8_t *read_file(const char *path, size_t max, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;

	if (!f)
		die("cannot open", path);
	buf = malloc(max ? max : 1);
	if (!buf)
		die("out of memory", NULL);
	*len = fread(buf, 1, max, f);
	fclose(f);
	return buf;
}

static void put32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');

	return s ? s + 1 : path;
}

static void bench_file(const char *path, size_t max, int rounds, int level)
{
	static const uint8_t gz_header[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
	size_t n, raw_len, gz_len, cap;
	uint8_t *orig = read_file(path, max, &n);
	uint8_t *raw, *gz, *out, *scratch;
	double best[NB][NC];
	double best_crc[NB], best_adler[NB];
	uint32_t crc_val[NB], adler_val[NB];
	size_t b, c;
	int r;

	if (n == 0)
		die("empty file", path);
	cap = n + n / 8 + 4096;
	raw = malloc(cap);
	gz = malloc(cap + 18);
	out = malloc(n + 64);
	scratch = malloc(CHUNK);
	if (!raw || !gz || !out || !scratch)
		die("out of memory", NULL);

	raw_len = ref_deflate_raw(orig, n, raw, cap, level);
	if (raw_len == BENCH_ERR)
		die("reference compression failed", path);
	memcpy(gz, gz_header, 10);
	memcpy(gz + 10, raw, raw_len);
	put32le(gz + 10 + raw_len, ref_crc32(orig, n));
	put32le(gz + 14 + raw_len, (uint32_t)n);
	gz_len = raw_len + 18;

	if (dump_dir) {
		char name[4096];
		FILE *f;

		snprintf(name, sizeof(name), "%s/%s.ref.gz", dump_dir,
			 base_name(path));
		f = fopen(name, "wb");
		if (!f || fwrite(gz, 1, gz_len, f) != gz_len || fclose(f) != 0)
			die("cannot write", name);
	}

	for (b = 0; b < NB; b++) {
		for (c = 0; c < NC; c++)
			best[b][c] = 0;
		best_crc[b] = best_adler[b] = 0;
	}

	for (r = 0; r < rounds; r++) {
		for (c = 0; c < NC; c++) {
			const uint8_t *in = configs[c].fmt == FMT_GZIP ? gz : raw;
			size_t in_len = configs[c].fmt == FMT_GZIP ? gz_len : raw_len;
			size_t chunk = configs[c].chunk;

			for (b = 0; b < NB; b++) {
				double t0, t;
				size_t got;

				if (best[b][c] < 0)
					continue;
				t0 = now();
				got = backends[b]->inflate(in, in_len,
							   chunk ? scratch : out,
							   chunk ? chunk : n + 64,
							   configs[c].fmt, chunk, chunk);
				t = now() - t0;
				if (got == BENCH_UNSUPPORTED) {
					best[b][c] = -1;
					continue;
				}
				if (got != n ||
				    (!chunk && r == 0 && memcmp(out, orig, n) != 0)) {
					fprintf(stderr, "%s failed on %s (%s): got %zu of %zu bytes\n",
						backends[b]->name, path,
						configs[c].label, got, n);
					best[b][c] = -2;
					continue;
				}
				if (best[b][c] == 0 || t < best[b][c])
					best[b][c] = t;
			}
		}
		for (b = 0; b < NB; b++) {
			double t0, t;

			if (backends[b]->crc32) {
				t0 = now();
				crc_val[b] = backends[b]->crc32(orig, n);
				t = now() - t0;
				if (best_crc[b] == 0 || t < best_crc[b])
					best_crc[b] = t;
			}
			if (backends[b]->adler32) {
				t0 = now();
				adler_val[b] = backends[b]->adler32(orig, n);
				t = now() - t0;
				if (best_adler[b] == 0 || t < best_adler[b])
					best_adler[b] = t;
			}
		}
	}

	printf("\n### %s: %.1f MB, deflate level %d to %.1f MB (%.1f%%)\n\n",
	       base_name(path), (double)n / 1e6, level, (double)raw_len / 1e6,
	       100.0 * (double)raw_len / (double)n);
	printf("Inflate, MB/s of output, best of %d interleaved rounds:\n\n", rounds);
	printf("| backend | version |");
	for (c = 0; c < NC; c++)
		printf(" %s |", configs[c].label);
	printf("\n|---|---|");
	for (c = 0; c < NC; c++)
		printf("---:|");
	printf("\n");
	for (b = 0; b < NB; b++) {
		printf("| %s | %s |", backends[b]->name, backends[b]->version());
		for (c = 0; c < NC; c++) {
			if (best[b][c] > 0)
				printf(" %.0f |", (double)n / 1e6 / best[b][c]);
			else if (best[b][c] == -1)
				printf(" n/a |");
			else
				printf(" FAILED |");
		}
		printf("\n");
	}

	printf("\nChecksums over the uncompressed data, GB/s:\n\n");
	printf("| backend | CRC-32 | Adler-32 |\n|---|---:|---:|\n");
	for (b = 0; b < NB; b++) {
		if (!backends[b]->crc32 && !backends[b]->adler32)
			continue;
		printf("| %s |", backends[b]->name);
		if (backends[b]->crc32)
			printf(" %.1f%s |", (double)n / 1e9 / best_crc[b],
			       crc_val[b] == crc_val[1] ? "" : " (WRONG)");
		else
			printf(" n/a |");
		if (backends[b]->adler32)
			printf(" %.1f%s |", (double)n / 1e9 / best_adler[b],
			       adler_val[b] == adler_val[1] ? "" : " (WRONG)");
		else
			printf(" n/a |");
		printf("\n");
	}
	fflush(stdout);

	free(orig);
	free(raw);
	free(gz);
	free(out);
	free(scratch);
}

int main(int argc, char **argv)
{
	size_t max = (size_t)128 << 20;
	int rounds = 9, level = 6, i, files = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--max-mb") && i + 1 < argc)
			max = (size_t)strtoul(argv[++i], NULL, 10) << 20;
		else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
			rounds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--level") && i + 1 < argc)
			level = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
			dump_dir = argv[++i];
		else {
			bench_file(argv[i], max, rounds, level);
			files++;
		}
	}
	if (!files)
		die("usage: bench [--max-mb N] [--rounds N] [--level N] FILE...", NULL);
	return 0;
}
