/*
 * gtm - session server
 *
 * One server process per session.  It owns the ptys, keeps a virtual
 * terminal per pane, composes windows/panes/status bar into a frame and
 * streams minimal diffs of that frame to every attached client.
 */
#include "gtm.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SB_LINES 5000

/* palette (xterm 256-colour indices) */
#define C(i)     COL_IDX(i)
#define P_BAR    C(235)
#define P_TAB    C(237)
#define P_TABFG  C(250)
#define P_ACCENT C(147)
#define P_SKY    C(117)
#define P_DARK   C(234)
#define P_PEACH  C(216)
#define P_GREEN  C(150)
#define P_RED    C(210)
#define P_YELLOW C(222)
#define P_BORDER C(239)
#define P_MUTED  C(245)
#define P_PANEL  C(236)
#define P_TEXT   C(252)

typedef struct Node   Node;
typedef struct Pane   Pane;
typedef struct Window Window;
typedef struct Client Client;

enum { N_LEAF, N_HSPLIT /* side by side */, N_VSPLIT /* stacked */ };
enum { D_LEFT, D_RIGHT, D_UP, D_DOWN };
enum { PR_NONE, PR_RENAME_WIN, PR_RENAME_SESS, PR_SEARCH, PR_KILL_PANE, PR_KILL_WIN };

struct Node {
	int     type;
	Node   *parent, *a, *b;
	double  ratio;
	Pane   *pane;
	int     x, y, w, h;
};

struct Pane {
	int     id, fd;
	pid_t   pid;
	VT      vt;
	Node   *node;
	Window *win;
	Buf     wbuf;
	int     x, y, w, h;
	int     dead;
	/* copy mode */
	int     cm, cm_cx, cm_sel, cm_sx;
	int64_t cm_top, cm_cy, cm_sy;
};

struct Window {
	char  name[64];
	int   named, bell;
	Node *root;
	Pane *active, *last, *zoom;
};

struct Client {
	int   fd, attached, closing, dead, prefix;
	int64_t prefix_ms; /* when the prefix was pressed */
	Buf   in, out;
	int   rows, cols;
	Cell *front;
	int   frows, fcols, full;
	int   lcx, lcy, lvis, lstyle, lappcur, lkeypad, lpaste;
};

static struct {
	char    name[64], path[PATH_MAX], startdir[PATH_MAX], term[32], host[64];
	time_t  created;
	int     lfd, rows, cols;
	Window **wins;
	int     nwin, cur, lastwin;
	Pane  **panes;
	int     npanes, nextid;
	Client **cls;
	int     ncl;
	int     dirty, bell, quit, lastmin;
	int64_t last_render;
	/* prompt / overlays */
	int     prompt, ptarget;
	char    plabel[48], pbuf[256];
	int     help;
	char    msg[160];
	int64_t msg_until;
	char   *paste;
	size_t  pastelen;
	uint32_t search[64];
	int     searchlen;
} S;

static struct {
	Cell *c;
	int   rows, cols, cx, cy, cvis, cstyle, appcur, keypad, paste;
} F;

static volatile sig_atomic_t got_term;

static void window_layout(Window *w);
static int menu_pending(void);
static void flash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static Window *curwin(void) { return S.nwin ? S.wins[S.cur] : NULL; }
static Pane *curpane(void) { Window *w = curwin(); return w ? w->active : NULL; }
static int area_rows(void) { return S.rows - 1 > 1 ? S.rows - 1 : 1; }

/* ------------------------------------------------------------------ */
/* process helpers                                                     */
/* ------------------------------------------------------------------ */
static pid_t fg_pid(Pane *p)
{
	pid_t g = tcgetpgrp(p->fd);
	return g > 0 ? g : p->pid;
}

static int proc_name(pid_t pid, char *out, size_t n)
{
	char path[64];
	snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)n, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	out[strcspn(out, "\n")] = 0;
	return out[0] ? 0 : -1;
}

static void pane_cwd(Pane *p, char *out, size_t n)
{
	char link[64];
	ssize_t k;
	snprintf(link, sizeof link, "/proc/%d/cwd", (int)fg_pid(p));
	k = readlink(link, out, n - 1);
	if (k <= 0) {
		snprintf(link, sizeof link, "/proc/%d/cwd", (int)p->pid);
		k = readlink(link, out, n - 1);
	}
	if (k > 0)
		out[k] = 0;
	else
		snprintf(out, n, "%s", S.startdir);
}

static const char *shell_path(void)
{
	const char *sh = getenv("SHELL");
	if (!sh || !*sh || access(sh, X_OK) != 0)
		sh = "/bin/sh";
	return sh;
}

static void shell_name(char *out, size_t n)
{
	const char *sh = shell_path();
	const char *b = strrchr(sh, '/');
	snprintf(out, n, "%s", b ? b + 1 : sh);
}

/* ------------------------------------------------------------------ */
/* panes                                                               */
/* ------------------------------------------------------------------ */
static Pane *pane_new(int rows, int cols, const char *cwd)
{
	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;
	int mfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (mfd < 0)
		return NULL;
	char sname[128];
	if (grantpt(mfd) < 0 || unlockpt(mfd) < 0 || ptsname_r(mfd, sname, sizeof sname) != 0) {
		close(mfd);
		return NULL;
	}
	struct winsize ws = { .ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols };
	ioctl(mfd, TIOCSWINSZ, &ws);

	int id = ++S.nextid;
	pid_t pid = fork();
	if (pid < 0) {
		close(mfd);
		return NULL;
	}
	if (pid == 0) {
		setsid();
		int sfd = open(sname, O_RDWR);
		if (sfd < 0)
			_exit(127);
		ioctl(sfd, TIOCSCTTY, 0);
		dup2(sfd, 0);
		dup2(sfd, 1);
		dup2(sfd, 2);
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		int sigs[] = { SIGPIPE, SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGTSTP,
		               SIGTTIN, SIGTTOU, SIGWINCH };
		for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
			signal(sigs[i], SIG_DFL);
		sigset_t ss;
		sigemptyset(&ss);
		sigprocmask(SIG_SETMASK, &ss, NULL);

		char idb[32];
		snprintf(idb, sizeof idb, "%%%d", id);
		setenv("TERM", S.term, 1);
		setenv("COLORTERM", "truecolor", 1);
		setenv("GTM", S.path, 1);
		setenv("GTM_PANE", idb, 1);
		setenv("GTM_SESSION", S.name, 1);
		unsetenv("COLUMNS");
		unsetenv("LINES");
		if (!cwd || chdir(cwd) < 0)
			if (chdir(S.startdir) < 0 && chdir(getenv("HOME") ? getenv("HOME") : "/") < 0)
				(void)!chdir("/");
		const char *sh = shell_path();
		const char *base = strrchr(sh, '/');
		char argv0[256];
		snprintf(argv0, sizeof argv0, "-%s", base ? base + 1 : sh);
		execl(sh, argv0, (char *)NULL);
		_exit(127);
	}
	fcntl(mfd, F_SETFL, fcntl(mfd, F_GETFL) | O_NONBLOCK);
	fcntl(mfd, F_SETFD, FD_CLOEXEC);

	Pane *p = calloc(1, sizeof *p);
	if (!p)
		abort();
	p->id = id;
	p->fd = mfd;
	p->pid = pid;
	p->w = cols;
	p->h = rows;
	vt_init(&p->vt, rows, cols, SB_LINES);
	Pane **np = realloc(S.panes, sizeof *np * (size_t)(S.npanes + 1));
	if (!np)
		abort();
	S.panes = np;
	S.panes[S.npanes++] = p;
	return p;
}

static void pane_flush(Pane *p)
{
	while (p->wbuf.len) {
		ssize_t w = write(p->fd, p->wbuf.d, p->wbuf.len);
		if (w > 0)
			buf_consume(&p->wbuf, (size_t)w);
		else if (w < 0 && errno == EINTR)
			continue;
		else
			break;
	}
}

static void pane_write(Pane *p, const void *d, size_t n)
{
	if (!p || p->dead || !n)
		return;
	if (p->wbuf.len > (1 << 20))
		return; /* child isn't reading; drop */
	buf_add(&p->wbuf, d, n);
	pane_flush(p);
}

static void pane_apply(Pane *p)
{
	int w = p->w < 1 ? 1 : p->w, h = p->h < 1 ? 1 : p->h;
	if (p->vt.rows != h || p->vt.cols != w) {
		vt_resize(&p->vt, h, w);
		struct winsize ws = { .ws_row = (unsigned short)h, .ws_col = (unsigned short)w };
		ioctl(p->fd, TIOCSWINSZ, &ws);
	}
}

static Node *first_leaf(Node *n)
{
	while (n && n->type != N_LEAF)
		n = n->a;
	return n;
}

static int win_panes(Node *n, Pane **out, int max, int k)
{
	if (!n)
		return k;
	if (n->type == N_LEAF) {
		if (k < max)
			out[k++] = n->pane;
		return k;
	}
	k = win_panes(n->a, out, max, k);
	return win_panes(n->b, out, max, k);
}

static int pane_count(Window *w)
{
	int n = 0;
	for (int i = 0; i < S.npanes; i++)
		if (S.panes[i]->win == w)
			n++;
	return n;
}

static void window_remove(Window *w);

static void pane_destroy(Pane *p)
{
	for (int i = 0; i < S.npanes; i++) {
		if (S.panes[i] == p) {
			memmove(&S.panes[i], &S.panes[i + 1], sizeof *S.panes * (size_t)(S.npanes - i - 1));
			S.npanes--;
			break;
		}
	}
	if (p->fd >= 0)
		close(p->fd);
	if (p->pid > 0)
		kill(p->pid, SIGHUP);
	vt_free(&p->vt);
	buf_free(&p->wbuf);
	free(p);
}

