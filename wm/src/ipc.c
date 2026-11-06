/*
 * IPC: a Unix socket at $XDG_RUNTIME_DIR/gride.sock speaking newline-
 * delimited JSON. gride-msg and the shell (phase 4) use it.
 *
 *   request   {"command": "snap", "range": "A1:B2"}\n
 *   response  {"success": true}\n            or {"success": false, "error": "..."}\n
 *   events    {"event": "window", "change": "focus", "window": {...}}\n
 *             (after {"command": "subscribe", "events": ["window", ...]})
 *
 * Workspace numbers are 1-9 on the wire, as people say them.
 * If gride.sock is already served by another GriDE (a nested session next
 * to a real one), this one uses gride-$WAYLAND_DISPLAY.sock. Either way the
 * path is exported as $GRIDE_SOCK to the programs GriDE starts.
 */
#include <errno.h>
#include <fcntl.h>
#include <json.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <wlr/version.h>

#include "server.h"

#define MAX_REQUEST (64 * 1024)      /* one request line */
#define MAX_PENDING (4 * 1024 * 1024) /* unsent output before we drop a client */

enum ipc_event_mask {
	IPC_EVENT_WINDOW = 1 << 0,
	IPC_EVENT_WORKSPACE = 1 << 1,
	IPC_EVENT_GRID = 1 << 2,
};

struct gride_ipc {
	struct gride_server *server;
	int fd;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	struct wl_event_source *source;
	struct wl_list clients; /* ipc_client.link */
};

struct ipc_client {
	struct wl_list link;
	struct gride_ipc *ipc;
	int fd;
	struct wl_event_source *source;
	char *in;
	size_t in_len, in_cap;
	char *out;
	size_t out_len, out_cap;
	uint32_t events;
	/* Failed while another client's request was being handled: shut down,
	 * freed when its own event or request handler next runs. */
	bool dead;
};

static void client_destroy(struct ipc_client *client) {
	wl_list_remove(&client->link);
	wl_event_source_remove(client->source);
	close(client->fd);
	free(client->in);
	free(client->out);
	free(client);
}

/* ---- output ------------------------------------------------------------- */

