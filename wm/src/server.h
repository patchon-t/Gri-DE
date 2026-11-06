/*
 * gride-wm: shared types.
 *
 * Ownership in short:
 *  - struct gride_server lives on main()'s stack for the whole run.
 *  - Every other struct here is heap-allocated when wlroots announces the
 *    object (new_output, new_toplevel, new_surface, new_input...) and freed
 *    in the handler for that object's `destroy` signal. A struct never
 *    outlives the wlroots object it wraps.
 *  - Every wl_listener is removed (wl_list_remove) before its struct is
 *    freed. A freed listener left in a signal list is a use-after-free the
 *    next time the signal fires.
 */
#ifndef GRIDE_SERVER_H
#define GRIDE_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#include "gride-tokens.h"

struct wlr_allocator;
struct wlr_backend;
struct wlr_cursor;
struct wlr_input_device;
struct wlr_keyboard;
struct wlr_output;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_scene_node;
struct wlr_compositor;
struct wlr_scene_output_layout;
struct wlr_scene_rect;
struct wlr_scene_tree;
struct wlr_seat;
struct wlr_session;
struct wlr_surface;
struct wlr_xcursor_manager;
struct wlr_xdg_decoration_manager_v1;
struct wlr_xdg_popup;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct draw_buffer;
struct gride_deco;
struct gride_flyout;
struct gride_grid_overlay;
struct gride_ipc;

/* ---- sizes from the design spec that tokens.json does not carry --------- */

#define GRIDE_TITLEBAR_HEIGHT 36
#define GRIDE_TITLEBAR_BUTTON_WIDTH 46
#define GRIDE_TITLEBAR_GLYPH 14
#define GRIDE_TITLEBAR_GLYPH_STROKE 1.8
#define GRIDE_TITLEBAR_ICON 18
#define GRIDE_SNAP_DURATION_MS 180
/* Invisible band outside a decorated window where dragging resizes it. */
#define GRIDE_RESIZE_BORDER 6
/* Hovering Maximise this long opens the snap layouts flyout. */
#define GRIDE_SNAP_FLYOUT_HOVER_MS 400
#define GRIDE_SNAP_FLYOUT_WIDTH 380
/* Workspaces form a 3x3 grid. */
#define GRIDE_WORKSPACE_COLUMNS 3
#define GRIDE_WORKSPACE_ROWS 3
#define GRIDE_WORKSPACES (GRIDE_WORKSPACE_COLUMNS * GRIDE_WORKSPACE_ROWS)
#define GRIDE_GRID_MAX_COLUMNS 8
#define GRIDE_GRID_MAX_ROWS 6

enum gride_cursor_mode {
	GRIDE_CURSOR_PASSTHROUGH,
	GRIDE_CURSOR_MOVE,
	GRIDE_CURSOR_RESIZE,
	/* A title bar button is held down; it fires on release over it. */
	GRIDE_CURSOR_BUTTON,
	/* Pressed on a title bar or with Super: becomes a move or resize once
	 * the pointer travels a few pixels, so a plain click stays a click. */
	GRIDE_CURSOR_PENDING,
	/* Dragging across cells in the snap layouts flyout's cell picker. */
	GRIDE_CURSOR_FLYOUT,
};

enum gride_grid_mode {
	GRIDE_GRID_FREE_SNAP, /* drop a window and it snaps to the nearest cells */
	GRIDE_GRID_AUTO_TILE, /* new windows fill empty cells */
};

/* Options from the command line. Phase 6 adds config.toml on top. */
struct gride_options {
	const char *startup_cmd; /* -s: run once the compositor is up */
	const char *terminal;    /* -t: command for Super+Enter */
	uint32_t mod_mask;       /* WLR_MODIFIER_LOGO, or ALT for nested dev */
	bool dark;               /* palette choice */
	bool animations;         /* -A turns them off */
	bool xwayland;           /* -X turns XWayland off */
	int grid_columns, grid_rows;
	enum gride_grid_mode grid_mode;
};

/* An inclusive cell range: columns are letters, rows numbers. A1 = (0,0). */
struct gride_range {
	int c0, r0, c1, r1;
};

/* Where a pointer position lands. */
enum gride_hit_type {
	GRIDE_HIT_NONE,    /* the desktop */
	GRIDE_HIT_SURFACE, /* a client surface; view may be NULL (X11 menus) */
	GRIDE_HIT_DECO,    /* our title bar */
	GRIDE_HIT_RESIZE,  /* the invisible resize band */
};

