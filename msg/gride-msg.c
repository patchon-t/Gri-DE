/*
 * gride-msg: talk to a running GriDE over its IPC socket.
 *
 *   gride-msg windows
 *   gride-msg snap A1:B2
 *   gride-msg workspace 5
 *   gride-msg grid set 4x3
 *
 * Queries print readable text (or JSON with -j); actions print nothing on
 * success. Errors go to stderr with exit status 1.
 */
#include <errno.h>
#include <getopt.h>
#include <json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char usage_text[] =
	"Usage: gride-msg [-j] [-s socket] <command> [arguments]\n"
	"\n"
	"Queries:\n"
	"  windows                       List windows\n"
	"  workspaces                    List the 3x3 workspaces\n"
	"  grid                          Show the grid size and mode\n"
	"  outputs                       List monitors\n"
	"  version\n"
	"\n"
	"Workspaces:\n"
	"  workspace <1-9|left|right|up|down>\n"
	"  move-to-workspace <1-9> [window]\n"
	"  workspace-name <1-9> [name]   Name a workspace (no name clears it)\n"
	"\n"
	"Windows (the focused window unless a window id is given):\n"
	"  focus <window>\n"
	"  close [window]\n"
	"  snap <range> [window]         e.g. snap A1:B2\n"
	"  move <dx> <dy> [window]       Move by whole cells\n"
	"  resize <dx> <dy> [window]     Grow or shrink by whole cells\n"
	"  maximize|minimize|fullscreen [on|off|toggle] [window]\n"
	"  snap-layouts [window]         Open the snap layouts flyout\n"
	"\n"
	"Grid:\n"
	"  grid set <CxR>                e.g. grid set 4x3 (up to 8x6)\n"
	"  grid mode <free|auto>\n"
	"\n"
	"Other:\n"
	"  spawn <command...>\n"
	"  subscribe <window|workspace|grid>...   Print events as JSON lines\n"
	"  raw <json>                    Send a request as is\n"
	"  exit                          Quit GriDE\n"
	"\n"
	"Options:\n"
	"  -j          Print JSON\n"
	"  -s <path>   Socket (default: $GRIDE_SOCK, then $XDG_RUNTIME_DIR/gride.sock)\n";

static bool json_output;

static void die(const char *fmt, const char *arg) {
	fprintf(stderr, "gride-msg: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

static int connect_socket(const char *path) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(addr.sun_path)) {
		die("socket path too long: %s", path);
	}
	strcpy(addr.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		die("cannot reach GriDE at %s (is it running?)", path);
	}
	return fd;
}

static void send_request(int fd, json_object *req) {
	const char *text = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);
	size_t len = strlen(text);
	char *line = malloc(len + 2);
	if (!line) {
		die("%s", "out of memory");
	}
	memcpy(line, text, len);
	line[len] = '\n';
	line[len + 1] = '\0';
	size_t off = 0;
	while (off < len + 1) {
		ssize_t n = write(fd, line + off, len + 1 - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			die("%s", "lost the connection to GriDE");
		}
		off += (size_t)n;
	}
	free(line);
}

/* Read one line from the socket; NULL at end of stream. Caller frees. */
static char *read_line(int fd) {
	static char buf[65536];
	static size_t len;
	for (;;) {
		char *nl = memchr(buf, '\n', len);
		if (nl) {
			size_t n = (size_t)(nl - buf);
			char *line = malloc(n + 1);
			if (!line) {
				die("%s", "out of memory");
			}
			memcpy(line, buf, n);
			line[n] = '\0';
			memmove(buf, nl + 1, len - n - 1);
			len -= n + 1;
			return line;
		}
		if (len == sizeof(buf)) {
			die("%s", "reply too long");
		}
		ssize_t r = read(fd, buf + len, sizeof(buf) - len);
		if (r < 0 && errno == EINTR) {
			continue;
		}
		if (r <= 0) {
			return NULL;
		}
		len += (size_t)r;
	}
}

static json_object *call(int fd, json_object *req) {
	send_request(fd, req);
	char *line = read_line(fd);
	if (!line) {
		die("%s", "GriDE closed the connection");
	}
	json_object *resp = json_tokener_parse(line);
	free(line);
	if (!resp) {
		die("%s", "GriDE sent a reply that is not JSON");
	}
	json_object *success;
	if (!json_object_object_get_ex(resp, "success", &success) ||
			!json_object_get_boolean(success)) {
		json_object *err;
		const char *message = json_object_object_get_ex(resp, "error", &err) ?
			json_object_get_string(err) : "request failed";
		die("%s", message);
	}
	return resp;
}

static const char *str(json_object *o, const char *key) {
	json_object *v;
	if (json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string)) {
		return json_object_get_string(v);
	}
	return NULL;
}

static int num(json_object *o, const char *key) {
	json_object *v;
	return json_object_object_get_ex(o, key, &v) ? json_object_get_int(v) : 0;
}

static bool flag(json_object *o, const char *key) {
	json_object *v;
	return json_object_object_get_ex(o, key, &v) && json_object_get_boolean(v);
}

/* ---- readable output ---------------------------------------------------- */