static void pane_remove(Pane *p)
{
	Window *w = p->win;
	Node *n = p->node, *sib = NULL;

	if (!n->parent) {
		w->root = NULL;
	} else {
		Node *par = n->parent;
		sib = par->a == n ? par->b : par->a;
		Node *gp = par->parent;
		sib->parent = gp;
		if (!gp)
			w->root = sib;
		else if (gp->a == par)
			gp->a = sib;
		else
			gp->b = sib;
		free(par);
	}
	free(n);
	if (w->zoom == p)
		w->zoom = NULL;
	if (w->last == p)
		w->last = NULL;
	if (w->active == p) {
		if (w->last && !w->last->dead)
			w->active = w->last;
		else if (sib)
			w->active = first_leaf(sib)->pane;
		else
			w->active = NULL;
		w->last = NULL;
	}
	pane_destroy(p);
	if (!w->root)
		window_remove(w);
	else
		window_layout(w);
	S.dirty = 1;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */
static void node_layout(Node *n, int x, int y, int w, int h)
{
	n->x = x;
	n->y = y;
	n->w = w;
	n->h = h;
	if (n->type == N_LEAF) {
		n->pane->x = x;
		n->pane->y = y;
		n->pane->w = w;
		n->pane->h = h;
		return;
	}
	int total = (n->type == N_HSPLIT ? w : h) - 1;
	int a = (int)(total * n->ratio + 0.5);
	a = clampi(a, 1, total - 1 > 1 ? total - 1 : 1);
	int b = total - a;
	if (b < 1)
		b = 1;
	if (n->type == N_HSPLIT) {
		node_layout(n->a, x, y, a, h);
		node_layout(n->b, x + a + 1, y, b, h);
	} else {
		node_layout(n->a, x, y, w, a);
		node_layout(n->b, x, y + a + 1, w, b);
	}
}

static void window_layout(Window *w)
{
	if (!w->root)
		return;
	node_layout(w->root, 0, 0, S.cols, area_rows());
	if (w->zoom) {
		w->zoom->x = 0;
		w->zoom->y = 0;
		w->zoom->w = S.cols;
		w->zoom->h = area_rows();
	}
	for (int i = 0; i < S.npanes; i++)
		if (S.panes[i]->win == w)
			pane_apply(S.panes[i]);
	S.dirty = 1;
}

static void resize_session(void)
{
	int r = 0, c = 0;
	for (int i = 0; i < S.ncl; i++) {
		Client *cl = S.cls[i];
		if (!cl->attached || cl->dead)
			continue;
		if (!r || cl->rows < r)
			r = cl->rows;
		if (!c || cl->cols < c)
			c = cl->cols;
	}
	if (!r || !c)
		return;
	if (r < 3)
		r = 3;
	if (c < 10)
		c = 10;
	if (r == S.rows && c == S.cols)
		return;
	S.rows = r;
	S.cols = c;
	for (int i = 0; i < S.nwin; i++)
		window_layout(S.wins[i]);
	S.dirty = 1;
}

/* ------------------------------------------------------------------ */
/* windows                                                             */
/* ------------------------------------------------------------------ */
static Window *window_new(const char *cwd)
{
	Pane *p = pane_new(area_rows(), S.cols, cwd);
	if (!p)
		return NULL;
	Window *w = calloc(1, sizeof *w);
	Node *n = calloc(1, sizeof *n);
	if (!w || !n)
		abort();
	n->type = N_LEAF;
	n->pane = p;
	p->node = n;
	p->win = w;
	w->root = n;
	w->active = p;
	shell_name(w->name, sizeof w->name);
	Window **nw = realloc(S.wins, sizeof *nw * (size_t)(S.nwin + 1));
	if (!nw)
		abort();
	S.wins = nw;
	S.wins[S.nwin++] = w;
	window_layout(w);
	return w;
}

static void select_window(int i)
{
	if (i < 0 || i >= S.nwin || i == S.cur)
		return;
	S.lastwin = S.cur;
	S.cur = i;
	S.wins[i]->bell = 0;
	S.dirty = 1;
}

static void window_remove(Window *w)
{
	int i;
	for (i = 0; i < S.nwin; i++)
		if (S.wins[i] == w)
			break;
	if (i == S.nwin)
		return;
	memmove(&S.wins[i], &S.wins[i + 1], sizeof *S.wins * (size_t)(S.nwin - i - 1));
	S.nwin--;
	free(w);
	if (S.nwin == 0) {
		S.quit = EXIT_ENDED;
		return;
	}
	int last = S.lastwin;
	if (last > i)
		last--;
	if (i == S.cur)
		S.cur = (last >= 0 && last < S.nwin && last != i) ? last : (i > 0 ? i - 1 : 0);
	else if (i < S.cur)
		S.cur--;
	if (S.cur >= S.nwin)
		S.cur = S.nwin - 1;
	S.lastwin = S.cur;
	S.dirty = 1;
}

static void new_window(void)
{
	char cwd[PATH_MAX];
	Pane *p = curpane();
	if (p)
		pane_cwd(p, cwd, sizeof cwd);
	else
		snprintf(cwd, sizeof cwd, "%s", S.startdir);
	if (!window_new(cwd)) {
		flash("could not create window: %s", strerror(errno));
		return;
	}
	select_window(S.nwin - 1);
}

static void set_active(Window *w, Pane *p)
{
	if (!p || p == w->active)
		return;
	w->last = w->active;
	w->active = p;
	S.dirty = 1;
}

static void split(int horiz)
{
	Window *w = curwin();
	Pane *p = curpane();
	if (!w || !p)
		return;
	if (w->zoom) {
		w->zoom = NULL;
		window_layout(w);
	}
	if ((horiz && p->w < 5) || (!horiz && p->h < 3)) {
		flash("pane too small to split");
		return;
	}
	int total = (horiz ? p->w : p->h) - 1;
	int a = total / 2, b = total - a;
	char cwd[PATH_MAX];
	pane_cwd(p, cwd, sizeof cwd);
	Pane *np = horiz ? pane_new(p->h, b, cwd) : pane_new(b, p->w, cwd);
	if (!np) {
		flash("could not split: %s", strerror(errno));
		return;
	}
	Node *old = p->node;
	Node *sp = calloc(1, sizeof *sp), *leaf = calloc(1, sizeof *leaf);
	if (!sp || !leaf)
		abort();
	sp->type = horiz ? N_HSPLIT : N_VSPLIT;
	sp->ratio = 0.5;
	sp->parent = old->parent;
	if (!old->parent)
		w->root = sp;
	else if (old->parent->a == old)
		old->parent->a = sp;
	else
		old->parent->b = sp;
	old->parent = sp;
	leaf->type = N_LEAF;
	leaf->pane = np;
	leaf->parent = sp;
	np->node = leaf;
	np->win = w;
	sp->a = old;
	sp->b = leaf;
	set_active(w, np);
	window_layout(w);
}

static int overlap(int a0, int a1, int b0, int b1)
{
	int lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
	return hi - lo;
}

static Pane *pane_dir(Pane *p, int dir)
{
	Window *w = p->win;
	Pane *best = NULL;
	int bestscore = -1;
	int cx = p->x + p->vt.cx, cy = p->y + p->vt.cy;
	for (int i = 0; i < S.npanes; i++) {
		Pane *q = S.panes[i];
		if (q->win != w || q == p)
			continue;
		int ok = 0, ov = 0, has = 0;
		switch (dir) {
		case D_LEFT:
			ok = q->x + q->w + 1 == p->x;
			ov = overlap(q->y, q->y + q->h, p->y, p->y + p->h);
			has = cy >= q->y && cy < q->y + q->h;
			break;
		case D_RIGHT:
			ok = p->x + p->w + 1 == q->x;
			ov = overlap(q->y, q->y + q->h, p->y, p->y + p->h);
			has = cy >= q->y && cy < q->y + q->h;
			break;
		case D_UP:
			ok = q->y + q->h + 1 == p->y;
			ov = overlap(q->x, q->x + q->w, p->x, p->x + p->w);
			has = cx >= q->x && cx < q->x + q->w;
			break;
		case D_DOWN:
			ok = p->y + p->h + 1 == q->y;
			ov = overlap(q->x, q->x + q->w, p->x, p->x + p->w);
			has = cx >= q->x && cx < q->x + q->w;
			break;
		}
		if (!ok || ov <= 0)
			continue;
		int score = ov + (has ? 100000 : 0) + (q == w->last ? 50000 : 0);
		if (score > bestscore) {
			bestscore = score;
			best = q;
		}
	}
	return best;
}

static void focus_dir(int dir)
{
	Window *w = curwin();
	Pane *p = curpane();
	if (!p)
		return;
	if (w->zoom) {
		w->zoom = NULL;
		window_layout(w);
	}
	Pane *q = pane_dir(p, dir);
	if (q)
		set_active(w, q);
}

static void resize_dir(int dir, int amount)
{
	Pane *p = curpane();
	if (!p || p->win->zoom)
		return;
	int type = (dir == D_LEFT || dir == D_RIGHT) ? N_HSPLIT : N_VSPLIT;
	Node *n = p->node->parent;
	while (n && n->type != type)
		n = n->parent;
	if (!n)
		return;
	int total = (type == N_HSPLIT ? n->w : n->h) - 1;
	if (total < 2)
		return;
	int cur = type == N_HSPLIT ? n->a->w : n->a->h;
	int d = (dir == D_LEFT || dir == D_UP) ? -amount : amount;
	int nv = clampi(cur + d, 1, total - 1);
	n->ratio = (double)nv / total;
	window_layout(p->win);
}

static void cycle_pane(int step)
{
	Window *w = curwin();
	if (!w)
		return;
	Pane *list[256];
	int n = win_panes(w->root, list, 256, 0);
	if (n < 2)
		return;
	int i;
	for (i = 0; i < n; i++)
		if (list[i] == w->active)
			break;
	if (w->zoom) {
		w->zoom = NULL;
		window_layout(w);
	}
	set_active(w, list[((i + step) % n + n) % n]);
}

/* ------------------------------------------------------------------ */
/* messages / prompt                                                   */
/* ------------------------------------------------------------------ */
static void flash(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(S.msg, sizeof S.msg, fmt, ap);
	va_end(ap);
	S.msg_until = now_ms() + 2500;
	S.dirty = 1;
}

static void prompt_open(int kind, const char *label, const char *init)
{
	S.prompt = kind;
	snprintf(S.plabel, sizeof S.plabel, "%s", label);
	snprintf(S.pbuf, sizeof S.pbuf, "%s", init ? init : "");
	S.dirty = 1;
}

/* ------------------------------------------------------------------ */
/* copy mode                                                           */
/* ------------------------------------------------------------------ */
static void cm_clamp(Pane *p)
{
	VT *v = &p->vt;
	int64_t minG = v->sbtotal - v->sblen, maxG = v->sbtotal + v->rows - 1;
	if (p->cm_cy < minG)
		p->cm_cy = minG;
	if (p->cm_cy > maxG)
		p->cm_cy = maxG;
	p->cm_cx = clampi(p->cm_cx, 0, p->w - 1);
	if (p->cm_top > p->cm_cy)
		p->cm_top = p->cm_cy;
	if (p->cm_cy >= p->cm_top + p->h)
		p->cm_top = p->cm_cy - p->h + 1;
	if (p->cm_top < minG)
		p->cm_top = minG;
	if (p->cm_top > v->sbtotal)
		p->cm_top = v->sbtotal;
}

static void cm_enter(Pane *p)
{
	if (!p || p->cm)
		return;
	p->cm = 1;
	p->cm_sel = 0;
	p->cm_top = p->vt.sbtotal;
	p->cm_cy = p->vt.sbtotal + p->vt.cy;
	p->cm_cx = p->vt.cx;
	cm_clamp(p);
	S.dirty = 1;
}

static int line_len(Pane *p, int64_t g)
{
	int n;
	const Cell *l = vt_gline(&p->vt, g, &n);
	if (!l)
		return 0;
	while (n > 0 && (l[n - 1].ch == ' ' || l[n - 1].ch == 0))
		n--;
	return n;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(Buf *o, const unsigned char *s, size_t n)
{
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)s[i] << 16;
		if (i + 1 < n)
			v |= (uint32_t)s[i + 1] << 8;
		if (i + 2 < n)
			v |= s[i + 2];
		char q[4] = { b64[(v >> 18) & 63], b64[(v >> 12) & 63],
		              i + 1 < n ? b64[(v >> 6) & 63] : '=', i + 2 < n ? b64[v & 63] : '=' };
		buf_add(o, q, 4);
	}
}

