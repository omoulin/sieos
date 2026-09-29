/*
 * wordexp.c - POSIX word expansion for SIEOS.
 *
 * musl hands the whole string to /bin/sh; this does the expansions itself
 * and runs only command substitutions with the shell, so the results do
 * not depend on which shell /bin/sh is:
 *
 * - quoting: '...', "...", \, and POSIX.1-2024 $'...';
 * - parameters: $NAME, ${NAME}, the ${...} operators (- = ? + with or
 *   without ':', % %% # ##, ${#NAME}).  An assignment by ${NAME=...} lasts
 *   for the rest of the call and is visible to its command substitutions;
 *   there are no positional parameters ($# is 0);
 * - $((...)) arithmetic (the C integer operators with assignment);
 * - $(...) and `...` command substitution (WRDE_NOCMD: WRDE_CMDSUB);
 * - tilde expansion, field splitting with IFS, pathname expansion;
 * - errors: an unquoted | & ; < > or newline is WRDE_BADCHAR, as are ( )
 *   and { } with WRDE_NOCMD; without it ( ) are a syntax error and { } are
 *   ordinary characters, as the shell has them.  The whole string is
 *   checked before anything is expanded, so no command runs if a later
 *   part is invalid.  '#' at the start of a word begins a comment.
 */
#include <wordexp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <glob.h>
#include <fnmatch.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>

enum { C_TOP, C_DQ, C_BR, C_BRDQ, C_ARITH };
#define AQ 1                         /* quoted: no splitting, not a pattern character */
#define AS 2                         /* from an unquoted expansion: IFS splits here */

struct buf {
	char *s;
	unsigned char *a;                /* attributes, one per character */
	size_t n, cap;
	int q;                           /* quotes were seen: the word exists even if empty */
};

struct st {
	int flags, err;
	int exec;                        /* 0: only check the syntax */
	char **vars;                     /* assignments made by ${NAME=...}: "NAME=value" */
	size_t nv;
	char **w;                        /* the resulting words */
	size_t nw, cw;
};

static void seterr(struct st *st, int e)
{
	if (!st->err) st->err = e;
}

static void put(struct st *st, struct buf *b, char c, unsigned char a)
{
	if (b->n + 2 > b->cap) {
		size_t nc = b->cap ? 2 * b->cap : 64;
		char *s = realloc(b->s, nc);
		if (s) b->s = s;
		unsigned char *na = s ? realloc(b->a, nc) : 0;
		if (!s || !na) {
			seterr(st, WRDE_NOSPACE);
			return;
		}
		b->a = na;
		b->cap = nc;
	}
	b->s[b->n] = c;
	b->a[b->n++] = a;
	b->s[b->n] = 0;
}

static void putn(struct st *st, struct buf *b, const char *s, size_t n, unsigned char a)
{
	while (n--) put(st, b, *s++, a);
}

static void bfree(struct buf *b)
{
	free(b->s);
	free(b->a);
	memset(b, 0, sizeof *b);
}

static const char *bstr(const struct buf *b)
{
	return b->s ? b->s : "";
}

/* ---------------- variables ---------------- */

static const char *getvar(struct st *st, const char *name, size_t len)
{
	for (size_t i = st->nv; i--; )
		if (!strncmp(st->vars[i], name, len) && st->vars[i][len] == '=')
			return st->vars[i] + len + 1;
	char tmp[256];
	if (len >= sizeof tmp) return 0;
	memcpy(tmp, name, len);
	tmp[len] = 0;
	return getenv(tmp);
}

static void setvar(struct st *st, const char *name, size_t len, const char *val)
{
	size_t vl = strlen(val);
	char *v = malloc(len + vl + 2);
	char **nv = v ? realloc(st->vars, (st->nv + 1) * sizeof *nv) : 0;
	if (!nv) {
		free(v);
		seterr(st, WRDE_NOSPACE);
		return;
	}
	memcpy(v, name, len);
	v[len] = '=';
	memcpy(v + len + 1, val, vl + 1);
	st->vars = nv;
	st->vars[st->nv++] = v;
}

static int isname0(int c) { return c == '_' || ((c|32) >= 'a' && (c|32) <= 'z'); }
static int isnamec(int c) { return isname0(c) || (c >= '0' && c <= '9'); }

/* A parameter's value; *set is 0 if it is unset.  Special parameters:
 * no positional ones, $# 0, $? 0, $$ the process, $0 "sh". */
static const char *param(struct st *st, const char *name, size_t len, int *set)
{
	static char pid[24];
	*set = 1;
	if (isname0(*name)) {
		const char *v = getvar(st, name, len);
		*set = v != 0;
		return v ? v : "";
	}
	switch (*name) {
	case '#': case '?': return "0";
	case '-': case '@': case '*': return "";
	case '$': snprintf(pid, sizeof pid, "%d", (int)getpid()); return pid;
	case '0': if (len == 1) return "sh";
	}
	*set = 0;                        /* $1..., $! */
	return "";
}

/* ---------------- scanning (to find where a construct ends) ---------------- */

static const char *skip_cmd(const char *q);
static const char *skip_brace(const char *q);
static int scan_arith(const char *s, const char **end, int *bad);

