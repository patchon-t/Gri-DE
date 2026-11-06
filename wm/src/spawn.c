/* Launching programs from the compositor. */
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "server.h"

/*
 * Double fork: the intermediate child exits at once and is reaped here, so
 * the program is re-parented to init and never becomes our zombie. setsid()
 * keeps it alive if the compositor's terminal session goes away.
 */
void spawn(const char *cmd) {
	if (!cmd || !cmd[0]) {
		return;
	}
	wlr_log(WLR_INFO, "Spawning: %s", cmd);
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		setsid();
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		pid_t child = fork();
		if (child == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(child < 0 ? 1 : 0);
	}
	waitpid(pid, NULL, 0);
}

static bool in_path(const char *name) {
	const char *path = getenv("PATH");
	if (!path) {
		path = "/usr/local/bin:/usr/bin:/bin";
	}
	char buf[PATH_MAX];
	const char *p = path;
	while (*p) {
		const char *end = strchr(p, ':');
		size_t dlen = end ? (size_t)(end - p) : strlen(p);
		if (dlen > 0 && dlen + strlen(name) + 2 < sizeof(buf)) {
			memcpy(buf, p, dlen);
			buf[dlen] = '/';
			strcpy(buf + dlen + 1, name);
			if (access(buf, X_OK) == 0) {
				return true;
			}
		}
		if (!end) {
			break;
		}
		p = end + 1;
	}
	return false;
}

/*
 * The terminal for Super+Enter: -t, then $GRIDE_TERMINAL, then the first
 * Wayland-native terminal found. X11-only terminals need XWayland, which
 * arrives in phase 2, so x-terminal-emulator is the last resort.
 */
const char *find_terminal(const struct gride_options *opts) {
	if (opts->terminal && opts->terminal[0]) {
		return opts->terminal;
	}
	const char *env = getenv("GRIDE_TERMINAL");
	if (env && env[0]) {
		return env;
	}
	static const char *const candidates[] = {
		"foot", "alacritty", "kitty", "gnome-terminal", "xfce4-terminal",
		"weston-terminal", "x-terminal-emulator",
	};
	for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (in_path(candidates[i])) {
			return candidates[i];
		}
	}
	return NULL;
}