/*
 * Also hand copied text to a clipboard tool, because some terminals
 * (GNOME Terminal / VTE) ignore OSC 52.  Runs in a throwaway child so the
 * server never blocks; the SIGCHLD loop reaps it.
 */
static void clip_external(const char *d, size_t n)
{
	const char *cmd;
	if (getenv("WAYLAND_DISPLAY"))
		cmd = "wl-copy 2>/dev/null";
	else if (getenv("DISPLAY"))
		cmd = "{ xclip -selection clipboard -in || xsel --clipboard --input; } >/dev/null 2>&1";
	else
		return;
	if (!n)
		return;
	pid_t pid = fork();
	if (pid != 0)
		return;
	signal(SIGPIPE, SIG_DFL);
	FILE *f = popen(cmd, "w");
	if (f) {
		fwrite(d, 1, n, f);
		pclose(f);
	}
	_exit(0);
}

static void cm_yank(Pane *p)
{
	int64_t g0 = p->cm_sy, g1 = p->cm_cy;
	int x0 = p->cm_sx, x1 = p->cm_cx;
	if (!p->cm_sel) {
		g0 = g1 = p->cm_cy;
		x0 = 0;
		x1 = INT_MAX;
	} else if (g0 > g1 || (g0 == g1 && x0 > x1)) {
		int64_t tg = g0;
		g0 = g1;
		g1 = tg;
		int tx = x0;
		x0 = x1;
		x1 = tx;
	}
	Buf b = { 0 };
	for (int64_t g = g0; g <= g1; g++) {
		int n;
		const Cell *l = vt_gline(&p->vt, g, &n);
		int from = g == g0 ? x0 : 0, to = g == g1 ? x1 : INT_MAX;
		size_t keep = b.len;
		for (int c = from; l && c < n && c <= to; c++) {
			if (l[c].attr & A_WCONT)
				continue;
			buf_utf8(&b, l[c].ch ? l[c].ch : ' ');
			if (l[c].ch != ' ')
				keep = b.len;
		}
		b.len = keep;
		if (g != g1)
			buf_add(&b, "\n", 1);
	}
	free(S.paste);
	S.paste = b.d;
	S.pastelen = b.len;

	/* hand it to the outer terminal's clipboard too (OSC 52) */
	Buf o = { 0 };
	buf_puts(&o, "\033]52;c;");
	base64(&o, (const unsigned char *)b.d, b.len);
	buf_puts(&o, "\a");
	for (int i = 0; i < S.ncl; i++)
		if (S.cls[i]->attached)
			msg_pack(&S.cls[i]->out, MSG_OUTPUT, o.d, (uint32_t)o.len);
	buf_free(&o);
	clip_external(S.paste, S.pastelen);
	flash("copied %zu bytes", S.pastelen);
}

static int cm_match(const uint32_t *cp, int n, int i)
{
	if (i < 0 || i + S.searchlen > n)
		return 0;
	for (int k = 0; k < S.searchlen; k++) {
		uint32_t a = cp[i + k], b = S.search[k];
		if (a < 128 && a >= 'A' && a <= 'Z')
			a += 32;
		if (b < 128 && b >= 'A' && b <= 'Z')
			b += 32;
		if (a != b)
			return 0;
	}
	return 1;
}

static void cm_search(Pane *p, int dir)
{
	if (!S.searchlen)
		return;
	VT *v = &p->vt;
	int64_t minG = v->sbtotal - v->sblen, maxG = v->sbtotal + v->rows - 1;
	int64_t g = p->cm_cy;
	int startcol = p->cm_cx + dir;
	uint32_t *cp = malloc(sizeof(uint32_t) * (size_t)(v->cols + 1));
	int *col = malloc(sizeof(int) * (size_t)(v->cols + 1));
	if (!cp || !col)
		abort();
	for (int64_t iter = 0; iter <= maxG - minG + 1; iter++) {
		int n;
		const Cell *l = vt_gline(v, g, &n);
		int m = 0;
		for (int c = 0; l && c < n && c < v->cols; c++) {
			if (l[c].attr & A_WCONT)
				continue;
			cp[m] = l[c].ch;
			col[m] = c;
			m++;
		}
		/* translate the starting column into an index */
		int si;
		if (startcol == INT_MAX)
			si = m - 1;
		else if (startcol < 0)
			si = dir > 0 ? 0 : -1;
		else {
			si = dir > 0 ? m : -1;
			for (int k = 0; k < m; k++)
				if (col[k] >= startcol) {
					si = (dir > 0 || col[k] == startcol) ? k : k - 1;
					break;
				}
			if (dir < 0 && si == -1 && m && col[m - 1] < startcol)
				si = m - 1;
		}
		for (int i = si; i >= 0 && i < m; i += dir) {
			if (cm_match(cp, m, i)) {
				p->cm_cy = g;
				p->cm_cx = col[i];
				cm_clamp(p);
				free(cp);
				free(col);
				S.dirty = 1;
				return;
			}
		}
		g += dir;
		if (g < minG)
			g = maxG;
		if (g > maxG)
			g = minG;
		startcol = dir > 0 ? -1 : INT_MAX;
	}
	free(cp);
	free(col);
	flash("not found");
}

static void cm_key(Pane *p, int key)
{
	VT *v = &p->vt;
	int half = p->h / 2 > 0 ? p->h / 2 : 1;
	switch (key) {
	case 'q': case 27:
		p->cm = 0;
		break;
	case 3: /* C-c: copy the selection if there is one, else just leave */
		if (p->cm_sel)
			cm_yank(p);
		p->cm = 0;
		break;
	case 'k': case K_UP:
		p->cm_cy--;
		break;
	case 'j': case K_DOWN:
		p->cm_cy++;
		break;
	case 'h': case K_LEFT:
		p->cm_cx--;
		break;
	case 'l': case K_RIGHT:
		p->cm_cx++;
		break;
	case '0': case K_HOME:
		p->cm_cx = 0;
		break;
	case '$': case K_END: {
		int n = line_len(p, p->cm_cy);
		p->cm_cx = n > 0 ? n - 1 : 0;
		break;
	}
	case 0x15: /* C-u */
		p->cm_cy -= half;
		p->cm_top -= half;
		break;
	case 0x04: /* C-d */
		p->cm_cy += half;
		p->cm_top += half;
		break;
	case K_PGUP: case 0x02:
		p->cm_cy -= p->h;
		p->cm_top -= p->h;
		break;
	case K_PGDN:
		p->cm_cy += p->h;
		p->cm_top += p->h;
		break;
	case 'g':
		p->cm_cy = v->sbtotal - v->sblen;
		p->cm_cx = 0;
		break;
	case 'G':
		p->cm_top = v->sbtotal;
		p->cm_cy = v->sbtotal + v->cy;
		break;
	case 'H':
		p->cm_cy = p->cm_top;
		break;
	case 'L':
		p->cm_cy = p->cm_top + p->h - 1;
		break;
	case 'v': case ' ':
		p->cm_sel = !p->cm_sel;
		p->cm_sy = p->cm_cy;
		p->cm_sx = p->cm_cx;
		break;
	case 'V':
		p->cm_sel = 1;
		p->cm_sy = p->cm_cy;
		p->cm_sx = 0;
		p->cm_cx = p->w - 1;
		break;
	case 'y': case '\r':
		cm_yank(p);
		p->cm = 0;
		break;
	case '/': case 0x06: /* C-f */
		prompt_open(PR_SEARCH, "search ↑", "");
		break;
	case 'n':
		cm_search(p, -1);
		break;
	case 'N':
		cm_search(p, 1);
		break;
	case 'w': case 'b': case K_RIGHT | K_CTRL: case K_LEFT | K_CTRL: {
		/* crude word motion within the current line */
		int n;
		const Cell *l = vt_gline(v, p->cm_cy, &n);
		int c = p->cm_cx, d = (key == 'w' || key == (K_RIGHT | K_CTRL)) ? 1 : -1;
		if (l) {
			c += d;
			while (c > 0 && c < n && !(l[c].ch != ' ' && l[c - 1].ch == ' '))
				c += d;
			if (c >= n)
				c = n > 0 ? n - 1 : 0;
			p->cm_cx = c < 0 ? 0 : c;
		}
		break;
	}
	}
	if (p->cm)
		cm_clamp(p);
	S.dirty = 1;
}

/* ------------------------------------------------------------------ */
/* frame composition                                                   */
/* ------------------------------------------------------------------ */
static void fput(int x, int y, Cell c)
{
	if (x >= 0 && y >= 0 && x < F.cols && y < F.rows)
		F.c[y * F.cols + x] = c;
}

static int fstr(int x, int y, const char *s, uint32_t fg, uint32_t bg, uint16_t attr, int maxx)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t n = strlen(s);
	if (maxx > F.cols)
		maxx = F.cols;
	while (n) {
		uint32_t cp;
		int k = utf8_dec(p, n, &cp);
		if (k <= 0)
			break;
		p += k;
		n -= (size_t)k;
		if (cp < 0x20)
			cp = '?';
		int w = cwidth(cp);
		if (w == 0)
			continue;
		if (w < 0)
			w = 1;
		if (x + w > maxx)
			break;
		Cell c = { cp, fg, bg, (uint16_t)(attr | (w == 2 ? A_WIDE : 0)) };
		fput(x, y, c);
		if (w == 2) {
			Cell c2 = { 0, fg, bg, (uint16_t)(attr | A_WCONT) };
			fput(x + 1, y, c2);
		}
		x += w;
	}
	return x;
}

static void fill(int x, int y, int w, int h, uint32_t fg, uint32_t bg)
{
	Cell c = { ' ', fg, bg, 0 };
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			fput(i, j, c);
}

