/* adler32 kernel investigation. Swaps isal_adler32's dispatch pointer to time
 * kernels inside deflate and inflate, and probes core clock and upper state.
 * Links statically against an isa-l build plus the variant objects.
 *
 * adx <dispatch offset> kern
 * adx <dispatch offset> sizes <tar>
 * adx <dispatch offset> e2e [-r rounds] <tar> <files...>
 * adx <dispatch offset> dirty <tar>
 * adx <dispatch offset> clock
 *
 * The dispatch offset is &isal_adler32_dispatched - &isal_adler32 (from nm).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>
#include "igzip_lib.h"

uint32_t adler32_base(uint32_t, uint8_t *, uint64_t);
uint32_t adler32_avx2_4(uint32_t, const unsigned char *, uint64_t);
uint32_t adler32_avx512(uint32_t, const unsigned char *, uint64_t);
uint32_t adler32_avx512_zu(uint32_t, const unsigned char *, uint64_t);
uint32_t adler32_avx512_ymm(uint32_t, const unsigned char *, uint64_t);

typedef uint32_t (*afn)(uint32_t, const unsigned char *, uint64_t);
static struct {
	const char *name;
	afn f;
} vars[] = {
	{ "avx2_4", adler32_avx2_4 },
	{ "avx512", adler32_avx512 },
	{ "avx512_zu", adler32_avx512_zu },
	{ "ymm", adler32_avx512_ymm },
};
#define NV 4

static afn *dispatch;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return rs;
}

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 64);
	if (fread(b, 1, *len, f) != *len)
		exit(1);
	fclose(f);
	return b;
}

/* Zero the upper bits of zmm16-31, which vzeroupper leaves alone */
static void zero_zmm16_31(void)
{
	__asm__ volatile("vpxord %xmm16, %xmm16, %xmm16\n\t vpxord %xmm17, %xmm17, %xmm17\n\t"
			 "vpxord %xmm18, %xmm18, %xmm18\n\t vpxord %xmm19, %xmm19, %xmm19\n\t"
			 "vpxord %xmm20, %xmm20, %xmm20\n\t vpxord %xmm21, %xmm21, %xmm21\n\t"
			 "vpxord %xmm22, %xmm22, %xmm22\n\t vpxord %xmm23, %xmm23, %xmm23\n\t"
			 "vpxord %xmm24, %xmm24, %xmm24\n\t vpxord %xmm25, %xmm25, %xmm25\n\t"
			 "vpxord %xmm26, %xmm26, %xmm26\n\t vpxord %xmm27, %xmm27, %xmm27\n\t"
			 "vpxord %xmm28, %xmm28, %xmm28\n\t vpxord %xmm29, %xmm29, %xmm29\n\t"
			 "vpxord %xmm30, %xmm30, %xmm30\n\t vpxord %xmm31, %xmm31, %xmm31\n\t");
}

/* ---- kern: correctness and kernel throughput ---- */

static void kern(void)
{
	size_t max = 16 << 20;
	uint8_t *buf = malloc(max + 128);
	for (size_t i = 0; i < max + 128; i++)
		buf[i] = rnd();
	long fails = 0, checks = 0;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1)
			memset(buf, 0xff, max + 128);
		for (uint64_t n = 0; n <= 20000; n += (n < 6000 ? 1 : 7)) {
			uint8_t *p = buf + rnd() % 64;
			uint32_t init = pass ? 0xFFF0FFF0 : (uint32_t)rnd();
			uint32_t want = adler32_base(init, p, n);
			for (int v = 0; v < NV; v++, checks++)
				if (vars[v].f(init, p, n) != want && fails++ < 10)
					printf("FAIL %s len %lu\n", vars[v].name, (unsigned long)n);
		}
	}
	for (int i = 0; i < 30; i++) {
		uint64_t n = rnd() % max;
		uint8_t *p = buf + rnd() % 64;
		uint32_t init = (uint32_t)rnd();
		uint32_t want = adler32_base(init, p, n);
		for (int v = 0; v < NV; v++, checks++)
			if (vars[v].f(init, p, n) != want && fails++ < 10)
				printf("FAIL %s big len %lu\n", vars[v].name, (unsigned long)n);
	}
	printf("correctness: %ld checks, %ld failures\n", checks, fails);

	uint64_t sizes[] = { 64, 1024, 4096, 65536, 262144 };
	for (unsigned s = 0; s < 5; s++) {
		uint64_t len = sizes[s];
		uint64_t reps = (64u << 20) / len / 8 + 1;
		double t[NV][21];
		for (int r = 0; r < 21; r++)
			for (int j = 0; j < NV; j++) {
				int v = (r & 1) ? NV - 1 - j : j;
				uint32_t sink = 1;
				double t0 = now();
				for (uint64_t q = 0; q < reps; q++)
					sink = vars[v].f(sink, buf, len);
				t[v][r] = (now() - t0) / reps;
				if (sink == 42)
					puts("");
			}
		printf("kern %7lu", (unsigned long)len);
		for (int v = 0; v < NV; v++) {
			qsort(t[v], 21, sizeof(double), cmpd);
			printf("  %s %.1f", vars[v].name, len / t[v][0] / 1e9);
		}
		printf("  GB/s (chained calls)\n");
	}
}