enum gride_deco_part {
	GRIDE_DECO_NONE,
	GRIDE_DECO_TITLE,
	GRIDE_DECO_MINIMIZE,
	GRIDE_DECO_MAXIMIZE,
	GRIDE_DECO_CLOSE,
};

struct gride_hit {
	enum gride_hit_type type;
	struct gride_view *view;
	struct wlr_surface *surface;
	double sx, sy;
	enum gride_deco_part part;
	uint32_t edges;
};

struct gride_server {
	struct wl_display *wl_display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_session *session; /* NULL when nested */
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;

	const struct gride_options *opts;
	const struct gride_palette *palette;
	const struct gride_shadow_layer *shadow;
	/* Largest output scale: decorations are drawn at it so they stay sharp
	 * on every output (wlroots scales them down elsewhere). */
	float deco_scale;

	/*
	 * Scene graph. wlroots renders the tree back to front, so the order
	 * these trees are created in is the stacking order:
	 *   background  desktop fill and dot grid, one node per output
	 *   windows     one gride_view tree per window; popups nest inside
	 *   unmanaged   X11 menus and tooltips (override-redirect windows)
	 *   overlay     the grid drag overlay and the snap preview
	 *   flyouts     the snap layouts flyout
	 * The trees are owned by the scene and destroyed with it.
	 */
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct {
		struct wlr_scene_tree *background;
		struct wlr_scene_tree *windows;
		struct wlr_scene_tree *unmanaged;
		struct wlr_scene_tree *overlay;
		struct wlr_scene_tree *flyouts;
	} layers;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs; /* gride_output.link */
	struct wl_listener new_output;
	struct wl_listener layout_change;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration;
	struct wl_listener new_xdg_decoration;

	struct wlr_xwayland *xwayland; /* NULL when disabled */
	struct wl_listener xwayland_ready;
	struct wl_listener new_xwayland_surface;

	/* Mapped views, front-to-back: the first entry is on top. Minimised
	 * views stay in the list but are hidden and skipped. */
	struct wl_list views; /* gride_view.link */
	struct gride_view *focused_view;
	/* Most recently minimised first, for restoring. */
	struct wl_list minimized; /* gride_view.minimized_link */

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	const char *cursor_image; /* the xcursor name we last set, or NULL */
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

	/* Interactive move/resize/button state. */
	enum gride_cursor_mode cursor_mode;
	struct gride_view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox; /* frame box at grab start */
	uint32_t resize_edges;
	enum gride_deco_part pressed_part;
	enum gride_cursor_mode pending_mode; /* what PENDING turns into */
	uint32_t pending_edges;
	double press_x, press_y;
	/* The decorated view whose buttons the pointer is over, if any. */
	struct gride_view *hover_view;
	/* True when the grab came from us (title bar, Super+drag) rather than
	 * the client: the button release then belongs to us. */
	bool grab_is_ours;
	/* Title bar double-click detection. */
	uint32_t last_title_click_msec;
	struct gride_view *last_title_click_view;

	/* The grid. Starts from the options; gride-msg can change it. */
	int grid_columns, grid_rows;
	enum gride_grid_mode grid_mode;
	struct gride_grid_overlay *grid_overlay; /* non-NULL while dragging */
	struct wlr_scene_buffer *grid_preview;   /* snap target preview, or NULL */
	struct wl_event_source *anim_timer;
	int animating; /* views with a running snap animation */

	/* Workspaces: one 3x3 set shared by all outputs. */
	int workspace;                          /* current, 0..8 */
	char *workspace_names[GRIDE_WORKSPACES]; /* NULL = unnamed */
	struct gride_view *workspace_focus[GRIDE_WORKSPACES]; /* last focused */

	struct gride_flyout *flyout;            /* the open snap layouts flyout */
	struct wl_event_source *flyout_timer;   /* hover-to-open and hover-to-close */
	struct gride_view *flyout_hover_view;   /* Maximise hovered, timer running */