/* Write as much as the socket takes; the rest waits for WRITABLE. */
static bool client_flush(struct ipc_client *client) {
	while (client->out_len > 0) {
		ssize_t n = write(client->fd, client->out, client->out_len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		memmove(client->out, client->out + n, client->out_len - (size_t)n);
		client->out_len -= (size_t)n;
	}
	uint32_t mask = WL_EVENT_READABLE;
	if (client->out_len > 0) {
		mask |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(client->source, mask);
	return true;
}

/* Queue one JSON object as a line. Returns false if the client is gone. */
static bool client_send(struct ipc_client *client, json_object *obj) {
	if (client->dead) {
		return false;
	}
	const char *text = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
	size_t len = strlen(text);
	if (client->out_len + len + 1 > MAX_PENDING) {
		wlr_log(WLR_INFO, "IPC client is not reading its events; disconnecting it");
		return false;
	}
	if (client->out_len + len + 1 > client->out_cap) {
		size_t cap = client->out_cap ? client->out_cap : 4096;
		while (cap < client->out_len + len + 1) {
			cap *= 2;
		}
		char *out = realloc(client->out, cap);
		if (!out) {
			return false;
		}
		client->out = out;
		client->out_cap = cap;
	}
	memcpy(client->out + client->out_len, text, len);
	client->out[client->out_len + len] = '\n';
	client->out_len += len + 1;
	return client_flush(client);
}

/* ---- describing state --------------------------------------------------- */

static json_object *window_json(struct gride_view *view) {
	struct gride_server *server = view->server;
	json_object *o = json_object_new_object();
	struct wlr_box frame;
	view_get_frame(view, &frame);
	json_object_object_add(o, "id", json_object_new_int64(view->id));
	json_object_object_add(o, "title", json_object_new_string(view_title(view)));
	json_object_object_add(o, "app_id", json_object_new_string(view_app_id(view)));
	json_object_object_add(o, "workspace", json_object_new_int(view->workspace + 1));
	json_object_object_add(o, "focused", json_object_new_boolean(server->focused_view == view));
	json_object_object_add(o, "minimized", json_object_new_boolean(view->minimized));
	json_object_object_add(o, "maximized", json_object_new_boolean(view->maximized));
	json_object_object_add(o, "fullscreen", json_object_new_boolean(view->fullscreen));
	json_object_object_add(o, "snapped", json_object_new_boolean(view->snapped));
	if (view->snapped) {
		char ids[16], name[48];
		grid_range_label(server, &view->range, ids, sizeof(ids), name, sizeof(name));
		json_object_object_add(o, "range", json_object_new_string(ids));
		json_object_object_add(o, "layout", json_object_new_string(name));
	} else {
		json_object_object_add(o, "range", NULL);
		json_object_object_add(o, "layout", NULL);
	}
	struct wlr_output *output = view->mapped ? view_get_output(view) : NULL;
	json_object_object_add(o, "output", output ? json_object_new_string(output->name) : NULL);
	json_object_object_add(o, "x", json_object_new_int(frame.x));
	json_object_object_add(o, "y", json_object_new_int(frame.y));
	json_object_object_add(o, "width", json_object_new_int(frame.width));
	json_object_object_add(o, "height", json_object_new_int(frame.height));
	json_object_object_add(o, "decorated", json_object_new_boolean(view->ssd));
	json_object_object_add(o, "xwayland",
		json_object_new_boolean(view->type == GRIDE_VIEW_XWAYLAND));
	return o;
}

static json_object *workspaces_json(struct gride_server *server) {
	json_object *o = json_object_new_object();
	json_object_object_add(o, "current", json_object_new_int(server->workspace + 1));
	json_object_object_add(o, "columns", json_object_new_int(GRIDE_WORKSPACE_COLUMNS));
	json_object_object_add(o, "rows", json_object_new_int(GRIDE_WORKSPACE_ROWS));
	json_object *list = json_object_new_array();
	for (int i = 0; i < GRIDE_WORKSPACES; i++) {
		json_object *w = json_object_new_object();
		const char *name = workspace_name(server, i);
		json_object_object_add(w, "index", json_object_new_int(i + 1));
		json_object_object_add(w, "name", name ? json_object_new_string(name) : NULL);
		json_object_object_add(w, "windows",
			json_object_new_int(workspace_window_count(server, i)));
		json_object_object_add(w, "current", json_object_new_boolean(i == server->workspace));
		json_object_array_add(list, w);
	}
	json_object_object_add(o, "workspaces", list);
	return o;
}

static json_object *grid_json(struct gride_server *server) {
	json_object *o = json_object_new_object();
	json_object_object_add(o, "columns", json_object_new_int(server->grid_columns));
	json_object_object_add(o, "rows", json_object_new_int(server->grid_rows));
	json_object_object_add(o, "gap", json_object_new_int(GRIDE_SPACE_3));
	json_object_object_add(o, "mode", json_object_new_string(
		server->grid_mode == GRIDE_GRID_AUTO_TILE ? "auto" : "free"));
	return o;
}

static json_object *outputs_json(struct gride_server *server) {
	json_object *list = json_object_new_array();
	struct gride_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		output_get_box(server, output->wlr_output, &box);
		json_object *o = json_object_new_object();
		json_object_object_add(o, "name", json_object_new_string(output->wlr_output->name));
		json_object_object_add(o, "x", json_object_new_int(box.x));
		json_object_object_add(o, "y", json_object_new_int(box.y));
		json_object_object_add(o, "width", json_object_new_int(box.width));
		json_object_object_add(o, "height", json_object_new_int(box.height));
		json_object_object_add(o, "scale", json_object_new_double(output->wlr_output->scale));
		json_object_array_add(list, o);
	}
	return list;
}

/* ---- commands ----------------------------------------------------------- */

static json_object *ok(void) {
	json_object *o = json_object_new_object();
	json_object_object_add(o, "success", json_object_new_boolean(true));
	return o;
}

static json_object *fail(const char *message) {
	json_object *o = json_object_new_object();
	json_object_object_add(o, "success", json_object_new_boolean(false));
	json_object_object_add(o, "error", json_object_new_string(message));
	return o;
}

static const char *get_string(json_object *req, const char *key) {
	json_object *v;
	if (json_object_object_get_ex(req, key, &v) && json_object_is_type(v, json_type_string)) {
		return json_object_get_string(v);
	}
	return NULL;
}

static bool get_int(json_object *req, const char *key, int *out) {
	json_object *v;
	if (json_object_object_get_ex(req, key, &v) && json_object_is_type(v, json_type_int)) {
		*out = json_object_get_int(v);
		return true;
	}
	return false;
}

/* The window a request names with "window", else the focused one. */
static struct gride_view *target_view(struct gride_server *server, json_object *req,
		const char **error) {
	int id;
	if (get_int(req, "window", &id)) {
		struct gride_view *view = id > 0 ? view_by_id(server, (uint32_t)id) : NULL;
		if (!view || !view->mapped) {
			*error = "No window with that id";
			return NULL;
		}
		return view;
	}
	if (!server->focused_view) {
		*error = "No focused window";
		return NULL;
	}
	return server->focused_view;
}

/* "on", "off" or "toggle" (the default) against the current state. */
static bool want_state(json_object *req, bool current, bool *ok_out) {
	const char *state = get_string(req, "state");
	*ok_out = true;
	if (!state || strcmp(state, "toggle") == 0) {
		return !current;
	}
	if (strcmp(state, "on") == 0) {
		return true;
	}
	if (strcmp(state, "off") == 0) {
		return false;
	}
	*ok_out = false;
	return current;
}

static json_object *handle_request(struct ipc_client *client, json_object *req) {
	struct gride_server *server = client->ipc->server;
	const char *cmd = get_string(req, "command");
	const char *error = NULL;
	if (!cmd) {
		return fail("Missing \"command\"");
	}

	if (strcmp(cmd, "get_version") == 0) {
		json_object *o = ok();
		json_object_object_add(o, "version", json_object_new_string(GRIDE_VERSION));
		json_object_object_add(o, "wlroots", json_object_new_string(WLR_VERSION_STR));
		return o;
	}
	if (strcmp(cmd, "get_windows") == 0) {
		json_object *o = ok();
		json_object *list = json_object_new_array();
		struct gride_view *view;
		/* Back to front, the order a taskbar wants for stable buttons. */
		wl_list_for_each_reverse(view, &server->views, link) {
			if (view->mapped) {
				json_object_array_add(list, window_json(view));
			}
		}
		json_object_object_add(o, "windows", list);
		return o;
	}
	if (strcmp(cmd, "get_workspaces") == 0) {
		json_object *o = workspaces_json(server);
		json_object_object_add(o, "success", json_object_new_boolean(true));
		return o;
	}
	if (strcmp(cmd, "get_grid") == 0) {
		json_object *o = grid_json(server);
		json_object_object_add(o, "success", json_object_new_boolean(true));
		return o;
	}
	if (strcmp(cmd, "get_outputs") == 0) {
		json_object *o = ok();
		json_object_object_add(o, "outputs", outputs_json(server));
		return o;
	}
	if (strcmp(cmd, "subscribe") == 0) {
		json_object *events;
		if (!json_object_object_get_ex(req, "events", &events) ||
				!json_object_is_type(events, json_type_array)) {
			return fail("\"events\" must be a list: window, workspace, grid");
		}
		uint32_t mask = 0;
		for (size_t i = 0; i < json_object_array_length(events); i++) {
			const char *name = json_object_get_string(json_object_array_get_idx(events, i));
			if (name && strcmp(name, "window") == 0) {
				mask |= IPC_EVENT_WINDOW;
			} else if (name && strcmp(name, "workspace") == 0) {
				mask |= IPC_EVENT_WORKSPACE;
			} else if (name && strcmp(name, "grid") == 0) {
				mask |= IPC_EVENT_GRID;
			} else {
				return fail("Unknown event type (use window, workspace, grid)");
			}
		}
		client->events |= mask;
		return ok();
	}
	if (strcmp(cmd, "workspace") == 0) {
		int index;
		const char *dir = get_string(req, "direction");
		if (get_int(req, "index", &index)) {
			if (index < 1 || index > GRIDE_WORKSPACES) {
				return fail("Workspace index must be 1-9");
			}
			workspace_switch(server, index - 1);
		} else if (dir) {
			int dx = strcmp(dir, "left") == 0 ? -1 : strcmp(dir, "right") == 0 ? 1 : 0;
			int dy = strcmp(dir, "up") == 0 ? -1 : strcmp(dir, "down") == 0 ? 1 : 0;
			if (!dx && !dy) {
				return fail("Direction must be left, right, up or down");
			}
			workspace_switch_direction(server, dx, dy);
		} else {
			return fail("Give \"index\" (1-9) or \"direction\"");
		}
		return ok();
	}
	if (strcmp(cmd, "set_workspace_name") == 0) {
		int index;
		if (!get_int(req, "index", &index) || index < 1 || index > GRIDE_WORKSPACES) {
			return fail("Workspace index must be 1-9");
		}
		workspace_set_name(server, index - 1, get_string(req, "name"));
		return ok();
	}
	if (strcmp(cmd, "set_grid") == 0) {
		int cols, rows;
		bool has_size = get_int(req, "columns", &cols) && get_int(req, "rows", &rows);
		const char *mode = get_string(req, "mode");
		if (!has_size && !mode) {
			return fail("Give \"columns\" and \"rows\", and/or \"mode\"");
		}
		if (has_size && !grid_set_size(server, cols, rows)) {
			return fail("Grid must be 1-8 columns by 1-6 rows");
		}
		if (mode) {
			if (strcmp(mode, "free") == 0) {
				grid_set_mode(server, GRIDE_GRID_FREE_SNAP);
			} else if (strcmp(mode, "auto") == 0) {
				grid_set_mode(server, GRIDE_GRID_AUTO_TILE);
			} else {
				return fail("Mode must be free or auto");
			}
		}
		return ok();
	}
	if (strcmp(cmd, "spawn") == 0) {
		const char *command = get_string(req, "command");
		if (!command || !command[0]) {
			return fail("Missing \"command\"");
		}
		spawn(command);
		return ok();
	}
	if (strcmp(cmd, "exit") == 0) {
		wlr_log(WLR_INFO, "Exit requested over IPC");
		wl_display_terminate(server->wl_display);
		return ok();
	}

	/* Everything below acts on one window. */
	struct gride_view *view = target_view(server, req, &error);
	if (strcmp(cmd, "focus") == 0) {
		if (!view) {
			return fail(error);
		}
		if (view->minimized) {
			view_set_minimized(view, false);
		}
		view_focus(view);
		return ok();
	}
	if (strcmp(cmd, "close") == 0) {
		if (!view) {
			return fail(error);
		}
		view_close(view);
		return ok();
	}
	if (strcmp(cmd, "move_to_workspace") == 0) {
		int index;
		if (!view) {
			return fail(error);
		}
		if (!get_int(req, "index", &index) || index < 1 || index > GRIDE_WORKSPACES) {
			return fail("Workspace index must be 1-9");
		}
		workspace_move_view(view, index - 1);
		return ok();
	}
	if (strcmp(cmd, "snap") == 0) {
		const char *text = get_string(req, "range");
		struct gride_range range;
		if (!view) {
			return fail(error);
		}
		if (!text || !grid_parse_range(server, text, &range)) {
			return fail("Range must be cells on the grid, like A1:B2");
		}
		if (!view_is_snappable(view)) {
			return fail("That window cannot be snapped (dialog or fixed size)");
		}
		if (!view_on_current_workspace(view)) {
			workspace_switch(server, view->workspace);
		}
		grid_snap_view(view, view_get_output(view), &range, true);
		return ok();
	}
	if (strcmp(cmd, "cell_move") == 0 || strcmp(cmd, "cell_resize") == 0) {
		int dx = 0, dy = 0;
		if (!view) {
			return fail(error);
		}
		get_int(req, "dx", &dx);
		get_int(req, "dy", &dy);
		if (strcmp(cmd, "cell_move") == 0) {
			grid_move_view(view, dx, dy);
		} else {
			grid_resize_view(view, dx, dy);
		}
		return ok();
	}
	if (strcmp(cmd, "maximize") == 0 || strcmp(cmd, "minimize") == 0 ||
			strcmp(cmd, "fullscreen") == 0) {
		bool valid;
		if (!view) {
			return fail(error);
		}
		if (strcmp(cmd, "maximize") == 0) {
			bool want = want_state(req, view->maximized, &valid);
			if (valid) {
				view_set_maximized(view, want);
			}
		} else if (strcmp(cmd, "minimize") == 0) {
			bool want = want_state(req, view->minimized, &valid);
			if (valid) {
				view_set_minimized(view, want);
			}
		} else {
			bool want = want_state(req, view->fullscreen, &valid);
			if (valid) {
				view_set_fullscreen(view, want, NULL);
			}
		}
		return valid ? ok() : fail("State must be on, off or toggle");
	}
	if (strcmp(cmd, "snap_layouts") == 0) {
		if (!view) {
			return fail(error);
		}
		if (!view_is_snappable(view)) {
			return fail("That window cannot be snapped (dialog or fixed size)");
		}
		flyout_open(view);
		return ok();
	}
	return fail("Unknown command");
}

/* ---- input -------------------------------------------------------------- */

/* Handle every complete line in the input buffer. */
static bool client_process(struct ipc_client *client) {
	char *start = client->in;
	char *end = client->in + client->in_len;
	char *nl;
	while ((nl = memchr(start, '\n', (size_t)(end - start)))) {
		*nl = '\0';
		if (nl > start) {
			json_tokener *tok = json_tokener_new();
			json_object *req = json_tokener_parse_ex(tok, start, (int)(nl - start));
			bool parsed = req && json_tokener_get_error(tok) == json_tokener_success &&
				json_object_is_type(req, json_type_object);
			json_tokener_free(tok);
			json_object *resp = parsed ? handle_request(client, req) :
				fail("Request must be one JSON object per line");
			json_object_put(req);
			bool alive = !client->dead && client_send(client, resp);
			json_object_put(resp);
			if (!alive) {
				return false;
			}
		}
		start = nl + 1;
	}
	size_t rest = (size_t)(end - start);
	memmove(client->in, start, rest);
	client->in_len = rest;
	return true;
}

static int client_handle_event(int fd, uint32_t mask, void *data) {
	struct ipc_client *client = data;
	if (client->dead || (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP))) {
		client_destroy(client);
		return 0;
	}
	if (mask & WL_EVENT_WRITABLE) {
		if (!client_flush(client)) {
			client_destroy(client);
			return 0;
		}
	}
	if (!(mask & WL_EVENT_READABLE)) {
		return 0;
	}
	if (client->in_cap - client->in_len < 4096) {
		size_t cap = client->in_cap ? client->in_cap * 2 : 8192;
		if (cap > MAX_REQUEST * 2) {
			wlr_log(WLR_INFO, "IPC request too long; disconnecting the client");
			client_destroy(client);
			return 0;
		}
		char *in = realloc(client->in, cap);
		if (!in) {
			client_destroy(client);
			return 0;
		}
		client->in = in;
		client->in_cap = cap;
	}
	ssize_t n = read(fd, client->in + client->in_len, client->in_cap - client->in_len - 1);
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			return 0;
		}
		client_destroy(client); /* closed */
		return 0;
	}
	client->in_len += (size_t)n;
	if (!client_process(client)) {
		client_destroy(client);
	}
	return 0;
}

