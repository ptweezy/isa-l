/* Kernel A/B: times the adler32 and crc32_iscsi kernels of one isa-l build
 * against each other in one process, alternating them round by round so
 * frequency drift cancels. Links statically against that build's isa-l.a.
 * Build with -DNO_ADLER32_AVX512 or -DNO_CRC32_ISCSI_FUSED for a build that
 * lacks that kernel. The AVX-512 kernels run only on CPUs with the second
 * group of AVX-512 extensions isa-l checks for.
 * kab [rounds]
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

uint32_t isal_adler32(uint32_t, const unsigned char *, uint64_t);
uint32_t adler32_avx2_4(uint32_t, const unsigned char *, uint64_t);
unsigned int crc32_iscsi(unsigned char *, int, unsigned int);
unsigned int crc32_iscsi_by16_10(unsigned char *, uint64_t, unsigned int);
#ifdef NO_ADLER32_AVX512
#define adler32_avx512 ((uint32_t (*)(uint32_t, const unsigned char *, uint64_t))0)
#else
uint32_t adler32_avx512(uint32_t, const unsigned char *, uint64_t);
#endif
#ifdef NO_CRC32_ISCSI_FUSED
#define crc32_iscsi_by16_10_fused ((unsigned int (*)(unsigned char *, uint64_t, unsigned int))0)
#else
unsigned int crc32_iscsi_by16_10_fused(unsigned char *, uint64_t, unsigned int);
#endif

typedef uint32_t (*kfn)(uint8_t *, uint64_t);

static uint32_t k_isal_adler32(uint8_t *p, uint64_t n) { return isal_adler32(1, p, n); }
static uint32_t k_adler32_avx2_4(uint8_t *p, uint64_t n) { return adler32_avx2_4(1, p, n); }
static uint32_t k_adler32_avx512(uint8_t *p, uint64_t n) { return adler32_avx512(1, p, n); }
static uint32_t k_crc32_iscsi(uint8_t *p, uint64_t n) { return crc32_iscsi(p, (int)n, 0); }
static uint32_t k_by16_10(uint8_t *p, uint64_t n) { return crc32_iscsi_by16_10(p, n, 0); }
static uint32_t k_by16_10_fused(uint8_t *p, uint64_t n) { return crc32_iscsi_by16_10_fused(p, n, 0); }

struct kernel {
	const char *name;
	kfn f;
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

static void run(const char *what, struct kernel *k, int nk, uint8_t *buf, uint64_t len)
{
	int n = 0;
	struct kernel *use[8];
	for (int i = 0; i < nk; i++)
		if (k[i].present)
			use[n++] = &k[i];
	if (n == 0)
		return;
	uint32_t want = use[0]->f(buf, len);
	for (int i = 1; i < n; i++)
		if (use[i]->f(buf, len) != want) {
			printf("%s %lu: %s DIFFERS from %s\n", what, (unsigned long)len, use[i]->name,
			       use[0]->name);
			exit(1);
		}
	/* Enough calls for about 5 ms per kernel per round */
	uint64_t reps = 1;
	for (;;) {
		double t0 = now();
		for (uint64_t r = 0; r < reps; r++)
			sink += use[0]->f(buf, len);
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
				sink += use[i]->f(buf, len);
			t[i][r] = (now() - t0) / reps;
		}
	printf("%-12s %8lu", what, (unsigned long)len);
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
	int avx2 = !!__builtin_cpu_supports("avx2");
	int g2 = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl") &&
		 __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq") &&
		 __builtin_cpu_supports("avx512cd") && __builtin_cpu_supports("avx512vbmi2") &&
		 __builtin_cpu_supports("gfni") && __builtin_cpu_supports("vaes") &&
		 __builtin_cpu_supports("vpclmulqdq") && __builtin_cpu_supports("avx512vnni") &&
		 __builtin_cpu_supports("avx512bitalg") &&
		 __builtin_cpu_supports("avx512vpopcntdq");
	printf("avx2 %d, avx512 group 2 %d\n", avx2, g2);

	struct kernel adler[] = {
		{ "adler32_avx2_4", k_adler32_avx2_4, avx2 },
		{ "adler32_avx512", k_adler32_avx512, adler32_avx512 && g2 },
		{ "isal_adler32", k_isal_adler32, 1 },
	};
	struct kernel crc[] = {
		{ "by16_10", k_by16_10, g2 },
		{ "by16_10_fused", k_by16_10_fused, crc32_iscsi_by16_10_fused && g2 },
		{ "crc32_iscsi", k_crc32_iscsi, 1 },
	};

	uint64_t max = 16 << 20;
	uint8_t *buf = malloc(max + 64);
	buf += (64 - (uintptr_t)buf % 64) % 64;
	srand(1);
	for (uint64_t i = 0; i < max; i++)
		buf[i] = rand();

	uint64_t adler_sizes[] = { 64, 4096, 65536, 262144 };
	uint64_t crc_sizes[] = { 1024, 4096, 16384, 65536, 262144, 1 << 20, 4 << 20 };
	if (adler32_avx512)
		for (unsigned i = 0; i < sizeof(adler_sizes) / sizeof(adler_sizes[0]); i++)
			run("adler32", adler, 3, buf, adler_sizes[i]);
	if (crc32_iscsi_by16_10_fused)
		for (unsigned i = 0; i < sizeof(crc_sizes) / sizeof(crc_sizes[0]); i++)
			run("crc32_iscsi", crc, 3, buf, crc_sizes[i]);
	return 0;
}