static const char *skip_sq(const char *q)
{
	q = strchr(q, '\'');
	return q ? q + 1 : 0;
}

static const char *skip_bq(const char *q)
{
	for (;;) {
		if (!*q) return 0;
		if (*q == '\\' && q[1]) { q += 2; continue; }
		if (*q == '`') return q + 1;
		q++;
	}
}

/* At "$(": past the command substitution or arithmetic expansion. */
static const char *skip_dollar_paren(const char *q)
{
	if (q[2] == '(') {
		const char *e;
		int bad, k = scan_arith(q + 3, &e, &bad);
		if (k == 1) return e + 2;
		if (k == 2) return skip_cmd(q + 2);
		return 0;
	}
	return skip_cmd(q + 2);
}

static const char *skip_dq(const char *q)
{
	for (;;) {
		char c = *q;
		if (!c) return 0;
		if (c == '"') return q + 1;
		if (c == '\\') { q += q[1] ? 2 : 1; continue; }
		if (c == '`') q = skip_bq(q + 1);
		else if (c == '$' && q[1] == '(') q = skip_dollar_paren(q);
		else if (c == '$' && q[1] == '{') q = skip_brace(q + 2);
		else q++;
		if (!q) return 0;
	}
}

/* After "${": past the closing brace. */
static const char *skip_brace(const char *q)
{
	for (;;) {
		char c = *q;
		if (!c) return 0;
		if (c == '}') return q + 1;
		if (c == '\\') { q += q[1] ? 2 : 1; continue; }
		if (c == '\'') q = skip_sq(q + 1);
		else if (c == '"') q = skip_dq(q + 1);
		else if (c == '`') q = skip_bq(q + 1);
		else if (c == '$' && q[1] == '(') q = skip_dollar_paren(q);
		else if (c == '$' && q[1] == '{') q = skip_brace(q + 2);
		else q++;
		if (!q) return 0;
	}
}

/* After "$(": past the closing parenthesis of the command.  Parentheses
 * nest; a ')' ending a case pattern does not count, nor do comments. */
static const char *skip_cmd(const char *q)
{
	int depth = 0, cases[32], nc = 0, wstart = 1;
	for (;;) {
		char c = *q;
		if (!c) return 0;
		if (wstart && c == '#') {
			while (*q && *q != '\n') q++;
			continue;
		}
		if (c == '\\') {
			if (!q[1]) return 0;
			q += 2;
			wstart = 0;
			continue;
		}
		if (c == '(') { depth++; q++; wstart = 1; continue; }
		if (c == ')') {
			q++;
			wstart = 1;
			if (nc && cases[nc-1] == depth) continue;
			if (!depth) return q;
			depth--;
			continue;
		}
		if (strchr(" \t\n;&|<>", c)) { q++; wstart = 1; continue; }
		if (c == '\'') q = skip_sq(q + 1);
		else if (c == '"') q = skip_dq(q + 1);
		else if (c == '`') q = skip_bq(q + 1);
		else if (c == '$' && q[1] == '(') q = skip_dollar_paren(q);
		else if (c == '$' && q[1] == '{') q = skip_brace(q + 2);
		else if (wstart) {
			size_t n = 0;
			while (q[n] && !strchr(" \t\n;&|<>()'\"`\\$", q[n])) n++;
			if (n == 4 && !memcmp(q, "case", 4) && nc < 32) cases[nc++] = depth;
			else if (n == 4 && !memcmp(q, "esac", 4) && nc) nc--;
			q += n ? n : 1;
		} else q++;
		if (!q) return 0;
		wstart = 0;
	}
}

/* After "$((": 1 if it is arithmetic (*end at its closing "))"), 2 if it
 * is a command substitution of a subshell, "$( (...) ...)", 0 if it is
 * unterminated.  *bad: a quote, backslash or '#' at the top level, which
 * arithmetic cannot have. */
static int scan_arith(const char *s, const char **end, int *bad)
{
	int depth = 0;
	*bad = 0;
	for (const char *q = s; q; ) {
		char c = *q;
		if (!c) return 0;
		if (c == '(') { depth++; q++; }
		else if (c == ')') {
			if (depth) { depth--; q++; continue; }
			if (q[1] != ')') return 2;
			*end = q;
			return 1;
		}
		else if (c == '\'') { *bad = 1; q = skip_sq(q + 1); }
		else if (c == '"') { *bad = 1; q = skip_dq(q + 1); }
		else if (c == '\\') { *bad = 1; q += q[1] ? 2 : 1; }
		else if (c == '`') q = skip_bq(q + 1);
		else if (c == '$' && q[1] == '(') q = skip_dollar_paren(q);
		else if (c == '$' && q[1] == '{') q = skip_brace(q + 2);
		else { if (c == '#') *bad = 1; q++; }
	}
	return 0;
}

/* ---------------- command substitution ---------------- */