/* ---- deflate and inflate helpers ---- */

static uint8_t *level_buf;

static size_t defl(uint8_t *in, size_t len, uint8_t *out, size_t cap, int wrapper)
{
	struct isal_zstream s;
	isal_deflate_stateless_init(&s);
	s.next_in = in;
	s.avail_in = len;
	s.next_out = out;
	s.avail_out = cap;
	s.level = 1;
	s.level_buf = level_buf;
	s.level_buf_size = ISAL_DEF_LVL1_DEFAULT;
	s.end_of_stream = 1;
	s.gzip_flag = wrapper ? IGZIP_ZLIB : 0;
	if (isal_deflate_stateless(&s) != COMP_OK) {
		fprintf(stderr, "deflate error\n");
		exit(1);
	}
	return s.total_out;
}

static size_t infl(uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
	struct inflate_state st;
	isal_inflate_init(&st);
	st.next_in = in;
	st.avail_in = len;
	st.next_out = out;
	st.avail_out = cap;
	st.crc_flag = ISAL_ZLIB;
	if (isal_inflate_stateless(&st) != ISAL_DECOMP_OK) {
		fprintf(stderr, "inflate error\n");
		exit(1);
	}
	return st.total_out;
}

struct buf {
	uint8_t *in, *out, *comp;
	size_t in_len, out_cap, comp_len;
};

enum op { DEFL_ZLIB, INFL_ZLIB, DEFL_RAW };
static const char *opname[] = { "defl L1 zlib", "infl zlib", "defl L1 raw" };

static void pass(struct buf *b, int n, enum op op)
{
	for (int i = 0; i < n; i++) {
		if (op == INFL_ZLIB)
			infl(b[i].comp, b[i].comp_len, b[i].out, b[i].out_cap);
		else
			defl(b[i].in, b[i].in_len, b[i].out, b[i].out_cap, op == DEFL_ZLIB);
	}
}

static struct buf *slices(uint8_t *tar, size_t tar_len, size_t size, int n)
{
	struct buf *b = calloc(n, sizeof(*b));
	for (int i = 0; i < n; i++) {
		b[i].in = tar + (tar_len - size) / n * i;
		b[i].in_len = size;
		b[i].out_cap = size * 2 + 1024;
		b[i].out = malloc(b[i].out_cap);
		b[i].comp = malloc(b[i].out_cap);
	}
	return b;
}

static void prep_infl(struct buf *b, int n)
{
	*dispatch = adler32_avx2_4;
	for (int i = 0; i < n; i++)
		b[i].comp_len = defl(b[i].in, b[i].in_len, b[i].comp, b[i].out_cap, 1);
}

static int rounds = 11;

