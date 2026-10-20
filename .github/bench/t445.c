// Differential tests for PR 445: adler32_avx512 and crc32_iscsi_by16_10_fused
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint32_t adler32_base(uint32_t, uint8_t *, uint64_t);
// Weak, so the checker links against builds that have only one of the two kernels
__attribute__((weak)) uint32_t adler32_avx512(uint32_t, const unsigned char *, uint64_t);
uint32_t adler32_avx2_4(uint32_t, const unsigned char *, uint64_t);
uint32_t isal_adler32(uint32_t, const unsigned char *, uint64_t);

unsigned int crc32_iscsi(unsigned char *, int, unsigned int);
unsigned int crc32_iscsi_base(unsigned char *, int, unsigned int);
unsigned int crc32_iscsi_by16_10(unsigned char *, uint64_t, unsigned int);
__attribute__((weak)) unsigned int crc32_iscsi_by16_10_fused(unsigned char *, uint64_t, unsigned int);

#define BASE 65521

static uint64_t rs = 0x9e3779b97f4a7c15ull;
static inline uint64_t
rnd(void)
{
        rs ^= rs << 13;
        rs ^= rs >> 7;
        rs ^= rs << 17;
        return rs;
}

static long fails = 0, checks = 0;

static uint32_t
adler_ref(uint32_t init, const uint8_t *p, uint64_t n)
{
        uint64_t a = (init & 0xffff) % BASE, b = (init >> 16) % BASE;
        for (uint64_t i = 0; i < n; i++) {
                a += p[i];
                if (a >= BASE)
                        a -= BASE;
                b += a;
                if (b >= BASE)
                        b -= BASE;
        }
        return (uint32_t) (b << 16 | a);
}

static uint32_t
crc_ref(uint32_t crc, const uint8_t *p, uint64_t n)
{
        // bitwise reflected CRC32C (0x82F63B78), no pre/post inversion (like crc32_iscsi)
        for (uint64_t i = 0; i < n; i++) {
                crc ^= p[i];
                for (int k = 0; k < 8; k++)
                        crc = (crc >> 1) ^ (0x82F63B78u & -(crc & 1));
        }
        return crc;
}

static uint32_t
pick_init(void)
{
        static const uint32_t special[] = { 0,          1,          0xFFFFFFFF, 0xFFF0FFF0,
                                            0xFFF1FFF1, 0x0000FFF0, 0xFFF00000, 0xFFFF0000,
                                            0x0000FFFF };
        uint64_t r = rnd() % 16;
        if (r < 9)
                return special[r];
        if (r < 12) // valid adler value
                return (uint32_t) ((rnd() % BASE) << 16 | (rnd() % BASE));
        return (uint32_t) rnd();
}

static void
fill(uint8_t *p, uint64_t n, int pat)
{
        switch (pat) {
        case 0:
                for (uint64_t i = 0; i < n; i++)
                        p[i] = (uint8_t) rnd();
                break;
        case 1:
                memset(p, 0xff, n);
                break;
        case 2:
                for (uint64_t i = 0; i < n; i++)
                        p[i] = (rnd() & 1) ? 0xff : 0;
                break;
        default:
                memset(p, 0, n);
        }
}

static void
check_adler(const char *what, uint32_t init, uint8_t *p, uint64_t n, int with_ref)
{
        if (!adler32_avx512)
                return;
        uint32_t want = adler32_base(init, p, n);
        uint32_t g1 = adler32_avx512(init, p, n);
        uint32_t g2 = isal_adler32(init, p, n);
        checks++;
        if (g1 != want || g2 != want) {
                if (fails++ < 20)
                        printf("ADLER FAIL %s len=%lu init=%08x align=%lu want=%08x avx512=%08x "
                               "isal=%08x\n",
                               what, (unsigned long) n, init, (unsigned long) ((uintptr_t) p & 63),
                               want, g1, g2);
        }
        if (with_ref) {
                uint32_t r = adler_ref(init, p, n);
                if (r != want && fails++ < 20)
                        printf("ADLER REF MISMATCH %s len=%lu init=%08x ref=%08x base=%08x\n", what,
                               (unsigned long) n, init, r, want);
        }
}

static void
check_crc(const char *what, uint32_t init, uint8_t *p, uint64_t n, int with_ref)
{
        uint32_t want = crc32_iscsi_base(p, (int) n, init);
        if (!crc32_iscsi_by16_10_fused)
                return;
        uint32_t g1 = crc32_iscsi_by16_10_fused(p, n, init);
        uint32_t g2 = crc32_iscsi(p, (int) n, init);
        uint32_t g3 = crc32_iscsi_by16_10(p, n, init);
        checks++;
        if (g1 != want || g2 != want || g3 != want) {
                if (fails++ < 20)
                        printf("CRC FAIL %s len=%lu init=%08x align=%lu want=%08x fused=%08x "
                               "iscsi=%08x by16=%08x\n",
                               what, (unsigned long) n, init, (unsigned long) ((uintptr_t) p & 63),
                               want, g1, g2, g3);
        }
        if (with_ref) {
                uint32_t r = crc_ref(init, p, n);
                if (r != want && fails++ < 20)
                        printf("CRC REF MISMATCH %s len=%lu ref=%08x base=%08x\n", what,
                               (unsigned long) n, r, want);
        }
}