static int handle_accept(int fd, uint32_t mask, void *data) {
	struct gride_ipc *ipc = data;
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) {
		wlr_log_errno(WLR_ERROR, "IPC accept failed");
		return 0;
	}
	/* Close on exec (spawned programs must not inherit it), non-blocking. */
	if (fcntl(cfd, F_SETFD, FD_CLOEXEC) != 0 ||
			fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL) | O_NONBLOCK) != 0) {
		close(cfd);
		return 0;
	}
	struct ipc_client *client = calloc(1, sizeof(*client));
	if (!client) {
		close(cfd);
		return 0;
	}
	client->ipc = ipc;
	client->fd = cfd;
	client->source = wl_event_loop_add_fd(ipc->server->event_loop, cfd, WL_EVENT_READABLE,
		client_handle_event, client);
	if (!client->source) {
		close(cfd);
		free(client);
		return 0;
	}
	wl_list_insert(&ipc->clients, &client->link);
	return 0;
}

/* ---- events ------------------------------------------------------------- */

static void broadcast(struct gride_server *server, uint32_t kind, json_object *(*build)(
		struct gride_server *, void *), void *arg) {
	struct gride_ipc *ipc = server->ipc;
	if (!ipc) {
		return;
	}
	json_object *event = NULL;
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
		if (!(client->events & kind)) {
			continue;
		}
		if (!event) {
			event = build(server, arg); /* only built when someone listens */
		}
		if (!client_send(client, event)) {
			/* Never free here: this may be the client whose request is
			 * being handled. The hang-up wakes its handler, which frees it. */
			client->dead = true;
			shutdown(client->fd, SHUT_RDWR);
		}
	}
	json_object_put(event);
}