/* Time op on b with each variant dispatched, alternating round by round */
static void run_case(const char *tag, struct buf *b, int n, enum op op)
{
	size_t total = 0;
	for (int i = 0; i < n; i++)
		total += b[i].in_len;
	*dispatch = vars[0].f;
	int reps = 0;
	double t0 = now();
	do {
		pass(b, n, op);
		reps++;
	} while (now() - t0 < 0.02);
	double t[NV][64];
	for (int r = 0; r < rounds; r++)
		for (int j = 0; j < NV; j++) {
			int v = (r & 1) ? NV - 1 - j : j;
			*dispatch = vars[v].f;
			double t1 = now();
			for (int q = 0; q < reps; q++)
				pass(b, n, op);
			t[v][r] = (now() - t1) / reps;
		}
	printf("%-6s %-13s", tag, opname[op]);
	double base = 0;
	for (int v = 0; v < NV; v++) {
		qsort(t[v], rounds, sizeof(double), cmpd);
		double best = total / t[v][0] / 1e6;
		if (v == 0) {
			base = best;
			printf(" %s %7.1f", vars[v].name, best);
		} else
			printf("  %s %7.1f %+5.1f%%", vars[v].name, best, 100 * (best / base - 1));
	}
	printf("  MB/s\n");
	fflush(stdout);
}

static void e2e(uint8_t *tar, size_t tar_len, int nfiles, char **files)
{
	size_t sizes[] = { 1024, 4096, 16384, 65536 };
	for (unsigned s = 0; s < 4; s++) {
		struct buf *b = slices(tar, tar_len, sizes[s], 256);
		char tag[16];
		snprintf(tag, sizeof(tag), "%zuK", sizes[s] >> 10);
		prep_infl(b, 256);
		run_case(tag, b, 256, DEFL_ZLIB);
		run_case(tag, b, 256, INFL_ZLIB);
		run_case(tag, b, 256, DEFL_RAW);
	}
	if (nfiles) {
		struct buf *b = calloc(nfiles, sizeof(*b));
		for (int i = 0; i < nfiles; i++) {
			b[i].in = read_file(files[i], &b[i].in_len);
			b[i].out_cap = b[i].in_len + b[i].in_len / 8 + 65536;
			b[i].out = malloc(b[i].out_cap);
			b[i].comp = malloc(b[i].out_cap);
		}
		prep_infl(b, nfiles);
		run_case("whole", b, nfiles, DEFL_ZLIB);
		run_case("whole", b, nfiles, INFL_ZLIB);
		run_case("whole", b, nfiles, DEFL_RAW);
	}
}

/* ---- sizes: how deflate and inflate call adler32 ---- */

static long hist[8], calls;
static uint32_t counting(uint32_t init, const unsigned char *buf, uint64_t len)
{
	int k = len == 0 ? 0 : len < 64 ? 1 : len < 1024 ? 2 : len < 4096 ? 3 : len < 16384 ? 4
		: len < 65536 ? 5 : len < 262144 ? 6 : 7;
	hist[k]++;
	calls++;
	return adler32_avx2_4(init, buf, len);
}

static void sizes(uint8_t *tar, size_t tar_len)
{
	static const char *bk[] = { "0", "1-63", "64-1K", "1K-4K", "4K-16K", "16K-64K", "64K-256K", "256K+" };
	size_t sz[] = { 1024, 4096, 16384, 65536 };
	for (unsigned s = 0; s < 4; s++) {
		struct buf *b = slices(tar, tar_len, sz[s], 256);
		prep_infl(b, 256);
		for (int op = 0; op < 2; op++) {
			memset(hist, 0, sizeof(hist));
			calls = 0;
			*dispatch = counting;
			pass(b, 256, op);
			printf("%3zuK %-13s %.2f calls per stream:", sz[s] >> 10, opname[op], calls / 256.0);
			for (int k = 0; k < 8; k++)
				if (hist[k])
					printf(" %s:%ld", bk[k], hist[k]);
			printf("\n");
		}
	}
}

/* ---- dirty: does a dirty zmm16-31 state slow code that never calls adler32? ---- */

static void dirty(uint8_t *tar, size_t tar_len)
{
	struct buf *b = slices(tar, tar_len, 65536, 64);
	uint8_t small[256];
	memset(small, 1, sizeof(small));
	const char *st[] = { "clean", "after avx512", "after avx512_zu", "after ymm" };
	*dispatch = adler32_avx2_4;
	int reps = 0;
	double t0 = now();
	do {
		pass(b, 64, DEFL_RAW);
		reps++;
	} while (now() - t0 < 0.02);
	double t[4][64];
	for (int r = 0; r < 21; r++)
		for (int j = 0; j < 4; j++) {
			int k = (r & 1) ? 3 - j : j;
			zero_zmm16_31();
			if (k == 1)
				adler32_avx512(1, small, sizeof(small));
			else if (k == 2)
				adler32_avx512_zu(1, small, sizeof(small));
			else if (k == 3)
				adler32_avx512_ymm(1, small, sizeof(small));
			double t1 = now();
			for (int q = 0; q < reps; q++)
				pass(b, 64, DEFL_RAW);
			t[k][r] = (now() - t1) / reps;
		}
	printf("dirty: defl L1 raw on 64K slices, no adler32 calls inside the timing:");
	double base = 0;
	for (int k = 0; k < 4; k++) {
		qsort(t[k], 21, sizeof(double), cmpd);
		double best = 64 * 65536 / t[k][0] / 1e6;
		if (k == 0)
			base = best;
		printf("  %s %.1f (%+.1f%%)", st[k], best, 100 * (best / base - 1));
	}
	printf("\n");
}

