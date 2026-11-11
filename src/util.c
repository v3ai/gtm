/*
 * gtm - utilities: buffers, utf-8, keys, terminal, protocol, sessions
 */
#include "gtm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* buffers                                                             */
/* ------------------------------------------------------------------ */
static void buf_grow(Buf *b, size_t need)
{
	if (b->len + need <= b->cap)
		return;
	size_t c = b->cap ? b->cap : 256;
	while (c < b->len + need)
		c *= 2;
	char *d = realloc(b->d, c);
	if (!d)
		abort();
	b->d = d;
	b->cap = c;
}

void buf_add(Buf *b, const void *p, size_t n)
{
	if (!n)
		return;
	buf_grow(b, n);
	memcpy(b->d + b->len, p, n);
	b->len += n;
}

void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	buf_grow(b, (size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(b->d + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

void buf_utf8(Buf *b, uint32_t cp)
{
	char t[4];
	buf_add(b, t, (size_t)utf8_enc(t, cp));
}

void buf_consume(Buf *b, size_t n)
{
	if (n >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->d, b->d + n, b->len - n);
	b->len -= n;
}

void buf_reset(Buf *b) { b->len = 0; }

void buf_free(Buf *b)
{
	free(b->d);
	b->d = NULL;
	b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */
int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void setup_locale(void)
{
	if (!setlocale(LC_CTYPE, "") || strcmp(nl_langinfo(CODESET), "UTF-8") != 0) {
		if (!setlocale(LC_CTYPE, "C.UTF-8"))
			setlocale(LC_CTYPE, "en_US.UTF-8");
	}
}

int cwidth(uint32_t cp)
{
	if (cp < 0x20)
		return 0;
	if (cp < 0x7f)
		return 1;
	return wcwidth((wchar_t)cp);
}

int utf8_enc(char *o, uint32_t cp)
{
	if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
		cp = 0xfffd;
	if (cp < 0x80) {
		o[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = (char)(0xc0 | (cp >> 6));
		o[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (char)(0xe0 | (cp >> 12));
		o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		o[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	o[0] = (char)(0xf0 | (cp >> 18));
	o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	o[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	o[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

/* returns bytes consumed (>= 1), or 0 if the sequence is incomplete */
int utf8_dec(const unsigned char *s, size_t n, uint32_t *cp)
{
	unsigned c = s[0];
	int len;
	uint32_t v;

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if (c >= 0xc2 && c <= 0xdf) {
		len = 2;
		v = c & 0x1f;
	} else if (c >= 0xe0 && c <= 0xef) {
		len = 3;
		v = c & 0x0f;
	} else if (c >= 0xf0 && c <= 0xf4) {
		len = 4;
		v = c & 0x07;
	} else {
		*cp = 0xfffd;
		return 1;
	}
	for (int i = 1; i < len; i++) {
		if ((size_t)i >= n)
			return 0;
		if ((s[i] & 0xc0) != 0x80) {
			*cp = 0xfffd;
			return i;
		}
		v = (v << 6) | (s[i] & 0x3f);
	}
	*cp = v;
	return len;
}

int strwidth(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t n = strlen(s);
	int w = 0;
	while (n) {
		uint32_t cp;
		int k = utf8_dec(p, n, &cp);
		if (k <= 0)
			break;
		int cw = cwidth(cp);
		w += cw > 0 ? cw : (cw < 0 ? 1 : 0);
		p += k;
		n -= (size_t)k;
	}
	return w;
}

int write_all(int fd, const void *p, size_t n)
{
	const char *c = p;
	while (n) {
		ssize_t w = write(fd, c, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd pf = { fd, POLLOUT, 0 };
				poll(&pf, 1, 100);
				continue;
			}
			return -1;
		}
		c += w;
		n -= (size_t)w;
	}
	return 0;
}

const char *fmt_age(time_t t, char *out, size_t n)
{
	long d = (long)(time(NULL) - t);
	if (d < 0)
		d = 0;
	if (d < 60)
		snprintf(out, n, "%lds", d);
	else if (d < 3600)
		snprintf(out, n, "%ldm", d / 60);
	else if (d < 86400)
		snprintf(out, n, "%ldh", d / 3600);
	else
		snprintf(out, n, "%ldd", d / 86400);
	return out;
}

/* ------------------------------------------------------------------ */
/* keys                                                                */
/* ------------------------------------------------------------------ */
int parse_key(const unsigned char *s, size_t n, int *key)
{
	if (!n) {
		*key = K_NONE;
		return 0;
	}
	if (s[0] == 0x1b) {
		if (n == 1) {
			*key = 27;
			return 1;
		}
		if (s[1] == '[' || s[1] == 'O') {
			size_t i = 2;
			int par[4] = { 0 }, np = 0;
			while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == ';')) {
				if (s[i] == ';') {
					if (np < 3)
						np++;
				} else {
					par[np] = par[np] * 10 + (s[i] - '0');
				}
				i++;
			}
			if (i >= n) {
				*key = K_NONE;
				return (int)n;
			}
			unsigned char f = s[i++];
			int k = K_NONE;
			switch (f) {
			case 'A': k = K_UP; break;
			case 'B': k = K_DOWN; break;
			case 'C': k = K_RIGHT; break;
			case 'D': k = K_LEFT; break;
			case 'H': k = K_HOME; break;
			case 'F': k = K_END; break;
			case 'Z': k = K_BTAB; break;
			case '~':
				switch (par[0]) {
				case 1: case 7: k = K_HOME; break;
				case 4: case 8: k = K_END; break;
				case 2: k = K_INS; break;
				case 3: k = K_DEL; break;
				case 5: k = K_PGUP; break;
				case 6: k = K_PGDN; break;
				}
				break;
			}
			int mod = np >= 1 ? par[1] : 0;
			if (k != K_NONE && mod > 1) {
				int m = mod - 1;
				if (m & 1) k |= K_SHIFT;
				if (m & 2) k |= K_ALT;
				if (m & 4) k |= K_CTRL;
			}
			*key = k;
			return (int)i;
		}
		/* ESC followed by anything else: a lone escape, then that key */
		*key = 27;
		return 1;
	}
	uint32_t cp;
	int c = utf8_dec(s, n, &cp);
	if (c == 0) {
		*key = K_NONE;
		return (int)n;
	}
	*key = (int)cp;
	return c;
}

/* ------------------------------------------------------------------ */
/* prefix key                                                          */
/* ------------------------------------------------------------------ */
/* "esc" / "C-space" / "C-a" / "^a" / "C-]" -> control byte, or -1 if unusable */
int prefix_parse(const char *s)
{
	if (!s)
		return -1;
	if (!strcasecmp(s, "esc") || !strcasecmp(s, "escape") || !strcmp(s, "C-[") || !strcmp(s, "^["))
		return 0x1b;
	if ((s[0] == 'C' || s[0] == 'c') && s[1] == '-')
		s += 2;
	else if (s[0] == '^')
		s += 1;
	else
		return -1;
	if (!strcasecmp(s, "space") || !strcmp(s, "@"))
		return 0x00;
	if (!s[0] || s[1])
		return -1;
	int ch = (unsigned char)s[0], k = -1;
	if (ch >= 'a' && ch <= 'z')
		k = ch - 'a' + 1;
	else if (ch >= 'A' && ch <= 'Z')
		k = ch - 'A' + 1;
	else if (ch >= '\\' && ch <= '_')
		k = ch - '@';
	/* these are Tab, Enter, newline and backspace: never a prefix */
	if (k == 0x09 || k == 0x0a || k == 0x0d || k == 0x08)
		return -1;
	return k;
}

int prefix_key(void)
{
	static int k = -2;
	if (k == -2) {
		k = prefix_parse(getenv("GTM_PREFIX"));
		if (k < 0)
			k = PREFIX_DEFAULT;
	}
	return k;
}

const char *prefix_name(int key)
{
	static char out[16];
	if (key == 0x00)
		return "Ctrl-Space";
	if (key == 0x1b)
		return "Esc";
	if (key >= 1 && key <= 26)
		snprintf(out, sizeof out, "Ctrl-%c", 'a' + key - 1);
	else
		snprintf(out, sizeof out, "Ctrl-%c", key + '@');
	return out;
}

/* ------------------------------------------------------------------ */
/* terminal                                                            */
/* ------------------------------------------------------------------ */
int term_raw(int fd, struct termios *saved)
{
	struct termios t;
	if (tcgetattr(fd, &t) < 0)
		return -1;
	if (saved)
		*saved = t;
	cfmakeraw(&t);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	return tcsetattr(fd, TCSANOW, &t);
}

void term_restore(int fd, const struct termios *saved)
{
	tcsetattr(fd, TCSANOW, saved);
}

void term_size(int fd, int *rows, int *cols)
{
	struct winsize ws;
	if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
}

/* ------------------------------------------------------------------ */
/* protocol                                                            */
/* ------------------------------------------------------------------ */
void msg_pack(Buf *b, uint32_t type, const void *p, uint32_t n)
{
	uint32_t h[2] = { type, n };
	buf_add(b, h, sizeof h);
	buf_add(b, p, n);
}

int msg_send(int fd, uint32_t type, const void *p, uint32_t n)
{
	Buf b = { 0 };
	msg_pack(&b, type, p, n);
	int r = write_all(fd, b.d, b.len);
	buf_free(&b);
	return r;
}

int msg_parse(Buf *in, uint32_t *type, Buf *pl)
{
	if (in->len < 8)
		return 0;
	uint32_t h[2];
	memcpy(h, in->d, sizeof h);
	if (h[1] > 64u * 1024 * 1024) {
		in->len = 0;
		return 0;
	}
	if (in->len < 8 + (size_t)h[1])
		return 0;
	*type = h[0];
	buf_reset(pl);
	buf_add(pl, in->d + 8, h[1]);
	buf_consume(in, 8 + (size_t)h[1]);
	return 1;
}

static int read_full(int fd, void *p, size_t n, int timeout_ms)
{
	char *c = p;
	while (n) {
		struct pollfd pf = { fd, POLLIN, 0 };
		int r = poll(&pf, 1, timeout_ms);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		ssize_t k = read(fd, c, n);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			return -1;
		c += k;
		n -= (size_t)k;
	}
	return 0;
}

int msg_recv(int fd, uint32_t *type, Buf *pl, int timeout_ms)
{
	uint32_t h[2];
	if (read_full(fd, h, sizeof h, timeout_ms) < 0)
		return -1;
	if (h[1] > 64u * 1024 * 1024)
		return -1;
	*type = h[0];
	buf_reset(pl);
	if (h[1]) {
		char *tmp = malloc(h[1]);
		if (!tmp)
			return -1;
		if (read_full(fd, tmp, h[1], timeout_ms) < 0) {
			free(tmp);
			return -1;
		}
		buf_add(pl, tmp, h[1]);
		free(tmp);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* sessions                                                            */
/* ------------------------------------------------------------------ */
const char *sock_dir(void)
{
	static char dir[512];
	if (dir[0])
		return dir;
	const char *x = getenv("GTM_DIR");
	struct stat st;
	if (x && *x) {
		snprintf(dir, sizeof dir, "%s", x);
	} else {
		const char *r = getenv("XDG_RUNTIME_DIR");
		if (r && *r && stat(r, &st) == 0 && S_ISDIR(st.st_mode))
			snprintf(dir, sizeof dir, "%s/gtm", r);
		else
			snprintf(dir, sizeof dir, "/tmp/gtm-%d", (int)getuid());
	}
	mkdir(dir, 0700);
	return dir;
}

void sock_path(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/%s", sock_dir(), name);
}

int valid_name(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > MAX_NAME || s[0] == '.' || s[0] == '-')
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return 0;
	}
	return 1;
}

int session_connect(const char *name)
{
	if (!valid_name(name))
		return -1;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char path[sizeof sa.sun_path];
	sock_path(path, sizeof path, name);
	memcpy(sa.sun_path, path, strlen(path) + 1);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		if (errno == ECONNREFUSED)
			unlink(path); /* stale socket left by a crashed server */
		close(fd);
		return -1;
	}
	return fd;
}

int session_alive(const char *name)
{
	int fd = session_connect(name);
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

int session_request(const char *name, uint32_t type, const void *p, uint32_t n, Buf *reply)
{
	int fd = session_connect(name);
	if (fd < 0)
		return -1;
	int r = -1;
	if (msg_send(fd, type, p, n) == 0) {
		uint32_t t;
		Buf tmp = { 0 };
		Buf *out = reply ? reply : &tmp;
		if (msg_recv(fd, &t, out, 1500) == 0 && t == MSG_REPLY)
			r = 0;
		buf_free(&tmp);
	}
	close(fd);
	return r;
}

static void parse_info(const char *d, size_t n, SessInfo *si)
{
	char *txt = malloc(n + 1);
	if (!txt)
		return;
	memcpy(txt, d, n);
	txt[n] = 0;
	char *save = NULL;
	for (char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		char *eq = strchr(ln, '=');
		if (!eq)
			continue;
		*eq = 0;
		const char *k = ln, *v = eq + 1;
		if (!strcmp(k, "name"))
			snprintf(si->name, sizeof si->name, "%s", v);
		else if (!strcmp(k, "pid"))
			si->pid = atoi(v);
		else if (!strcmp(k, "created"))
			si->created = atol(v);
		else if (!strcmp(k, "clients"))
			si->nclients = atoi(v);
		else if (!strcmp(k, "windows"))
			si->nwin = atoi(v);
		else if (!strcmp(k, "active"))
			si->cur = atoi(v);
		else if (!strcmp(k, "panes"))
			si->npanes = atoi(v);
		else if (!strcmp(k, "win") && si->nw < MAX_WINS_INFO) {
			/* "<flags>\t<panes>\t<name>" */
			int fl = atoi(v);
			const char *t1 = strchr(v, '\t');
			const char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
			si->wflags[si->nw] = fl;
			si->wpanes[si->nw] = t1 ? atoi(t1 + 1) : 1;
			snprintf(si->wname[si->nw], sizeof si->wname[0], "%s", t2 ? t2 + 1 : "?");
			si->nw++;
		}
	}
	free(txt);
}

int session_query(const char *name, SessInfo *si)
{
	Buf r = { 0 };
	memset(si, 0, sizeof *si);
	if (session_request(name, MSG_INFO, NULL, 0, &r) < 0) {
		buf_free(&r);
		return -1;
	}
	parse_info(r.d, r.len, si);
	if (!si->name[0])
		snprintf(si->name, sizeof si->name, "%s", name);
	buf_free(&r);
	return 0;
}

static int si_cmp(const void *a, const void *b)
{
	return strverscmp(((const SessInfo *)a)->name, ((const SessInfo *)b)->name);
}

int session_list(SessInfo **out)
{
	*out = NULL;
	DIR *d = opendir(sock_dir());
	if (!d)
		return 0;
	int n = 0, cap = 0;
	SessInfo *v = NULL;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !valid_name(e->d_name))
			continue;
		SessInfo si;
		if (session_query(e->d_name, &si) < 0)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			SessInfo *nv = realloc(v, sizeof *v * (size_t)cap);
			if (!nv)
				break;
			v = nv;
		}
		v[n++] = si;
	}
	closedir(d);
	if (n)
		qsort(v, (size_t)n, sizeof *v, si_cmp);
	*out = v;
	return n;
}

int session_kill(const char *name)
{
	return session_request(name, MSG_KILL, NULL, 0, NULL);
}

int session_rename(const char *from, const char *to, char *err, size_t errn)
{
	Buf r = { 0 };
	if (session_request(from, MSG_RENAME, to, (uint32_t)strlen(to), &r) < 0) {
		snprintf(err, errn, "no session named '%s'", from);
		buf_free(&r);
		return -1;
	}
	int ok = r.len == 2 && !memcmp(r.d, "ok", 2);
	if (!ok)
		snprintf(err, errn, "%.*s", (int)r.len, r.d ? r.d : "");
	buf_free(&r);
	return ok ? 0 : -1;
}

void session_autoname(char *out, size_t n)
{
	for (int i = 0;; i++) {
		snprintf(out, n, "%d", i);
		if (!session_alive(out))
			return;
	}
}

int write_str(int fd, const char *s)
{
	return write_all(fd, s, strlen(s));
}