static void print_windows(json_object *resp) {
	json_object *list;
	if (!json_object_object_get_ex(resp, "windows", &list)) {
		return;
	}
	printf("%-5s %-3s %-7s %-10s %-20s %s\n", "ID", "WS", "CELLS", "STATE", "APP", "TITLE");
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *w = json_object_array_get_idx(list, i);
		const char *state = flag(w, "fullscreen") ? "fullscreen" :
			flag(w, "minimized") ? "minimized" : flag(w, "maximized") ? "maximized" :
			flag(w, "snapped") ? "snapped" : "floating";
		const char *range = str(w, "range");
		printf("%-5d %-3d %-7s %-10s %-20.20s %s%s\n", num(w, "id"), num(w, "workspace"),
			range ? range : "-", state, str(w, "app_id") ? str(w, "app_id") : "",
			str(w, "title") ? str(w, "title") : "", flag(w, "focused") ? "  (focused)" : "");
	}
}

static void print_workspaces(json_object *resp) {
	json_object *list;
	if (!json_object_object_get_ex(resp, "workspaces", &list)) {
		return;
	}
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *w = json_object_array_get_idx(list, i);
		const char *name = str(w, "name");
		int windows = num(w, "windows");
		printf("%s Workspace %d%s%s  %d window%s\n", flag(w, "current") ? "*" : " ",
			num(w, "index"), name ? " · " : "", name ? name : "", windows,
			windows == 1 ? "" : "s");
	}
}

static void print_grid(json_object *resp) {
	const char *mode = str(resp, "mode");
	printf("Grid %d × %d, gap %dpx, %s\n", num(resp, "columns"), num(resp, "rows"),
		num(resp, "gap"), mode && strcmp(mode, "auto") == 0 ? "auto tile" : "free snap");
}

static void print_outputs(json_object *resp) {
	json_object *list;
	if (!json_object_object_get_ex(resp, "outputs", &list)) {
		return;
	}
	for (size_t i = 0; i < json_object_array_length(list); i++) {
		json_object *o = json_object_array_get_idx(list, i);
		json_object *scale;
		double s = json_object_object_get_ex(o, "scale", &scale) ?
			json_object_get_double(scale) : 1.0;
		printf("%s  %dx%d at %d,%d  scale %.2f\n", str(o, "name"), num(o, "width"),
			num(o, "height"), num(o, "x"), num(o, "y"), s);
	}
}

/* ---- building requests -------------------------------------------------- */

static json_object *request(const char *command) {
	json_object *req = json_object_new_object();
	json_object_object_add(req, "command", json_object_new_string(command));
	return req;
}

static int parse_int(const char *text, const char *what) {
	char *end;
	errno = 0;
	long v = strtol(text, &end, 10);
	if (errno || *end || end == text) {
		die("expected a number for %s", what);
	}
	return (int)v;
}

static void add_window(json_object *req, int argc, char **argv, int index) {
	if (index < argc) {
		json_object_object_add(req, "window",
			json_object_new_int(parse_int(argv[index], "the window id")));
	}
}