/* ---- clock: core clock relative to the TSC after each kernel ---- */

/* TSC ticks per core cycle: a chain of dependent imuls takes 3 cycles each */
static double probe(void)
{
	uint64_t n = 100000, x = 1;
	uint64_t t0 = __rdtsc();
	__asm__ volatile("1:\n\t imul %0, %0\n\t dec %1\n\t jnz 1b" : "+r"(x), "+r"(n));
	return (__rdtsc() - t0) / (3.0 * 100000);
}

static void spin_scalar(double sec)
{
	double t0 = now();
	volatile uint64_t x = 0;
	while (now() - t0 < sec)
		for (int i = 0; i < 1000; i++)
			x += i;
}

static void clock_probe(void)
{
	uint8_t *buf = malloc(65536);
	for (int i = 0; i < 65536; i++)
		buf[i] = rnd();
	double base[9], hot[NV][9], after[NV][9];
	for (int r = 0; r < 9; r++) {
		zero_zmm16_31();
		spin_scalar(0.02);
		base[r] = probe();
		for (int v = 0; v < NV; v++) {
			uint32_t sink = 1;
			double t0 = now();
			while (now() - t0 < 0.005)
				sink = vars[v].f(sink, buf, 65536);
			hot[v][r] = probe();
			spin_scalar(0.005);
			after[v][r] = probe();
			zero_zmm16_31();
			spin_scalar(0.02);
			if (sink == 42)
				puts("");
		}
	}
	qsort(base, 9, sizeof(double), cmpd);
	printf("clock: core cycles per TSC tick, scalar only %.3f\n", 1 / base[4]);
	for (int v = 0; v < NV; v++) {
		qsort(hot[v], 9, sizeof(double), cmpd);
		qsort(after[v], 9, sizeof(double), cmpd);
		printf("clock: %-10s right after 5 ms of calls %.3f (%+.1f%%), 5 ms later %.3f (%+.1f%%)\n",
		       vars[v].name, 1 / hot[v][4], 100 * (base[4] / hot[v][4] - 1), 1 / after[v][4],
		       100 * (base[4] / after[v][4] - 1));
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: adx <dispatch offset> kern|sizes|e2e|dirty|clock [args]\n");
		return 1;
	}
	uint8_t one = 0;
	isal_adler32(1, &one, 1); /* resolve the dispatcher before replacing it */
	dispatch = (afn *)((char *)isal_adler32 + strtol(argv[1], 0, 0));
	if (*dispatch != adler32_avx512 && *dispatch != adler32_avx2_4) {
		fprintf(stderr, "dispatch offset does not point at an adler32 kernel\n");
		return 1;
	}
	level_buf = malloc(ISAL_DEF_LVL1_DEFAULT);
	const char *mode = argv[2];
	int a = 3;
	if (!strcmp(mode, "kern")) {
		kern();
		return 0;
	}
	if (!strcmp(mode, "clock")) {
		clock_probe();
		return 0;
	}
	if (a < argc && !strcmp(argv[a], "-r")) {
		rounds = atoi(argv[a + 1]);
		a += 2;
	}
	if (rounds < 1 || rounds > 64)
		rounds = 11;
	size_t tar_len;
	uint8_t *tar = read_file(argv[a++], &tar_len);
	if (!strcmp(mode, "sizes"))
		sizes(tar, tar_len);
	else if (!strcmp(mode, "dirty"))
		dirty(tar, tar_len);
	else if (!strcmp(mode, "e2e"))
		e2e(tar, tar_len, argc - a, argv + a);
	return 0;
}
