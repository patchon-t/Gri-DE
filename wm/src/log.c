/*
 * Logging. Everything wlroots and gride-wm log goes through wlr_log, whose
 * callback we replace so lines land in ~/.local/state/gride/gride.log
 * (or $XDG_STATE_HOME/gride/gride.log) as well as on stderr.
 * The previous run's log is kept as gride.log.old.
 */
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "server.h"

static FILE *log_file;
static enum wlr_log_importance log_level = WLR_INFO;
static bool log_to_stderr;
static char log_path[PATH_MAX];

static const char *const level_names[] = {
	[WLR_SILENT] = "",
	[WLR_ERROR] = "error",
	[WLR_INFO] = "info",
	[WLR_DEBUG] = "debug",
};

static void write_line(FILE *f, const char *stamp, const char *level,
		const char *fmt, va_list args) {
	fprintf(f, "%s [%s] ", stamp, level);
	vfprintf(f, fmt, args);
	fputc('\n', f);
}

static void log_callback(enum wlr_log_importance importance, const char *fmt,
		va_list args) {
	if (importance > log_level || importance <= WLR_SILENT ||
			importance >= WLR_LOG_IMPORTANCE_LAST) {
		return;
	}
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);
	char stamp[32];
	size_t n = strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
	snprintf(stamp + n, sizeof(stamp) - n, ".%03ld", ts.tv_nsec / 1000000);

	if (log_file) {
		va_list copy;
		va_copy(copy, args);
		write_line(log_file, stamp, level_names[importance], fmt, copy);
		va_end(copy);
	}
	if (log_to_stderr) {
		write_line(stderr, stamp, level_names[importance], fmt, args);
	}
}

/* mkdir -p for the directory part of an absolute path. */
static bool make_dirs(char *path) {
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		bool ok = mkdir(path, 0700) == 0 || errno == EEXIST;
		*p = '/';
		if (!ok) {
			return false;
		}
	}
	return true;
}

const char *gride_log_init(bool debug) {
	log_level = debug ? WLR_DEBUG : WLR_INFO;
	log_to_stderr = true;
	wlr_log_init(log_level, log_callback);

	const char *state = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	int len;
	if (state && state[0] == '/') {
		len = snprintf(log_path, sizeof(log_path), "%s/gride/gride.log", state);
	} else if (home && home[0] == '/') {
		len = snprintf(log_path, sizeof(log_path), "%s/.local/state/gride/gride.log", home);
	} else {
		wlr_log(WLR_ERROR, "Neither XDG_STATE_HOME nor HOME is set; logging to stderr only");
		return NULL;
	}
	if (len < 0 || (size_t)len >= sizeof(log_path) || !make_dirs(log_path)) {
		wlr_log(WLR_ERROR, "Cannot create the log directory for %s", log_path);
		return NULL;
	}

	char old_path[PATH_MAX + 8];
	snprintf(old_path, sizeof(old_path), "%s.old", log_path);
	rename(log_path, old_path); /* fails harmlessly on the first run */

	log_file = fopen(log_path, "w");
	if (!log_file) {
		wlr_log_errno(WLR_ERROR, "Cannot open %s", log_path);
		return NULL;
	}
	setvbuf(log_file, NULL, _IOLBF, 0);
	return log_path;
}

void gride_log_finish(void) {
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
}
