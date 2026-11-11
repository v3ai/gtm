/*
 * gtm - session manager: an nnn-style full screen browser for sessions
 */
#include "gtm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* palette (xterm 256-colour indices) */
enum {
	ACC = 147, SKY = 117, DARK = 234, BAR = 235, PANEL = 236, SEL = 237,
	BORDER = 238, FAINT = 240, DIM = 243, MUTED = 245, TEXT = 252, BRIGHT = 255,
	GREEN = 150, RED = 210, PEACH = 216, YELLOW = 222,
};

enum { M_NORMAL, M_FILTER, M_NEW, M_NEW_DET, M_RENAME, M_KILL, M_HELP };

static struct {
	SessInfo *all;
	int       nall;
	int      *vis;
	int       nvis, sel, scroll;
	char      filter[MAX_NAME + 1];
	char      input[MAX_NAME + 1];
	char      keep[64];
	int       mode;
	char      msg[256];
	int       msg_err;
	int64_t   msg_until;
	Buf       pv;
	char      pv_name[64];
	int       pv_rows, pv_cols;
	int64_t   pv_time, last_refresh;
	int       esc_armed; /* last key was a lone Esc: another one quits */
	int       rows, cols, curx, cury, curvis;
} M;

static Buf o;
static volatile sig_atomic_t m_winch;

