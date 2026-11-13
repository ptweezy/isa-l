/* A sampling profiler inside the benchmark: a timer interrupts the process
 * every half millisecond of CPU time, and the handler counts the address of
 * the instruction that was running. prof_write() saves the counts, with the
 * address of prof_start() itself so that tools/hot.py can work out where the
 * binary was loaded. Needs no privileges and no debugger. */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#include "backends.h"

#define SLOTS (1u << 16)

static struct {
	uintptr_t pc;
	unsigned count;
} table[SLOTS];
static unsigned long lost;

static void on_prof(int sig, siginfo_t *info, void *ctx)
{
	ucontext_t *uc = ctx;
	uintptr_t pc;
	unsigned i, n;

	(void)sig;
	(void)info;
#if defined(__APPLE__) && defined(__aarch64__)
	pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
	pc = (uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__aarch64__)
	pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
	pc = (uintptr_t)uc->uc_mcontext.gregs[16]; /* REG_RIP */
#else
	pc = 0;
#endif
	i = (unsigned)((pc * 0x9E3779B97F4A7C15ull) >> 48) & (SLOTS - 1);
	for (n = 0; n < 64; n++, i = (i + 1) & (SLOTS - 1)) {
		if (table[i].pc == pc || table[i].count == 0) {
			table[i].pc = pc;
			table[i].count++;
			return;
		}
	}
	lost++;
}

static void set_timer(long usec)
{
	struct itimerval t;

	memset(&t, 0, sizeof(t));
	t.it_interval.tv_usec = usec;
	t.it_value.tv_usec = usec;
	setitimer(ITIMER_PROF, &t, NULL);
}

void prof_start(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPROF, &sa, NULL);
	set_timer(500);
}

void prof_stop(void)
{
	set_timer(0);
}

int prof_write(const char *path)
{
	FILE *f = fopen(path, "w");
	unsigned i;

	if (!f)
		return -1;
	fprintf(f, "ref prof_start 0x%llx\n",
		(unsigned long long)(uintptr_t)&prof_start);
	fprintf(f, "lost %lu\n", lost);
	for (i = 0; i < SLOTS; i++)
		if (table[i].count)
			fprintf(f, "0x%llx %u\n", (unsigned long long)table[i].pc,
				table[i].count);
	return fclose(f);
}