static int in_sel(Pane *p, int64_t g, int c)
{
	int64_t g0 = p->cm_sy, g1 = p->cm_cy;
	int x0 = p->cm_sx, x1 = p->cm_cx;
	if (g0 > g1 || (g0 == g1 && x0 > x1)) {
		int64_t t = g0;
		g0 = g1;
		g1 = t;
		int tx = x0;
		x0 = x1;
		x1 = tx;
	}
	if (g < g0 || g > g1)
		return 0;
	if (g == g0 && c < x0)
		return 0;
	if (g == g1 && c > x1)
		return 0;
	return 1;
}

static void draw_pane(Pane *p)
{
	static const Cell blank = { ' ', 0, 0, 0 };
	if (p->cm)
		cm_clamp(p);
	for (int r = 0; r < p->h; r++) {
		int64_t g = p->cm ? p->cm_top + r : p->vt.sbtotal + r;
		int n = 0;
		const Cell *l = vt_gline(&p->vt, g, &n);
		for (int c = 0; c < p->w; c++) {
			Cell cell = (l && c < n) ? l[c] : blank;
			if ((cell.attr & A_WIDE) && c + 1 >= p->w) {
				cell.ch = ' ';
				cell.attr &= ~A_WIDE;
			}
			if ((cell.attr & A_WCONT) && c == 0) {
				cell.ch = ' ';
				cell.attr &= ~A_WCONT;
			}
			if (p->cm && p->cm_sel && in_sel(p, g, c)) {
				cell.fg = P_DARK;
				cell.bg = P_ACCENT;
				cell.attr &= ~(A_REVERSE | A_DIM | A_INVIS);
			}
			fput(p->x + c, p->y + r, cell);
		}
	}
	if (p->cm) {
		char tmp[64];
		snprintf(tmp, sizeof tmp, " %lld/%d ", (long long)(p->vt.sbtotal - p->cm_top), p->vt.sblen);
		int x = p->x + p->w - strwidth(tmp);
		if (x >= p->x)
			fstr(x, p->y, tmp, P_DARK, P_YELLOW, A_BOLD, p->x + p->w);
	}
}

static const uint32_t boxch[16] = {
	0x2502, 0x2500, 0x2500, 0x2500, 0x2502, 0x250c, 0x2510, 0x252c,
	0x2502, 0x2514, 0x2518, 0x2534, 0x2502, 0x251c, 0x2524, 0x253c,
};

static void mark_borders(Node *n, uint8_t *m, int W, int H)
{
	if (!n || n->type == N_LEAF)
		return;
	if (n->type == N_HSPLIT) {
		int bx = n->a->x + n->a->w;
		for (int y = n->y; y < n->y + n->h; y++)
			if (bx >= 0 && bx < W && y >= 0 && y < H)
				m[y * W + bx] = 1;
	} else {
		int by = n->a->y + n->a->h;
		for (int x = n->x; x < n->x + n->w; x++)
			if (by >= 0 && by < H && x >= 0 && x < W)
				m[by * W + x] = 1;
	}
	mark_borders(n->a, m, W, H);
	mark_borders(n->b, m, W, H);
}

static void draw_borders(Window *w)
{
	int H = area_rows(), W = S.cols;
	uint8_t *m = calloc((size_t)(W * H), 1);
	if (!m)
		return;
	mark_borders(w->root, m, W, H);
	Pane *a = w->active;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (!m[y * W + x])
				continue;
			int u = y > 0 && m[(y - 1) * W + x];
			int d = y < H - 1 && m[(y + 1) * W + x];
			int l = x > 0 && m[y * W + x - 1];
			int r = x < W - 1 && m[y * W + x + 1];
			int act = a && x >= a->x - 1 && x <= a->x + a->w && y >= a->y - 1 && y <= a->y + a->h;
			Cell c = { boxch[u << 3 | d << 2 | l << 1 | r], act ? P_ACCENT : P_BORDER, 0, 0 };
			fput(x, y, c);
		}
	}
	free(m);
}

static void draw_status(void)
{
	int y = S.rows - 1, W = S.cols;
	fill(0, y, W, 1, 0, P_BAR);

	if (S.prompt) {
		char lab[64];
		snprintf(lab, sizeof lab, " %s ", S.plabel);
		int danger = S.prompt == PR_KILL_PANE || S.prompt == PR_KILL_WIN;
		int x = fstr(0, y, lab, P_DARK, danger ? P_RED : P_PEACH, A_BOLD, W);
		x = fstr(x, y, " ", 0, P_BAR, 0, W);
		const char *s = S.pbuf;
		while (*s && strwidth(s) > W - x - 1)
			s++;
		while ((*s & 0xc0) == 0x80)
			s++;
		x = fstr(x, y, s, C(255), P_BAR, 0, W);
		if (!danger) {
			F.cx = x < W ? x : W - 1;
			F.cy = y;
			F.cvis = 1;
			F.cstyle = 6;
		} else {
			F.cvis = 0;
		}
		return;
	}

	int prefix = menu_pending();
	Pane *ap = curpane();

	char sb[96];
	snprintf(sb, sizeof sb, " ◆ %s ", S.name);
	int x = fstr(0, y, sb, P_DARK, prefix ? P_PEACH : P_SKY, A_BOLD, W);
	if (ap && ap->cm)
		x = fstr(x + 1, y, " COPY ", P_DARK, P_YELLOW, A_BOLD, W);
	x += 1;

	/*
	 * Left side, after the tabs: a hint for getting out (always shown).
	 * Right side: clock and host.  As space runs out, drop the host, then
	 * the clock, then the "menu" half of the hint.
	 */
	char clock[16], host[80];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(clock, sizeof clock, " %H:%M ", &tm);
	snprintf(host, sizeof host, " %s ", S.host);
	const char *pn = prefix_name(prefix_key());
	int pw = strwidth(pn);
	int hshort = pw + 2 + 8;                  /* "P d detach "          */
	int hlong = pw + 8 + hshort;              /* "P menu · " + short    */
	int cw = strwidth(clock), hw = strwidth(host);
	static const int opts[][3] = {            /* long hint?, clock, host */
		{ 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 }, { 0, 0, 0 },
	};
	int avail = W - x - 12, o = 0;            /* keep room for a tab or two */
	for (; o < 4; o++)
		if ((opts[o][0] ? hlong : hshort) + (opts[o][1] ? cw : 0) +
		    (opts[o][2] ? hw : 0) <= avail)
			break;
	int hintw = opts[o][0] ? hlong : hshort;
	int rw = (opts[o][1] ? cw : 0) + (opts[o][2] ? hw : 0);
	int rx = W - rw;
	if (rx < x)
		rx = x;
	{
		int xx = rx;
		if (opts[o][1])
			xx = fstr(xx, y, clock, P_TABFG, P_TAB, 0, W);
		if (opts[o][2])
			fstr(xx, y, host, P_DARK, P_GREEN, A_BOLD, W);
	}
	int lim = rx - 2 - hintw;                 /* tabs stop short of the hint */
	if (lim < x)
		lim = x;

	if (S.msg[0]) {
		char m[200];
		snprintf(m, sizeof m, " %s ", S.msg);
		x = fstr(x, y, m, P_DARK, P_YELLOW, A_BOLD, lim) + 1;
	} else {
		for (int i = 0; i < S.nwin; i++) {
			Window *w = S.wins[i];
			char lab[96];
			int np = pane_count(w);
			snprintf(lab, sizeof lab, " %d %.20s%s%s ", i + 1, w->name,
			         np > 1 ? (w->zoom ? " ▣" : " ◫") : "", w->bell && i != S.cur ? " !" : "");
			uint32_t fg = P_TABFG, bg = P_TAB;
			uint16_t at = 0;
			if (i == S.cur) {
				fg = P_DARK;
				bg = P_ACCENT;
				at = A_BOLD;
			} else if (w->bell) {
				fg = P_DARK;
				bg = P_RED;
			}
			if (x + strwidth(lab) > lim) {
				x = fstr(x, y, "…", P_MUTED, P_BAR, 0, lim + 1) + 1;
				break;
			}
			x = fstr(x, y, lab, fg, bg, at, lim);
			x += 1;
		}
	}

	/* the hint follows the tabs, one space after them */
	int xx = x + 1, hmax = rx;
	if (opts[o][0]) {
		xx = fstr(xx, y, pn, P_ACCENT, P_BAR, A_BOLD, hmax);
		xx = fstr(xx, y, " menu · ", P_MUTED, P_BAR, 0, hmax);
	}
	xx = fstr(xx, y, pn, P_ACCENT, P_BAR, A_BOLD, hmax);
	xx = fstr(xx, y, " d", P_PEACH, P_BAR, A_BOLD, hmax);
	fstr(xx, y, " detach ", P_MUTED, P_BAR, 0, hmax);
}

/* rounded panel with an accent border */
static void draw_box(int x0, int y0, int bw, int bh)
{
	fill(x0, y0, bw, bh, P_TEXT, P_PANEL);
	Cell bc = { 0x2500, P_ACCENT, P_PANEL, 0 };
	for (int i = 1; i < bw - 1; i++) {
		fput(x0 + i, y0, bc);
		fput(x0 + i, y0 + bh - 1, bc);
	}
	bc.ch = 0x2502;
	for (int j = 1; j < bh - 1; j++) {
		fput(x0, y0 + j, bc);
		fput(x0 + bw - 1, y0 + j, bc);
	}
	bc.ch = 0x256d; fput(x0, y0, bc);
	bc.ch = 0x256e; fput(x0 + bw - 1, y0, bc);
	bc.ch = 0x2570; fput(x0, y0 + bh - 1, bc);
	bc.ch = 0x256f; fput(x0 + bw - 1, y0 + bh - 1, bc);
}

static const char *help_keys[][2] = {
	{ "c  C-t", "new window" },     { "n  Tab", "next window" },
	{ "p  S-Tab", "prev window" },  { "Home End", "first / last window" },
	{ "1 … 9", "jump to window" },  { ",", "rename window" },
	{ "&", "kill window" },         { "$", "rename session" },
	{ "d", "detach" },              { "s", "session manager" },
	{ "r  C-l", "redraw" },         { "?", "toggle this help" },
	{ "%  |", "split left / right" }, { "\"  -", "split top / bottom" },
	{ "hjkl ←↓↑→", "move focus" },  { "HJKL C-←→", "resize pane" },
	{ "o  ;", "next / last pane" }, { "z", "zoom pane" },
	{ "x  C-w", "kill pane" },      { "[  PgUp", "copy / scroll mode" },
	{ "]", "paste buffer" },        { "C-f", "search scrollback" },
	{ "e", "send the prefix key" }, { "prefix ×2", "send the prefix" },
};