struct window_event {
	struct gride_view *view;
	const char *change;
};

static json_object *build_window_event(struct gride_server *server, void *arg) {
	struct window_event *we = arg;
	json_object *o = json_object_new_object();
	json_object_object_add(o, "event", json_object_new_string("window"));
	json_object_object_add(o, "change", json_object_new_string(we->change));
	json_object_object_add(o, "window", window_json(we->view));
	return o;
}

/* {"event": name, ...fields of body}: the event name comes first so a
 * reader can dispatch on the start of the line. Takes ownership of body. */
static json_object *event_object(const char *name, json_object *body) {
	json_object *o = json_object_new_object();
	json_object_object_add(o, "event", json_object_new_string(name));
	json_object_object_foreach(body, key, value) {
		json_object_object_add(o, key, json_object_get(value));
	}
	json_object_put(body);
	return o;
}

static json_object *build_workspace_event(struct gride_server *server, void *arg) {
	return event_object("workspace", workspaces_json(server));
}

static json_object *build_grid_event(struct gride_server *server, void *arg) {
	return event_object("grid", grid_json(server));
}

void ipc_event_window(struct gride_server *server, struct gride_view *view, const char *change) {
	struct window_event we = { view, change };
	broadcast(server, IPC_EVENT_WINDOW, build_window_event, &we);
}

