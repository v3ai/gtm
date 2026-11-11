/*
 * gtm - a tiny terminal multiplexer with a session manager
 *
 * shared declarations
 */
#ifndef GTM_H
#define GTM_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>

#define GTM_VERSION "1.0.0"
/*
 * Default prefix: Esc.  Only a lone Esc counts (see handle_input), so
 * arrow keys, Alt-keys and pastes, which also start with ESC, pass
 * through.  Ctrl-b is avoided because desktop shortcuts commonly grab
 * it; override with $GTM_PREFIX.
 */
#define PREFIX_DEFAULT 0x1b

/* ------------------------------------------------------------------ */
/* growable byte buffer                                                */
/* ------------------------------------------------------------------ */
typedef struct {
	char  *d;
	size_t len, cap;
} Buf;

void buf_add(Buf *b, const void *p, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_utf8(Buf *b, uint32_t cp);
void buf_consume(Buf *b, size_t n);
void buf_reset(Buf *b);
void buf_free(Buf *b);

/* ------------------------------------------------------------------ */
/* misc utilities                                                      */
/* ------------------------------------------------------------------ */
int64_t     now_ms(void);
void        setup_locale(void);
int         cwidth(uint32_t cp);
int         strwidth(const char *s);
int         utf8_dec(const unsigned char *s, size_t n, uint32_t *cp);
int         utf8_enc(char *out, uint32_t cp);
int         write_all(int fd, const void *p, size_t n);
int         write_str(int fd, const char *s);
const char *fmt_age(time_t t, char *out, size_t n);

/* keys produced by parse_key() */
enum {
	K_NONE = -1,
	K_UP = 0x110000, K_DOWN, K_LEFT, K_RIGHT,
	K_HOME, K_END, K_PGUP, K_PGDN, K_INS, K_DEL, K_BTAB,
};
#define K_SHIFT 0x1000000
#define K_ALT   0x2000000
#define K_CTRL  0x4000000
#define K_MODS  (K_SHIFT | K_ALT | K_CTRL)
int parse_key(const unsigned char *s, size_t n, int *key);

/* prefix key: $GTM_PREFIX ("esc", "C-space", "C-a", "^]", ...) or PREFIX_DEFAULT */
int         prefix_parse(const char *s);
int         prefix_key(void);
const char *prefix_name(int key);

/* terminal */
int  term_raw(int fd, struct termios *saved);
void term_restore(int fd, const struct termios *saved);
void term_size(int fd, int *rows, int *cols);

/* ------------------------------------------------------------------ */
/* client <-> server protocol                                          */
/* ------------------------------------------------------------------ */
enum {
	MSG_ATTACH = 1,   /* u32 rows, u32 cols                */
	MSG_INPUT,        /* raw key bytes                      */
	MSG_RESIZE,       /* u32 rows, u32 cols                */
	MSG_DETACH,
	MSG_INFO,         /* -> MSG_REPLY (key=value text)      */
	MSG_PREVIEW,      /* u32 rows, u32 cols -> MSG_REPLY    */
	MSG_KILL,
	MSG_RENAME,       /* new name -> MSG_REPLY "ok" / err   */

	MSG_OUTPUT = 64,  /* bytes for the terminal             */
	MSG_EXIT,         /* 1 byte reason                      */
	MSG_REPLY,
};

#define EXIT_DETACH  'd'
#define EXIT_MANAGER 'm'
#define EXIT_ENDED   'x'
#define EXIT_KILLED  'k'

void msg_pack(Buf *b, uint32_t type, const void *p, uint32_t n);
int  msg_send(int fd, uint32_t type, const void *p, uint32_t n);
int  msg_parse(Buf *in, uint32_t *type, Buf *payload);
int  msg_recv(int fd, uint32_t *type, Buf *payload, int timeout_ms);

/* ------------------------------------------------------------------ */
/* sessions                                                            */
/* ------------------------------------------------------------------ */
#define MAX_NAME 32
#define MAX_WINS_INFO 32

typedef struct {
	char name[64];
	int  pid, nclients, nwin, cur, npanes;
	long created;
	int  nw;
	char wname[MAX_WINS_INFO][48];
	int  wflags[MAX_WINS_INFO];     /* 1 = bell, 2 = zoomed */
	int  wpanes[MAX_WINS_INFO];
} SessInfo;

const char *sock_dir(void);
void        sock_path(char *out, size_t n, const char *name);
int         valid_name(const char *s);
int         session_connect(const char *name);
int         session_alive(const char *name);
int         session_request(const char *name, uint32_t type, const void *p,
                            uint32_t n, Buf *reply);
int         session_query(const char *name, SessInfo *si);
int         session_list(SessInfo **out);
int         session_kill(const char *name);
int         session_rename(const char *from, const char *to, char *err, size_t errn);
void        session_autoname(char *out, size_t n);

int server_start(const char *name, int rows, int cols, char *err, size_t errn);
int client_attach(const char *name, int managed, char *err, size_t errn);
int manager_run(void);
void manager_focus(const char *name);

/* ------------------------------------------------------------------ */
/* virtual terminal                                                    */
/* ------------------------------------------------------------------ */
enum {
	A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_BLINK = 16,
	A_REVERSE = 32, A_INVIS = 64, A_STRIKE = 128,
	A_WIDE = 256,  /* first half of a double-width glyph */
	A_WCONT = 512, /* second half (placeholder)          */
};

#define COL_DEFAULT  0u
#define COL_IDX(i)   (0x01000000u | (uint32_t)(i))
#define COL_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

typedef struct {
	uint32_t ch, fg, bg;
	uint16_t attr;
} Cell;

typedef struct {
	Cell *c;
	int   n;
} SbLine;

typedef struct {
	int  cx, cy, origin, wrapnext, cs0, cs1, gl, valid;
	Cell pen;
} VTSave;

typedef struct VT {
	int    rows, cols;
	Cell **lines, **altlines, **scr;
	int    alt;
	int    cx, cy, wrapnext;
	Cell   pen;
	int    top, bot;
	int    m_wrap, m_origin, m_insert, m_appcursor, m_appkeypad,
	       m_cursor, m_paste, m_lnm;
	int    curstyle;
	uint8_t *tabs;
	VTSave save[2];
	int    cs[2], gl;
	/* parser */
	int    state;
	int    par[32], npar;
	int    priv, inter;
	char   osc[1024];
	int    osclen;
	uint32_t ucp;
	int    urem, ulen;
	uint32_t lastch;
	/* scrollback ring */
	SbLine *sb;
	int    sbcap, sblen, sbhead;
	int64_t sbtotal;
	/* misc */
	char   title[128];
	int    dirty, bell;
	Buf    reply;
} VT;

void        vt_init(VT *v, int rows, int cols, int sbcap);
void        vt_free(VT *v);
void        vt_write(VT *v, const char *s, size_t n);
void        vt_resize(VT *v, int rows, int cols);
const Cell *vt_gline(const VT *v, int64_t g, int *n);

#endif