static void draw_help(void)
{
	int nk = (int)(sizeof help_keys / sizeof *help_keys), half = (nk + 1) / 2;
	int bw = 72, bh = half + 8;
	if (bw > S.cols)
		bw = S.cols;
	if (bh > S.rows - 1)
		bh = S.rows - 1;
	int x0 = (S.cols - bw) / 2, y0 = (S.rows - 1 - bh) / 2;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	draw_box(x0, y0, bw, bh);

	const char *title = " gtm · key bindings ";
	fstr(x0 + (bw - strwidth(title)) / 2, y0, title, P_DARK, P_ACCENT, A_BOLD, x0 + bw - 1);
	const char *foot = " press any key ";
	fstr(x0 + (bw - strwidth(foot)) / 2, y0 + bh - 1, foot, P_MUTED, P_PANEL, 0, x0 + bw - 1);

	int x = fstr(x0 + 3, y0 + 2, "prefix ", P_MUTED, P_PANEL, 0, x0 + bw - 1);
	char pn[24];
	snprintf(pn, sizeof pn, " %s ", prefix_name(prefix_key()));
	x = fstr(x, y0 + 2, pn, P_DARK, P_SKY, A_BOLD, x0 + bw - 1);
	fstr(x, y0 + 2, "  then …", P_MUTED, P_PANEL, 0, x0 + bw - 1);

	for (int i = 0; i < nk; i++) {
		int col = i / half, row = i % half;
		int kx = x0 + 3 + col * 34, ky = y0 + 4 + row;
		if (ky >= y0 + bh - 1)
			continue;
		fstr(kx, ky, help_keys[i][0], P_ACCENT, P_PANEL, A_BOLD, x0 + bw - 1);
		const char *lab = help_keys[i][1];
		if (prefix_key() == 0x1b && !strcmp(help_keys[i][0], "prefix ×2"))
			lab = "session manager";
		fstr(kx + 12, ky, lab, P_TEXT, P_PANEL, 0, x0 + bw - 1);
	}
	int cy = y0 + 4 + half + 1;
	if (cy < y0 + bh - 1) {
		x = fstr(x0 + 3, cy, "copy mode ", P_YELLOW, P_PANEL, A_BOLD, x0 + bw - 1);
		fstr(x + 1, cy, "hjkl move · v select · y C-c copy · / C-f find · q quit",
		     P_MUTED, P_PANEL, 0, x0 + bw - 1);
	}
}

/*
 * Prefix menu: shown while a prefix is pending, so the next key can be
 * read off the screen instead of remembered.  Pressing a key runs it
 * exactly as before; esc (or any unbound key) closes it.
 */
typedef struct {
	const char *key, *label; /* key NULL: group heading */
	int hot;                 /* ways out of the session */
} MenuItem;

static const MenuItem menu_session[] = {
	{ NULL, "SESSION", 0 },
	{ "d", "detach (leave)", 1 },   { "s", "session manager", 1 },
	{ "$", "rename session", 0 },   { "r", "redraw screen", 0 },
	{ "?", "all key bindings", 0 },
	{ NULL, "COPY & PASTE", 0 },
	{ "[", "scroll / copy", 0 },    { "]", "paste", 0 },
	{ "C-f", "search history", 0 },
};
static const MenuItem menu_windows[] = {
	{ NULL, "WINDOWS", 0 },
	{ "c", "new window", 0 },       { "n", "next window", 0 },
	{ "p", "previous window", 0 },  { "1-9", "go to window", 0 },
	{ ",", "rename window", 0 },    { "&", "kill window", 0 },
};
static const MenuItem menu_panes[] = {
	{ NULL, "PANES", 0 },
	{ "|", "split left/right", 0 }, { "-", "split top/bottom", 0 },
	{ "hjkl", "move focus", 0 },    { "HJKL", "resize pane", 0 },
	{ "o", "next pane", 0 },        { "z", "zoom pane", 0 },
	{ "x", "kill pane", 0 },
};
static const struct {
	const MenuItem *it;
	int n;
} menu_cols[] = {
	{ menu_session, (int)(sizeof menu_session / sizeof *menu_session) },
	{ menu_windows, (int)(sizeof menu_windows / sizeof *menu_windows) },
	{ menu_panes, (int)(sizeof menu_panes / sizeof *menu_panes) },
};
#define MENU_NCOLS ((int)(sizeof menu_cols / sizeof *menu_cols))

static int menu_pending(void)
{
	for (int i = 0; i < S.ncl; i++)
		if (S.cls[i]->attached && S.cls[i]->prefix)
			return 1;
	return 0;
}

static void draw_menu(void)
{
	/* column geometry: key field, label field */
	int kw = 0, lw = 0;
	for (int c = 0; c < MENU_NCOLS; c++)
		for (int i = 0; i < menu_cols[c].n; i++) {
			const MenuItem *m = &menu_cols[c].it[i];
			int a = m->key ? strwidth(m->key) : 0, b = strwidth(m->label);
			if (a > kw)
				kw = a;
			if (b > lw)
				lw = b;
		}
	int colw = kw + 2 + lw, gap = 3, pad = 2;

	/* as many grid columns as fit; extra groups stack underneath */
	int ncol = MENU_NCOLS;
	while (ncol > 1 && 2 + 2 * pad + ncol * colw + (ncol - 1) * gap > S.cols)
		ncol--;
	int colh[MENU_NCOLS] = { 0 };
	for (int c = 0; c < MENU_NCOLS; c++)
		colh[c % ncol] += menu_cols[c].n + (c >= ncol ? 1 : 0);
	int maxh = 0;
	for (int g = 0; g < ncol; g++)
		if (colh[g] > maxh)
			maxh = colh[g];

	int bw = 2 + 2 * pad + ncol * colw + (ncol - 1) * gap;
	int bh = maxh + 4;
	if (bw > S.cols)
		bw = S.cols;
	if (bh > S.rows - 1)
		bh = S.rows - 1;
	if (bh < 3)
		return;
	int x0 = (S.cols - bw) / 2, y0 = S.rows - 1 - bh;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	int xmax = x0 + bw - 1, ymax = y0 + bh - 1;
	draw_box(x0, y0, bw, bh);

	char title[48];
	snprintf(title, sizeof title, " %s · press a key ", prefix_name(prefix_key()));
	fstr(x0 + 2, y0, title, P_DARK, P_ACCENT, A_BOLD, xmax);
	const char *foot = prefix_key() == 0x1b ? " Esc again: session manager · e sends Esc · q cancel " : " esc cancel ";
	fstr(xmax - 1 - strwidth(foot), ymax, foot, P_MUTED, P_PANEL, 0, xmax);

	int gy[MENU_NCOLS] = { 0 };
	for (int c = 0; c < MENU_NCOLS; c++) {
		int g = c % ncol;
		int cx = x0 + 1 + pad + g * (colw + gap);
		if (c >= ncol)
			gy[g]++; /* spacer above a stacked group */
		for (int i = 0; i < menu_cols[c].n; i++) {
			const MenuItem *m = &menu_cols[c].it[i];
			int y = y0 + 2 + gy[g]++;
			if (y >= ymax)
				break;
			if (!m->key) {
				fstr(cx, y, m->label, P_SKY, P_PANEL, A_BOLD, xmax);
				continue;
			}
			fstr(cx + kw - strwidth(m->key), y, m->key,
			     m->hot ? P_PEACH : P_ACCENT, P_PANEL, A_BOLD, xmax);
			fstr(cx + kw + 2, y, m->label, m->hot ? C(255) : P_TEXT,
			     P_PANEL, m->hot ? A_BOLD : 0, xmax);
		}
	}
}

static void compose(void)
{
	int rows = S.rows, cols = S.cols;
	if (F.rows != rows || F.cols != cols || !F.c) {
		free(F.c);
		F.c = malloc(sizeof(Cell) * (size_t)(rows * cols));
		if (!F.c)
			abort();
		F.rows = rows;
		F.cols = cols;
	}
	Cell blank = { ' ', 0, 0, 0 };
	for (int i = 0; i < rows * cols; i++)
		F.c[i] = blank;
	F.cvis = 0;
	F.cx = F.cy = 0;
	F.cstyle = 0;
	F.appcur = F.keypad = F.paste = 0;

	Window *w = curwin();
	if (w) {
		if (w->zoom) {
			draw_pane(w->zoom);
		} else {
			for (int i = 0; i < S.npanes; i++)
				if (S.panes[i]->win == w)
					draw_pane(S.panes[i]);
			draw_borders(w);
		}
		Pane *p = w->active;
		if (p) {
			F.appcur = p->vt.m_appcursor;
			F.keypad = p->vt.m_appkeypad;
			F.paste = p->vt.m_paste;
			F.cstyle = p->vt.curstyle;
			if (p->cm) {
				F.cx = p->x + p->cm_cx;
				F.cy = p->y + (int)(p->cm_cy - p->cm_top);
				F.cvis = 1;
			} else if (p->vt.m_cursor) {
				F.cx = p->x + p->vt.cx;
				F.cy = p->y + p->vt.cy;
				F.cvis = 1;
			}
		}
	}
	draw_status();
	if (S.help) {
		draw_help();
		F.cvis = 0;
	} else if (menu_pending()) {
		draw_menu();
		F.cvis = 0;
	}
}