static void run(struct st *st, const char *cmd, size_t len, unsigned char a, struct buf *out)
{
	struct buf script = { 0 };
	for (size_t i = 0; i < st->nv; i++) {               /* our assignments, for the command */
		const char *v = st->vars[i], *eq = strchr(v, '=');
		putn(st, &script, v, eq - v + 1, 0);
		put(st, &script, '\'', 0);
		for (v = eq + 1; *v; v++) {
			if (*v == '\'') putn(st, &script, "'\\''", 4, 0);
			else put(st, &script, *v, 0);
		}
		putn(st, &script, "'; ", 3, 0);
	}
	putn(st, &script, cmd, len, 0);
	if (st->err) {
		bfree(&script);
		return;
	}
	int p[2];
	if (pipe(p) < 0) {
		bfree(&script);
		seterr(st, WRDE_NOSPACE);
		return;
	}
	sigset_t all, old;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	pid_t pid = fork();
	if (!pid) {
		pthread_sigmask(SIG_SETMASK, &old, 0);
		close(p[0]);
		if (p[1] != 1) {
			dup2(p[1], 1);
			close(p[1]);
		}
		if (!(st->flags & WRDE_SHOWERR)) {
			int null = open("/dev/null", O_WRONLY);
			if (null >= 0 && null != 2) {
				dup2(null, 2);
				close(null);
			}
		}
		execl("/bin/sh", "sh", "-c", bstr(&script), (char *)0);
		_exit(127);
	}
	pthread_sigmask(SIG_SETMASK, &old, 0);
	close(p[1]);
	bfree(&script);
	if (pid < 0) {
		close(p[0]);
		seterr(st, WRDE_NOSPACE);
		return;
	}
	struct buf res = { 0 };
	char tmp[512];
	for (;;) {
		ssize_t n = read(p[0], tmp, sizeof tmp);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		for (ssize_t i = 0; i < n; i++)
			if (tmp[i]) put(st, &res, tmp[i], a);
	}
	close(p[0]);
	while (waitpid(pid, 0, 0) < 0 && errno == EINTR);
	while (res.n && res.s[res.n-1] == '\n') res.n--;    /* trailing newlines go */
	for (size_t i = 0; i < res.n; i++) put(st, out, res.s[i], a);
	bfree(&res);
}

/* ---------------- parsing and expansion ---------------- */

static void parse(struct st *st, const char **pp, int ctx, struct buf *out);
static void dollar(struct st *st, const char **pp, int ctx, struct buf *out);
static int arith(struct st *st, const char *s, long long *v);

static int dqlike(int ctx) { return ctx == C_DQ || ctx == C_BRDQ; }

/* The attribute of what an expansion produces in context ctx. */
static unsigned char exp_attr(int ctx)
{
	return dqlike(ctx) ? AQ : ctx == C_ARITH ? 0 : AS;
}

static void putval(struct st *st, struct buf *out, const char *v, unsigned char a)
{
	putn(st, out, v, strlen(v), a);
}

/* At "$(" (cmd points after it) or at "`". */
static void cmdsub(struct st *st, const char **pp, const char *cmd, int ctx, struct buf *out)
{
	if (st->flags & WRDE_NOCMD) {
		seterr(st, WRDE_CMDSUB);
		return;
	}
	const char *e = skip_cmd(cmd);
	if (!e) {
		seterr(st, WRDE_SYNTAX);
		return;
	}
	*pp = e;
	if (st->exec) run(st, cmd, e - 1 - cmd, exp_attr(ctx), out);
}

static void backquote(struct st *st, const char **pp, int ctx, struct buf *out)
{
	if (st->flags & WRDE_NOCMD) {
		seterr(st, WRDE_CMDSUB);
		return;
	}
	const char *q = *pp + 1;
	struct buf cmd = { 0 };
	while (*q && *q != '`') {
		if (*q == '\\' && q[1] && strchr(dqlike(ctx) ? "$`\\\"" : "$`\\", q[1])) {
			put(st, &cmd, q[1], 0);
			q += 2;
		} else put(st, &cmd, *q++, 0);
	}
	if (!*q) {
		seterr(st, WRDE_SYNTAX);
		bfree(&cmd);
		return;
	}
	*pp = q + 1;
	if (st->exec && !st->err) run(st, bstr(&cmd), cmd.n, exp_attr(ctx), out);
	bfree(&cmd);
}

/* At "$((": arithmetic, or a command substitution of a subshell. */
static void arith_or_cmd(struct st *st, const char **pp, int ctx, struct buf *out)
{
	const char *s = *pp + 3, *end;
	int bad, k = scan_arith(s, &end, &bad);
	if (k == 2) {
		cmdsub(st, pp, *pp + 2, ctx, out);
		return;
	}
	if (!k || bad) {
		seterr(st, WRDE_SYNTAX);
		return;
	}
	struct buf t = { 0 };
	for (const char *q = s; q < end && !st->err; ) {
		if (*q == '$') dollar(st, &q, C_ARITH, &t);
		else if (*q == '`') backquote(st, &q, C_ARITH, &t);
		else put(st, &t, *q++, 0);
	}
	*pp = end + 2;
	long long v;
	if (st->exec && !st->err && arith(st, bstr(&t), &v)) {
		char num[24];
		snprintf(num, sizeof num, "%lld", v);
		putval(st, out, num, exp_attr(ctx));
	}
	bfree(&t);
}