static void on_winch(int sig)
{
	(void)sig;
	m_winch = 1;
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                  */
/* ------------------------------------------------------------------ */
static void at(int x, int y) { buf_printf(&o, "\033[%d;%dH", y + 1, x + 1); }

#define BOLD 1
#define FAINTA 2

static void sty(int fg, int bg, int attr)
{
	buf_puts(&o, "\033[0");
	if (attr & BOLD)
		buf_puts(&o, ";1");
	if (attr & FAINTA)
		buf_puts(&o, ";2");
	if (fg >= 0)
		buf_printf(&o, ";38;5;%d", fg);
	if (bg >= 0)
		buf_printf(&o, ";48;5;%d", bg);
	buf_puts(&o, "m");
}

static void pad(int n)
{
	while (n-- > 0)
		buf_add(&o, " ", 1);
}

static void rep(const char *s, int n)
{
	while (n-- > 0)
		buf_puts(&o, s);
}

/* print at most maxw columns of s (ellipsised); returns columns used */
static int put(const char *s, int maxw)
{
	if (maxw <= 0)
		return 0;
	int w = strwidth(s);
	if (w <= maxw) {
		buf_puts(&o, s);
		return w;
	}
	const unsigned char *p = (const unsigned char *)s;
	size_t n = strlen(s);
	int cw = 0;
	while (n) {
		uint32_t cp;
		int k = utf8_dec(p, n, &cp);
		if (k <= 0)
			break;
		int x = cwidth(cp);
		if (x < 0)
			x = 1;
		if (cw + x > maxw - 1)
			break;
		buf_add(&o, p, (size_t)k);
		cw += x;
		p += k;
		n -= (size_t)k;
	}
	buf_puts(&o, "…");
	return cw + 1;
}

static void flash(int err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void flash(int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(M.msg, sizeof M.msg, fmt, ap);
	va_end(ap);
	M.msg_err = err;
	M.msg_until = now_ms() + 3000;
}

/* ------------------------------------------------------------------ */
/* data                                                                */
/* ------------------------------------------------------------------ */
static SessInfo *selected(void)
{
	if (!M.nvis || M.sel < 0 || M.sel >= M.nvis)
		return NULL;
	return &M.all[M.vis[M.sel]];
}

static void remember(void)
{
	SessInfo *s = selected();
	if (s)
		snprintf(M.keep, sizeof M.keep, "%s", s->name);
}

static void apply_filter(void)
{
	free(M.vis);
	M.vis = malloc(sizeof(int) * (size_t)(M.nall ? M.nall : 1));
	if (!M.vis)
		abort();
	M.nvis = 0;
	int want = -1;
	for (int i = 0; i < M.nall; i++) {
		if (M.filter[0] && !strcasestr(M.all[i].name, M.filter))
			continue;
		if (!strcmp(M.all[i].name, M.keep))
			want = M.nvis;
		M.vis[M.nvis++] = i;
	}
	if (want >= 0)
		M.sel = want;
	if (M.sel >= M.nvis)
		M.sel = M.nvis - 1;
	if (M.sel < 0)
		M.sel = 0;
}

static void refresh(void)
{
	remember();
	free(M.all);
	M.all = NULL;
	M.nall = session_list(&M.all);
	apply_filter();
	M.last_refresh = now_ms();
}

static void move_sel(int d)
{
	if (!M.nvis)
		return;
	M.sel += d;
	if (M.sel < 0)
		M.sel = 0;
	if (M.sel >= M.nvis)
		M.sel = M.nvis - 1;
	remember();
}

static void fetch_preview(const char *name, int rows, int cols)
{
	int64_t now = now_ms();
	if (!strcmp(M.pv_name, name) && M.pv_rows == rows && M.pv_cols == cols &&
	    now - M.pv_time < 900)
		return;
	uint32_t sz[2] = { (uint32_t)rows, (uint32_t)cols };
	buf_reset(&M.pv);
	if (session_request(name, MSG_PREVIEW, sz, sizeof sz, &M.pv) < 0)
		buf_reset(&M.pv);
	snprintf(M.pv_name, sizeof M.pv_name, "%s", name);
	M.pv_rows = rows;
	M.pv_cols = cols;
	M.pv_time = now;
}

/* ------------------------------------------------------------------ */
/* screen                                                              */
/* ------------------------------------------------------------------ */
static void draw_header(int W)
{
	at(0, 0);
	sty(DARK, ACC, BOLD);
	int col = put(" ◆ gtm ", W);
	sty(MUTED, BAR, 0);
	col += put("  session manager", W - col);

	char right[96], clock[16];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(clock, sizeof clock, "%H:%M", &tm);
	snprintf(right, sizeof right, "%d session%s  ·  %s ", M.nall, M.nall == 1 ? "" : "s", clock);
	int rw = strwidth(right);
	if (col + rw + 2 <= W) {
		pad(W - col - rw);
		sty(DIM, BAR, 0);
		buf_puts(&o, right);
	} else {
		pad(W - col);
	}
}

static void draw_empty(int x0, int y0, int w, int h)
{
	static const char *logo[] = {
		"╭───┬───╮",
		"│   │   │",
		"├───┴───┤",
		"│       │",
		"╰───────╯",
	};
	const char *t1 = M.nall ? "no matches" : "no sessions yet";
	const char *t2 = M.nall ? "esc clears the filter" : "press  n  to start one";
	int block = 5 + 3;
	int top = y0 + (h - block) / 2;
	for (int r = 0; r < h; r++) {
		int y = y0 + r;
		at(x0, y);
		sty(-1, -1, 0);
		int k = y - top;
		const char *s = NULL;
		int fg = ACC, attr = 0;
		if (!M.nall && k >= 0 && k < 5) {
			s = logo[k];
			fg = k < 2 ? ACC : SKY;
		} else if (k == (M.nall ? 3 : 6)) {
			s = t1;
			fg = TEXT;
			attr = BOLD;
		} else if (k == (M.nall ? 4 : 7)) {
			s = t2;
			fg = DIM;
		}
		if (!s) {
			pad(w);
			continue;
		}
		int sw = strwidth(s);
		int lp = (w - sw) / 2;
		if (lp < 0)
			lp = 0;
		pad(lp);
		sty(fg, -1, attr);
		int used = put(s, w - lp);
		sty(-1, -1, 0);
		pad(w - lp - used);
	}
}

static void draw_list(int x0, int y0, int w, int h)
{
	at(x0, y0);
	sty(DIM, -1, BOLD);
	int col = put("  SESSIONS", w);
	if (M.nall) {
		char cnt[32];
		snprintf(cnt, sizeof cnt, "  %d", M.nvis);
		sty(FAINT, -1, 0);
		col += put(cnt, w - col);
	}
	if (M.filter[0] || M.mode == M_FILTER) {
		sty(ACC, -1, BOLD);
		col += put("  /", w - col);
		sty(BRIGHT, -1, 0);
		col += put(M.filter, w - col);
		if (M.mode == M_FILTER) {
			M.curx = x0 + col;
			M.cury = y0;
			M.curvis = 1;
		}
	}
	sty(-1, -1, 0);
	pad(w - col);
	at(x0, y0 + 1);
	pad(w);

	int lh = h - 2, ly = y0 + 2;
	if (lh <= 0)
		return;
	if (!M.nvis) {
		draw_empty(x0, ly, w, lh);
		return;
	}
	if (M.sel < M.scroll)
		M.scroll = M.sel;
	if (M.sel >= M.scroll + lh)
		M.scroll = M.sel - lh + 1;
	if (M.scroll > 0 && M.scroll + lh > M.nvis)
		M.scroll = M.nvis - lh > 0 ? M.nvis - lh : 0;

	for (int i = 0; i < lh; i++) {
		int idx = M.scroll + i;
		at(x0, ly + i);
		if (idx >= M.nvis) {
			sty(-1, -1, 0);
			pad(w);
			continue;
		}
		SessInfo *s = &M.all[M.vis[idx]];
		int on = idx == M.sel;
		int bg = on ? SEL : -1;
		sty(ACC, bg, 0);
		buf_puts(&o, on ? "▌" : " ");
		sty(s->nclients ? GREEN : FAINT, bg, 0);
		buf_puts(&o, s->nclients ? " ● " : " ○ ");
		int c = 4;
		char meta[64], age[16];
		snprintf(meta, sizeof meta, "%d win · %s ", s->nwin, fmt_age(s->created, age, sizeof age));
		int mw = strwidth(meta);
		int nw = w - c - mw - 1;
		if (nw < 6) {
			mw = 0;
			nw = w - c - 1;
		}
		sty(on ? BRIGHT : TEXT, bg, on ? BOLD : 0);
		int k = put(s->name, nw);
		pad(nw - k + 1);
		if (mw) {
			sty(on ? MUTED : DIM, bg, 0);
			buf_puts(&o, meta);
		}
	}
	/* scroll hint */
	if (M.nvis > lh) {
		char sc[32];
		snprintf(sc, sizeof sc, " %d/%d ", M.sel + 1, M.nvis);
		int sw = strwidth(sc);
		if (sw < w) {
			at(x0 + w - sw, y0);
			sty(FAINT, -1, 0);
			buf_puts(&o, sc);
		}
	}
}

static void draw_preview(int x0, int y0, int w, int h)
{
	SessInfo *s = selected();
	int y = y0;
	if (!s || w < 10) {
		for (; y < y0 + h; y++) {
			at(x0, y);
			sty(-1, -1, 0);
			pad(w);
		}
		return;
	}

	/* title */
	at(x0, y);
	sty(ACC, -1, BOLD);
	int col = put(s->name, w);
	sty(-1, -1, 0);
	col += put("  ", w - col);
	char chip[48];
	if (s->nclients) {
		if (s->nclients > 1)
			snprintf(chip, sizeof chip, " attached ×%d ", s->nclients);
		else
			snprintf(chip, sizeof chip, " attached ");
		sty(DARK, GREEN, BOLD);
	} else {
		snprintf(chip, sizeof chip, " detached ");
		sty(TEXT, SEL, 0);
	}
	col += put(chip, w - col);
	char info[128], age[16];
	snprintf(info, sizeof info, "   %d pane%s · up %s · pid %d", s->npanes,
	         s->npanes == 1 ? "" : "s", fmt_age(s->created, age, sizeof age), s->pid);
	sty(DIM, -1, 0);
	col += put(info, w - col);
	sty(-1, -1, 0);
	pad(w - col);
	y++;

	at(x0, y++);
	pad(w);

	/* window tabs */
	at(x0, y);
	col = 0;
	for (int i = 0; i < s->nw && col < w; i++) {
		char lab[80];
		snprintf(lab, sizeof lab, " %d %s%s ", i + 1, s->wname[i],
		         s->wpanes[i] > 1 ? ((s->wflags[i] & 2) ? " ▣" : " ◫") : "");
		int lw = strwidth(lab);
		if (col + lw > w) {
			sty(DIM, -1, 0);
			col += put("…", w - col);
			break;
		}
		if (i == s->cur)
			sty(DARK, ACC, BOLD);
		else if (s->wflags[i] & 1)
			sty(DARK, RED, 0);
		else
			sty(TEXT, SEL, 0);
		col += put(lab, w - col);
		sty(-1, -1, 0);
		if (col < w) {
			buf_add(&o, " ", 1);
			col++;
		}
	}
	sty(-1, -1, 0);
	pad(w - col);
	y++;
	at(x0, y++);
	pad(w);

	/* live preview box */
	int bh = y0 + h - y;
	if (bh < 3) {
		for (; y < y0 + h; y++) {
			at(x0, y);
			pad(w);
		}
		return;
	}
	int iw = w - 4, ih = bh - 2;
	fetch_preview(s->name, ih, iw);

	at(x0, y++);
	sty(BORDER, -1, 0);
	buf_puts(&o, "╭─");
	sty(DIM, -1, 0);
	int tw = put(" live preview ", w - 3);
	sty(BORDER, -1, 0);
	rep("─", w - 3 - tw);
	buf_puts(&o, "╮");

	const char *p = M.pv.d, *end = M.pv.d ? M.pv.d + M.pv.len : NULL;
	for (int r = 0; r < ih; r++) {
		at(x0, y++);
		sty(BORDER, -1, 0);
		buf_puts(&o, "│ ");
		const char *nl = p ? memchr(p, '\n', (size_t)(end - p)) : NULL;
		if (nl) {
			buf_add(&o, p, (size_t)(nl - p));
			p = nl + 1;
		} else {
			sty(-1, -1, 0);
			if (r == ih / 2 && !M.pv.len) {
				const char *t = "(preview unavailable)";
				int lp = (iw - strwidth(t)) / 2;
				pad(lp);
				sty(FAINT, -1, 0);
				int u = put(t, iw - lp);
				pad(iw - lp - u);
			} else {
				pad(iw);
			}
			p = NULL;
		}
		sty(BORDER, -1, 0);
		buf_puts(&o, " │");
	}
	at(x0, y++);
	sty(BORDER, -1, 0);
	buf_puts(&o, "╰");
	rep("─", w - 2);
	buf_puts(&o, "╯");
}

static void draw_promptline(int W, int y)
{
	at(0, y);
	int col = 0;
	SessInfo *s = selected();
	switch (M.mode) {
	case M_NEW: case M_NEW_DET: case M_RENAME:
		sty(DARK, PEACH, BOLD);
		col += put(M.mode == M_RENAME ? " rename " : M.mode == M_NEW ? " new session " :
		           " new (detached) ", W);
		sty(-1, -1, 0);
		col += put(" ", W - col);
		sty(BRIGHT, -1, BOLD);
		col += put(M.input, W - col);
		M.curx = col;
		M.cury = y;
		M.curvis = 1;
		sty(FAINT, -1, 0);
		col += put(M.mode == M_RENAME ? "    ⏎ rename · esc cancel"
		                              : "    ⏎ create · esc cancel · empty = auto name", W - col);
		break;
	case M_KILL:
		sty(DARK, RED, BOLD);
		col += put(" kill ", W);
		sty(TEXT, -1, 0);
		{
			char q[128];
			snprintf(q, sizeof q, "  end session '%s' and everything in it?  ", s ? s->name : "?");
			col += put(q, W - col);
		}
		sty(RED, -1, BOLD);
		col += put("y", W - col);
		sty(DIM, -1, 0);
		col += put(" / ", W - col);
		sty(TEXT, -1, BOLD);
		col += put("n", W - col);
		break;
	case M_FILTER:
		sty(FAINT, -1, 0);
		col += put("  type to filter · ↑↓ move · ⏎ keep · esc clear", W);
		break;
	default:
		if (M.msg[0] && now_ms() < M.msg_until) {
			sty(M.msg_err ? RED : GREEN, -1, 0);
			col += put(M.msg_err ? "  ✗ " : "  ● ", W);
			sty(M.msg_err ? RED : TEXT, -1, 0);
			col += put(M.msg, W - col);
		}
	}
	sty(-1, -1, 0);
	pad(W - col);
}

static void draw_footer(int W, int y)
{
	static const char *keys[][2] = {
		{ "⏎", "attach" }, { "n", "new" }, { "r", "rename" }, { "x", "kill" },
		{ "/", "filter" }, { "j k", "move" }, { "?", "help" }, { "q", "quit" },
	};
	at(0, y);
	sty(-1, BAR, 0);
	int col = 0;
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		char lab[32];
		snprintf(lab, sizeof lab, " %s ", keys[i][1]);
		int need = 1 + strwidth(keys[i][0]) + strwidth(lab) + 1;
		if (col + need > W)
			break;
		sty(ACC, BAR, BOLD);
		buf_puts(&o, " ");
		buf_puts(&o, keys[i][0]);
		sty(MUTED, BAR, 0);
		buf_puts(&o, lab);
		buf_puts(&o, " ");
		col += need;
	}
	const char *ver = "gtm " GTM_VERSION " ";
	int vw = strwidth(ver);
	if (col + vw <= W) {
		pad(W - col - vw);
		sty(FAINT, BAR, 0);
		buf_puts(&o, ver);
	} else {
		pad(W - col);
	}
}

static void draw_help(int W, int H)
{
	static const char *keys[][2] = {
		{ "j k  ↑ ↓", "move" },
		{ "g G  Home End", "first / last session" },
		{ "PgUp PgDn", "page up / down" },
		{ "⏎  l  →", "attach to selected" },
		{ "n  C-t", "new session and attach" },
		{ "N", "new session, stay here" },
		{ "r", "rename selected" },
		{ "x  C-w", "kill selected (asks y/n)" },
		{ "/  C-f", "filter · esc clears" },
		{ "R  C-l", "refresh" },
		{ "?", "toggle this help" },
		{ "q  C-c  Esc Esc", "quit" },
	};
	int nk = (int)(sizeof keys / sizeof *keys);
	int bw = 58, bh = nk + 7;
	if (bw > W)
		bw = W;
	if (bh > H)
		bh = H;
	int x0 = (W - bw) / 2, y0 = (H - bh) / 2, in = bw - 2;
	const char *pn = prefix_name(prefix_key());

	for (int j = 0; j < bh; j++) {
		at(x0, y0 + j);
		sty(ACC, PANEL, 0);
		if (j == 0 || j == bh - 1) {
			buf_puts(&o, j ? "╰" : "╭");
			rep("─", in);
			buf_puts(&o, j ? "╯" : "╮");
		} else {
			buf_puts(&o, "│");
			pad(in);
			buf_puts(&o, "│");
		}
	}
	const char *title = " gtm · session manager keys ";
	at(x0 + (bw - strwidth(title)) / 2, y0);
	sty(DARK, ACC, BOLD);
	put(title, in);
	const char *foot = " press any key ";
	at(x0 + (bw - strwidth(foot)) / 2, y0 + bh - 1);
	sty(MUTED, PANEL, 0);
	put(foot, in);

	for (int i = 0; i < nk && y0 + 2 + i < y0 + bh - 1; i++) {
		at(x0 + 3, y0 + 2 + i);
		sty(ACC, PANEL, BOLD);
		put(keys[i][0], 15);
		at(x0 + 3 + 16, y0 + 2 + i);
		sty(TEXT, PANEL, 0);
		put(keys[i][1], in - 18);
	}
	int y = y0 + 3 + nk;
	if (y < y0 + bh - 2) {
		char line[128];
		at(x0 + 3, y);
		sty(YELLOW, PANEL, BOLD);
		int col = put("in a session ", in - 4);
		sty(MUTED, PANEL, 0);
		snprintf(line, sizeof line, "press %s, then", pn);
		put(line, in - 4 - col);
		at(x0 + 3, y + 1);
		sty(TEXT, PANEL, 0);
		put("d detach · s back to this manager · ? all keys", in - 4);
	}
}

static void draw(void)
{
	term_size(1, &M.rows, &M.cols);
	int W = M.cols, H = M.rows;
	buf_reset(&o);
	buf_puts(&o, "\033[?2026h\033[?25l");
	M.curvis = 0;

	if (W < 40 || H < 10) {
		buf_puts(&o, "\033[0m\033[2J");
		const char *t = "terminal too small";
		at((W - (int)strlen(t)) / 2 > 0 ? (W - (int)strlen(t)) / 2 : 0, H / 2);
		sty(DIM, -1, 0);
		put(t, W);
	} else {
		draw_header(W);
		at(0, 1);
		sty(-1, -1, 0);
		pad(W);

		int top = 2, bh = H - 4;
		int lw = W;
		if (W >= 72) {
			lw = W * 34 / 100;
			if (lw < 28)
				lw = 28;
			if (lw > 44)
				lw = 44;
		}
		draw_list(0, top, lw, bh);
		if (lw < W) {
			for (int y = top; y < top + bh; y++) {
				at(lw, y);
				sty(BORDER, -1, 0);
				buf_puts(&o, "│");
				sty(-1, -1, 0);
				buf_puts(&o, " ");
			}
			int pw = W - lw - 3;
			draw_preview(lw + 2, top, pw, bh);
			for (int y = top; y < top + bh; y++) {
				at(W - 1, y);
				sty(-1, -1, 0);
				buf_puts(&o, " ");
			}
		}
		draw_promptline(W, H - 2);
		draw_footer(W, H - 1);
		if (M.mode == M_HELP)
			draw_help(W, H);
	}
	sty(-1, -1, 0);
	if (M.curvis)
		buf_printf(&o, "\033[%d;%dH\033[5 q\033[?25h", M.cury + 1, M.curx + 1);
	buf_puts(&o, "\033[?2026l");
	write_all(1, o.d, o.len);
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
/* ------------------------------------------------------------------ */
static void do_attach(const char *name)
{
	char nm[64], err[256];
	snprintf(nm, sizeof nm, "%s", name);
	write_str(1, "\033[0m\033[0 q\033[H\033[2J");
	int r = client_attach(nm, 1, err, sizeof err);
	write_str(1, "\033[0m\033[?25l\033[H\033[2J");
	if (r < 0)
		flash(1, "%s", err);
	else if (r == EXIT_ENDED)
		flash(0, "session '%s' ended", nm);
	else if (r == EXIT_KILLED)
		flash(0, "session '%s' was killed", nm);
	else
		flash(0, "detached from '%s'", nm);
	snprintf(M.keep, sizeof M.keep, "%s", nm);
	M.pv_name[0] = 0;
	refresh();
}

static void do_new(int attach)
{
	char name[64], err[256];
	if (M.input[0])
		snprintf(name, sizeof name, "%s", M.input);
	else
		session_autoname(name, sizeof name);
	int rows, cols;
	term_size(1, &rows, &cols);
	if (server_start(name, rows, cols, err, sizeof err) < 0) {
		flash(1, "%s", err);
		return;
	}
	snprintf(M.keep, sizeof M.keep, "%s", name);
	if (M.filter[0] && !strcasestr(name, M.filter))
		M.filter[0] = 0;
	refresh();
	if (attach)
		do_attach(name);
	else
		flash(0, "created session '%s'", name);
}

static void do_kill(void)
{
	SessInfo *s = selected();
	if (!s)
		return;
	char nm[64];
	snprintf(nm, sizeof nm, "%s", s->name);
	if (session_kill(nm) < 0)
		flash(1, "could not kill '%s'", nm);
	else
		flash(0, "killed session '%s'", nm);
	/* give the server a moment to remove its socket */
	for (int i = 0; i < 20 && session_alive(nm); i++)
		usleep(10000);
	refresh();
}

static void do_rename(void)
{
	SessInfo *s = selected();
	if (!s || !M.input[0])
		return;
	char from[64], err[256];
	snprintf(from, sizeof from, "%s", s->name);
	if (!strcmp(from, M.input))
		return;
	if (session_rename(from, M.input, err, sizeof err) < 0) {
		flash(1, "%s", err);
		return;
	}
	flash(0, "renamed '%s' → '%s'", from, M.input);
	snprintf(M.keep, sizeof M.keep, "%s", M.input);
	M.pv_name[0] = 0;
	refresh();
}

static int edit_key(int key)
{
	int filter = M.mode == M_FILTER;
	char *buf = filter ? M.filter : M.input;
	size_t len = strlen(buf);

	if (key == 27 || key == 3 || key == 7) {
		if (filter) {
			M.filter[0] = 0;
			apply_filter();
		}
		M.mode = M_NORMAL;
		return 0;
	}
	if (key == '\r' || key == '\n') {
		int mode = M.mode;
		M.mode = M_NORMAL;
		if (mode == M_NEW)
			do_new(1);
		else if (mode == M_NEW_DET)
			do_new(0);
		else if (mode == M_RENAME)
			do_rename();
		return 0;
	}
	if (filter && (key == K_UP || key == K_DOWN)) {
		move_sel(key == K_UP ? -1 : 1);
		return 0;
	}
	if (key == 0x7f || key == 8) {
		if (len)
			buf[len - 1] = 0;
	} else if (key == 0x15) {
		buf[0] = 0;
	} else if (key == 0x17) { /* C-w: delete word */
		while (len && buf[len - 1] == ' ')
			len--;
		while (len && buf[len - 1] != ' ')
			len--;
		buf[len] = 0;
	} else if (key >= 0x20 && key < 0x7f) {
		int ok = filter || (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
		         (key >= '0' && key <= '9') || key == '-' || key == '_' || key == '.';
		if (ok && len < MAX_NAME) {
			buf[len] = (char)key;
			buf[len + 1] = 0;
		}
	}
	if (filter)
		apply_filter();
	return 0;
}

static int on_key(int key)
{
	if (M.mode == M_KILL) {
		M.mode = M_NORMAL;
		if (key == 'y' || key == 'Y')
			do_kill();
		return 0;
	}
	if (M.mode == M_HELP) {
		M.mode = M_NORMAL;
		return 0;
	}
	if (M.mode != M_NORMAL)
		return edit_key(key);

	SessInfo *s = selected();
	int page = M.rows - 6 > 1 ? M.rows - 6 : 1;
	int esc_prev = M.esc_armed;
	M.esc_armed = 0;
	switch (key) {
	case 'q': case 'Q': case 3: case 0x11:
		return 1;
	case 'j': case K_DOWN: case 0x0e:
		move_sel(1);
		break;
	case 'k': case K_UP: case 0x10:
		move_sel(-1);
		break;
	case 'g': case K_HOME:
		move_sel(-M.nvis);
		break;
	case 'G': case K_END:
		move_sel(M.nvis);
		break;
	case K_PGDN: case 0x04:
		move_sel(page);
		break;
	case K_PGUP: case 0x15:
		move_sel(-page);
		break;
	case '\r': case 'l': case 'a': case K_RIGHT:
		if (s)
			do_attach(s->name);
		else if (key == '\r') {
			M.input[0] = 0;
			M.mode = M_NEW;
		}
		break;
	case 'n': case 'c': case 0x14: /* C-t */
		M.input[0] = 0;
		M.mode = M_NEW;
		break;
	case 'N':
		M.input[0] = 0;
		M.mode = M_NEW_DET;
		break;
	case 'r':
		if (s) {
			snprintf(M.input, sizeof M.input, "%s", s->name);
			M.mode = M_RENAME;
		}
		break;
	case 'x': case 'd': case 'D': case K_DEL: case 0x17: /* C-w */
		if (s)
			M.mode = M_KILL;
		break;
	case '/': case 0x06: /* C-f */
		M.mode = M_FILTER;
		break;
	case 27:
		if (M.filter[0]) {
			M.filter[0] = 0;
			apply_filter();
		} else {
			if (esc_prev)
				return 1;
			M.esc_armed = 1;
		}
		break;
	case '?':
		M.mode = M_HELP;
		break;
	case 'R': case 0x0c:
		write_str(1, "\033[2J");
		M.pv_name[0] = 0;
		refresh();
		break;
	}
	return 0;
}

void manager_focus(const char *name)
{
	snprintf(M.keep, sizeof M.keep, "%s", name);
	flash(0, "detached from '%s'", name);
}

int manager_run(void)
{
	if (!isatty(0) || !isatty(1)) {
		fprintf(stderr, "gtm: not a terminal\n");
		return 1;
	}
	struct termios saved;
	if (term_raw(0, &saved) < 0) {
		fprintf(stderr, "gtm: cannot set raw mode: %s\n", strerror(errno));
		return 1;
	}
	struct sigaction sa = { .sa_handler = on_winch }, old;
	sigaction(SIGWINCH, &sa, &old);
	write_str(1, "\033[?1049h\033[?25l\033[H\033[2J");

	refresh();
	for (int quit = 0, help_up = 0; !quit;) {
		int64_t now = now_ms();
		if (now - M.last_refresh >= 1000)
			refresh();
		int winched = 0;
		if (m_winch) {
			m_winch = 0;
			winched = 1;
			write_str(1, "\033[0m\033[2J");
		}
		/* The help overlay is static: repainting the list/preview under it
		 * on every refresh tick makes the box flicker, so paint it once. */
		if (M.mode != M_HELP || !help_up || winched)
			draw();
		help_up = M.mode == M_HELP;
		now = now_ms();
		int timeout = 1000 - (int)(now - M.last_refresh);
		if (timeout < 0)
			timeout = 0;
		if (M.msg[0] && M.msg_until > now && M.msg_until - now < timeout)
			timeout = (int)(M.msg_until - now) + 5;
		struct pollfd pf = { 0, POLLIN, 0 };
		int r = poll(&pf, 1, timeout);
		if (r <= 0)
			continue;
		unsigned char b[512];
		ssize_t n = read(0, b, sizeof b);
		if (n <= 0) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			break;
		}
		for (size_t i = 0; i < (size_t)n && !quit;) {
			int key;
			int k = parse_key(b + i, (size_t)n - i, &key);
			i += (size_t)(k > 0 ? k : 1);
			if (key != K_NONE && on_key(key))
				quit = 1;
		}
	}
	write_str(1, "\033[0m\033[0 q\033[?25h\033[?1049l");
	term_restore(0, &saved);
	sigaction(SIGWINCH, &old, NULL);
	buf_free(&o);
	buf_free(&M.pv);
	free(M.all);
	free(M.vis);
	return 0;
}