/* ------------------------------------------------------------------ */
/* output to clients                                                   */
/* ------------------------------------------------------------------ */
static void sgr_col(Buf *o, uint32_t c, int bg)
{
	if (c == COL_DEFAULT) {
		buf_puts(o, bg ? ";49" : ";39");
	} else if ((c >> 24) == 1) {
		int i = (int)(c & 0xff);
		if (i < 8)
			buf_printf(o, ";%d", (bg ? 40 : 30) + i);
		else if (i < 16)
			buf_printf(o, ";%d", (bg ? 100 : 90) + i - 8);
		else
			buf_printf(o, ";%d;5;%d", bg ? 48 : 38, i);
	} else {
		buf_printf(o, ";%d;2;%u;%u;%u", bg ? 48 : 38, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
	}
}

static void sgr_out(Buf *o, const Cell *c)
{
	buf_puts(o, "\033[0");
	uint16_t a = c->attr;
	if (a & A_BOLD) buf_puts(o, ";1");
	if (a & A_DIM) buf_puts(o, ";2");
	if (a & A_ITALIC) buf_puts(o, ";3");
	if (a & A_UNDER) buf_puts(o, ";4");
	if (a & A_BLINK) buf_puts(o, ";5");
	if (a & A_REVERSE) buf_puts(o, ";7");
	if (a & A_INVIS) buf_puts(o, ";8");
	if (a & A_STRIKE) buf_puts(o, ";9");
	if (c->fg != COL_DEFAULT)
		sgr_col(o, c->fg, 0);
	if (c->bg != COL_DEFAULT)
		sgr_col(o, c->bg, 1);
	buf_puts(o, "m");
}

#define STYLE_MASK ((uint16_t)~(A_WIDE | A_WCONT))

static int same_style(const Cell *a, const Cell *b)
{
	return a->fg == b->fg && a->bg == b->bg && (a->attr & STYLE_MASK) == (b->attr & STYLE_MASK);
}

static int cell_eq(const Cell *a, const Cell *b)
{
	return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

static void put_glyph(Buf *o, uint32_t ch)
{
	if (ch < 0x20 || (ch >= 0x7f && ch < 0xa0))
		ch = ch ? '?' : ' ';
	buf_utf8(o, ch);
}

static void client_draw(Client *c)
{
	if (!c->attached || c->closing || c->out.len > 0)
		return;
	Buf o = { 0 }, t = { 0 };
	int rows = F.rows < c->rows ? F.rows : c->rows;
	int cols = F.cols < c->cols ? F.cols : c->cols;
	size_t cells = (size_t)(F.rows * F.cols);

	if (!c->front || c->frows != F.rows || c->fcols != F.cols || c->full) {
		free(c->front);
		c->front = malloc(sizeof(Cell) * cells);
		if (!c->front)
			abort();
		Cell inv = { 0xffffffffu, 0, 0, 0 };
		for (size_t i = 0; i < cells; i++)
			c->front[i] = inv;
		c->frows = F.rows;
		c->fcols = F.cols;
		c->full = 0;
		buf_puts(&o, "\033[0m\033[H\033[2J");
		c->lcx = c->lcy = c->lvis = c->lstyle = c->lappcur = c->lkeypad = c->lpaste = -1;
	}

	Cell pen = { 0 };
	int havepen = 0, px = -1, py = -1;
	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			size_t i = (size_t)(y * F.cols + x);
			if (cell_eq(&F.c[i], &c->front[i]))
				continue;
			int dx = x;
			if ((F.c[i].attr & A_WCONT) && x > 0 && (F.c[i - 1].attr & A_WIDE))
				dx = x - 1;
			size_t di = (size_t)(y * F.cols + dx);
			const Cell *n = &F.c[di];
			if (py != y || px != dx)
				buf_printf(&t, "\033[%d;%dH", y + 1, dx + 1);
			if (!havepen || !same_style(&pen, n)) {
				sgr_out(&t, n);
				pen = *n;
				havepen = 1;
			}
			int w = 1;
			if (n->attr & A_WIDE) {
				if (dx + 1 < cols) {
					put_glyph(&t, n->ch);
					w = 2;
				} else {
					buf_add(&t, " ", 1);
				}
			} else if (n->attr & A_WCONT) {
				buf_add(&t, " ", 1);
			} else {
				put_glyph(&t, n->ch ? n->ch : ' ');
			}
			c->front[di] = F.c[di];
			if (w == 2)
				c->front[di + 1] = F.c[di + 1];
			px = dx + w;
			py = y;
			if (px >= cols)
				px = -1;
			x = dx + w - 1;
		}
	}

	int moved = F.cx != c->lcx || F.cy != c->lcy || F.cvis != c->lvis;
	if (t.len) {
		buf_puts(&o, "\033[?2026h\033[?25l");
		buf_add(&o, t.d, t.len);
	}
	if (F.appcur != c->lappcur) {
		buf_puts(&o, F.appcur ? "\033[?1h" : "\033[?1l");
		c->lappcur = F.appcur;
	}
	if (F.keypad != c->lkeypad) {
		buf_puts(&o, F.keypad ? "\033=" : "\033>");
		c->lkeypad = F.keypad;
	}
	if (F.paste != c->lpaste) {
		buf_puts(&o, F.paste ? "\033[?2004h" : "\033[?2004l");
		c->lpaste = F.paste;
	}
	if (F.cstyle != c->lstyle) {
		buf_printf(&o, "\033[%d q", F.cstyle);
		c->lstyle = F.cstyle;
	}
	if (t.len || moved) {
		if (F.cvis && F.cy < c->rows && F.cx < c->cols)
			buf_printf(&o, "\033[%d;%dH\033[?25h", F.cy + 1, F.cx + 1);
		else
			buf_puts(&o, "\033[?25l");
		c->lcx = F.cx;
		c->lcy = F.cy;
		c->lvis = F.cvis;
	}
	if (t.len)
		buf_puts(&o, "\033[?2026l");
	if (S.bell)
		buf_puts(&o, "\a");
	if (o.len)
		msg_pack(&c->out, MSG_OUTPUT, o.d, (uint32_t)o.len);
	buf_free(&o);
	buf_free(&t);
}

static void client_flush(Client *c)
{
	while (c->out.len) {
		ssize_t w = write(c->fd, c->out.d, c->out.len);
		if (w > 0) {
			buf_consume(&c->out, (size_t)w);
		} else if (w < 0 && errno == EINTR) {
			continue;
		} else if (w < 0 && errno == EAGAIN) {
			break;
		} else {
			c->dead = 1;
			break;
		}
	}
	if (!c->out.len && c->closing)
		c->dead = 1;
}

static void render(void)
{
	int pending = 0;
	compose();
	for (int i = 0; i < S.ncl; i++) {
		Client *c = S.cls[i];
		client_flush(c);
		if (c->attached && !c->closing && c->out.len) {
			pending = 1; /* still draining: draw it next time round */
			continue;
		}
		client_draw(c);
		client_flush(c);
	}
	if (!pending)
		S.bell = 0;
	S.dirty = pending;
	S.last_render = now_ms();
}