/* $'...' (POSIX.1-2024). */
static void dsq(struct st *st, const char **pp, struct buf *out)
{
	const char *q = *pp + 2;
	while (*q != '\'') {
		if (!*q) {
			seterr(st, WRDE_SYNTAX);
			return;
		}
		if (*q != '\\') {
			put(st, out, *q++, AQ);
			continue;
		}
		q++;
		int c = *q++, v;
		switch (c) {
		case 0: seterr(st, WRDE_SYNTAX); return;
		case 'a': c = 7; break;
		case 'b': c = 8; break;
		case 'e': case 'E': c = 27; break;
		case 'f': c = 12; break;
		case 'n': c = 10; break;
		case 'r': c = 13; break;
		case 't': c = 9; break;
		case 'v': c = 11; break;
		case '\\': case '\'': case '"': case '?': break;
		case 'c':
			if (!*q) { seterr(st, WRDE_SYNTAX); return; }
			c = *q == '\\' && q[1] == '\\' ? (q++, 28) : (*q & 0x1f);
			q++;
			break;
		case 'x':
			for (v = 0, c = 0; v < 2 && ((*q >= '0' && *q <= '9') || ((*q|32) >= 'a' && (*q|32) <= 'f')); v++, q++)
				c = c * 16 + (*q <= '9' ? *q - '0' : (*q|32) - 'a' + 10);
			break;
		default:
			if (c >= '0' && c <= '7') {
				c -= '0';
				for (v = 1; v < 3 && *q >= '0' && *q <= '7'; v++) c = c * 8 + (*q++ - '0');
			} else {
				put(st, out, '\\', AQ);
			}
		}
		if (c) put(st, out, (char)c, AQ);
	}
	*pp = q + 1;
	out->q = 1;
}

/* ~ or ~user at the start of a word. */
static void tilde(struct st *st, const char **pp, int ctx, struct buf *out)
{
	const char *q = *pp + 1;
	size_t n = 0;
	while (isnamec(q[n]) || q[n] == '.' || q[n] == '-') n++;
	char e = q[n];
	if (!(e == 0 || e == '/' || (ctx == C_TOP ? e == ' ' || e == '\t' : e == '}'))) {
		put(st, out, '~', ctx == C_BR ? AS : 0);
		(*pp)++;
		return;
	}
	const char *dir = 0;
	if (!n) {
		dir = getvar(st, "HOME", 4);
	} else {
		char name[256];
		if (n < sizeof name) {
			memcpy(name, q, n);
			name[n] = 0;
			struct passwd *pw = getpwnam(name);
			if (pw) dir = pw->pw_dir;
		}
	}
	if (!dir) {
		put(st, out, '~', ctx == C_BR ? AS : 0);
		(*pp)++;
		return;
	}
	putval(st, out, dir, AQ);
	*pp = q + n;
}

/* Remove a prefix (#, ##) or suffix (%, %%) matching the pattern. */
static char *trim(const char *v, const char *pat, char op, int longest)
{
	size_t n = strlen(v);
	char *tmp = malloc(n + 1);
	if (!tmp) return 0;
	if (op == '#') {
		for (size_t k = 0; k <= n; k++) {
			size_t i = longest ? n - k : k;
			memcpy(tmp, v, i);
			tmp[i] = 0;
			if (!fnmatch(pat, tmp, 0)) {
				memmove(tmp, v + i, n - i + 1);
				return tmp;
			}
		}
	} else {
		for (size_t k = 0; k <= n; k++) {
			size_t i = longest ? k : n - k;
			if (!fnmatch(pat, v + i, 0)) {
				memcpy(tmp, v, i);
				tmp[i] = 0;
				return tmp;
			}
		}
	}
	memcpy(tmp, v, n + 1);
	return tmp;
}

/* A pattern for fnmatch/glob: quoted special characters escaped. */
static char *pattern(struct st *st, const struct buf *b)
{
	struct buf p = { 0 };
	for (size_t i = 0; i < b->n; i++) {
		if ((b->a[i] & AQ) && b->s[i] && strchr("\\*?[]", b->s[i])) put(st, &p, '\\', 0);
		put(st, &p, b->s[i], 0);
	}
	put(st, &p, 0, 0);
	free(p.a);
	return p.s;
}

