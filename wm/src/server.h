/*
 * gride-wm: shared types.
 *
 * Ownership in short:
 *  - struct gride_server lives on main()'s stack for the whole run.
 *  - Every other struct here is heap-allocated when wlroots announces the
 *    object (new_output, new_toplevel, new_input...) and freed in the
 *    handler for that object's `destroy` signal. A struct never outlives
 *    the wlroots object it wraps.
 *  - Every wl_listener is removed (wl_list_remove) before its struct is
 *    freed. A freed listener left in a signal list is a use-after-free the
 *    next time the signal fires.
 */
#ifndef GRIDE_SERVER_H
#define GRIDE_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#include "gride-tokens.h"

struct wlr_allocator;
struct wlr_backend;
struct wlr_cursor;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_output;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_scene_output_layout;
struct wlr_scene_tree;
struct wlr_seat;
struct wlr_session;
struct wlr_surface;
struct wlr_xcursor_manager;
struct wlr_xdg_popup;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;

enum gride_cursor_mode {
	GRIDE_CURSOR_PASSTHROUGH,
	GRIDE_CURSOR_MOVE,
	GRIDE_CURSOR_RESIZE,
};

/* Options from the command line. Phase 6 adds config.toml on top. */
struct gride_options {
	const char *startup_cmd; /* -s: run once the compositor is up */
	const char *terminal;    /* -t: command for Super+Enter */
	uint32_t mod_mask;       /* WLR_MODIFIER_LOGO, or ALT for nested dev */
	bool dark;               /* palette choice */
};

struct gride_server {
	struct wl_display *wl_display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_session *session; /* NULL when nested */
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	const struct gride_options *opts;
	const struct gride_palette *palette;

	/*
	 * Scene graph. wlroots renders the tree back to front, so the order
	 * these trees are created in is the stacking order:
	 *   background  desktop fill and dot grid, one node per output
	 *   windows     one gride_view tree per toplevel; popups nest inside
	 *   overlay     reserved: grid overlay (phase 2), flyouts (phase 3)
	 * The trees are owned by the scene and destroyed with it.
	 */
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct {
		struct wlr_scene_tree *background;
		struct wlr_scene_tree *windows;
		struct wlr_scene_tree *overlay;
	} layers;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs; /* gride_output.link */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	/* Mapped views, front-to-back: the first entry is on top. */
	struct wl_list views; /* gride_view.link */
	struct gride_view *focused_view;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_list keyboards; /* gride_keyboard.link */
	/* Virtual input from clients: on-screen keyboards, remote desktop
	 * (wayvnc), automation (wtype, wlrctl). */
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;

	/* Interactive move/resize state. */
	enum gride_cursor_mode cursor_mode;
	struct gride_view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;
	/* True when the grab came from a Super+drag rather than the client:
	 * the button release then belongs to us and is not sent on. */
	bool grab_is_ours;

	struct wl_event_source *sigint_source;
	struct wl_event_source *sigterm_source;
};

struct gride_output {
	struct wl_list link;
	struct gride_server *server;
	struct wlr_output *wlr_output;
	struct gride_background *background;

	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

/*
 * A toplevel window.
 *
 * Coordinates: (x, y) is where the window's *visible content* (its xdg
 * window geometry) sits in layout space, not the surface origin. Clients
 * that draw their own shadows put them outside the geometry, so this is
 * the box the grid engine snaps in phase 2. scene_tree sits at (x, y);
 * wlroots offsets the surfaces inside surface_tree by (-geometry.x,
 * -geometry.y), so the content's top-left lands on (x, y).
 */
struct gride_view {
	struct wl_list link;
	struct gride_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;   /* ours; node.data = this view */
	struct wlr_scene_tree *surface_tree; /* the xdg surface subtree */
	int x, y;
	bool mapped;
	bool maximized;
	bool fullscreen;
	/* Floating geometry to go back to after maximise/fullscreen. */
	struct wlr_box saved;
	bool has_saved;
	bool center_pending; /* centre on the next commit (client picks size) */

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct gride_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
};

struct gride_keyboard {
	struct wl_list link;
	struct gride_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* server.c */
bool server_init(struct gride_server *server, const struct gride_options *opts);
bool server_start(struct gride_server *server);
void server_run(struct gride_server *server);
void server_finish(struct gride_server *server);

/* output.c */
void output_handle_new(struct wl_listener *listener, void *data);
void output_handle_layout_change(struct wl_listener *listener, void *data);
struct wlr_output *output_at_cursor(struct gride_server *server);
struct wlr_output *output_for_box(struct gride_server *server, const struct wlr_box *box);
void output_get_box(struct gride_server *server, struct wlr_output *output, struct wlr_box *box);

/* background.c */
struct gride_background *background_create(struct wlr_scene_tree *parent);
void background_update(struct gride_background *bg, const struct gride_palette *palette,
	struct wlr_output *output, const struct wlr_box *layout_box);
void background_destroy(struct gride_background *bg);

/* view.c */
void view_handle_new_toplevel(struct wl_listener *listener, void *data);
void view_handle_new_popup(struct wl_listener *listener, void *data);
void view_focus(struct gride_view *view);
struct gride_view *view_at(struct gride_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);
void view_get_geometry(struct gride_view *view, struct wlr_box *box);
void view_move_to(struct gride_view *view, int x, int y);
void view_set_size(struct gride_view *view, int width, int height);
void view_set_maximized(struct gride_view *view, bool maximized);
void view_set_fullscreen(struct gride_view *view, bool fullscreen, struct wlr_output *output);
void view_close(struct gride_view *view);
void views_arrange(struct gride_server *server);

/* input.c */
void input_init(struct gride_server *server);
void input_finish(struct gride_server *server);
void input_handle_new(struct wl_listener *listener, void *data);
void seat_handle_request_cursor(struct wl_listener *listener, void *data);
void seat_handle_request_set_selection(struct wl_listener *listener, void *data);
void seat_handle_request_set_primary_selection(struct wl_listener *listener, void *data);

/* cursor.c */
void cursor_init(struct gride_server *server);
void cursor_finish(struct gride_server *server);
void cursor_begin_interactive(struct gride_view *view, enum gride_cursor_mode mode,
	uint32_t edges, bool ours);
void cursor_end_interactive(struct gride_server *server);
void cursor_rebase(struct gride_server *server, uint32_t time_msec);

/* spawn.c */
void spawn(const char *cmd);
const char *find_terminal(const struct gride_options *opts);

/* log.c */
const char *gride_log_init(bool debug);
void gride_log_finish(void);

#endif