static void client_exit(Client *c, char reason)
{
	if (c->attached)
		msg_pack(&c->out, MSG_EXIT, &reason, 1);
	c->attached = 0;
	c->closing = 1;
	c->prefix = 0;
	resize_session();
	S.dirty = 1;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */
static void paste_buffer(Pane *p)
{
	if (!p)
		return;
	if (!S.paste || !S.pastelen) {
		flash("paste buffer is empty");
		return;
	}
	p->cm = 0;
	Buf b = { 0 };
	if (p->vt.m_paste)
		buf_puts(&b, "\033[200~");
	for (size_t i = 0; i < S.pastelen; i++)
		buf_add(&b, S.paste[i] == '\n' ? "\r" : &S.paste[i], 1);
	if (p->vt.m_paste)
		buf_puts(&b, "\033[201~");
	pane_write(p, b.d, b.len);
	buf_free(&b);
}

static int do_rename_session(const char *nn, char *err, size_t en)
{
	if (!valid_name(nn)) {
		snprintf(err, en, "invalid name (use letters, digits, - _ .)");
		return -1;
	}
	if (!strcmp(nn, S.name))
		return 0;
	if (session_alive(nn)) {
		snprintf(err, en, "a session named '%s' already exists", nn);
		return -1;
	}
	char np[PATH_MAX];
	sock_path(np, sizeof np, nn);
	if (rename(S.path, np) < 0) {
		snprintf(err, en, "%s", strerror(errno));
		return -1;
	}
	snprintf(S.name, sizeof S.name, "%s", nn);
	snprintf(S.path, sizeof S.path, "%s", np);
	S.dirty = 1;
	return 0;
}

static Pane *pane_by_id(int id)
{
	for (int i = 0; i < S.npanes; i++)
		if (S.panes[i]->id == id)
			return S.panes[i];
	return NULL;
}

static void prompt_submit(void)
{
	int kind = S.prompt;
	S.prompt = PR_NONE;
	S.dirty = 1;
	Window *w = curwin();
	switch (kind) {
	case PR_RENAME_WIN:
		if (w) {
			if (S.pbuf[0]) {
				snprintf(w->name, sizeof w->name, "%s", S.pbuf);
				w->named = 1;
			} else {
				w->named = 0; /* empty name: back to automatic */
			}
		}
		break;
	case PR_RENAME_SESS: {
		char err[128];
		if (do_rename_session(S.pbuf, err, sizeof err) < 0)
			flash("%s", err);
		break;
	}
	case PR_SEARCH: {
		const unsigned char *s = (const unsigned char *)S.pbuf;
		size_t n = strlen(S.pbuf);
		S.searchlen = 0;
		while (n && S.searchlen < 64) {
			uint32_t cp;
			int k = utf8_dec(s, n, &cp);
			if (k <= 0)
				break;
			S.search[S.searchlen++] = cp;
			s += k;
			n -= (size_t)k;
		}
		Pane *p = curpane();
		if (p && p->cm)
			cm_search(p, -1);
		break;
	}
	}
}

static void prompt_key(int key)
{
	S.dirty = 1;
	if (S.prompt == PR_KILL_PANE || S.prompt == PR_KILL_WIN) {
		int kind = S.prompt;
		S.prompt = PR_NONE;
		if (key != 'y' && key != 'Y')
			return;
		if (kind == PR_KILL_PANE) {
			Pane *p = pane_by_id(S.ptarget);
			if (p)
				p->dead = 1;
		} else {
			for (int i = 0; i < S.nwin; i++) {
				if (i != S.ptarget)
					continue;
				for (int j = 0; j < S.npanes; j++)
					if (S.panes[j]->win == S.wins[i])
						S.panes[j]->dead = 1;
			}
		}
		return;
	}
	size_t len = strlen(S.pbuf);
	switch (key) {
	case '\r': case '\n':
		prompt_submit();
		return;
	case 27: case 3: case 7:
		S.prompt = PR_NONE;
		return;
	case 0x7f: case 8:
		while (len > 0 && (S.pbuf[len - 1] & 0xc0) == 0x80)
			len--;
		if (len > 0)
			len--;
		S.pbuf[len] = 0;
		return;
	case 0x15:
		S.pbuf[0] = 0;
		return;
	case 0x17:
		while (len > 0 && S.pbuf[len - 1] == ' ')
			len--;
		while (len > 0 && S.pbuf[len - 1] != ' ')
			len--;
		S.pbuf[len] = 0;
		return;
	}
	if (key >= 0x20 && key < 0x110000 && key != 0x7f) {
		char t[4];
		int k = utf8_enc(t, (uint32_t)key);
		if (len + (size_t)k < sizeof S.pbuf - 1) {
			memcpy(S.pbuf + len, t, (size_t)k);
			S.pbuf[len + (size_t)k] = 0;
		}
	}
}

static void do_command(Client *c, int key)
{
	Window *w = curwin();
	Pane *p = curpane();
	S.dirty = 1;
	if (!w)
		return;
	/*
	 * Prefix twice: with Esc that's the way back to the session manager
	 * (Esc Esc there quits); other prefixes send themselves, tmux-style.
	 * `e` always sends the prefix key, so a real Esc is still reachable.
	 */
	if (key == prefix_key() || key == 'e') {
		if (key == 0x1b) {
			client_exit(c, EXIT_MANAGER);
		} else {
			char b = (char)prefix_key();
			pane_write(p, &b, 1);
		}
		return;
	}
	/*
	 * Besides the tmux-style letters, accept what tab-style shortcuts send
	 * (C-t new, C-Tab / C-S-Tab next / prev, C-w close, C-f find), so
	 * Alt-layer remaps that emit those keep their meaning after the prefix.
	 */
	switch (key) {
	case 'c': case 0x14: /* C-t */
		new_window();
		break;
	case 'n': case '\t':
		select_window((S.cur + 1) % S.nwin);
		break;
	case 'p': case K_BTAB: case K_BTAB | K_CTRL:
		select_window((S.cur + S.nwin - 1) % S.nwin);
		break;
	case K_HOME:
		select_window(0);
		break;
	case K_END:
		select_window(S.nwin - 1);
		break;
	case 0x06: /* C-f: search scrollback */
		cm_enter(p);
		if (p && p->cm)
			prompt_open(PR_SEARCH, "search ↑", "");
		break;
	case '%': case '|':
		split(1);
		break;
	case '"': case '-':
		split(0);
		break;
	case 'h': case K_LEFT:
		focus_dir(D_LEFT);
		break;
	case 'j': case K_DOWN:
		focus_dir(D_DOWN);
		break;
	case 'k': case K_UP:
		focus_dir(D_UP);
		break;
	case 'l': case K_RIGHT:
		focus_dir(D_RIGHT);
		break;
	case 'H': case K_LEFT | K_CTRL: case K_LEFT | K_SHIFT:
		resize_dir(D_LEFT, 3);
		break;
	case 'L': case K_RIGHT | K_CTRL: case K_RIGHT | K_SHIFT:
		resize_dir(D_RIGHT, 3);
		break;
	case 'K': case K_UP | K_CTRL: case K_UP | K_SHIFT:
		resize_dir(D_UP, 2);
		break;
	case 'J': case K_DOWN | K_CTRL: case K_DOWN | K_SHIFT:
		resize_dir(D_DOWN, 2);
		break;
	case 'o':
		cycle_pane(1);
		break;
	case ';':
		if (w->last && !w->last->dead) {
			if (w->zoom) {
				w->zoom = NULL;
				window_layout(w);
			}
			set_active(w, w->last);
		}
		break;
	case 'z':
		if (pane_count(w) > 1 || w->zoom) {
			w->zoom = w->zoom ? NULL : p;
			window_layout(w);
		}
		break;
	case 'x': case 0x17: /* C-w */
		if (p) {
			S.ptarget = p->id;
			prompt_open(PR_KILL_PANE, "kill pane? y/n", NULL);
		}
		break;
	case '&': {
		char lab[96];
		snprintf(lab, sizeof lab, "kill window %d (%.30s)? y/n", S.cur + 1, w->name);
		S.ptarget = S.cur;
		prompt_open(PR_KILL_WIN, lab, NULL);
		break;
	}
	case 'd':
		client_exit(c, EXIT_DETACH);
		break;
	case 's':
		client_exit(c, EXIT_MANAGER);
		break;
	case ',':
		prompt_open(PR_RENAME_WIN, "rename window", w->name);
		break;
	case '$':
		prompt_open(PR_RENAME_SESS, "rename session", S.name);
		break;
	case '[':
		cm_enter(p);
		break;
	case K_PGUP:
		cm_enter(p);
		cm_key(p, K_PGUP);
		break;
	case ']':
		paste_buffer(p);
		break;
	case '?':
		S.help = 1;
		break;
	case 'r': case 0x0c: /* C-l */
		for (int i = 0; i < S.ncl; i++)
			S.cls[i]->full = 1;
		break;
	default:
		if (key >= '1' && key <= '9')
			select_window(key - '1');
		else if (key == '0')
			select_window(9);
		break;
	}
}

/*
 * With Esc as the prefix, only a lone Esc counts: one that ends a read.
 * Esc followed by more bytes is an arrow / function key, an Alt-key or a
 * paste, and goes to the pane untouched.
 */
#define ESC_GLUE_MS 20 /* bytes this soon after a lone Esc belong to it */

static const unsigned char *find_prefix(const unsigned char *s, size_t n, unsigned char pk)
{
	if (pk != 0x1b)
		return memchr(s, pk, n);
	return n && s[n - 1] == 0x1b ? s + n - 1 : NULL;
}

static void handle_input(Client *c, const char *d, size_t n)
{
	const unsigned char *s = (const unsigned char *)d;
	const unsigned char pk = (unsigned char)prefix_key();
	size_t i = 0;
	/*
	 * An escape sequence split across reads looks like a lone Esc followed
	 * almost immediately by the rest; hand the Esc back to the pane.
	 */
	if (c->prefix && pk == 0x1b && now_ms() - c->prefix_ms < ESC_GLUE_MS) {
		Pane *p = curpane();
		c->prefix = 0;
		S.dirty = 1;
		if (p && !p->cm)
			pane_write(p, "\033", 1);
	}
	while (i < n) {
		int key, k;
		if (S.help) {
			k = parse_key(s + i, n - i, &key);
			i += (size_t)(k > 0 ? k : 1);
			S.help = 0;
			S.dirty = 1;
			continue;
		}
		if (S.prompt) {
			k = parse_key(s + i, n - i, &key);
			i += (size_t)(k > 0 ? k : 1);
			if (key != K_NONE)
				prompt_key(key);
			continue;
		}
		if (c->prefix) {
			c->prefix = 0;
			k = parse_key(s + i, n - i, &key);
			i += (size_t)(k > 0 ? k : 1);
			if (key != K_NONE)
				do_command(c, key);
			S.dirty = 1;
			if (!c->attached)
				return;
			continue;
		}
		Pane *p = curpane();
		/* in copy mode an Esc prefix is just Esc: it leaves copy mode */
		if (p && p->cm && (s[i] != pk || pk == 0x1b)) {
			k = parse_key(s + i, n - i, &key);
			i += (size_t)(k > 0 ? k : 1);
			if (key != K_NONE)
				cm_key(p, key);
			continue;
		}
		const unsigned char *q = find_prefix(s + i, n - i, pk);
		size_t len = q ? (size_t)(q - (s + i)) : n - i;
		if (len && p && !p->cm)
			pane_write(p, s + i, len);
		i += len;
		if (q) {
			c->prefix = 1;
			c->prefix_ms = now_ms();
			i++;
			S.dirty = 1;
		}
	}
}

/* ------------------------------------------------------------------ */
/* client messages                                                     */
/* ------------------------------------------------------------------ */
static void send_info(Client *c)
{
	Buf b = { 0 };
	int nc = 0;
	for (int i = 0; i < S.ncl; i++)
		if (S.cls[i]->attached)
			nc++;
	buf_printf(&b, "name=%s\npid=%d\ncreated=%ld\nclients=%d\nwindows=%d\nactive=%d\npanes=%d\n",
	           S.name, (int)getpid(), (long)S.created, nc, S.nwin, S.cur, S.npanes);
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.wins[i];
		buf_printf(&b, "win=%d\t%d\t%s\n", (w->bell ? 1 : 0) | (w->zoom ? 2 : 0), pane_count(w), w->name);
	}
	msg_pack(&c->out, MSG_REPLY, b.d, (uint32_t)b.len);
	buf_free(&b);
}

static void send_preview(Client *c, int prow, int pcol)
{
	prow = clampi(prow, 1, 500);
	pcol = clampi(pcol, 1, 1000);
	compose();
	int area = area_rows();
	int cy = 0;
	Pane *p = curpane();
	if (p)
		cy = p->y + (p->cm ? (int)(p->cm_cy - p->cm_top) : p->vt.cy);
	int off = cy >= prow ? cy - prow + 1 : 0;
	if (p && !p->cm && !p->vt.m_cursor)
		off = 0; /* full-screen apps: the top is the interesting part */
	if (off + prow > area)
		off = area - prow > 0 ? area - prow : 0;

	Buf o = { 0 };
	static const Cell blank = { ' ', 0, 0, 0 };
	for (int r = 0; r < prow; r++) {
		int y = off + r;
		Cell pen = { 0 };
		int havepen = 0;
		for (int x = 0; x < pcol; x++) {
			Cell cell = (y < area && x < F.cols) ? F.c[y * F.cols + x] : blank;
			if (cell.attr & A_WCONT) {
				if (x == 0)
					cell = blank;
				else
					continue;
			}
			if ((cell.attr & A_WIDE) && (x + 1 >= pcol)) {
				cell.ch = ' ';
				cell.attr &= ~A_WIDE;
			}
			if (!havepen || !same_style(&pen, &cell)) {
				sgr_out(&o, &cell);
				pen = cell;
				havepen = 1;
			}
			put_glyph(&o, cell.ch ? cell.ch : ' ');
		}
		buf_puts(&o, "\033[0m\n");
	}
	msg_pack(&c->out, MSG_REPLY, o.d, (uint32_t)o.len);
	buf_free(&o);
}

static void client_msg(Client *c, uint32_t type, Buf *pl)
{
	uint32_t sz[2] = { 0, 0 };
	switch (type) {
	case MSG_ATTACH:
	case MSG_RESIZE:
		if (pl->len >= sizeof sz) {
			memcpy(sz, pl->d, sizeof sz);
			c->rows = clampi((int)sz[0], 1, 1000);
			c->cols = clampi((int)sz[1], 1, 2000);
			c->attached = 1;
			c->full = 1;
			resize_session();
			S.dirty = 1;
		}
		break;
	case MSG_INPUT:
		if (c->attached)
			handle_input(c, pl->d, pl->len);
		break;
	case MSG_DETACH:
		client_exit(c, EXIT_DETACH);
		break;
	case MSG_INFO:
		send_info(c);
		c->closing = 1;
		break;
	case MSG_PREVIEW:
		if (pl->len >= sizeof sz) {
			memcpy(sz, pl->d, sizeof sz);
			send_preview(c, (int)sz[0], (int)sz[1]);
		} else {
			msg_pack(&c->out, MSG_REPLY, NULL, 0);
		}
		c->closing = 1;
		break;
	case MSG_KILL:
		msg_pack(&c->out, MSG_REPLY, "ok", 2);
		c->closing = 1;
		S.quit = EXIT_KILLED;
		break;
	case MSG_RENAME: {
		char nn[64], err[128];
		snprintf(nn, sizeof nn, "%.*s", (int)(pl->len < 63 ? pl->len : 63), pl->d ? pl->d : "");
		if (do_rename_session(nn, err, sizeof err) == 0)
			msg_pack(&c->out, MSG_REPLY, "ok", 2);
		else
			msg_pack(&c->out, MSG_REPLY, err, (uint32_t)strlen(err));
		c->closing = 1;
		break;
	}
	}
}