/* At "${". */
static void brace(struct st *st, const char **pp, int ctx, struct buf *out)
{
	const char *s = *pp + 2, *name;
	size_t nl = 0;
	int len = 0;
	if (*s == '#' && s[1] && s[1] != '}' && (isnamec(s[1]) || strchr("@*#?-$!", s[1])) &&
	    !(strchr("-=?+%#", s[1]) && s[2] != '}')) {
		len = 1;                                       /* ${#NAME} */
		s++;
	}
	name = s;
	if (isname0(*s)) while (isnamec(s[nl])) nl++;
	else if (*s >= '0' && *s <= '9') while (s[nl] >= '0' && s[nl] <= '9') nl++;
	else if (*s && strchr("@*#?-$!", *s)) nl = 1;
	if (!nl) {
		seterr(st, WRDE_SYNTAX);
		return;
	}
	s += nl;
	int colon = 0;
	char op = 0;
	int longest = 0;
	if (!len && *s == ':' && s[1] && strchr("-=?+", s[1])) { colon = 1; op = s[1]; s += 2; }
	else if (!len && *s && strchr("-=?+", *s)) op = *s++;
	else if (!len && (*s == '%' || *s == '#')) {
		op = *s++;
		if (*s == op) { longest = 1; s++; }
	}
	else if (*s != '}') {
		seterr(st, WRDE_SYNTAX);
		return;
	}
	int set;
	const char *v = param(st, name, nl, &set);
	int null = !set || (colon && !*v);
	int need = op == '+' ? set && !(colon && !*v) : op == '%' || op == '#' ? 1 : op ? null : 0;
	struct buf wb = { 0 };
	int wctx = op == '%' || op == '#' || !dqlike(ctx) ? C_BR : C_BRDQ;
	int exec = st->exec;
	if (op) {
		st->exec = exec && need;
		parse(st, &s, wctx, &wb);
		st->exec = exec;
	}
	if (!st->err && *s != '}') seterr(st, WRDE_SYNTAX);
	*pp = s + 1;
	if (!exec || st->err) {
		bfree(&wb);
		return;
	}
	unsigned char ea = exp_attr(ctx);
	if (len || !op) {
		if (!set && (st->flags & WRDE_UNDEF)) seterr(st, WRDE_BADVAL);
		else if (len) {
			char num[24];
			snprintf(num, sizeof num, "%zu", strlen(v));
			putval(st, out, num, ea);
		} else putval(st, out, v, ea);
	} else if (op == '%' || op == '#') {
		if (!set && (st->flags & WRDE_UNDEF)) seterr(st, WRDE_BADVAL);
		char *pat = pattern(st, &wb), *r = pat ? trim(v, pat, op, longest) : 0;
		if (r) putval(st, out, r, ea);
		else seterr(st, WRDE_NOSPACE);
		free(pat);
		free(r);
	} else if (!need) {
		if (op != '+') putval(st, out, v, ea);
	} else if (op == '?') {
		if (st->flags & WRDE_SHOWERR)
			dprintf(2, "wordexp: %.*s: %s\n", (int)nl, name, wb.n ? bstr(&wb) : "parameter null or not set");
		seterr(st, WRDE_SYNTAX);
	} else if (op == '=') {
		if (!isname0(*name)) seterr(st, WRDE_SYNTAX);
		else {
			setvar(st, name, nl, bstr(&wb));
			putval(st, out, bstr(&wb), ea);
		}
	} else {                                           /* - or + with the word */
		for (size_t i = 0; i < wb.n; i++)
			put(st, out, wb.s[i], dqlike(ctx) ? AQ : wb.a[i]);
		out->q |= wb.q;
	}
	bfree(&wb);
}

static void dollar(struct st *st, const char **pp, int ctx, struct buf *out)
{
	const char *s = *pp + 1;
	unsigned char ea = exp_attr(ctx);
	if (s[0] == '(' && s[1] == '(') { arith_or_cmd(st, pp, ctx, out); return; }
	if (s[0] == '(') { cmdsub(st, pp, s + 1, ctx, out); return; }
	if (s[0] == '{') { brace(st, pp, ctx, out); return; }
	if (s[0] == '\'' && ctx != C_DQ) {
		if (ctx == C_TOP) dsq(st, pp, out);
		else seterr(st, WRDE_SYNTAX);                  /* $'...' inside ${...} or $((...)) */
		return;
	}
	size_t nl = 0;
	if (isname0(*s)) while (isnamec(s[nl])) nl++;
	else if ((*s >= '0' && *s <= '9') || (*s && strchr("@*#?-$!", *s))) nl = 1;
	if (!nl) {                                         /* a lone '$' */
		put(st, out, '$', ctx == C_BR ? AS : dqlike(ctx) ? AQ : 0);
		*pp = s;
		return;
	}
	*pp = s + nl;
	if (!st->exec) return;
	int set;
	const char *v = param(st, s, nl, &set);
	if (!set && (st->flags & WRDE_UNDEF)) seterr(st, WRDE_BADVAL);
	else putval(st, out, v, ea);
}

/* Parse into out up to the end of the context: a blank (C_TOP), '"'
 * (C_DQ) or '}' (C_BR, C_BRDQ), which *pp is left at. */