void ipc_event_workspace(struct gride_server *server) {
	broadcast(server, IPC_EVENT_WORKSPACE, build_workspace_event, NULL);
}

void ipc_event_grid(struct gride_server *server) {
	broadcast(server, IPC_EVENT_GRID, build_grid_event, NULL);
}

/* ---- setup -------------------------------------------------------------- */

/* True when a live GriDE already answers on this path. */
static bool socket_in_use(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return false;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	bool live = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
	close(fd);
	return live;
}

bool ipc_init(struct gride_server *server) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) {
		return false;
	}
	struct gride_ipc *ipc = calloc(1, sizeof(*ipc));
	if (!ipc) {
		return false;
	}
	ipc->server = server;
	wl_list_init(&ipc->clients);

	int len = snprintf(ipc->path, sizeof(ipc->path), "%s/gride.sock", runtime);
	if (len > 0 && (size_t)len < sizeof(ipc->path) && socket_in_use(ipc->path)) {
		const char *display = getenv("WAYLAND_DISPLAY");
		len = snprintf(ipc->path, sizeof(ipc->path), "%s/gride-%s.sock", runtime,
			display ? display : "nested");
	}
	if (len < 0 || (size_t)len >= sizeof(ipc->path)) {
		wlr_log(WLR_ERROR, "IPC socket path is too long; gride-msg will not work");
		free(ipc);
		return false;
	}
	unlink(ipc->path); /* a stale socket from a crashed session */

	ipc->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc->path);
	if (ipc->fd < 0 || bind(ipc->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
			chmod(ipc->path, 0600) != 0 || listen(ipc->fd, 16) != 0) {
		wlr_log_errno(WLR_ERROR, "Cannot open the IPC socket %s", ipc->path);
		if (ipc->fd >= 0) {
			close(ipc->fd);
		}
		free(ipc);
		return false;
	}
	ipc->source = wl_event_loop_add_fd(server->event_loop, ipc->fd, WL_EVENT_READABLE,
		handle_accept, ipc);
	server->ipc = ipc;
	setenv("GRIDE_SOCK", ipc->path, true);
	wlr_log(WLR_INFO, "IPC on %s", ipc->path);
	return true;
}

void ipc_finish(struct gride_server *server) {
	struct gride_ipc *ipc = server->ipc;
	if (!ipc) {
		return;
	}
	struct ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
		client_destroy(client);
	}
	if (ipc->source) {
		wl_event_source_remove(ipc->source);
	}
	close(ipc->fd);
	unlink(ipc->path);
	free(ipc);
	server->ipc = NULL;
}