	struct gride_ipc *ipc;
	uint32_t next_view_id;

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

enum gride_view_type {
	GRIDE_VIEW_XDG,
	GRIDE_VIEW_XWAYLAND,
};

struct gride_view;

/* What a view type (xdg toplevel, X11 window) must do for the generic code. */
struct gride_view_impl {
	/* Ask the client for a content size; (x, y) is where the content will
	 * be in layout space (X11 needs it; xdg ignores it). */
	void (*configure)(struct gride_view *view, int x, int y, int width, int height);
	void (*set_activated)(struct gride_view *view, bool activated);
	void (*set_maximized)(struct gride_view *view, bool maximized);
	void (*set_fullscreen)(struct gride_view *view, bool fullscreen);
	void (*set_minimized)(struct gride_view *view, bool minimized);
	void (*set_resizing)(struct gride_view *view, bool resizing);
	void (*close)(struct gride_view *view);
	const char *(*get_title)(struct gride_view *view);
	const char *(*get_app_id)(struct gride_view *view);
	/* 0 means no limit. */
	void (*get_constraints)(struct gride_view *view, int *min_w, int *min_h,
		int *max_w, int *max_h);
	/* Dialogs and other windows with a parent: never snapped or tiled. */
	bool (*is_transient)(struct gride_view *view);
};

/*
 * A window, from either xdg-shell or XWayland.
 *
 * Coordinates: (x, y) is the top-left of the *frame* in layout space: our
 * title bar when the window is decorated, else the client's visible content
 * (its xdg window geometry, which excludes client-drawn shadows). The frame
 * is the box the grid snaps. content_tree sits below the title bar.
 */
struct gride_view {
	struct wl_list link;
	struct wl_list minimized_link;
	struct gride_server *server;
	enum gride_view_type type;
	const struct gride_view_impl *impl;
	struct wlr_surface *surface;         /* set while mapped */
	uint32_t id;                         /* stable id for gride-msg */
	int workspace;                       /* 0..8 */

	struct wlr_scene_tree *scene_tree;   /* ours; node.data = this view */
	struct wlr_scene_tree *content_tree; /* holds the client's surfaces */
	struct gride_deco *deco;             /* title bar etc. while ssd */
	bool ssd;                            /* we draw the decorations */

	int x, y;
	int content_width, content_height;   /* last committed content size */

	bool mapped;
	bool maximized;
	bool fullscreen;
	bool minimized;

	/* Floating geometry to go back to after maximise/fullscreen. */
	struct wlr_box saved;
	bool has_saved;
	bool saved_snapped;
	struct gride_range saved_range;
	bool center_pending; /* centre on the next commit (client picks size) */

	/* Grid placement. */
	bool snapped;
	struct gride_range range;
	struct wlr_output *snap_output;

