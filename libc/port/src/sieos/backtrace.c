/*
 * backtrace.c - Stack backtraces, as Solaris's: backtrace(3C),
 * backtrace_symbols, backtrace_symbols_fd, walkcontext(3C), printstack,
 * addrtosymstr.
 *
 * backtrace unwinds with the unwinder's tables (_Unwind_Backtrace, from
 * libgcc_s when the program has it, as every C++ program does), else
 * follows the frame pointers; walkcontext, from a context, follows the
 * frame pointers.  A frame pointer is followed only upward on the stack,
 * by at most 8 MiB a frame, aligned.  The symbols are dladdr's, written
 * "object'symbol+0xoffset [0xpc]".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

typedef int (*unwind_fn)(void *, int);
struct ubt {
	void **buf;
	int n, max, skip;
};

typedef int (*unwind_backtrace_t)(int (*)(void *, void *), void *);
typedef uintptr_t (*unwind_getip_t)(void *);
static unwind_getip_t get_ip;

static int ubt_step(void *ctx, void *arg)
{
	struct ubt *u = arg;
	if (u->skip) {
		u->skip--;
		return 0;
	}
	if (u->n >= u->max)
		return 5;                               /* _URC_END_OF_STACK */
	uintptr_t ip = get_ip(ctx);
	if (!ip)
		return 5;
	u->buf[u->n++] = (void *)ip;
	return 0;
}

static int fp_walk(uintptr_t pc, uintptr_t fp, int (*fn)(uintptr_t, void *), void *arg)
{
	if (pc && fn(pc, arg))
		return 1;
	while (fp && !(fp & 7)) {
		uintptr_t *f = (uintptr_t *)fp;
		uintptr_t next = f[0], ret = f[1];
		if (!ret || fn(ret, arg))
			break;
		if (next <= fp || next - fp > (8u << 20))
			break;
		fp = next;
	}
	return 0;
}

struct fpc {
	void **buf;
	int n, max;
};

static int fp_collect(uintptr_t pc, void *arg)
{
	struct fpc *c = arg;
	if (c->n >= c->max)
		return 1;
	c->buf[c->n++] = (void *)pc;
	return 0;
}

int backtrace(void **buf, int max)
{
	if (max <= 0)
		return 0;
	unwind_backtrace_t ub = (unwind_backtrace_t)dlsym(RTLD_DEFAULT, "_Unwind_Backtrace");
	if (!get_ip)
		get_ip = (unwind_getip_t)dlsym(RTLD_DEFAULT, "_Unwind_GetIP");
	if (ub && get_ip) {
		struct ubt u = { buf, 0, max, 1 };      /* (not backtrace's own frame) */
		ub(ubt_step, &u);
		if (u.n)
			return u.n;
	}
	struct fpc c = { buf, 0, max };
	fp_walk(0, (uintptr_t)__builtin_frame_address(0), fp_collect, &c);
	return c.n;
}

int addrtosymstr(void *pc, char *buf, int len)
{
	Dl_info di;
	if (dladdr(pc, &di) && di.dli_fname) {
		if (di.dli_sname)
			return snprintf(buf, len, "%s'%s+0x%lx [%p]", di.dli_fname, di.dli_sname,
			                (unsigned long)((uintptr_t)pc - (uintptr_t)di.dli_saddr), pc);
		return snprintf(buf, len, "%s'0x%lx [%p]", di.dli_fname,
		                (unsigned long)((uintptr_t)pc - (uintptr_t)di.dli_fbase), pc);
	}
	return snprintf(buf, len, "[%p]", pc);
}

char **backtrace_symbols(void *const *pcs, int n)
{
	if (n <= 0)
		return 0;
	char line[1024];
	size_t total = n * sizeof(char *);
	for (int i = 0; i < n; i++) {
		int k = addrtosymstr(pcs[i], line, sizeof line);
		total += (k < (int)sizeof line ? k : (int)sizeof line - 1) + 1;
	}
	char **v = malloc(total);
	if (!v)
		return 0;
	char *s = (char *)(v + n);
	for (int i = 0; i < n; i++) {
		int k = addrtosymstr(pcs[i], line, sizeof line);
		if (k >= (int)sizeof line)
			k = sizeof line - 1;
		memcpy(s, line, k + 1);
		v[i] = s;
		s += k + 1;
	}
	return v;
}

void backtrace_symbols_fd(void *const *pcs, int n, int fd)
{
	char line[1024];
	for (int i = 0; i < n; i++) {
		int k = addrtosymstr(pcs[i], line, sizeof line - 1);
		if (k >= (int)sizeof line - 1)
			k = sizeof line - 2;
		line[k++] = '\n';
		write(fd, line, k);
	}
}

struct wc {
	int (*fn)(uintptr_t, int, void *);
	void *arg;
};

static int wc_step(uintptr_t pc, void *arg)
{
	struct wc *w = arg;
	return w->fn(pc, 0, w->arg);
}

int walkcontext(const ucontext_t *uc, int (*fn)(uintptr_t, int, void *), void *arg)
{
	struct wc w = { fn, arg };
	if (!uc)
		return -1;
	fp_walk(uc->uc_mcontext.gregs[REG_RIP], uc->uc_mcontext.gregs[REG_RBP], wc_step, &w);
	return 0;
}

static int ps_step(uintptr_t pc, void *arg)
{
	char line[1024];
	int fd = *(int *)arg;
	int k = addrtosymstr((void *)pc, line, sizeof line - 1);
	if (k >= (int)sizeof line - 1)
		k = sizeof line - 2;
	line[k++] = '\n';
	write(fd, line, k);
	return 0;
}

int printstack(int fd)
{
	void *pcs[128];
	int n = backtrace(pcs, 128);
	for (int i = 1; i < n; i++)                     /* (from its caller) */
		ps_step((uintptr_t)pcs[i], &fd);
	return 0;
}