// Map [guard][data pages][guard]; return pointer such that buffer of n ends at the upper guard
static uint8_t *
guard_buf(uint64_t maxn, uint8_t **lo_out, size_t *maplen)
{
        size_t pg = 4096;
        size_t data = (maxn + 64 + pg - 1) / pg * pg;
        size_t len = data + 2 * pg;
        uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
                perror("mmap");
                exit(1);
        }
        mprotect(m, pg, PROT_NONE);
        mprotect(m + pg + data, pg, PROT_NONE);
        *lo_out = m + pg;
        *maplen = len;
        return m + pg + data; // end of data (start of upper guard)
}

int
main(int argc, char **argv)
{
        long dispatch_off_adler = argc > 2 ? strtol(argv[1], 0, 0) : 0;
        long dispatch_off_crc = argc > 2 ? strtol(argv[2], 0, 0) : 0;
        int big = argc > 3 ? atoi(argv[3]) : 1;

        // dispatch check
        uint8_t tmp[8192];
        fill(tmp, sizeof(tmp), 0);
        isal_adler32(1, tmp, 100);
        crc32_iscsi(tmp, 5000, 0);
        if (dispatch_off_adler) {
                void *pa = *(void **) ((char *) isal_adler32 + dispatch_off_adler);
                void *pc = *(void **) ((char *) crc32_iscsi + dispatch_off_crc);
                printf("isal_adler32 -> %s\n", pa == (void *) adler32_avx512   ? "adler32_avx512"
                                               : pa == (void *) adler32_avx2_4 ? "adler32_avx2_4"
                                                                               : "other");
                printf("crc32_iscsi -> %s\n", pc == (void *) crc32_iscsi_by16_10_fused
                                                      ? "crc32_iscsi_by16_10_fused"
                                              : pc == (void *) crc32_iscsi_by16_10 ? "by16_10"
                                                                                   : "other");
        }

        // MOD_BASE identity: (x * 0x80078071) >> 47 == x / 65521 for all 32-bit x
        {
                uint64_t bad = 0;
                for (uint64_t x = 0; x < (1ull << 32); x++)
                        if (((x * 0x80078071ull) >> 47) != x / BASE)
                                bad++;
                printf("MOD_BASE reciprocal wrong for %lu 32-bit values\n", (unsigned long) bad);
                if (bad)
                        fails++;
        }

        // 1. Adler: every length 0..20000, 3 patterns, random alignment and init
        {
                uint8_t *buf = aligned_alloc(64, 20000 + 128);
                for (uint64_t n = 0; n <= 20000; n++) {
                        for (int pat = 0; pat < 3; pat++) {
                                uint8_t *p = buf + rnd() % 64;
                                fill(p, n, pat);
                                check_adler("all-len", pick_init(), p, n, n < 3000 && pat == 0);
                        }
                }
                // worst case for the 32-bit bound: all 0xFF, max a and b, chunk-sized lengths
                memset(buf, 0xff, 20000 + 128);
                uint32_t inits[] = { 0xFFF0FFF0, 0xFFFFFFFF, 0xFFF1FFF1, 0xFFFFFFF0, 0xFFF0FFFF };
                for (int i = 0; i < 5; i++)
                        for (uint64_t n = 5440; n <= 5600; n++)
                                for (int al = 0; al < 64; al += 7)
                                        check_adler("worst", inits[i], buf + al, n, 1);
                free(buf);
                printf("adler all lengths done, checks=%ld fails=%ld\n", checks, fails);
        }

        // 2. Adler: random lengths up to 64 MB
        {
                uint64_t maxn = 64ull << 20;
                uint8_t *buf = aligned_alloc(64, maxn + 128);
                fill(buf, maxn + 128, 0);
                for (int i = 0; i < 40 * big; i++) {
                        uint64_t n = rnd() % (maxn + 1);
                        uint8_t *p = buf + rnd() % 64;
                        int pat = i % 3;
                        if (pat == 1)
                                memset(p, 0xff, n);
                        else if (pat == 2)
                                fill(p, n, 2);
                        check_adler("big", pick_init(), p, n, 0);
                        if (pat)
                                fill(buf, maxn + 128, 0);
                }
                // exact 64 MB all-0xFF with max init
                memset(buf, 0xff, maxn);
                check_adler("64MB-ff", 0xFFF0FFF0, buf, maxn, 1);
                free(buf);
                printf("adler big done, checks=%ld fails=%ld\n", checks, fails);
        }

        // 3. Adler guard pages: buffers ending at PROT_NONE (and starting right after one)
        {
                uint8_t *lo;
                size_t ml;
                uint64_t maxn = 64ull << 20;
                uint8_t *end = guard_buf(maxn, &lo, &ml);
                fill(lo, end - lo, 0);
                for (uint64_t n = 0; n <= 20000; n++) {
                        check_adler("guard-end", pick_init(), end - n, n, 0);
                        check_adler("guard-start", pick_init(), lo, n, 0);
                }
                for (int i = 0; i < 20; i++) {
                        uint64_t n = rnd() % (maxn + 1);
                        check_adler("guard-end-big", pick_init(), end - n, n, 0);
                }
                check_adler("guard-end-64MB", pick_init(), end - maxn, maxn, 0);
                munmap(lo - 4096, ml);
                printf("adler guard done, checks=%ld fails=%ld\n", checks, fails);
        }

        // 4. CRC: every length 0..30000, 3 patterns
        {
                uint8_t *buf = aligned_alloc(64, 30000 + 128);
                for (uint64_t n = 0; n <= 30000; n++) {
                        for (int pat = 0; pat < 4; pat += (pat == 0 ? 1 : 2)) {
                                uint8_t *p = buf + rnd() % 64;
                                fill(p, n, pat);
                                check_crc("all-len", (uint32_t) rnd(), p, n,
                                          (n % 97 == 0) && pat == 0);
                        }
                }
                free(buf);
                printf("crc all lengths done, checks=%ld fails=%ld\n", checks, fails);
        }

        // 5. CRC: boundaries 4096 + k*3584 + d, and random lengths up to 16 MB
        {
                uint64_t maxn = 16ull << 20;
                uint8_t *buf = aligned_alloc(64, maxn + 128);
                fill(buf, maxn + 128, 0);
                int ds[] = { -513, -512, -257, -256, -255, -1, 0, 1, 255, 256, 257, 511, 512, 513 };
                for (uint64_t k = 0; 4096 + k * 3584 + 600 < maxn; k += (k < 50 ? 1 : 37))
                        for (unsigned j = 0; j < sizeof(ds) / sizeof(ds[0]); j++) {
                                uint64_t n = 4096 + k * 3584 + ds[j];
                                check_crc("bound", (uint32_t) rnd(), buf + rnd() % 64, n, 0);
                        }
                for (int i = 0; i < 300 * big; i++) {
                        uint64_t n = rnd() % (maxn + 1);
                        check_crc("big", (uint32_t) rnd(), buf + rnd() % 64, n, i < 3);
                }
                free(buf);
                printf("crc big done, checks=%ld fails=%ld\n", checks, fails);
        }

        // 6. CRC guard pages
        {
                uint8_t *lo;
                size_t ml;
                uint64_t maxn = 16ull << 20;
                uint8_t *end = guard_buf(maxn, &lo, &ml);
                fill(lo, end - lo, 0);
                for (uint64_t n = 0; n <= 30000; n++) {
                        check_crc("guard-end", (uint32_t) rnd(), end - n, n, 0);
                        check_crc("guard-start", (uint32_t) rnd(), lo, n, 0);
                }
                for (int i = 0; i < 50; i++) {
                        uint64_t n = rnd() % (maxn + 1);
                        check_crc("guard-end-big", (uint32_t) rnd(), end - n, n, 0);
                        check_crc("guard-start-big", (uint32_t) rnd(), lo, n, 0);
                }
                munmap(lo - 4096, ml);
                printf("crc guard done, checks=%ld fails=%ld\n", checks, fails);
        }

        // 7. Adler > 4 GB (sparse zero mapping with a few nonzero bytes)
        if (big) {
                uint64_t n = (4ull << 30) + 12345;
                uint8_t *m = mmap(NULL, n + 4096, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
                if (m != MAP_FAILED) {
                        for (uint64_t off = 0; off < n; off += (n / 7))
                                m[off] = 0xA5;
                        m[n - 1] = 0x5A;
                        m[(1ull << 32) - 1] = 0xFF;
                        m[(1ull << 32)] = 0xFE;
                        check_adler(">4GB", 0xFFF0FFF0, m, n, 0);
                        check_adler(">4GB-off", 0x12345678, m + 3, n - 3, 0);
                        munmap(m, n + 4096);
                        printf(">4GB adler done, checks=%ld fails=%ld\n", checks, fails);
                } else {
                        printf(">4GB mmap failed\n");
                }
        }

        printf("TOTAL checks=%ld fails=%ld => %s\n", checks, fails, fails ? "FAIL" : "PASS");
        return fails != 0;
}