static void parse(struct st *st, const char **pp, int ctx, struct buf *out)
{
	const char *p = *pp;
	int start = 1;
	unsigned char lit = ctx == C_BR ? AS : ctx == C_TOP ? 0 : AQ;
	while (!st->err) {
		char c = *p;
		if (!c) {
			if (ctx != C_TOP) seterr(st, WRDE_SYNTAX);
			break;
		}
		if (ctx == C_TOP && (c == ' ' || c == '\t')) break;
		if ((ctx == C_BR || ctx == C_BRDQ) && c == '}') break;
		if (ctx == C_DQ && c == '"') break;
		if (start && c == '~' && (ctx == C_TOP || ctx == C_BR)) {
			tilde(st, &p, ctx, out);
			start = 0;
			continue;
		}
		start = 0;
		switch (c) {
		case '\\':
			if (p[1] == '\n') {                        /* line continuation */
				p += 2;
				continue;
			}
			if (dqlike(ctx)) {
				if (p[1] && strchr(ctx == C_DQ ? "$`\"\\" : "$`\"\\}", p[1])) {
					put(st, out, p[1], AQ);
					p += 2;
				} else {
					put(st, out, '\\', AQ);
					p++;
				}
			} else if (p[1]) {
				put(st, out, p[1], AQ);
				p += 2;
			} else {
				put(st, out, '\\', lit);
				p++;
			}
			continue;
		case '\'':
			if (dqlike(ctx)) break;
			{
				const char *e = strchr(p + 1, '\'');
				if (!e) {
					seterr(st, WRDE_SYNTAX);
					continue;
				}
				putn(st, out, p + 1, e - p - 1, AQ);
				out->q = 1;
				p = e + 1;
			}
			continue;
		case '"':
			p++;
			parse(st, &p, C_DQ, out);
			if (!st->err) p++;
			out->q = 1;
			continue;
		case '$':
			dollar(st, &p, ctx, out);
			continue;
		case '`':
			backquote(st, &p, ctx, out);
			continue;
		}
		if (ctx == C_TOP) {
			if (strchr("|&;<>\n", c)) {
				seterr(st, WRDE_BADCHAR);
				break;
			}
			if (c == '(' || c == ')') {
				seterr(st, st->flags & WRDE_NOCMD ? WRDE_BADCHAR : WRDE_SYNTAX);
				break;
			}
			if ((c == '{' || c == '}') && (st->flags & WRDE_NOCMD)) {
				seterr(st, WRDE_BADCHAR);
				break;
			}
		}
		put(st, out, c, lit);
		p++;
	}
	*pp = p;
}

/* ---------------- arithmetic ---------------- */

struct ev {
	struct st *st;
	const char *p;
	int skip;                        /* not evaluated (short circuit): no side effects */
	int bad, depth;
};

static long long e_assign(struct ev *e);

static void e_ws(struct ev *e)
{
	while (*e->p == ' ' || *e->p == '\t' || *e->p == '\n') e->p++;
}

/* The operator t is next (and is not the start of a longer one in "not"). */
static int e_op(struct ev *e, const char *t, const char *not)
{
	e_ws(e);
	size_t n = strlen(t);
	if (strncmp(e->p, t, n) || (e->p[n] && not && strchr(not, e->p[n]))) return 0;
	e->p += n;
	return 1;
}

static int e_number(const char *s, long long *v)
{
	unsigned long long x = 0;
	int base = 10;
	if (*s == '0' && (s[1]|32) == 'x') { base = 16; s += 2; if (!*s) return 0; }
	else if (*s == '0') base = 8;
	for (; *s; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' : isname0(*s) ? (*s|32) - 'a' + 10 : 99;
		if (d >= base) return 0;
		x = x * base + d;
	}
	*v = (long long)x;
	return 1;
}

static long long e_var(struct ev *e, const char *name, size_t len)
{
	const char *v = getvar(e->st, name, len);
	if (!v) {
		if ((e->st->flags & WRDE_UNDEF) && !e->skip) seterr(e->st, WRDE_BADVAL);
		return 0;
	}
	long long x;
	while (*v == ' ' || *v == '\t') v++;
	if (!*v) return 0;
	if (e_number(v, &x)) return x;
	if (e->depth > 32) { e->bad = 1; return 0; }
	struct ev sub = { e->st, v, e->skip, 0, e->depth + 8 };   /* a value that is an expression */
	x = e_assign(&sub);
	e_ws(&sub);
	if (sub.bad || *sub.p) e->bad = 1;
	return x;
}

static long long e_primary(struct ev *e)
{
	e_ws(e);
	if (++e->depth > 1000) { e->bad = 1; return 0; }
	long long v = 0;
	if (*e->p == '(') {
		e->p++;
		v = e_assign(e);
		if (!e_op(e, ")", 0)) e->bad = 1;
	} else if (*e->p >= '0' && *e->p <= '9') {
		char num[72];
		size_t n = 0;
		while (isnamec(e->p[n])) n++;
		if (n >= sizeof num) e->bad = 1;
		else {
			memcpy(num, e->p, n);
			num[n] = 0;
			if (!e_number(num, &v)) e->bad = 1;
		}
		e->p += n;
	} else if (isname0(*e->p)) {
		const char *name = e->p;
		size_t n = 0;
		while (isnamec(name[n])) n++;
		e->p += n;
		v = e_var(e, name, n);
	} else e->bad = 1;
	e->depth--;
	return v;
}

static long long e_unary(struct ev *e)
{
	e_ws(e);
	char c = *e->p;
	if ((c == '+' || c == '-') && e->p[1] != c && e->p[1] != '=') {
		e->p++;
		if (++e->depth > 1000) { e->bad = 1; return 0; }
		long long v = e_unary(e);
		e->depth--;
		return c == '-' ? (long long)(0ULL - (unsigned long long)v) : v;
	}
	if (c == '!' && e->p[1] != '=') { e->p++; return !e_unary(e); }
	if (c == '~') { e->p++; return ~e_unary(e); }
	return e_primary(e);
}

