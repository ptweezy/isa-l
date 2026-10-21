/* CRC kernel A/B: times crc32_gzip_refl and crc32_ieee of one isa-l build (the
 * dispatched function, crc32_*_by16_10, and crc32_*_by16_10_hybrid called
 * directly) against each other in one process, alternating them round by round.
 * Links statically against that build's isa-l.a. The AVX-512 kernels run only
 * on CPUs with the second group of AVX-512 extensions isa-l checks for.
 * ckab [rounds]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

typedef uint32_t (*cfn)(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_gzip_refl(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_gzip_refl_base(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_gzip_refl_by16_10(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_gzip_refl_by16_10_hybrid(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_ieee(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_ieee_base(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_ieee_by16_10(uint32_t, const unsigned char *, uint64_t);
uint32_t crc32_ieee_by16_10_hybrid(uint32_t, const unsigned char *, uint64_t);

struct kernel {
	const char *name;
	cfn f;
	int present;
};

static double now(void)
{
#ifdef _WIN32
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (double)c.QuadPart / f.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static int rounds = 21;
static volatile uint32_t sink;

static void check(const char *what, struct kernel *k, int nk, cfn base, uint8_t *buf)
{
	static const uint64_t lens[] = { 0, 1, 15, 64, 4095, 8191, 8192, 8193, 12345, 65536, 65599,
					 262147, 1048576 + 17, 3 * 1048576 + 5 };
	for (unsigned l = 0; l < sizeof(lens) / sizeof(lens[0]); l++)
		for (int off = 0; off < 3; off++) {
			uint32_t seed = 0x12345678u * (l + 1) + off;
			uint32_t want = base(seed, buf + off, lens[l]);
			for (int i = 0; i < nk; i++)
				if (k[i].present && k[i].f(seed, buf + off, lens[l]) != want) {
					printf("%s %lu+%d: %s DIFFERS from base\n", what,
					       (unsigned long)lens[l], off, k[i].name);
					exit(1);
				}
		}
}

static void run(const char *what, struct kernel *k, int nk, uint8_t *buf, uint64_t len)
{
	int n = 0;
	struct kernel *use[8];
	for (int i = 0; i < nk; i++)
		if (k[i].present)
			use[n++] = &k[i];
	if (n == 0)
		return;
	/* Enough calls for about 5 ms per kernel per round */
	uint64_t reps = 1;
	for (;;) {
		double t0 = now();
		for (uint64_t r = 0; r < reps; r++)
			sink += use[0]->f(r, buf, len);
		if (now() - t0 > 0.005)
			break;
		reps *= 2;
	}
	double t[8][256];
	for (int r = 0; r < rounds; r++)
		for (int j = 0; j < n; j++) {
			int i = (r & 1) ? n - 1 - j : j;
			double t0 = now();
			for (uint64_t q = 0; q < reps; q++)
				sink += use[i]->f(q, buf, len);
			t[i][r] = (now() - t0) / reps;
		}
	printf("%-16s %8lu", what, (unsigned long)len);
	double best0 = 0;
	for (int i = 0; i < n; i++) {
		qsort(t[i], rounds, sizeof(double), cmpd);
		double best = len / t[i][0] / 1e9, med = len / t[i][rounds / 2] / 1e9;
		if (i == 0) {
			best0 = best;
			printf("  %s %.1f (med %.1f)", use[i]->name, best, med);
		} else {
			printf("  %s %.1f (med %.1f, %+.1f%%)", use[i]->name, best, med,
			       100 * (best / best0 - 1));
		}
	}
	printf("  GB/s\n");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		rounds = atoi(argv[1]);
	if (rounds < 1 || rounds > 256)
		rounds = 21;
	__builtin_cpu_init();
	int g2 = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl") &&
		 __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq") &&
		 __builtin_cpu_supports("avx512cd") && __builtin_cpu_supports("avx512vbmi2") &&
		 __builtin_cpu_supports("gfni") && __builtin_cpu_supports("vaes") &&
		 __builtin_cpu_supports("vpclmulqdq") && __builtin_cpu_supports("avx512vnni") &&
		 __builtin_cpu_supports("avx512bitalg") &&
		 __builtin_cpu_supports("avx512vpopcntdq") && __builtin_cpu_supports("bmi2");
	printf("avx512 group 2 %d\n", g2);

	struct kernel gz[] = {
		{ "by16_10", crc32_gzip_refl_by16_10, g2 },
		{ "hybrid", crc32_gzip_refl_by16_10_hybrid, g2 },
		{ "crc32_gzip_refl", crc32_gzip_refl, 1 },
	};
	struct kernel ie[] = {
		{ "by16_10", crc32_ieee_by16_10, g2 },
		{ "hybrid", crc32_ieee_by16_10_hybrid, g2 },
		{ "crc32_ieee", crc32_ieee, 1 },
	};
	uint64_t max = 4 << 20;
	uint8_t *buf = malloc(max + 64);
	for (uint64_t i = 0; i < max + 64; i++)
		buf[i] = (uint8_t)(rand() >> 7);
	check("crc32_gzip_refl", gz, 3, crc32_gzip_refl_base, buf);
	check("crc32_ieee", ie, 3, crc32_ieee_base, buf);
	printf("results match the base functions\n");
	static const uint64_t sizes[] = { 4096, 16384, 65536, 262144, 1048576 };
	for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
		run("crc32_gzip_refl", gz, 3, buf, sizes[s]);
	for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
		run("crc32_ieee", ie, 3, buf, sizes[s]);
	free(buf);
	return 0;
}