	/* Snap animation of the frame position. */
	struct {
		bool active;
		int from_x, from_y, to_x, to_y;
		uint32_t start_msec;
	} anim;
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
uint32_t gride_now_msec(void);

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

/* view.c: the generic window logic */
void view_init(struct gride_view *view, struct gride_server *server,
	enum gride_view_type type, const struct gride_view_impl *impl);
void view_destroy(struct gride_view *view);
void view_map(struct gride_view *view, struct wlr_surface *surface);
void view_unmap(struct gride_view *view);
void view_content_committed(struct gride_view *view, int width, int height);
void view_set_ssd(struct gride_view *view, bool ssd);
void view_title_changed(struct gride_view *view);
int view_titlebar_height(struct gride_view *view);
void view_get_frame(struct gride_view *view, struct wlr_box *box);
void view_move_to(struct gride_view *view, int x, int y);
void view_configure_frame(struct gride_view *view, const struct wlr_box *frame);
void view_set_content_size(struct gride_view *view, int width, int height);
void view_focus(struct gride_view *view);
void view_raise(struct gride_view *view);
void view_set_maximized(struct gride_view *view, bool maximized);
void view_set_fullscreen(struct gride_view *view, bool fullscreen, struct wlr_output *output);
void view_set_minimized(struct gride_view *view, bool minimized);
void view_restore_last_minimized(struct gride_server *server);
void view_close(struct gride_view *view);
bool view_is_snappable(struct gride_view *view);
bool view_is_terminal(struct gride_view *view);
const char *view_title(struct gride_view *view);
const char *view_app_id(struct gride_view *view);
void view_hit_test(struct gride_server *server, double lx, double ly, struct gride_hit *hit);
struct gride_view *view_from_surface(struct gride_server *server, struct wlr_surface *surface);
void views_arrange(struct gride_server *server);
void views_redraw_decorations(struct gride_server *server);
struct gride_view *view_by_id(struct gride_server *server, uint32_t id);
struct wlr_output *view_get_output(struct gride_view *view);
void view_maximize_button_box(struct gride_view *view, struct wlr_box *box);

/* xdg.c */
void xdg_handle_new_toplevel(struct wl_listener *listener, void *data);
void xdg_handle_new_popup(struct wl_listener *listener, void *data);
void xdg_handle_new_decoration(struct wl_listener *listener, void *data);

/* xwayland.c */
bool xwayland_init(struct gride_server *server);
void xwayland_finish(struct gride_server *server);

/* deco.c: server-side decorations */
struct gride_deco *deco_create(struct gride_view *view);
void deco_destroy(struct gride_deco *deco);
void deco_update(struct gride_deco *deco);          /* size, focus, title changed */
void deco_set_hover(struct gride_deco *deco, enum gride_deco_part part);
void deco_set_pressed(struct gride_deco *deco, enum gride_deco_part part);
enum gride_deco_part deco_part_at(struct gride_deco *deco, double fx, double fy);
bool deco_owns_node(struct gride_deco *deco, struct wlr_scene_node *node);
void deco_init_shared(struct gride_server *server);
/* The shadow-float token around a w x h rounded shape, as a scale-1 buffer
 * padded by shadow_reach() on every side. */
struct draw_buffer *shadow_render(int w, int h, double radius,
	const struct gride_shadow_layer *layers);
int shadow_reach(const struct gride_shadow_layer *layers);
void deco_finish_shared(void);

/* grid.c */
void grid_area(struct gride_server *server, struct wlr_output *output, struct wlr_box *area);
void grid_range_box(struct gride_server *server, struct wlr_output *output,
	const struct gride_range *range, struct wlr_box *box);
void grid_range_label(struct gride_server *server, const struct gride_range *range,
	char *ids, size_t ids_len, char *name, size_t name_len);
void grid_nearest_range(struct gride_server *server, struct wlr_output *output,
	const struct wlr_box *frame, struct gride_range *range);
void grid_snap_view(struct gride_view *view, struct wlr_output *output,
	const struct gride_range *range, bool animate);
void grid_unsnap_view(struct gride_view *view);
bool grid_auto_place(struct gride_view *view, struct wlr_output *output);
void grid_move_view(struct gride_view *view, int dc, int dr);
void grid_resize_view(struct gride_view *view, int dc, int dr);
void grid_drag_begin(struct gride_server *server, struct gride_view *view);
void grid_drag_update(struct gride_server *server);
void grid_drag_end(struct gride_server *server, bool snap);
void grid_output_removed(struct gride_server *server, struct wlr_output *output);
void grid_preview_show(struct gride_server *server, struct wlr_output *output,
	const struct gride_range *range);
void grid_preview_hide(struct gride_server *server);
bool grid_parse_range(struct gride_server *server, const char *text, struct gride_range *range);
bool grid_set_size(struct gride_server *server, int columns, int rows);
void grid_set_mode(struct gride_server *server, enum gride_grid_mode mode);
void grid_finish(struct gride_server *server);

/* workspace.c */
void workspace_switch(struct gride_server *server, int index);
void workspace_switch_direction(struct gride_server *server, int dx, int dy);
void workspace_move_view(struct gride_view *view, int index);
bool view_on_current_workspace(struct gride_view *view);
int workspace_window_count(struct gride_server *server, int index);
const char *workspace_name(struct gride_server *server, int index);
void workspace_set_name(struct gride_server *server, int index, const char *name);
void workspace_finish(struct gride_server *server);

/* flyout.c: the snap layouts flyout */
void flyout_open(struct gride_view *view);
void flyout_close(struct gride_server *server);
bool flyout_contains(struct gride_server *server, double lx, double ly);
void flyout_pointer_motion(struct gride_server *server, double lx, double ly);
bool flyout_pointer_button(struct gride_server *server, double lx, double ly, bool pressed);
bool flyout_handle_key(struct gride_server *server, xkb_keysym_t sym);
void flyout_hover_maximize(struct gride_server *server, struct gride_view *view);
void flyout_view_gone(struct gride_server *server, struct gride_view *view);
void flyout_finish(struct gride_server *server);

/* ipc.c: $XDG_RUNTIME_DIR/gride.sock */
bool ipc_init(struct gride_server *server);
void ipc_finish(struct gride_server *server);
void ipc_event_window(struct gride_server *server, struct gride_view *view, const char *change);
void ipc_event_workspace(struct gride_server *server);
void ipc_event_grid(struct gride_server *server);

/* input.c */
void input_init(struct gride_server *server);
void input_finish(struct gride_server *server);
void input_handle_new(struct wl_listener *listener, void *data);
uint32_t input_current_modifiers(struct gride_server *server);
void seat_handle_request_cursor(struct wl_listener *listener, void *data);
void seat_handle_request_set_selection(struct wl_listener *listener, void *data);
void seat_handle_request_set_primary_selection(struct wl_listener *listener, void *data);

/* cursor.c */
void cursor_init(struct gride_server *server);
void cursor_finish(struct gride_server *server);
void cursor_set_image(struct gride_server *server, const char *name);
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