static long long e_binary(struct ev *e, int level);

/* Binary operators by precedence, lowest first (below ?: and assignment). */
static long long e_binary(struct ev *e, int level)
{
	if (level > 9) return e_unary(e);
	long long a = e_binary(e, level + 1);
	for (;;) {
		long long b;
		unsigned long long ua = a;
		int skip = e->skip;
		switch (level) {
		case 0:
			if (!e_op(e, "||", 0)) return a;
			e->skip |= a != 0;
			b = e_binary(e, 1);
			e->skip = skip;
			a = a || b;
			continue;
		case 1:
			if (!e_op(e, "&&", 0)) return a;
			e->skip |= a == 0;
			b = e_binary(e, 2);
			e->skip = skip;
			a = a && b;
			continue;
		case 2:
			if (!e_op(e, "|", "|=")) return a;
			a |= e_binary(e, 3);
			continue;
		case 3:
			if (!e_op(e, "^", "=")) return a;
			a ^= e_binary(e, 4);
			continue;
		case 4:
			if (!e_op(e, "&", "&=")) return a;
			a &= e_binary(e, 5);
			continue;
		case 5:
			if (e_op(e, "==", 0)) a = a == e_binary(e, 6);
			else if (e_op(e, "!=", 0)) a = a != e_binary(e, 6);
			else return a;
			continue;
		case 6:
			if (e_op(e, "<=", 0)) a = a <= e_binary(e, 7);
			else if (e_op(e, ">=", 0)) a = a >= e_binary(e, 7);
			else if (e_op(e, "<", "<")) a = a < e_binary(e, 7);
			else if (e_op(e, ">", ">")) a = a > e_binary(e, 7);
			else return a;
			continue;
		case 7:
			if (e_op(e, "<<", "=")) a = (long long)(ua << (e_binary(e, 8) & 63));
			else if (e_op(e, ">>", "=")) a >>= e_binary(e, 8) & 63;
			else return a;
			continue;
		case 8:
			if (e_op(e, "+", "+=")) a = (long long)(ua + (unsigned long long)e_binary(e, 9));
			else if (e_op(e, "-", "-=")) a = (long long)(ua - (unsigned long long)e_binary(e, 9));
			else return a;
			continue;
		case 9: {
			char op;
			if (e_op(e, "*", "=")) op = '*';
			else if (e_op(e, "/", "=")) op = '/';
			else if (e_op(e, "%", "=")) op = '%';
			else return a;
			b = e_binary(e, 10);
			if (op == '*') a = (long long)(ua * (unsigned long long)b);
			else if (!b) { if (!e->skip) e->bad = 1; a = 0; }
			else if (b == -1) a = op == '/' ? (long long)(0ULL - ua) : 0;
			else a = op == '/' ? a / b : a % b;
			continue;
		}
		}
	}
}

static long long e_cond(struct ev *e)
{
	long long c = e_binary(e, 0);
	if (!e_op(e, "?", 0)) return c;
	int skip = e->skip;
	e->skip = skip || !c;
	long long a = e_assign(e);
	e->skip = skip;
	if (!e_op(e, ":", 0)) { e->bad = 1; return 0; }
	e->skip = skip || c;
	long long b = e_cond(e);
	e->skip = skip;
	return c ? a : b;
}

static long long e_assign(struct ev *e)
{
	e_ws(e);
	const char *save = e->p;
	if (isname0(*e->p)) {
		const char *name = e->p;
		size_t n = 0;
		while (isnamec(name[n])) n++;
		e->p += n;
		e_ws(e);
		static const char *ops[] = { "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=", "=" };
		for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) {
			size_t ol = strlen(ops[i]);
			if (strncmp(e->p, ops[i], ol) || (ol == 1 && e->p[1] == '=')) continue;
			e->p += ol;
			long long r = e_assign(e), v = r;
			if (ol > 1) {
				long long old = e_var(e, name, n);
				unsigned long long uo = old;
				switch (*ops[i]) {
				case '<': v = (long long)(uo << (r & 63)); break;
				case '>': v = old >> (r & 63); break;
				case '+': v = (long long)(uo + (unsigned long long)r); break;
				case '-': v = (long long)(uo - (unsigned long long)r); break;
				case '*': v = (long long)(uo * (unsigned long long)r); break;
				case '&': v = old & r; break;
				case '^': v = old ^ r; break;
				case '|': v = old | r; break;
				default:
					if (!r) { if (!e->skip) e->bad = 1; v = 0; }
					else if (r == -1) v = *ops[i] == '/' ? (long long)(0ULL - uo) : 0;
					else v = *ops[i] == '/' ? old / r : old % r;
				}
			}
			if (!e->skip && !e->bad) {
				char num[24];
				snprintf(num, sizeof num, "%lld", v);
				setvar(e->st, name, n, num);
			}
			return v;
		}
		e->p = save;
	}
	return e_cond(e);
}

static int arith(struct st *st, const char *s, long long *v)
{
	struct ev e = { st, s, 0, 0, 0 };
	*v = e_assign(&e);
	e_ws(&e);
	if (st->err) return 0;
	if (e.bad || *e.p) {
		seterr(st, WRDE_SYNTAX);
		return 0;
	}
	return 1;
}

