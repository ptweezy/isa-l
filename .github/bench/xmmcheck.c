/* Windows x64: checks that isa-l preserves the callee-saved xmm6-xmm15 across
 * isal_deflate_stateless() at levels 1-3 (which use encode_deflate_icf), and
 * across isal_inflate_stateless() and crc32_gzip_refl() for comparison. A small
 * asm thunk loads xmm6-xmm15 with known values, calls the function, and stores
 * them afterwards. Build with mingw gcc against isa-l.a. Exits 1 if any changed.
 * xmmcheck <file>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "igzip_lib.h"
#include "crc.h"

#if !defined(_WIN64)
#error "Windows x64 only"
#endif

/* int call_with_xmm(int (*fn)(void *), void *arg, const void *xin, void *xout) */
__asm__(".text\n"
	".globl call_with_xmm\n"
	"call_with_xmm:\n"
	"	push %rbp\n"
	"	mov %rsp, %rbp\n"
	"	push %rsi\n"
	"	push %rdi\n"
	"	push %rbx\n"
	"	push %r12\n"
	"	sub $208, %rsp\n" /* 32 shadow, 160 caller xmm6-15, 16 spare */
	"	movdqu %xmm6, 32(%rsp)\n"
	"	movdqu %xmm7, 48(%rsp)\n"
	"	movdqu %xmm8, 64(%rsp)\n"
	"	movdqu %xmm9, 80(%rsp)\n"
	"	movdqu %xmm10, 96(%rsp)\n"
	"	movdqu %xmm11, 112(%rsp)\n"
	"	movdqu %xmm12, 128(%rsp)\n"
	"	movdqu %xmm13, 144(%rsp)\n"
	"	movdqu %xmm14, 160(%rsp)\n"
	"	movdqu %xmm15, 176(%rsp)\n"
	"	mov %r9, %r12\n"
	"	mov %rcx, %rax\n"
	"	mov %rdx, %rcx\n"
	"	movdqu 0(%r8), %xmm6\n"
	"	movdqu 16(%r8), %xmm7\n"
	"	movdqu 32(%r8), %xmm8\n"
	"	movdqu 48(%r8), %xmm9\n"
	"	movdqu 64(%r8), %xmm10\n"
	"	movdqu 80(%r8), %xmm11\n"
	"	movdqu 96(%r8), %xmm12\n"
	"	movdqu 112(%r8), %xmm13\n"
	"	movdqu 128(%r8), %xmm14\n"
	"	movdqu 144(%r8), %xmm15\n"
	"	call *%rax\n"
	"	movdqu %xmm6, 0(%r12)\n"
	"	movdqu %xmm7, 16(%r12)\n"
	"	movdqu %xmm8, 32(%r12)\n"
	"	movdqu %xmm9, 48(%r12)\n"
	"	movdqu %xmm10, 64(%r12)\n"
	"	movdqu %xmm11, 80(%r12)\n"
	"	movdqu %xmm12, 96(%r12)\n"
	"	movdqu %xmm13, 112(%r12)\n"
	"	movdqu %xmm14, 128(%r12)\n"
	"	movdqu %xmm15, 144(%r12)\n"
	"	movdqu 32(%rsp), %xmm6\n"
	"	movdqu 48(%rsp), %xmm7\n"
	"	movdqu 64(%rsp), %xmm8\n"
	"	movdqu 80(%rsp), %xmm9\n"
	"	movdqu 96(%rsp), %xmm10\n"
	"	movdqu 112(%rsp), %xmm11\n"
	"	movdqu 128(%rsp), %xmm12\n"
	"	movdqu 144(%rsp), %xmm13\n"
	"	movdqu 160(%rsp), %xmm14\n"
	"	movdqu 176(%rsp), %xmm15\n"
	"	add $208, %rsp\n"
	"	pop %r12\n"
	"	pop %rbx\n"
	"	pop %rdi\n"
	"	pop %rsi\n"
	"	pop %rbp\n"
	"	ret\n");
int call_with_xmm(int (*fn)(void *), void *arg, const void *xin, void *xout);

struct job {
	uint8_t *in, *out, *dec;
	size_t len, cap, clen;
	int level;
	uint8_t *level_buf;
};

static int do_deflate(void *p)
{
	struct job *j = p;
	struct isal_zstream s;
	static const uint32_t lbs[4] = { ISAL_DEF_LVL0_DEFAULT, ISAL_DEF_LVL1_DEFAULT,
					 ISAL_DEF_LVL2_DEFAULT, ISAL_DEF_LVL3_DEFAULT };
	isal_deflate_stateless_init(&s);
	s.next_in = j->in;
	s.avail_in = j->len;
	s.next_out = j->out;
	s.avail_out = j->cap;
	s.level = j->level;
	s.level_buf = j->level_buf;
	s.level_buf_size = lbs[j->level];
	s.end_of_stream = 1;
	int r = isal_deflate_stateless(&s);
	j->clen = s.total_out;
	return r;
}

static int do_inflate(void *p)
{
	struct job *j = p;
	struct inflate_state st;
	isal_inflate_init(&st);
	st.next_in = j->out;
	st.avail_in = j->clen;
	st.next_out = j->dec;
	st.avail_out = j->len;
	int r = isal_inflate_stateless(&st);
	return r != ISAL_DECOMP_OK || st.total_out != j->len || memcmp(j->dec, j->in, j->len);
}

static int do_crc(void *p)
{
	struct job *j = p;
	return (int)(crc32_gzip_refl(0, j->in, j->len) & 0x7fffffff) * 0;
}

static int check(const char *what, int (*fn)(void *), struct job *j)
{
	uint8_t xin[160], xout[160];
	for (int i = 0; i < 160; i++)
		xin[i] = (uint8_t)(0x31 + 7 * i);
	memset(xout, 0, sizeof(xout));
	int r = call_with_xmm(fn, j, xin, xout);
	int bad = 0;
	for (int x = 0; x < 10; x++)
		if (memcmp(xin + 16 * x, xout + 16 * x, 16)) {
			printf("%s: xmm%d changed\n", what, 6 + x);
			bad = 1;
		}
	printf("%s: returned %d, xmm6-xmm15 %s\n", what, r, bad ? "CORRUPTED" : "preserved");
	return bad || r;
}

int main(int argc, char **argv)
{
	struct job j;
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL)
		return 2;
	fseek(f, 0, SEEK_END);
	j.len = ftell(f);
	if (j.len > (8 << 20))
		j.len = 8 << 20;
	fseek(f, 0, SEEK_SET);
	j.in = malloc(j.len);
	if (fread(j.in, 1, j.len, f) != j.len)
		return 2;
	fclose(f);
	j.cap = j.len + j.len / 8 + 65536;
	j.out = malloc(j.cap);
	j.dec = malloc(j.len);
	j.level_buf = malloc(ISAL_DEF_LVL3_DEFAULT);
	int fail = 0;
	for (j.level = 0; j.level <= 3; j.level++) {
		char what[64];
		snprintf(what, sizeof(what), "isal_deflate_stateless level %d", j.level);
		fail |= check(what, do_deflate, &j);
		snprintf(what, sizeof(what), "isal_inflate_stateless of level %d", j.level);
		fail |= check(what, do_inflate, &j);
	}
	fail |= check("crc32_gzip_refl", do_crc, &j);
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail;
}
