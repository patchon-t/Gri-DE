/* gride-wm: the GriDE compositor. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <wlr/version.h>

#include "server.h"

#define GRIDE_VERSION "0.1.0"

static void usage(FILE *out, const char *argv0) {
	fprintf(out,
		"Usage: %s [options]\n"
		"\n"
		"  -s <cmd>     Run <cmd> once GriDE is up\n"
		"  -t <cmd>     Terminal for Super+Enter (default: $GRIDE_TERMINAL, then foot)\n"
		"  -m <mod>     Main modifier: super (default) or alt. Use alt when nested\n"
		"               in a desktop that keeps Super for itself\n"
		"  -T <theme>   light (default) or dark\n"
		"  -d           Debug logging\n"
		"  -v           Print the version and exit\n"
		"  -h           Show this help\n"
		"\n"
		"Log: $XDG_STATE_HOME/gride/gride.log (~/.local/state/gride/gride.log)\n",
		argv0);
}

int main(int argc, char *argv[]) {
	struct gride_options opts = {
		.mod_mask = WLR_MODIFIER_LOGO,
	};
	bool debug = false;

	int c;
	while ((c = getopt(argc, argv, "s:t:m:T:dvh")) != -1) {
		switch (c) {
		case 's':
			opts.startup_cmd = optarg;
			break;
		case 't':
			opts.terminal = optarg;
			break;
		case 'm':
			if (strcmp(optarg, "super") == 0) {
				opts.mod_mask = WLR_MODIFIER_LOGO;
			} else if (strcmp(optarg, "alt") == 0) {
				opts.mod_mask = WLR_MODIFIER_ALT;
			} else {
				fprintf(stderr, "Unknown modifier '%s' (use super or alt)\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'T':
			if (strcmp(optarg, "light") == 0) {
				opts.dark = false;
			} else if (strcmp(optarg, "dark") == 0) {
				opts.dark = true;
			} else {
				fprintf(stderr, "Unknown theme '%s' (use light or dark)\n", optarg);
				return EXIT_FAILURE;
			}
			break;
		case 'd':
			debug = true;
			break;
		case 'v':
			printf("gride-wm %s (wlroots %s)\n", GRIDE_VERSION, WLR_VERSION_STR);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout, argv[0]);
			return EXIT_SUCCESS;
		default:
			usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (optind < argc) {
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	const char *log_path = gride_log_init(debug);
	wlr_log(WLR_INFO, "gride-wm %s starting (wlroots %s)", GRIDE_VERSION, WLR_VERSION_STR);
	if (log_path) {
		wlr_log(WLR_INFO, "Logging to %s", log_path);
	}
	if (!getenv("XDG_RUNTIME_DIR")) {
		wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR is not set; log in through a session "
			"manager or export it");
		gride_log_finish();
		return EXIT_FAILURE;
	}

	struct gride_server server = {0};
	int status = EXIT_SUCCESS;
	if (server_init(&server, &opts) && server_start(&server)) {
		server_run(&server);
		wlr_log(WLR_INFO, "Shutting down");
	} else {
		status = EXIT_FAILURE;
	}
	server_finish(&server);
	gride_log_finish();
	return status;
}