/* ---------------- fields ---------------- */

static void addword(struct st *st, char *w)
{
	if (!w) {
		seterr(st, WRDE_NOSPACE);
		return;
	}
	if (st->nw + 1 >= st->cw) {
		size_t nc = st->cw ? 2 * st->cw : 16;
		char **nw = realloc(st->w, nc * sizeof *nw);
		if (!nw) {
			free(w);
			seterr(st, WRDE_NOSPACE);
			return;
		}
		st->w = nw;
		st->cw = nc;
	}
	st->w[st->nw++] = w;
}

/* One field: pathname expansion if it has unquoted pattern characters. */
static void emit(struct st *st, struct buf *f)
{
	int pat = 0;
	for (size_t i = 0; i < f->n; i++)
		if (!(f->a[i] & AQ) && f->s[i] && strchr("*?[", f->s[i])) pat = 1;
	if (pat && !st->err) {
		char *p = pattern(st, f);
		glob_t g;
		int r = p ? glob(p, 0, 0, &g) : GLOB_NOSPACE;
		free(p);
		if (!r) {
			for (size_t i = 0; i < g.gl_pathc; i++) addword(st, strdup(g.gl_pathv[i]));
			globfree(&g);
			f->n = 0;
			return;
		}
		if (r == GLOB_NOSPACE) seterr(st, WRDE_NOSPACE);
	}
	addword(st, strndup(bstr(f), f->n));
	f->n = 0;
}

/* Split a word into fields at the IFS characters that came from unquoted
 * expansions. */
static void fields(struct st *st, struct buf *b)
{
	const char *ifs = getvar(st, "IFS", 3);
	if (!ifs) ifs = " \t\n";
	struct buf f = { 0 };
	int have = 0, wsend = 0, emitted = 0;
	for (size_t i = 0; i < b->n && !st->err; i++) {
		char c = b->s[i];
		if ((b->a[i] & AS) && c && strchr(ifs, c)) {
			if (strchr(" \t\n", c)) {
				if (have) {
					emit(st, &f);
					emitted = 1;
					have = 0;
					wsend = 1;
				}
				continue;
			}
			if (have || !wsend) {                      /* a non-blank delimiter */
				emit(st, &f);
				emitted = 1;
			}
			have = wsend = 0;
			while (i + 1 < b->n && (b->a[i+1] & AS) && b->s[i+1] && strchr(ifs, b->s[i+1]) &&
			       strchr(" \t\n", b->s[i+1]))
				i++;
			continue;
		}
		put(st, &f, c, b->a[i]);
		have = 1;
		wsend = 0;
	}
	if (have || (!emitted && b->q)) emit(st, &f);
	bfree(&f);
}

static void words(struct st *st, const char *s)
{
	const char *p = s;
	for (;;) {
		while (*p == ' ' || *p == '\t' || (*p == '\\' && p[1] == '\n')) p += *p == '\\' ? 2 : 1;
		if (!*p || st->err) break;
		if (*p == '#') {                               /* a comment, to the end of the line */
			while (*p && *p != '\n') p++;
			continue;
		}
		struct buf b = { 0 };
		parse(st, &p, C_TOP, &b);
		if (!st->err && st->exec) fields(st, &b);
		bfree(&b);
	}
}

/* ---------------- interface ---------------- */

static void freeall(struct st *st)
{
	for (size_t i = 0; i < st->nv; i++) free(st->vars[i]);
	free(st->vars);
}

int wordexp(const char *restrict s, wordexp_t *restrict we, int flags)
{
	int cs;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cs);
	if (flags & WRDE_REUSE) wordfree(we);
	struct st st = { flags, 0, 0 };
	words(&st, s);                                     /* check it all first */
	if (!st.err) {
		st.exec = 1;
		words(&st, s);
	}
	freeall(&st);
	size_t offs = flags & WRDE_DOOFFS ? we->we_offs : 0;
	size_t old = flags & WRDE_APPEND ? we->we_wordc : 0;
	char **v = 0;
	if (!st.err) {
		v = realloc(flags & WRDE_APPEND ? we->we_wordv : 0, (offs + old + st.nw + 1) * sizeof *v);
		if (!v) st.err = WRDE_NOSPACE;
	}
	if (st.err) {
		for (size_t i = 0; i < st.nw; i++) free(st.w[i]);
		free(st.w);
		pthread_setcancelstate(cs, 0);
		return st.err;
	}
	if (!(flags & WRDE_APPEND))
		for (size_t i = 0; i < offs; i++) v[i] = 0;
	memcpy(v + offs + old, st.w, st.nw * sizeof *v);
	v[offs + old + st.nw] = 0;
	free(st.w);
	we->we_wordv = v;
	we->we_wordc = old + st.nw;
	we->we_offs = offs;
	pthread_setcancelstate(cs, 0);
	return 0;
}

void wordfree(wordexp_t *we)
{
	if (!we->we_wordv) return;
	for (size_t i = 0; i < we->we_wordc; i++) free(we->we_wordv[we->we_offs + i]);
	free(we->we_wordv);
	we->we_wordv = 0;
	we->we_wordc = 0;
}
