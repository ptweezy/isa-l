/* In-process A/B igzip harness: dlopens several libisal.so builds and
 * alternates them round by round so frequency drift cancels.
 * ab [-r rounds] [-c filter] [-w|-s] [-z sizes] lib1.so lib2.so ... -- tar files...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include "igzip_lib.h"

#define MAXLIB 4
struct lib {
	const char *path;
	void (*defl_init)(struct isal_zstream *);
	int (*defl)(struct isal_zstream *);
	void (*infl_init)(struct inflate_state *);
	int (*infl)(struct inflate_state *);
	uint32_t (*adler)(uint32_t, const unsigned char *, uint64_t);
	uint32_t (*crc)(uint32_t, const unsigned char *, uint64_t);
} libs[MAXLIB];
static int nlib;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
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

struct buf {
	uint8_t *in;
	size_t in_len;
	uint8_t *out;
	size_t out_cap;
	size_t out_len;
	uint8_t *comp;
	size_t comp_len;
};

static uint32_t lvl_size[4] = { ISAL_DEF_LVL0_DEFAULT, ISAL_DEF_LVL1_DEFAULT,
				ISAL_DEF_LVL2_DEFAULT, ISAL_DEF_LVL3_DEFAULT };
static uint8_t *level_buf;

static size_t comp1(struct lib *L, uint8_t *in, size_t len, uint8_t *out, size_t cap, int level,
		    int wrapper)
{
	struct isal_zstream s;
	L->defl_init(&s);
	s.next_in = in;
	s.avail_in = len;
	s.next_out = out;
	s.avail_out = cap;
	s.level = level;
	s.level_buf = level_buf;
	s.level_buf_size = lvl_size[level];
	s.end_of_stream = 1;
	s.gzip_flag = wrapper;
	int r = L->defl(&s);
	if (r != COMP_OK) {
		fprintf(stderr, "deflate error %d\n", r);
		exit(1);
	}
	return s.total_out;
}

static size_t decomp1(struct lib *L, uint8_t *in, size_t len, uint8_t *out, size_t cap, int wrapper)
{
	struct inflate_state st;
	L->infl_init(&st);
	st.next_in = in;
	st.avail_in = len;
	st.next_out = out;
	st.avail_out = cap;
	st.crc_flag = wrapper;
	int r = L->infl(&st);
	if (r != ISAL_DECOMP_OK) {
		fprintf(stderr, "inflate error %d\n", r);
		exit(1);
	}
	return st.total_out;
}

static size_t zlib_comp(uint8_t *in, size_t len, uint8_t *out, size_t cap, int level)
{
	z_stream z;
	memset(&z, 0, sizeof(z));
	deflateInit2(&z, level, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
	z.next_in = in;
	z.avail_in = len;
	z.next_out = out;
	z.avail_out = cap;
	if (deflate(&z, Z_FINISH) != Z_STREAM_END)
		exit(1);
	size_t r = z.total_out;
	deflateEnd(&z);
	return r;
}

static int rounds = 15;
static const char *filter;
enum op { DEFL, INFL, ADLER, CRC, INFL_EXACT };

static uint32_t sink;

static void one_pass(struct lib *L, struct buf *b, int n, enum op op, int level, int wrapper)
{
	for (int i = 0; i < n; i++) {
		if (op == DEFL)
			b[i].out_len = comp1(L, b[i].in, b[i].in_len, b[i].out, b[i].out_cap, level, wrapper);
		else if (op == INFL)
			b[i].out_len = decomp1(L, b[i].comp, b[i].comp_len, b[i].out, b[i].out_cap, wrapper);
		else if (op == INFL_EXACT)
			b[i].out_len = decomp1(L, b[i].comp, b[i].comp_len, b[i].out, b[i].in_len, wrapper);
		else if (op == ADLER)
			sink += L->adler(1, b[i].in, b[i].in_len);
		else
			sink += L->crc(0, b[i].in, b[i].in_len);
	}
}

static uint32_t out_crc(struct buf *b, int n, enum op op, const char *name)
{
	uLong crc = crc32(0, NULL, 0);
	for (int i = 0; i < n; i++) {
		if (op != DEFL && op != INFL && op != INFL_EXACT)
			break;
		crc = crc32(crc, b[i].out, b[i].out_len);
		if ((op == INFL || op == INFL_EXACT) && (b[i].out_len != b[i].in_len || memcmp(b[i].out, b[i].in, b[i].in_len))) {
			fprintf(stderr, "%s: inflate mismatch on %d\n", name, i);
			exit(1);
		}
	}
	return crc;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(double *)a, y = *(double *)b;
	return x < y ? -1 : x > y;
}

static void run_case(const char *name, struct buf *b, int n, enum op op, int level, int wrapper)
{
	size_t total = 0;
	for (int i = 0; i < n; i++)
		total += b[i].in_len;
	uint32_t crcs[MAXLIB];
	int reps = 1;
	for (int l = 0; l < nlib; l++) {
		double t0 = now();
		int r = 0;
		do {
			one_pass(&libs[l], b, n, op, level, wrapper);
			r++;
		} while (now() - t0 < 0.02);
		if (l == 0)
			reps = r;
		crcs[l] = out_crc(b, n, op, name);
	}
	double t[MAXLIB][256];
	for (int r = 0; r < rounds; r++) {
		for (int k = 0; k < nlib; k++) {
			int l = (r & 1) ? nlib - 1 - k : k;
			double t0 = now();
			for (int j = 0; j < reps; j++)
				one_pass(&libs[l], b, n, op, level, wrapper);
			t[l][r] = (now() - t0) / reps;
		}
	}
	printf("%-22s", name);
	double best0 = 0, med0 = 0;
	for (int l = 0; l < nlib; l++) {
		qsort(t[l], rounds, sizeof(double), cmpd);
		double best = total / t[l][0] / 1e6, med = total / t[l][rounds / 2] / 1e6;
		if (l == 0) {
			best0 = best;
			med0 = med;
			printf(" %8.1f", best);
		} else {
			printf(" %8.1f %+6.1f%% (med %+5.1f%%)", best, 100 * (best / best0 - 1),
			       100 * (med / med0 - 1));
		}
	}
	int same = 1;
	for (int l = 1; l < nlib; l++)
		same &= crcs[l] == crcs[0];
	printf("  %s\n", same ? "" : "OUTPUT DIFFERS");
	fflush(stdout);
}

static void prep_comp(struct buf *b, int n, int how, int wrapper)
{
	for (int i = 0; i < n; i++) {
		free(b[i].comp);
		b[i].comp = malloc(b[i].out_cap);
		if (how < 0)
			b[i].comp_len = zlib_comp(b[i].in, b[i].in_len, b[i].comp, b[i].out_cap, -how);
		else
			b[i].comp_len = comp1(&libs[0], b[i].in, b[i].in_len, b[i].comp, b[i].out_cap, how,
					      wrapper);
	}
}

static int want(const char *name) { return !filter || strstr(name, filter); }

static void suite(const char *tag, struct buf *b, int n)
{
	char name[64];
	for (int l = 0; l <= 3; l++) {
		snprintf(name, sizeof(name), "%s defl L%d", tag, l);
		if (want(name))
			run_case(name, b, n, DEFL, l, 0);
	}
	snprintf(name, sizeof(name), "%s defl L1 zlib", tag);
	if (want(name))
		run_case(name, b, n, DEFL, 1, IGZIP_ZLIB);
	snprintf(name, sizeof(name), "%s infl zlib6", tag);
	if (want(name)) {
		prep_comp(b, n, -6, 0);
		run_case(name, b, n, INFL, 0, 0);
	}
	snprintf(name, sizeof(name), "%s infl isal1", tag);
	if (want(name)) {
		prep_comp(b, n, 1, 0);
		run_case(name, b, n, INFL, 0, 0);
	}
	snprintf(name, sizeof(name), "%s infl isal0", tag);
	if (want(name)) {
		prep_comp(b, n, 0, 0);
		run_case(name, b, n, INFL, 0, 0);
	}
	snprintf(name, sizeof(name), "%s infl isal1 zlib", tag);
	if (want(name)) {
		prep_comp(b, n, 1, IGZIP_ZLIB);
		run_case(name, b, n, INFL, 0, ISAL_ZLIB);
	}
	snprintf(name, sizeof(name), "%s infl zlib6 exact", tag);
	if (want(name)) {
		prep_comp(b, n, -6, 0);
		run_case(name, b, n, INFL_EXACT, 0, 0);
	}
	snprintf(name, sizeof(name), "%s infl isal1 exact", tag);
	if (want(name)) {
		prep_comp(b, n, 1, 0);
		run_case(name, b, n, INFL_EXACT, 0, 0);
	}
	snprintf(name, sizeof(name), "%s adler32", tag);
	if (want(name) && libs[0].adler)
		run_case(name, b, n, ADLER, 0, 0);
}

int main(int argc, char **argv)
{
	int a = 1;
	int do_whole = 1, do_small = 1;
	size_t sizes[8] = { 1024, 4096, 16384, 65536 };
	int nsizes = 4;
	while (a < argc && argv[a][0] == '-' && strcmp(argv[a], "--")) {
		if (!strcmp(argv[a], "-r"))
			rounds = atoi(argv[++a]);
		else if (!strcmp(argv[a], "-c"))
			filter = argv[++a];
		else if (!strcmp(argv[a], "-w"))
			do_small = 0;
		else if (!strcmp(argv[a], "-s"))
			do_whole = 0;
		else if (!strcmp(argv[a], "-z")) {
			nsizes = 0;
			for (char *p = strtok(argv[++a], ","); p && nsizes < 8; p = strtok(NULL, ","))
				sizes[nsizes++] = atol(p) * 1024;
		}
		a++;
	}
	if (rounds > 256)
		rounds = 256;
	while (a < argc && strcmp(argv[a], "--")) {
		struct lib *L = &libs[nlib++];
		L->path = argv[a];
		void *h = dlopen(argv[a], RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			fprintf(stderr, "%s\n", dlerror());
			return 1;
		}
		L->defl_init = dlsym(h, "isal_deflate_stateless_init");
		L->defl = dlsym(h, "isal_deflate_stateless");
		L->infl_init = dlsym(h, "isal_inflate_init");
		L->infl = dlsym(h, "isal_inflate_stateless");
		L->adler = dlsym(h, "isal_adler32");
		L->crc = dlsym(h, "crc32_gzip_refl");
		a++;
	}
	a++;
	printf("%-22s", "case");
	for (int l = 0; l < nlib; l++) {
		const char *p = libs[l].path, *q = strstr(p, "/isal/");
		char nm[32];
		snprintf(nm, sizeof(nm), "%s", q ? q + 6 : p);
		char *s = strchr(nm, '/');
		if (s)
			*s = 0;
		printf(l ? " %8s %24s" : " %8s", nm, "");
	}
	printf("\n");

	level_buf = malloc(ISAL_DEF_LVL3_DEFAULT);
	size_t tar_len;
	uint8_t *tar = read_file(argv[a++], &tar_len);

	if (do_whole && a < argc) {
		int n = argc - a;
		struct buf *b = calloc(n, sizeof(*b));
		for (int i = 0; i < n; i++) {
			b[i].in = read_file(argv[a + i], &b[i].in_len);
			b[i].out_cap = b[i].in_len + b[i].in_len / 8 + 65536;
			b[i].out = malloc(b[i].out_cap);
		}
		suite("whole", b, n);
	}
	if (do_small) {
		for (int s = 0; s < nsizes; s++) {
			int n = 256;
			struct buf *b = calloc(n, sizeof(*b));
			for (int i = 0; i < n; i++) {
				size_t off = (tar_len - sizes[s]) / n * i;
				b[i].in = tar + off;
				b[i].in_len = sizes[s];
				b[i].out_cap = sizes[s] * 2 + 1024;
				b[i].out = malloc(b[i].out_cap);
			}
			char tag[32];
			snprintf(tag, sizeof(tag), "%dK", (int)(sizes[s] / 1024));
			suite(tag, b, n);
		}
	}
	return 0;
}
