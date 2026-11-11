/*
 * gtm - command line entry point
 */
#include "gtm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tty_colors;

static const char *c(const char *code) { return tty_colors ? code : ""; }

static void usage(FILE *f)
{
	fprintf(f,
	        "%s◆ gtm%s %s — terminal multiplexer with a session manager\n\n"
	        "%susage%s\n"
	        "  gtm                       open the session manager\n"
	        "  gtm new [-d] [name]       create a session and attach (-d: stay detached)\n"
	        "  gtm attach [name]         attach to a session (alias: a)\n"
	        "  gtm ls                    list sessions\n"
	        "  gtm kill <name>           end a session\n"
	        "  gtm rename <old> <new>    rename a session\n"
	        "  gtm help | version\n\n"
	        "%sinside a session%s  press %s%s ?%s for key bindings"
	        "  (change the prefix with $GTM_PREFIX, e.g. C-a)\n",
	        c("\033[1;38;5;147m"), c("\033[0m"), GTM_VERSION,
	        c("\033[1m"), c("\033[0m"),
	        c("\033[1m"), c("\033[0m"), c("\033[38;5;147m"), prefix_name(prefix_key()), c("\033[0m"));
}

static int after_attach(const char *name, int r, const char *err)
{
	if (r < 0) {
		fprintf(stderr, "gtm: %s\n", err);
		return 1;
	}
	if (r == EXIT_MANAGER) {
		manager_focus(name);
		return manager_run();
	}
	if (r == EXIT_ENDED)
		printf("[session %s ended]\n", name);
	else if (r == EXIT_KILLED)
		printf("[session %s killed]\n", name);
	else
		printf("[detached from %s]\n", name);
	return 0;
}

static int cmd_ls(void)
{
	SessInfo *v;
	int n = session_list(&v);
	if (!n) {
		printf("%sno sessions%s\n", c("\033[38;5;243m"), c("\033[0m"));
		return 0;
	}
	for (int i = 0; i < n; i++) {
		char age[16];
		printf("%s%s%s %s%-16s%s %s%d window%s · %d pane%s · up %s%s%s\n",
		       c(v[i].nclients ? "\033[38;5;150m" : "\033[38;5;240m"),
		       v[i].nclients ? "●" : "○", c("\033[0m"),
		       c("\033[1m"), v[i].name, c("\033[0m"),
		       c("\033[38;5;245m"), v[i].nwin, v[i].nwin == 1 ? "" : "s",
		       v[i].npanes, v[i].npanes == 1 ? "" : "s",
		       fmt_age(v[i].created, age, sizeof age),
		       v[i].nclients ? " · attached" : "", c("\033[0m"));
	}
	free(v);
	return 0;
}

int main(int argc, char **argv)
{
	setup_locale();
	signal(SIGPIPE, SIG_IGN);
	tty_colors = isatty(1);

	const char *gp = getenv("GTM_PREFIX");
	if (gp && *gp && prefix_parse(gp) < 0)
		fprintf(stderr, "gtm: ignoring GTM_PREFIX='%s' (want e.g. esc, C-space, C-a, C-]); using %s\n",
		        gp, prefix_name(PREFIX_DEFAULT));

	if (argc < 2)
		return manager_run();

	const char *cmd = argv[1];
	char err[256];

	if (!strcmp(cmd, "new") || !strcmp(cmd, "n")) {
		int detached = 0;
		const char *name = NULL;
		for (int i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "-d"))
				detached = 1;
			else if (!name)
				name = argv[i];
		}
		char nm[64];
		if (name)
			snprintf(nm, sizeof nm, "%s", name);
		else
			session_autoname(nm, sizeof nm);
		int rows = 24, cols = 80;
		if (isatty(1))
			term_size(1, &rows, &cols);
		if (server_start(nm, rows, cols, err, sizeof err) < 0) {
			fprintf(stderr, "gtm: %s\n", err);
			return 1;
		}
		if (detached) {
			printf("created session %s\n", nm);
			return 0;
		}
		int r = client_attach(nm, 0, err, sizeof err);
		return after_attach(nm, r, err);
	}

	if (!strcmp(cmd, "attach") || !strcmp(cmd, "a") || !strcmp(cmd, "at")) {
		char nm[64] = "";
		if (argc > 2) {
			snprintf(nm, sizeof nm, "%s", argv[2]);
		} else {
			/* no name: prefer a detached session, else the first one */
			SessInfo *v;
			int n = session_list(&v);
			for (int i = 0; i < n && !nm[0]; i++)
				if (!v[i].nclients)
					snprintf(nm, sizeof nm, "%s", v[i].name);
			if (!nm[0] && n)
				snprintf(nm, sizeof nm, "%s", v[0].name);
			free(v);
			if (!nm[0]) {
				fprintf(stderr, "gtm: no sessions\n");
				return 1;
			}
		}
		int r = client_attach(nm, 0, err, sizeof err);
		return after_attach(nm, r, err);
	}

	if (!strcmp(cmd, "ls") || !strcmp(cmd, "list"))
		return cmd_ls();

	if (!strcmp(cmd, "kill") || !strcmp(cmd, "k")) {
		if (argc < 3) {
			fprintf(stderr, "usage: gtm kill <name>\n");
			return 1;
		}
		int rc = 0;
		for (int i = 2; i < argc; i++) {
			if (session_kill(argv[i]) < 0) {
				fprintf(stderr, "gtm: no session named '%s'\n", argv[i]);
				rc = 1;
			}
		}
		return rc;
	}

	if (!strcmp(cmd, "rename")) {
		if (argc < 4) {
			fprintf(stderr, "usage: gtm rename <old> <new>\n");
			return 1;
		}
		if (session_rename(argv[2], argv[3], err, sizeof err) < 0) {
			fprintf(stderr, "gtm: %s\n", err);
			return 1;
		}
		return 0;
	}

	if (!strcmp(cmd, "version") || !strcmp(cmd, "-V") || !strcmp(cmd, "--version")) {
		printf("gtm %s\n", GTM_VERSION);
		return 0;
	}
	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage(stdout);
		return 0;
	}
	fprintf(stderr, "gtm: unknown command '%s'\n\n", cmd);
	usage(stderr);
	return 1;
}