int main(int argc, char **argv) {
	const char *socket_path = NULL;
	int c;
	while ((c = getopt(argc, argv, "+js:h")) != -1) {
		switch (c) {
		case 'j':
			json_output = true;
			break;
		case 's':
			socket_path = optarg;
			break;
		case 'h':
			fputs(usage_text, stdout);
			return EXIT_SUCCESS;
		default:
			fputs(usage_text, stderr);
			return EXIT_FAILURE;
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		fputs(usage_text, stderr);
		return EXIT_FAILURE;
	}

	char fallback[256];
	if (!socket_path) {
		socket_path = getenv("GRIDE_SOCK");
	}
	if (!socket_path) {
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		if (!runtime) {
			die("%s", "XDG_RUNTIME_DIR is not set; pass -s <socket>");
		}
		snprintf(fallback, sizeof(fallback), "%s/gride.sock", runtime);
		socket_path = fallback;
	}

	const char *cmd = argv[0];
	json_object *req = NULL;
	void (*printer)(json_object *) = NULL;
	bool subscribe = false;

	if (strcmp(cmd, "windows") == 0) {
		req = request("get_windows");
		printer = print_windows;
	} else if (strcmp(cmd, "workspaces") == 0) {
		req = request("get_workspaces");
		printer = print_workspaces;
	} else if (strcmp(cmd, "grid") == 0 && argc == 1) {
		req = request("get_grid");
		printer = print_grid;
	} else if (strcmp(cmd, "outputs") == 0) {
		req = request("get_outputs");
		printer = print_outputs;
	} else if (strcmp(cmd, "version") == 0) {
		req = request("get_version");
	} else if (strcmp(cmd, "workspace") == 0 && argc >= 2) {
		req = request("workspace");
		const char *a = argv[1];
		if (strcmp(a, "left") == 0 || strcmp(a, "right") == 0 || strcmp(a, "up") == 0 ||
				strcmp(a, "down") == 0) {
			json_object_object_add(req, "direction", json_object_new_string(a));
		} else {
			json_object_object_add(req, "index",
				json_object_new_int(parse_int(a, "the workspace")));
		}
	} else if (strcmp(cmd, "move-to-workspace") == 0 && argc >= 2) {
		req = request("move_to_workspace");
		json_object_object_add(req, "index",
			json_object_new_int(parse_int(argv[1], "the workspace")));
		add_window(req, argc, argv, 2);
	} else if (strcmp(cmd, "workspace-name") == 0 && argc >= 2) {
		req = request("set_workspace_name");
		json_object_object_add(req, "index",
			json_object_new_int(parse_int(argv[1], "the workspace")));
		if (argc >= 3) {
			json_object_object_add(req, "name", json_object_new_string(argv[2]));
		}
	} else if (strcmp(cmd, "focus") == 0 && argc >= 2) {
		req = request("focus");
		add_window(req, argc, argv, 1);
	} else if (strcmp(cmd, "close") == 0) {
		req = request("close");
		add_window(req, argc, argv, 1);
	} else if (strcmp(cmd, "snap") == 0 && argc >= 2) {
		req = request("snap");
		json_object_object_add(req, "range", json_object_new_string(argv[1]));
		add_window(req, argc, argv, 2);
	} else if ((strcmp(cmd, "move") == 0 || strcmp(cmd, "resize") == 0) && argc >= 3) {
		req = request(strcmp(cmd, "move") == 0 ? "cell_move" : "cell_resize");
		json_object_object_add(req, "dx", json_object_new_int(parse_int(argv[1], "dx")));
		json_object_object_add(req, "dy", json_object_new_int(parse_int(argv[2], "dy")));
		add_window(req, argc, argv, 3);
	} else if (strcmp(cmd, "maximize") == 0 || strcmp(cmd, "minimize") == 0 ||
			strcmp(cmd, "fullscreen") == 0) {
		req = request(cmd);
		int next = 1;
		if (argc >= 2 && (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0 ||
				strcmp(argv[1], "toggle") == 0)) {
			json_object_object_add(req, "state", json_object_new_string(argv[1]));
			next = 2;
		}
		add_window(req, argc, argv, next);
	} else if (strcmp(cmd, "snap-layouts") == 0) {
		req = request("snap_layouts");
		add_window(req, argc, argv, 1);
	} else if (strcmp(cmd, "grid") == 0 && argc >= 3 && strcmp(argv[1], "set") == 0) {
		int cols, rows;
		char extra;
		if (sscanf(argv[2], "%dx%d%c", &cols, &rows, &extra) != 2) {
			die("grid size must look like 4x3, not %s", argv[2]);
		}
		req = request("set_grid");
		json_object_object_add(req, "columns", json_object_new_int(cols));
		json_object_object_add(req, "rows", json_object_new_int(rows));
	} else if (strcmp(cmd, "grid") == 0 && argc >= 3 && strcmp(argv[1], "mode") == 0) {
		req = request("set_grid");
		json_object_object_add(req, "mode", json_object_new_string(argv[2]));
	} else if (strcmp(cmd, "spawn") == 0 && argc >= 2) {
		/* Join the rest back into one shell command. */
		size_t len = 1;
		for (int i = 1; i < argc; i++) {
			len += strlen(argv[i]) + 1;
		}
		char *joined = calloc(1, len);
		if (!joined) {
			die("%s", "out of memory");
		}
		for (int i = 1; i < argc; i++) {
			if (i > 1) {
				strcat(joined, " ");
			}
			strcat(joined, argv[i]);
		}
		req = request("spawn");
		json_object_object_add(req, "command", json_object_new_string(joined));
		free(joined);
	} else if (strcmp(cmd, "subscribe") == 0 && argc >= 2) {
		req = request("subscribe");
		json_object *events = json_object_new_array();
		for (int i = 1; i < argc; i++) {
			json_object_array_add(events, json_object_new_string(argv[i]));
		}
		json_object_object_add(req, "events", events);
		subscribe = true;
	} else if (strcmp(cmd, "raw") == 0 && argc >= 2) {
		req = json_tokener_parse(argv[1]);
		if (!req || !json_object_is_type(req, json_type_object)) {
			die("%s", "raw needs one JSON object");
		}
		json_output = true;
	} else if (strcmp(cmd, "exit") == 0) {
		req = request("exit");
	} else {
		fputs(usage_text, stderr);
		return EXIT_FAILURE;
	}

	int fd = connect_socket(socket_path);
	json_object *resp = call(fd, req);
	json_object_put(req);

	if (subscribe) {
		/* One event per line until GriDE goes away or we are interrupted. */
		json_object_put(resp);
		setvbuf(stdout, NULL, _IOLBF, 0);
		char *line;
		while ((line = read_line(fd))) {
			puts(line);
			free(line);
		}
		close(fd);
		return EXIT_SUCCESS;
	}

	if (json_output) {
		puts(json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PRETTY));
	} else if (printer) {
		printer(resp);
	} else if (strcmp(cmd, "version") == 0) {
		printf("GriDE %s (wlroots %s)\n", str(resp, "version"), str(resp, "wlroots"));
	}
	json_object_put(resp);
	close(fd);
	return EXIT_SUCCESS;
}
