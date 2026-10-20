/* Differential deflate scan: compresses slices of a file with several
 * libisal.so builds (stateless, levels 0 to 3) and reports every slice whose
 * output differs from the first build's, with whether each output inflates
 * back to the slice with zlib.
 * With -p, the level buffer is filled with a different byte before every
 * call, to show output that depends on what an earlier call left there.
 * scan [-p] [-z sizes in KB] [-t stride] [-l levels] lib1.so lib2.so ... -- file
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "igzip_lib.h"

#define MAXLIB 4
struct lib {
	const char *name;
	void (*defl_init)(struct isal_zstream *);
	int (*defl)(struct isal_zstream *);
} libs[MAXLIB];
static int nlib;

static uint32_t lvl_size[4] = { ISAL_DEF_LVL0_DEFAULT, ISAL_DEF_LVL1_DEFAULT,
				ISAL_DEF_LVL2_DEFAULT, ISAL_DEF_LVL3_DEFAULT };

static size_t comp1(struct lib *L, uint8_t *in, size_t len, uint8_t *out, size_t cap, int level,
		    uint8_t *level_buf)
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
	int r = L->defl(&s);
	if (r != COMP_OK) {
		fprintf(stderr, "%s: deflate error %d\n", L->name, r);
		exit(1);
	}
	return s.total_out;
}

/* 1 if the raw deflate stream inflates to exactly in[0..len) */
static int valid(uint8_t *comp, size_t clen, uint8_t *in, size_t len, uint8_t *tmp)
{
	z_stream z;
	memset(&z, 0, sizeof(z));
	inflateInit2(&z, -15);
	z.next_in = comp;
	z.avail_in = clen;
	z.next_out = tmp;
	z.avail_out = len + 1;
	int r = inflate(&z, Z_FINISH);
	size_t out = z.total_out;
	inflateEnd(&z);
	return r == Z_STREAM_END && out == len && !memcmp(tmp, in, len);
}

int main(int argc, char **argv)
{
	size_t sizes[8] = { 1024, 4096, 16384, 65536 };
	int nsizes = 4;
	size_t stride = 4093;
	int levels = 0xf;
	int poison = 0;
	int a = 1;
	while (a < argc && argv[a][0] == '-' && strcmp(argv[a], "--")) {
		if (!strcmp(argv[a], "-p"))
			poison = 1;
		else if (!strcmp(argv[a], "-z")) {
			nsizes = 0;
			for (char *p = strtok(argv[++a], ","); p && nsizes < 8; p = strtok(NULL, ","))
				sizes[nsizes++] = atol(p) * 1024;
		} else if (!strcmp(argv[a], "-t"))
			stride = atol(argv[++a]);
		else if (!strcmp(argv[a], "-l")) {
			levels = 0;
			for (char *p = strtok(argv[++a], ","); p; p = strtok(NULL, ","))
				levels |= 1 << atoi(p);
		}
		a++;
	}
	while (a < argc && strcmp(argv[a], "--") && nlib < MAXLIB) {
		struct lib *L = &libs[nlib++];
		void *h = dlopen(argv[a], RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			fprintf(stderr, "%s\n", dlerror());
			return 1;
		}
		const char *q = strstr(argv[a], "/isal/");
		char *nm = strdup(q ? q + 6 : argv[a]);
		char *s = strchr(nm, '/');
		if (s)
			*s = 0;
		L->name = nm;
		L->defl_init = dlsym(h, "isal_deflate_stateless_init");
		L->defl = dlsym(h, "isal_deflate_stateless");
		a++;
	}
	a++;
	if (a >= argc || nlib < 2) {
		fprintf(stderr,
			"usage: scan [-p] [-z KB,..] [-t stride] [-l levels] lib1 lib2 .. -- file\n");
		return 1;
	}
	FILE *f = fopen(argv[a], "rb");
	if (!f) {
		perror(argv[a]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size_t flen = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *file = malloc(flen);
	if (fread(file, 1, flen, f) != flen)
		return 1;
	fclose(f);

	uint8_t *level_buf = malloc(ISAL_DEF_LVL3_DEFAULT);
	size_t cap = (64 << 20) + 65536;
	uint8_t *out[MAXLIB], *tmp = malloc(cap);
	for (int l = 0; l < nlib; l++)
		out[l] = malloc(cap);

	long total_diff = 0, total_bad = 0;
	for (int lv = 0; lv <= 3; lv++) {
		if (!(levels & (1 << lv)))
			continue;
		for (int si = 0; si < nsizes; si++) {
			size_t len = sizes[si];
			long n = 0, ndiff = 0, nbad = 0;
			/* Slices where each build differs from the first build, and from the second */
			long vs0[MAXLIB] = { 0 }, vs1[MAXLIB] = { 0 };
			for (size_t off = 0; off + len <= flen; off += stride, n++) {
				size_t clen[MAXLIB];
				for (int l = 0; l < nlib; l++) {
					if (poison)
						memset(level_buf, (int)(off * 31 + l * 97), lvl_size[lv]);
					clen[l] = comp1(&libs[l], file + off, len, out[l], cap, lv,
							level_buf);
				}
				int differs = 0;
				for (int l = 1; l < nlib; l++) {
					int d0 = clen[l] != clen[0] || memcmp(out[l], out[0], clen[0]);
					int d1 = clen[l] != clen[1] || memcmp(out[l], out[1], clen[1]);
					vs0[l] += d0;
					vs1[l] += d1;
					differs |= d0;
				}
				if (!differs)
					continue;
				ndiff++;
				char line[256];
				int p = snprintf(line, sizeof(line), "  L%d %zuK off %zu:", lv, len >> 10,
						 off);
				for (int l = 0; l < nlib; l++) {
					int ok = valid(out[l], clen[l], file + off, len, tmp);
					nbad += !ok;
					p += snprintf(line + p, sizeof(line) - p, " %s %zu%s", libs[l].name,
						      clen[l], ok ? "" : " INVALID");
				}
				if (ndiff <= 10)
					printf("%s\n", line);
			}
			printf("L%d %5zuK: %ld slices, %ld differ, %ld invalid outputs\n", lv, len >> 10,
			       n, ndiff, nbad);
			if (ndiff)
				for (int l = 1; l < nlib; l++)
					printf("    %s differs from %s in %ld, from %s in %ld\n",
					       libs[l].name, libs[0].name, vs0[l], libs[1].name, vs1[l]);
			fflush(stdout);
			total_diff += ndiff;
			total_bad += nbad;
		}
	}
	printf("TOTAL differ %ld invalid %ld\n", total_diff, total_bad);
	return total_bad != 0;
}