static void client_read(Client *c)
{
	char buf[65536];
	ssize_t n = read(c->fd, buf, sizeof buf);
	if (n > 0) {
		buf_add(&c->in, buf, (size_t)n);
		uint32_t t;
		Buf pl = { 0 };
		while (!c->dead && msg_parse(&c->in, &t, &pl))
			client_msg(c, t, &pl);
		buf_free(&pl);
	} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
		c->dead = 1;
	}
}

static void accept_client(void)
{
	int fd = accept4(S.lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0)
		return;
	Client *c = calloc(1, sizeof *c);
	Client **nc = realloc(S.cls, sizeof *nc * (size_t)(S.ncl + 1));
	if (!c || !nc)
		abort();
	c->fd = fd;
	S.cls = nc;
	S.cls[S.ncl++] = c;
}

static void reap_clients(void)
{
	int changed = 0;
	for (int i = 0; i < S.ncl;) {
		Client *c = S.cls[i];
		if (!c->dead) {
			i++;
			continue;
		}
		if (c->attached)
			changed = 1;
		close(c->fd);
		buf_free(&c->in);
		buf_free(&c->out);
		free(c->front);
		free(c);
		memmove(&S.cls[i], &S.cls[i + 1], sizeof *S.cls * (size_t)(S.ncl - i - 1));
		S.ncl--;
	}
	if (changed) {
		resize_session();
		S.dirty = 1;
	}
}

/* ------------------------------------------------------------------ */
/* pane io                                                             */
/* ------------------------------------------------------------------ */
static void pane_read(Pane *p)
{
	char buf[65536];
	ssize_t n = read(p->fd, buf, sizeof buf);
	if (n > 0) {
		vt_write(&p->vt, buf, (size_t)n);
		if (p->vt.reply.len) {
			pane_write(p, p->vt.reply.d, p->vt.reply.len);
			buf_reset(&p->vt.reply);
		}
		if (p->win == curwin())
			S.dirty = 1;
		if (p->vt.bell) {
			p->vt.bell = 0;
			if (p->win == curwin())
				S.bell = 1;
			else
				p->win->bell = 1;
			S.dirty = 1;
		}
	} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
		p->dead = 1;
	}
}

static void tick(void)
{
	int64_t now = now_ms();
	if (S.msg[0] && now >= S.msg_until) {
		S.msg[0] = 0;
		S.dirty = 1;
	}
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	if (tm.tm_min != S.lastmin) {
		S.lastmin = tm.tm_min;
		S.dirty = 1;
	}
	for (int i = 0; i < S.nwin; i++) {
		Window *w = S.wins[i];
		if (w->named || !w->active)
			continue;
		char nm[64];
		if (proc_name(fg_pid(w->active), nm, sizeof nm) == 0 && strcmp(nm, w->name)) {
			snprintf(w->name, sizeof w->name, "%s", nm);
			S.dirty = 1;
		}
	}
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */
static void on_term(int sig)
{
	(void)sig;
	got_term = 1;
}

static void on_chld(int sig) { (void)sig; }

static void server_loop(void)
{
	struct pollfd *pf = NULL;
	int pfcap = 0;
	int64_t last_tick = 0;

	while (!S.quit) {
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		if (got_term) {
			S.quit = EXIT_KILLED;
			break;
		}
		int ncl = S.ncl, np = S.npanes;
		int need = 1 + ncl + np;
		if (need > pfcap) {
			pfcap = need * 2;
			pf = realloc(pf, sizeof *pf * (size_t)pfcap);
			if (!pf)
				abort();
		}
		pf[0] = (struct pollfd){ S.lfd, POLLIN, 0 };
		for (int i = 0; i < ncl; i++)
			pf[1 + i] = (struct pollfd){ S.cls[i]->fd,
				(short)(POLLIN | (S.cls[i]->out.len ? POLLOUT : 0)), 0 };
		for (int i = 0; i < np; i++)
			pf[1 + ncl + i] = (struct pollfd){ S.panes[i]->fd,
				(short)(POLLIN | (S.panes[i]->wbuf.len ? POLLOUT : 0)), 0 };

		int64_t now = now_ms();
		int timeout = 1000 - (int)(now % 1000);
		if (S.dirty) {
			int dt = 8 - (int)(now - S.last_render);
			if (dt < 0)
				dt = 0;
			if (dt < timeout)
				timeout = dt;
		}
		int r = poll(pf, (nfds_t)need, timeout);
		if (r < 0 && errno != EINTR)
			break;

		if (r > 0) {
			Pane **snap = malloc(sizeof *snap * (size_t)(np ? np : 1));
			Client **csnap = malloc(sizeof *csnap * (size_t)(ncl ? ncl : 1));
			if (!snap || !csnap)
				abort();
			memcpy(snap, S.panes, sizeof *snap * (size_t)np);
			memcpy(csnap, S.cls, sizeof *csnap * (size_t)ncl);
			for (int i = 0; i < np; i++) {
				Pane *p = snap[i];
				short re = pf[1 + ncl + i].revents;
				if (p->dead)
					continue;
				if (re & POLLOUT)
					pane_flush(p);
				if (re & (POLLIN | POLLHUP | POLLERR))
					pane_read(p);
			}
			for (int i = 0; i < ncl; i++) {
				Client *c = csnap[i];
				short re = pf[1 + i].revents;
				if (c->dead)
					continue;
				if (re & POLLOUT) {
					client_flush(c);
					if (!c->out.len)
						S.dirty = 1;
				}
				if (re & (POLLIN | POLLHUP | POLLERR))
					client_read(c);
			}
			free(snap);
			free(csnap);
			if (pf[0].revents & POLLIN)
				accept_client();
		}

		/* dead panes may cascade into window/session removal */
		for (int i = 0; i < S.npanes;) {
			if (S.panes[i]->dead) {
				pane_remove(S.panes[i]);
				i = 0;
				if (S.quit)
					break;
			} else {
				i++;
			}
		}
		if (S.quit)
			break;

		now = now_ms();
		if (now - last_tick >= 1000 || now / 1000 != last_tick / 1000) {
			last_tick = now;
			tick();
		}
		if (S.dirty && now - S.last_render >= 8)
			render();
		else
			for (int i = 0; i < S.ncl; i++)
				if (S.cls[i]->out.len)
					client_flush(S.cls[i]);
		reap_clients();
	}
	free(pf);
}

static void server_shutdown(void)
{
	char reason = (char)S.quit;
	for (int i = 0; i < S.ncl; i++) {
		Client *c = S.cls[i];
		if (c->attached)
			msg_pack(&c->out, MSG_EXIT, &reason, 1);
		int fl = fcntl(c->fd, F_GETFL);
		fcntl(c->fd, F_SETFL, fl & ~O_NONBLOCK);
		if (c->out.len)
			write_all(c->fd, c->out.d, c->out.len);
		close(c->fd);
	}
	for (int i = 0; i < S.npanes; i++) {
		kill(S.panes[i]->pid, SIGHUP);
		close(S.panes[i]->fd);
	}
	unlink(S.path);
	close(S.lfd);
}

static void pick_term(void)
{
	const char *dirs[] = { "/usr/share/terminfo", "/lib/terminfo", "/etc/terminfo",
	                       "/usr/lib/terminfo", NULL };
	char p[PATH_MAX];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(p, sizeof p, "%s/.terminfo/s/screen-256color", home);
		if (access(p, R_OK) == 0)
			goto found;
	}
	for (int i = 0; dirs[i]; i++) {
		snprintf(p, sizeof p, "%s/s/screen-256color", dirs[i]);
		if (access(p, R_OK) == 0)
			goto found;
	}
	snprintf(S.term, sizeof S.term, "xterm-256color");
	return;
found:
	snprintf(S.term, sizeof S.term, "screen-256color");
}

/* fork a daemon serving session `name`; returns once it is accepting */
int server_start(const char *name, int rows, int cols, char *err, size_t errn)
{
	if (!valid_name(name)) {
		snprintf(err, errn, "invalid session name '%s' (use letters, digits, - _ .)", name);
		return -1;
	}
	if (session_alive(name)) {
		snprintf(err, errn, "session '%s' already exists", name);
		return -1;
	}
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char path[sizeof sa.sun_path];
	sock_path(path, sizeof path, name);
	unlink(path);
	memcpy(sa.sun_path, path, strlen(path) + 1);
	int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(lfd, 16) < 0) {
		snprintf(err, errn, "cannot listen on %s: %s", path, strerror(errno));
		if (lfd >= 0)
			close(lfd);
		return -1;
	}
	chmod(path, 0600);
	fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);

	int pp[2];
	if (pipe(pp) < 0) {
		snprintf(err, errn, "pipe: %s", strerror(errno));
		close(lfd);
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		snprintf(err, errn, "fork: %s", strerror(errno));
		close(lfd);
		return -1;
	}
	if (pid == 0) {
		close(pp[0]);
		setsid();
		if (fork() != 0)
			_exit(0);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
		signal(SIGPIPE, SIG_IGN);
		signal(SIGHUP, SIG_IGN);
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, SIG_IGN);
		signal(SIGTSTP, SIG_IGN);
		signal(SIGTTIN, SIG_IGN);
		signal(SIGTTOU, SIG_IGN);
		struct sigaction sa_t = { .sa_handler = on_term };
		sigaction(SIGTERM, &sa_t, NULL);
		struct sigaction sa_c = { .sa_handler = on_chld, .sa_flags = SA_NOCLDSTOP };
		sigaction(SIGCHLD, &sa_c, NULL);

		memset(&S, 0, sizeof S);
		snprintf(S.name, sizeof S.name, "%s", name);
		snprintf(S.path, sizeof S.path, "%s", path);
		S.lfd = lfd;
		S.created = time(NULL);
		S.rows = rows < 3 ? 3 : rows;
		S.cols = cols < 10 ? 10 : cols;
		S.lastmin = -1;
		if (!getcwd(S.startdir, sizeof S.startdir))
			snprintf(S.startdir, sizeof S.startdir, "%s", getenv("HOME") ? getenv("HOME") : "/");
		gethostname(S.host, sizeof S.host - 1);
		S.host[strcspn(S.host, ".")] = 0;
		pick_term();
		unsetenv("GTM");
		unsetenv("GTM_PANE");

		if (!window_new(S.startdir)) {
			(void)!write(pp[1], "e", 1);
			unlink(path);
			_exit(1);
		}
		(void)!write(pp[1], "k", 1);
		close(pp[1]);
		server_loop();
		server_shutdown();
		_exit(0);
	}
	close(pp[1]);
	close(lfd);
	waitpid(pid, NULL, 0);
	char c = 0;
	ssize_t k;
	do
		k = read(pp[0], &c, 1);
	while (k < 0 && errno == EINTR);
	close(pp[0]);
	if (k != 1 || c != 'k') {
		snprintf(err, errn, "session server failed to start");
		return -1;
	}
	return 0;
}
