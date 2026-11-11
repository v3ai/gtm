/*
 * gtm - attach client: raw terminal <-> session server
 */
#include "gtm.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t winch;

static void on_winch(int sig)
{
	(void)sig;
	winch = 1;
}

/*
 * Attach to `name`.  `managed` means the session manager owns the screen
 * (alternate screen already active).  Returns the EXIT_* reason, or -1.
 */
int client_attach(const char *name, int managed, char *err, size_t errn)
{
	if (getenv("GTM") && !getenv("GTM_FORCE_NEST")) {
		snprintf(err, errn, "already inside a gtm session (unset $GTM to nest anyway)");
		return -1;
	}
	if (!isatty(0) || !isatty(1)) {
		snprintf(err, errn, "not a terminal");
		return -1;
	}
	int fd = session_connect(name);
	if (fd < 0) {
		snprintf(err, errn, "no session named '%s'", name);
		return -1;
	}

	struct termios saved;
	if (term_raw(0, &saved) < 0) {
		snprintf(err, errn, "cannot set raw mode: %s", strerror(errno));
		close(fd);
		return -1;
	}
	struct sigaction sa = { .sa_handler = on_winch }, old;
	sigaction(SIGWINCH, &sa, &old);
	winch = 0;

	if (!managed)
		write_str(1, "\033[?1049h");

	int rows, cols;
	term_size(1, &rows, &cols);
	uint32_t sz[2] = { (uint32_t)rows, (uint32_t)cols };
	msg_send(fd, MSG_ATTACH, sz, sizeof sz);

	Buf in = { 0 }, pl = { 0 };
	int reason = EXIT_ENDED;
	for (;;) {
		if (winch) {
			winch = 0;
			term_size(1, &rows, &cols);
			sz[0] = (uint32_t)rows;
			sz[1] = (uint32_t)cols;
			msg_send(fd, MSG_RESIZE, sz, sizeof sz);
		}
		struct pollfd pf[2] = { { 0, POLLIN, 0 }, { fd, POLLIN, 0 } };
		int r = poll(pf, 2, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			char b[4096];
			ssize_t n = read(0, b, sizeof b);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				reason = EXIT_DETACH;
				break;
			}
			if (msg_send(fd, MSG_INPUT, b, (uint32_t)n) < 0)
				break;
		}
		if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			char b[65536];
			ssize_t n = read(fd, b, sizeof b);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			buf_add(&in, b, (size_t)n);
			uint32_t t;
			while (msg_parse(&in, &t, &pl)) {
				if (t == MSG_OUTPUT) {
					write_all(1, pl.d, pl.len);
				} else if (t == MSG_EXIT) {
					reason = pl.len ? pl.d[0] : EXIT_ENDED;
					goto done;
				}
			}
		}
	}
done:
	/* undo any modes the inner programs left in the outer terminal */
	{
		const char *rst = "\033[0m\033[?25h\033[?1l\033>\033[?2004l\033[0 q";
		write_all(1, rst, strlen(rst));
	}
	if (!managed)
		write_str(1, "\033[?1049l");
	term_restore(0, &saved);
	sigaction(SIGWINCH, &old, NULL);
	buf_free(&in);
	buf_free(&pl);
	close(fd);
	return reason;
}
