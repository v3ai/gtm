/*
 * gtm - virtual terminal emulator (a pragmatic xterm/screen subset)
 */
#include "gtm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S_GROUND, S_ESC, S_ESCI, S_CSI, S_OSC, S_OSCESC, S_STR, S_STRESC };

#define MAXPAR ((int)(sizeof(((VT *)0)->par) / sizeof(int)))

/* DEC special graphics, 0x5f .. 0x7e */
static const uint16_t decgfx[32] = {
	0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
	0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
	0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
	0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

static const Cell dcell = { ' ', COL_DEFAULT, COL_DEFAULT, 0 };

static Cell ecell(const VT *v)
{
	Cell c = { ' ', COL_DEFAULT, v->pen.bg, 0 };
	return c;
}

static Cell *line_new(int cols, Cell b)
{
	Cell *l = malloc(sizeof(Cell) * (size_t)cols);
	if (!l)
		abort();
	for (int i = 0; i < cols; i++)
		l[i] = b;
	return l;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------------ */
/* scrollback                                                          */
/* ------------------------------------------------------------------ */
static void sb_push(VT *v, Cell *l, int n)
{
	if (v->sbcap <= 0) {
		free(l);
		return;
	}
	while (n > 0 && l[n - 1].ch == ' ' && l[n - 1].bg == COL_DEFAULT &&
	       !(l[n - 1].attr & (A_REVERSE | A_UNDER | A_STRIKE)))
		n--;
	Cell *c = NULL;
	if (n > 0) {
		c = realloc(l, sizeof(Cell) * (size_t)n);
		if (!c)
			c = l;
	} else {
		free(l);
	}
	SbLine *s;
	if (v->sblen < v->sbcap) {
		s = &v->sb[(v->sbhead + v->sblen) % v->sbcap];
		v->sblen++;
	} else {
		s = &v->sb[v->sbhead];
		free(s->c);
		v->sbhead = (v->sbhead + 1) % v->sbcap;
	}
	s->c = c;
	s->n = n;
	v->sbtotal++;
}

static void sb_pop(VT *v, Cell *dst, int cols)
{
	SbLine *s = &v->sb[(v->sbhead + v->sblen - 1) % v->sbcap];
	for (int i = 0; i < cols; i++)
		dst[i] = dcell;
	int n = s->n < cols ? s->n : cols;
	if (n)
		memcpy(dst, s->c, sizeof(Cell) * (size_t)n);
	if (cols > 0 && (dst[cols - 1].attr & A_WIDE)) {
		dst[cols - 1].ch = ' ';
		dst[cols - 1].attr &= ~A_WIDE;
	}
	free(s->c);
	s->c = NULL;
	s->n = 0;
	v->sblen--;
	v->sbtotal--;
}

static void sb_clear(VT *v)
{
	for (int i = 0; i < v->sblen; i++) {
		SbLine *s = &v->sb[(v->sbhead + i) % v->sbcap];
		free(s->c);
		s->c = NULL;
		s->n = 0;
	}
	v->sblen = 0;
	v->sbhead = 0;
}

/* global line access: scrollback lines then the visible screen */
const Cell *vt_gline(const VT *v, int64_t g, int *n)
{
	int64_t first = v->sbtotal - v->sblen;
	*n = 0;
	if (g < first)
		return NULL;
	if (g < v->sbtotal) {
		const SbLine *s = &v->sb[(v->sbhead + (int)(g - first)) % v->sbcap];
		*n = s->n;
		return s->c;
	}
	int64_t r = g - v->sbtotal;
	if (r < v->rows) {
		*n = v->cols;
		return v->scr[r];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* init / free / reset                                                 */
/* ------------------------------------------------------------------ */
static void tabs_default(VT *v, int from)
{
	for (int i = from; i < v->cols; i++)
		v->tabs[i] = (i % 8 == 0) && i;
}

static void reset_state(VT *v)
{
	v->pen = dcell;
	v->cx = v->cy = 0;
	v->wrapnext = 0;
	v->top = 0;
	v->bot = v->rows - 1;
	v->m_wrap = 1;
	v->m_origin = v->m_insert = v->m_appcursor = v->m_appkeypad = 0;
	v->m_cursor = 1;
	v->m_paste = v->m_lnm = 0;
	v->curstyle = 0;
	v->cs[0] = v->cs[1] = 0;
	v->gl = 0;
	memset(v->save, 0, sizeof v->save);
	tabs_default(v, 0);
}

void vt_init(VT *v, int rows, int cols, int sbcap)
{
	memset(v, 0, sizeof *v);
	v->rows = rows;
	v->cols = cols;
	v->lines = calloc((size_t)rows, sizeof(Cell *));
	v->altlines = calloc((size_t)rows, sizeof(Cell *));
	if (!v->lines || !v->altlines)
		abort();
	for (int r = 0; r < rows; r++) {
		v->lines[r] = line_new(cols, dcell);
		v->altlines[r] = line_new(cols, dcell);
	}
	v->scr = v->lines;
	v->tabs = calloc((size_t)cols, 1);
	v->sbcap = sbcap;
	v->sb = sbcap > 0 ? calloc((size_t)sbcap, sizeof(SbLine)) : NULL;
	reset_state(v);
	v->dirty = 1;
}

void vt_free(VT *v)
{
	for (int r = 0; r < v->rows; r++) {
		free(v->lines[r]);
		free(v->altlines[r]);
	}
	free(v->lines);
	free(v->altlines);
	if (v->sb) {
		sb_clear(v);
		free(v->sb);
	}
	free(v->tabs);
	buf_free(&v->reply);
}

static void fill_screen(VT *v, Cell **ls, Cell b)
{
	for (int r = 0; r < v->rows; r++)
		for (int c = 0; c < v->cols; c++)
			ls[r][c] = b;
}

static void full_reset(VT *v)
{
	reset_state(v);
	fill_screen(v, v->lines, dcell);
	fill_screen(v, v->altlines, dcell);
	v->alt = 0;
	v->scr = v->lines;
	v->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */
static void reply(VT *v, const char *s) { buf_puts(&v->reply, s); }

/* we're about to overwrite cell x: break any wide glyph that spans it */
static void fixwide(VT *v, Cell *l, int x)
{
	if (x < 0 || x >= v->cols)
		return;
	if ((l[x].attr & A_WCONT) && x > 0) {
		l[x - 1].ch = ' ';
		l[x - 1].attr &= ~A_WIDE;
	}
	if ((l[x].attr & A_WIDE) && x + 1 < v->cols) {
		l[x + 1].ch = ' ';
		l[x + 1].attr &= ~A_WCONT;
	}
}

static void erase(VT *v, int y, int x0, int x1)
{
	if (y < 0 || y >= v->rows)
		return;
	if (x0 < 0)
		x0 = 0;
	if (x1 > v->cols)
		x1 = v->cols;
	if (x0 >= x1)
		return;
	Cell *l = v->scr[y];
	fixwide(v, l, x0);
	fixwide(v, l, x1 - 1);
	Cell b = ecell(v);
	for (int x = x0; x < x1; x++)
		l[x] = b;
	v->dirty = 1;
}

static void scroll_up(VT *v, int top, int bot, int n, int save)
{
	int h = bot - top + 1;
	if (n > h)
		n = h;
	if (n <= 0)
		return;
	Cell b = ecell(v);
	for (int i = 0; i < n; i++) {
		Cell *l = v->scr[top];
		memmove(&v->scr[top], &v->scr[top + 1], sizeof(Cell *) * (size_t)(h - 1));
		if (save && top == 0 && !v->alt) {
			sb_push(v, l, v->cols);
			l = line_new(v->cols, b);
		} else {
			for (int c = 0; c < v->cols; c++)
				l[c] = b;
		}
		v->scr[bot] = l;
	}
	v->dirty = 1;
}

static void scroll_down(VT *v, int top, int bot, int n)
{
	int h = bot - top + 1;
	if (n > h)
		n = h;
	if (n <= 0)
		return;
	Cell b = ecell(v);
	for (int i = 0; i < n; i++) {
		Cell *l = v->scr[bot];
		memmove(&v->scr[top + 1], &v->scr[top], sizeof(Cell *) * (size_t)(h - 1));
		for (int c = 0; c < v->cols; c++)
			l[c] = b;
		v->scr[top] = l;
	}
	v->dirty = 1;
}

static void lf(VT *v)
{
	if (v->cy == v->bot)
		scroll_up(v, v->top, v->bot, 1, 1);
	else if (v->cy < v->rows - 1)
		v->cy++;
	v->wrapnext = 0;
}

static void ri(VT *v)
{
	if (v->cy == v->top)
		scroll_down(v, v->top, v->bot, 1);
	else if (v->cy > 0)
		v->cy--;
	v->wrapnext = 0;
}

static void moveto(VT *v, int x, int y)
{
	int lo = 0, hi = v->rows - 1;
	if (v->m_origin) {
		y += v->top;
		lo = v->top;
		hi = v->bot;
	}
	v->cx = clampi(x, 0, v->cols - 1);
	v->cy = clampi(y, lo, hi);
	v->wrapnext = 0;
}

static void ins_chars(VT *v, int n)
{
	Cell *l = v->scr[v->cy];
	int x = v->cx;
	if (n > v->cols - x)
		n = v->cols - x;
	if (n <= 0)
		return;
	fixwide(v, l, x);
	memmove(&l[x + n], &l[x], sizeof(Cell) * (size_t)(v->cols - x - n));
	Cell b = ecell(v);
	for (int i = 0; i < n; i++)
		l[x + i] = b;
	if (l[v->cols - 1].attr & A_WIDE) {
		l[v->cols - 1].ch = ' ';
		l[v->cols - 1].attr &= ~A_WIDE;
	}
	v->dirty = 1;
}

static void del_chars(VT *v, int n)
{
	Cell *l = v->scr[v->cy];
	int x = v->cx;
	if (n > v->cols - x)
		n = v->cols - x;
	if (n <= 0)
		return;
	fixwide(v, l, x);
	fixwide(v, l, x + n - 1);
	memmove(&l[x], &l[x + n], sizeof(Cell) * (size_t)(v->cols - x - n));
	Cell b = ecell(v);
	for (int i = v->cols - n; i < v->cols; i++)
		l[i] = b;
	v->dirty = 1;
}

static void tab(VT *v, int n)
{
	int x = v->cx;
	if (n > 0) {
		while (n-- > 0 && x < v->cols - 1) {
			x++;
			while (x < v->cols - 1 && !v->tabs[x])
				x++;
		}
	} else {
		while (n++ < 0 && x > 0) {
			x--;
			while (x > 0 && !v->tabs[x])
				x--;
		}
	}
	v->cx = x;
	v->wrapnext = 0;
}

static void putch(VT *v, uint32_t c)
{
	if (c >= 0x5f && c <= 0x7e && v->cs[v->gl])
		c = decgfx[c - 0x5f];
	int w = cwidth(c);
	if (w == 0)
		return; /* combining marks etc. are dropped */
	if (w < 0)
		w = 1;
	if (w > v->cols)
		return;
	if (v->wrapnext && v->m_wrap) {
		v->cx = 0;
		lf(v);
	}
	v->wrapnext = 0;
	if (w == 2 && v->cx + 1 >= v->cols) {
		if (v->m_wrap) {
			erase(v, v->cy, v->cx, v->cols);
			v->cx = 0;
			lf(v);
		} else {
			v->cx = v->cols - 2;
		}
	}
	if (v->m_insert)
		ins_chars(v, w);
	Cell *l = v->scr[v->cy];
	fixwide(v, l, v->cx);
	if (w == 2)
		fixwide(v, l, v->cx + 1);
	Cell nc = v->pen;
	nc.ch = c;
	nc.attr &= (uint16_t)~(A_WIDE | A_WCONT);
	if (w == 2) {
		nc.attr |= A_WIDE;
		l[v->cx] = nc;
		nc.ch = 0;
		nc.attr = (uint16_t)((nc.attr & ~A_WIDE) | A_WCONT);
		l[v->cx + 1] = nc;
	} else {
		l[v->cx] = nc;
	}
	v->lastch = c;
	if (v->cx + w >= v->cols) {
		v->cx = v->cols - 1;
		v->wrapnext = 1;
	} else {
		v->cx += w;
	}
	v->dirty = 1;
}

static void save_cursor(VT *v)
{
	VTSave *s = &v->save[v->alt];
	s->cx = v->cx;
	s->cy = v->cy;
	s->pen = v->pen;
	s->origin = v->m_origin;
	s->wrapnext = v->wrapnext;
	s->cs0 = v->cs[0];
	s->cs1 = v->cs[1];
	s->gl = v->gl;
	s->valid = 1;
}

static void restore_cursor(VT *v)
{
	VTSave *s = &v->save[v->alt];
	if (!s->valid) {
		v->cx = v->cy = 0;
		v->pen = dcell;
		v->m_origin = 0;
		v->wrapnext = 0;
		return;
	}
	v->cx = clampi(s->cx, 0, v->cols - 1);
	v->cy = clampi(s->cy, 0, v->rows - 1);
	v->pen = s->pen;
	v->m_origin = s->origin;
	v->wrapnext = s->wrapnext;
	v->cs[0] = s->cs0;
	v->cs[1] = s->cs1;
	v->gl = s->gl;
}

static void alt_screen(VT *v, int on, int clear)
{
	if (on && clear)
		fill_screen(v, v->altlines, ecell(v));
	if (on == v->alt)
		return;
	v->alt = on;
	v->scr = on ? v->altlines : v->lines;
	v->dirty = 1;
}

static void soft_reset(VT *v)
{
	v->m_insert = v->m_origin = v->m_appcursor = v->m_appkeypad = 0;
	v->m_wrap = 1;
	v->m_cursor = 1;
	v->pen = dcell;
	v->top = 0;
	v->bot = v->rows - 1;
	v->cs[0] = v->cs[1] = 0;
	v->gl = 0;
	memset(v->save, 0, sizeof v->save);
}

/* ------------------------------------------------------------------ */
/* control characters / escape sequences                               */
/* ------------------------------------------------------------------ */
static void ctrl(VT *v, unsigned char c)
{
	switch (c) {
	case 7:
		v->bell = 1;
		break;
	case 8:
		if (v->cx > 0)
			v->cx--;
		v->wrapnext = 0;
		break;
	case 9:
		tab(v, 1);
		break;
	case 10: case 11: case 12:
		lf(v);
		if (v->m_lnm)
			v->cx = 0;
		break;
	case 13:
		v->cx = 0;
		v->wrapnext = 0;
		break;
	case 14:
		v->gl = 1;
		break;
	case 15:
		v->gl = 0;
		break;
	}
}

static void esc(VT *v, unsigned char c)
{
	switch (c) {
	case '[':
		v->state = S_CSI;
		return;
	case ']':
		v->state = S_OSC;
		v->osclen = 0;
		return;
	case 'P': case '_': case '^': case 'X':
		v->state = S_STR;
		return;
	case '(': case ')': case '*': case '+': case '#': case '%': case ' ':
		v->inter = c;
		v->state = S_ESCI;
		return;
	case '7': save_cursor(v); break;
	case '8': restore_cursor(v); break;
	case 'D': lf(v); break;
	case 'E': v->cx = 0; lf(v); break;
	case 'M': ri(v); break;
	case 'c': full_reset(v); break;
	case '=': v->m_appkeypad = 1; break;
	case '>': v->m_appkeypad = 0; break;
	case 'H': v->tabs[v->cx] = 1; break;
	}
	v->state = S_GROUND;
}

static void esci(VT *v, unsigned char c)
{
	switch (v->inter) {
	case '(': v->cs[0] = c == '0'; break;
	case ')': v->cs[1] = c == '0'; break;
	case '#':
		if (c == '8') { /* DECALN */
			Cell e = dcell;
			e.ch = 'E';
			fill_screen(v, v->scr, e);
			v->dirty = 1;
		}
		break;
	}
}

static int P(VT *v, int i, int d) { return (i < v->npar && v->par[i] > 0) ? v->par[i] : d; }

static uint32_t sgr_color(VT *v, int *i)
{
	int k = *i;
	if (k + 2 < v->npar && v->par[k + 1] == 5) {
		*i = k + 2;
		return COL_IDX(v->par[k + 2] & 255);
	}
	if (k + 4 < v->npar && v->par[k + 1] == 2) {
		*i = k + 4;
		return COL_RGB(v->par[k + 2] & 255, v->par[k + 3] & 255, v->par[k + 4] & 255);
	}
	*i = v->npar;
	return COL_DEFAULT;
}

static void sgr(VT *v)
{
	if (!v->npar) {
		v->pen = dcell;
		return;
	}
	for (int i = 0; i < v->npar; i++) {
		int p = v->par[i];
		Cell *pen = &v->pen;
		switch (p) {
		case 0: *pen = dcell; break;
		case 1: pen->attr |= A_BOLD; break;
		case 2: pen->attr |= A_DIM; break;
		case 3: pen->attr |= A_ITALIC; break;
		case 4: case 21: pen->attr |= A_UNDER; break;
		case 5: case 6: pen->attr |= A_BLINK; break;
		case 7: pen->attr |= A_REVERSE; break;
		case 8: pen->attr |= A_INVIS; break;
		case 9: pen->attr |= A_STRIKE; break;
		case 22: pen->attr &= (uint16_t)~(A_BOLD | A_DIM); break;
		case 23: pen->attr &= (uint16_t)~A_ITALIC; break;
		case 24: pen->attr &= (uint16_t)~A_UNDER; break;
		case 25: pen->attr &= (uint16_t)~A_BLINK; break;
		case 27: pen->attr &= (uint16_t)~A_REVERSE; break;
		case 28: pen->attr &= (uint16_t)~A_INVIS; break;
		case 29: pen->attr &= (uint16_t)~A_STRIKE; break;
		case 38: pen->fg = sgr_color(v, &i); break;
		case 39: pen->fg = COL_DEFAULT; break;
		case 48: pen->bg = sgr_color(v, &i); break;
		case 49: pen->bg = COL_DEFAULT; break;
		default:
			if (p >= 30 && p <= 37)
				pen->fg = COL_IDX(p - 30);
			else if (p >= 40 && p <= 47)
				pen->bg = COL_IDX(p - 40);
			else if (p >= 90 && p <= 97)
				pen->fg = COL_IDX(p - 90 + 8);
			else if (p >= 100 && p <= 107)
				pen->bg = COL_IDX(p - 100 + 8);
		}
	}
}

static void setmode(VT *v, int on)
{
	int n = v->npar ? v->npar : 1;
	for (int i = 0; i < n; i++) {
		int m = v->par[i];
		if (v->priv == '?') {
			switch (m) {
			case 1: v->m_appcursor = on; break;
			case 6: v->m_origin = on; moveto(v, 0, 0); break;
			case 7: v->m_wrap = on; break;
			case 25: v->m_cursor = on; break;
			case 47: alt_screen(v, on, 0); break;
			case 1047: alt_screen(v, on, on); break;
			case 1048: if (on) save_cursor(v); else restore_cursor(v); break;
			case 1049:
				if (on) {
					if (!v->alt)
						save_cursor(v);
					alt_screen(v, 1, 1);
				} else if (v->alt) {
					alt_screen(v, 0, 0);
					restore_cursor(v);
				}
				break;
			case 2004: v->m_paste = on; break;
			}
		} else {
			switch (m) {
			case 4: v->m_insert = on; break;
			case 20: v->m_lnm = on; break;
			}
		}
	}
	v->dirty = 1;
}

static void csi(VT *v, int f)
{
	int n = P(v, 0, 1);
	int p0 = v->npar ? v->par[0] : 0;

	if (v->priv == '>' || v->priv == '<' || v->priv == '=') {
		if (f == 'c' && v->priv == '>')
			reply(v, "\033[>84;0;0c");
		return;
	}
	if (v->inter) {
		if (v->inter == ' ' && f == 'q')
			v->curstyle = P(v, 0, 0);
		else if (v->inter == '!' && f == 'p')
			soft_reset(v);
		return;
	}
	if (v->priv == '?') {
		if (f == 'h' || f == 'l')
			setmode(v, f == 'h');
		return;
	}
	if (!strchr("mnchltq", f))
		v->wrapnext = 0;

	switch (f) {
	case '@':
		ins_chars(v, n);
		break;
	case 'A': {
		int lim = v->cy >= v->top ? v->top : 0;
		v->cy = v->cy - n < lim ? lim : v->cy - n;
		break;
	}
	case 'B': case 'e': {
		int lim = v->cy <= v->bot ? v->bot : v->rows - 1;
		v->cy = v->cy + n > lim ? lim : v->cy + n;
		break;
	}
	case 'C': case 'a':
		v->cx = clampi(v->cx + n, 0, v->cols - 1);
		break;
	case 'D':
		v->cx = clampi(v->cx - n, 0, v->cols - 1);
		break;
	case 'E': {
		int lim = v->cy <= v->bot ? v->bot : v->rows - 1;
		v->cy = v->cy + n > lim ? lim : v->cy + n;
		v->cx = 0;
		break;
	}
	case 'F': {
		int lim = v->cy >= v->top ? v->top : 0;
		v->cy = v->cy - n < lim ? lim : v->cy - n;
		v->cx = 0;
		break;
	}
	case 'G': case '`':
		v->cx = clampi(n - 1, 0, v->cols - 1);
		break;
	case 'H': case 'f':
		moveto(v, P(v, 1, 1) - 1, P(v, 0, 1) - 1);
		break;
	case 'I':
		tab(v, n);
		break;
	case 'Z':
		tab(v, -n);
		break;
	case 'J':
		if (p0 == 0) {
			erase(v, v->cy, v->cx, v->cols);
			for (int y = v->cy + 1; y < v->rows; y++)
				erase(v, y, 0, v->cols);
		} else if (p0 == 1) {
			for (int y = 0; y < v->cy; y++)
				erase(v, y, 0, v->cols);
			erase(v, v->cy, 0, v->cx + 1);
		} else if (p0 == 2) {
			for (int y = 0; y < v->rows; y++)
				erase(v, y, 0, v->cols);
		} else if (p0 == 3) {
			sb_clear(v);
			v->dirty = 1;
		}
		break;
	case 'K':
		if (p0 == 0)
			erase(v, v->cy, v->cx, v->cols);
		else if (p0 == 1)
			erase(v, v->cy, 0, v->cx + 1);
		else if (p0 == 2)
			erase(v, v->cy, 0, v->cols);
		break;
	case 'L':
		if (v->cy >= v->top && v->cy <= v->bot) {
			scroll_down(v, v->cy, v->bot, n);
			v->cx = 0;
		}
		break;
	case 'M':
		if (v->cy >= v->top && v->cy <= v->bot) {
			scroll_up(v, v->cy, v->bot, n, 0);
			v->cx = 0;
		}
		break;
	case 'P':
		del_chars(v, n);
		break;
	case 'X':
		erase(v, v->cy, v->cx, v->cx + n);
		break;
	case 'S':
		scroll_up(v, v->top, v->bot, n, 1);
		break;
	case 'T':
		if (v->npar <= 1)
			scroll_down(v, v->top, v->bot, n);
		break;
	case 'b':
		if (v->lastch) {
			if (n > v->rows * v->cols)
				n = v->rows * v->cols;
			uint32_t c = v->lastch;
			int save = v->cs[v->gl];
			v->cs[v->gl] = 0;
			while (n-- > 0)
				putch(v, c);
			v->cs[v->gl] = save;
		}
		break;
	case 'c':
		if (p0 == 0)
			reply(v, "\033[?1;2c");
		break;
	case 'd':
		moveto(v, v->cx, n - 1);
		break;
	case 'g':
		if (p0 == 0)
			v->tabs[v->cx] = 0;
		else if (p0 == 3)
			memset(v->tabs, 0, (size_t)v->cols);
		break;
	case 'h': case 'l':
		setmode(v, f == 'h');
		break;
	case 'm':
		sgr(v);
		break;
	case 'n':
		if (p0 == 5) {
			reply(v, "\033[0n");
		} else if (p0 == 6) {
			char b[32];
			snprintf(b, sizeof b, "\033[%d;%dR",
			         v->cy + 1 - (v->m_origin ? v->top : 0), v->cx + 1);
			reply(v, b);
		}
		break;
	case 'r': {
		int t = P(v, 0, 1) - 1, b = P(v, 1, v->rows) - 1;
		if (b > v->rows - 1)
			b = v->rows - 1;
		if (t < b) {
			v->top = t;
			v->bot = b;
			moveto(v, 0, 0);
		}
		break;
	}
	case 's':
		save_cursor(v);
		break;
	case 'u':
		restore_cursor(v);
		break;
	}
	v->dirty = 1;
}

static void osc_done(VT *v)
{
	v->osc[v->osclen] = 0;
	char *semi = strchr(v->osc, ';');
	if (!semi)
		return;
	int code = atoi(v->osc);
	if (code == 0 || code == 2) {
		int j = 0;
		for (const char *s = semi + 1; *s && j < (int)sizeof v->title - 1; s++)
			if ((unsigned char)*s >= 0x20)
				v->title[j++] = *s;
		v->title[j] = 0;
	}
}

static void vt_byte(VT *v, unsigned char c)
{
	if (c < 0x20 || c == 0x7f) {
		if (c == 0x7f)
			return;
		switch (v->state) {
		case S_OSC: case S_OSCESC:
			if (c == 7) {
				osc_done(v);
				v->state = S_GROUND;
			} else if (c == 0x1b) {
				v->state = S_OSCESC;
			} else if (c == 0x18 || c == 0x1a) {
				v->state = S_GROUND;
			}
			return;
		case S_STR: case S_STRESC:
			if (c == 0x1b)
				v->state = S_STRESC;
			else if (c == 0x18 || c == 0x1a)
				v->state = S_GROUND;
			return;
		default:
			if (c == 0x1b) {
				v->state = S_ESC;
				v->npar = 0;
				v->priv = 0;
				v->inter = 0;
				memset(v->par, 0, sizeof v->par);
				return;
			}
			if (c == 0x18 || c == 0x1a) {
				v->state = S_GROUND;
				return;
			}
			ctrl(v, c);
			return;
		}
	}

	switch (v->state) {
	case S_GROUND:
		putch(v, c);
		break;
	case S_ESC:
		esc(v, c);
		break;
	case S_ESCI:
		esci(v, c);
		v->state = S_GROUND;
		break;
	case S_CSI:
		if (c >= '0' && c <= '9') {
			if (!v->npar)
				v->npar = 1;
			int *p = &v->par[v->npar - 1];
			if (*p < 100000)
				*p = *p * 10 + (c - '0');
		} else if (c == ';' || c == ':') {
			if (!v->npar)
				v->npar = 1;
			if (v->npar < MAXPAR)
				v->par[v->npar++] = 0;
		} else if (c >= 0x3c && c <= 0x3f) {
			v->priv = c;
		} else if (c >= 0x20 && c <= 0x2f) {
			v->inter = c;
		} else if (c >= 0x40 && c <= 0x7e) {
			v->state = S_GROUND;
			csi(v, c);
		}
		break;
	case S_OSC:
		if (v->osclen < (int)sizeof v->osc - 1)
			v->osc[v->osclen++] = (char)c;
		break;
	case S_OSCESC:
		if (c == '\\') {
			osc_done(v);
			v->state = S_GROUND;
		} else {
			v->state = S_ESC;
			v->npar = 0;
			v->priv = 0;
			v->inter = 0;
			memset(v->par, 0, sizeof v->par);
			vt_byte(v, c);
		}
		break;
	case S_STR:
		break;
	case S_STRESC:
		v->state = c == '\\' ? S_GROUND : S_STR;
		break;
	}
}

void vt_write(VT *v, const char *buf, size_t n)
{
	const unsigned char *s = (const unsigned char *)buf;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = s[i];
		if (v->urem) {
			if ((c & 0xc0) == 0x80) {
				v->ucp = (v->ucp << 6) | (c & 0x3f);
				if (--v->urem == 0) {
					uint32_t cp = v->ucp;
					int bad = (v->ulen == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
					          (v->ulen == 4 && (cp < 0x10000 || cp > 0x10ffff));
					putch(v, bad ? 0xfffd : cp);
				}
				continue;
			}
			v->urem = 0;
			putch(v, 0xfffd);
		}
		if (c >= 0x80) {
			if (v->state == S_OSC) {
				if (v->osclen < (int)sizeof v->osc - 1)
					v->osc[v->osclen++] = (char)c;
				continue;
			}
			if (v->state == S_STR)
				continue;
			v->state = S_GROUND;
			if (c >= 0xc2 && c <= 0xdf) {
				v->ucp = c & 0x1f;
				v->urem = 1;
				v->ulen = 2;
			} else if (c >= 0xe0 && c <= 0xef) {
				v->ucp = c & 0x0f;
				v->urem = 2;
				v->ulen = 3;
			} else if (c >= 0xf0 && c <= 0xf4) {
				v->ucp = c & 0x07;
				v->urem = 3;
				v->ulen = 4;
			} else {
				putch(v, 0xfffd);
			}
			continue;
		}
		vt_byte(v, c);
	}
}

/* ------------------------------------------------------------------ */
/* resize                                                              */
/* ------------------------------------------------------------------ */
static Cell **resize_lines(VT *v, Cell **ls, int orows, int ocols, int rows, int cols,
                           int drop, int save)
{
	for (int i = 0; i < drop && i < orows; i++) {
		if (save)
			sb_push(v, ls[i], ocols);
		else
			free(ls[i]);
	}
	Cell **nl = calloc((size_t)rows, sizeof(Cell *));
	if (!nl)
		abort();
	for (int r = 0; r < rows; r++) {
		int src = r + drop;
		if (src < orows) {
			Cell *l = realloc(ls[src], sizeof(Cell) * (size_t)cols);
			if (!l)
				abort();
			for (int c = ocols; c < cols; c++)
				l[c] = dcell;
			if (cols < ocols && (l[cols - 1].attr & A_WIDE)) {
				l[cols - 1].ch = ' ';
				l[cols - 1].attr &= ~A_WIDE;
			}
			nl[r] = l;
		} else {
			nl[r] = line_new(cols, dcell);
		}
	}
	for (int src = rows + drop; src < orows; src++)
		free(ls[src]);
	free(ls);
	return nl;
}

void vt_resize(VT *v, int rows, int cols)
{
	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;
	if (rows == v->rows && cols == v->cols)
		return;
	int orows = v->rows, ocols = v->cols;

	/* main screen: keep the cursor line visible, spill the top into history */
	int *mcy = v->alt ? &v->save[0].cy : &v->cy;
	int mdrop = *mcy - (rows - 1);
	if (mdrop < 0)
		mdrop = 0;
	v->lines = resize_lines(v, v->lines, orows, ocols, rows, cols, mdrop, 1);
	*mcy -= mdrop;

	int adrop = 0;
	if (v->alt) {
		adrop = v->cy - (rows - 1);
		if (adrop < 0)
			adrop = 0;
	}
	v->altlines = resize_lines(v, v->altlines, orows, ocols, rows, cols, adrop, 0);
	if (v->alt)
		v->cy -= adrop;

	/* growing with the cursor at the bottom: pull history back down */
	if (rows > orows && *mcy == orows - 1 && v->sblen > 0) {
		int k = rows - orows < v->sblen ? rows - orows : v->sblen;
		Cell **L = v->lines;
		Cell **tmp = malloc(sizeof(Cell *) * (size_t)k);
		if (tmp) {
			memcpy(tmp, &L[rows - k], sizeof(Cell *) * (size_t)k);
			memmove(&L[k], &L[0], sizeof(Cell *) * (size_t)(rows - k));
			for (int j = 0; j < k; j++)
				L[j] = tmp[j];
			for (int j = k - 1; j >= 0; j--)
				sb_pop(v, L[j], cols);
			free(tmp);
			*mcy += k;
		}
	}

	v->rows = rows;
	v->cols = cols;
	v->scr = v->alt ? v->altlines : v->lines;

	uint8_t *t = calloc((size_t)cols, 1);
	if (!t)
		abort();
	memcpy(t, v->tabs, (size_t)(ocols < cols ? ocols : cols));
	free(v->tabs);
	v->tabs = t;
	if (cols > ocols)
		tabs_default(v, ocols);

	v->top = 0;
	v->bot = rows - 1;
	v->cx = clampi(v->cx, 0, cols - 1);
	v->cy = clampi(v->cy, 0, rows - 1);
	for (int i = 0; i < 2; i++) {
		v->save[i].cx = clampi(v->save[i].cx, 0, cols - 1);
		v->save[i].cy = clampi(v->save[i].cy, 0, rows - 1);
	}
	v->wrapnext = 0;
	v->dirty = 1;
}
