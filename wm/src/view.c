/*
 * Generic window logic shared by xdg-shell (xdg.c) and X11 (xwayland.c)
 * windows: frame geometry, placement, focus, stacking, maximise,
 * fullscreen, minimise and pointer hit-testing.
 *
 * Each window type embeds a struct gride_view, calls view_init() when the
 * window is created, view_map()/view_unmap() as it shows and hides,
 * view_content_committed() on every commit, and view_destroy() last.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "server.h"

void view_init(struct gride_view *view, struct gride_server *server,
		enum gride_view_type type, const struct gride_view_impl *impl) {
	view->server = server;
	view->type = type;
	view->impl = impl;
	wl_list_init(&view->link);
	wl_list_init(&view->minimized_link);
	/*
	 * Our tree sits at the frame origin and holds, back to front: the
	 * decorations (created on demand, see deco.c) and content_tree, which
	 * holds the client's surfaces and moves down by the title bar height.
	 * Hidden until map.
	 */
	view->scene_tree = wlr_scene_tree_create(server->layers.windows);
	view->scene_tree->node.data = view;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	view->content_tree = wlr_scene_tree_create(view->scene_tree);
}

void view_destroy(struct gride_view *view) {
	struct gride_server *server = view->server;
	if (server->grabbed_view == view) {
		cursor_end_interactive(server);
	}
	if (server->focused_view == view) {
		server->focused_view = NULL;
	}
	if (server->last_title_click_view == view) {
		server->last_title_click_view = NULL;
	}
	if (server->hover_view == view) {
		server->hover_view = NULL;
	}
	if (view->anim.active) {
		view->anim.active = false;
		server->animating--;
	}
	wl_list_remove(&view->link);
	wl_list_remove(&view->minimized_link);
	if (view->deco) {
		deco_destroy(view->deco);
		view->deco = NULL;
	}
	/* Takes the content tree and anything still inside it along. */
	wlr_scene_node_destroy(&view->scene_tree->node);
}

const char *view_title(struct gride_view *view) {
	const char *title = view->impl->get_title(view);
	return title ? title : "";
}

const char *view_app_id(struct gride_view *view) {
	const char *app_id = view->impl->get_app_id(view);
	return app_id ? app_id : "";
}

int view_titlebar_height(struct gride_view *view) {
	return view->ssd && !view->fullscreen ? GRIDE_TITLEBAR_HEIGHT : 0;
}

void view_get_frame(struct gride_view *view, struct wlr_box *box) {
	*box = (struct wlr_box){
		.x = view->x,
		.y = view->y,
		.width = view->content_width,
		.height = view->content_height + view_titlebar_height(view),
	};
}

/* Lay out our children after a size, decoration or fullscreen change. */
static void view_update_layout(struct gride_view *view) {
	wlr_scene_node_set_position(&view->content_tree->node, 0, view_titlebar_height(view));
	if (view->deco) {
		deco_update(view->deco);
	}
}

void view_move_to(struct gride_view *view, int x, int y) {
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	/* X11 clients position their own menus from where they think they
	 * are, so they hear about every move. xdg clients do not need to. */
	if (view->type == GRIDE_VIEW_XWAYLAND && view->mapped) {
		view->impl->configure(view, x, y + view_titlebar_height(view),
			view->content_width, view->content_height);
	}
}

void view_set_content_size(struct gride_view *view, int width, int height) {
	view->impl->configure(view, view->x, view->y + view_titlebar_height(view),
		width > 0 ? width : 0, height > 0 ? height : 0);
}

void view_configure_frame(struct gride_view *view, const struct wlr_box *frame) {
	int th = view_titlebar_height(view);
	view->x = frame->x;
	view->y = frame->y;
	if (!view->anim.active) {
		wlr_scene_node_set_position(&view->scene_tree->node, frame->x, frame->y);
	}
	view_set_content_size(view, frame->width, frame->height - th);
}

void view_set_ssd(struct gride_view *view, bool ssd) {
	if (view->ssd == ssd) {
		return;
	}
	/* Keep the frame where it is: the content grows or shrinks by the
	 * title bar instead, so a snapped window still fills its cells. */
	struct wlr_box frame;
	view_get_frame(view, &frame);
	view->ssd = ssd;
	if (ssd && !view->deco) {
		view->deco = deco_create(view);
	} else if (!ssd && view->deco) {
		deco_destroy(view->deco);
		view->deco = NULL;
	}
	view_update_layout(view);
	if (view->mapped && !view->fullscreen) {
		view_configure_frame(view, &frame);
	}
}

void view_title_changed(struct gride_view *view) {
	if (view->deco) {
		deco_update(view->deco);
	}
}

bool view_is_terminal(struct gride_view *view) {
	static const char *const terminals[] = {
		"foot", "footclient", "Alacritty", "kitty", "org.wezfurlong.wezterm",
		"org.gnome.Terminal", "org.gnome.Console", "org.gnome.Ptyxis",
		"xfce4-terminal", "org.kde.konsole", "konsole", "XTerm", "URxvt",
		"weston-terminal", "org.freedesktop.weston.wayland-terminal",
		"terminator", "tilix", "com.gexperts.Tilix",
	};
	const char *app_id = view_app_id(view);
	for (size_t i = 0; i < sizeof(terminals) / sizeof(terminals[0]); i++) {
		if (strcasecmp(app_id, terminals[i]) == 0) {
			return true;
		}
	}
	return false;
}

bool view_is_snappable(struct gride_view *view) {
	if (view->impl->is_transient(view)) {
		return false;
	}
	int min_w = 0, min_h = 0, max_w = 0, max_h = 0;
	view->impl->get_constraints(view, &min_w, &min_h, &max_w, &max_h);
	/* A window that cannot be resized cannot fill cells. */
	if (max_w > 0 && max_w == min_w && max_h > 0 && max_h == min_h) {
		return false;
	}
	return true;
}

static struct wlr_output *view_output(struct gride_view *view) {
	if (view->snapped && view->snap_output) {
		return view->snap_output;
	}
	if (!view->mapped) {
		return output_at_cursor(view->server);
	}
	struct wlr_box frame;
	view_get_frame(view, &frame);
	return output_for_box(view->server, &frame);
}

/* ---- focus and stacking ------------------------------------------------- */

void view_raise(struct gride_view *view) {
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&view->server->views, &view->link);
}

void view_focus(struct gride_view *view) {
	if (!view || !view->mapped || view->minimized) {
		return;
	}
	struct gride_server *server = view->server;
	struct gride_view *prev = server->focused_view;
	view_raise(view);
	if (prev == view && server->seat->keyboard_state.focused_surface == view->surface) {
		return;
	}
	if (prev && prev != view) {
		/* The previous window draws its inactive state (and loses the
		 * shadow, which only the focused window has). */
		prev->impl->set_activated(prev, false);
		if (prev->deco) {
			deco_update(prev->deco);
		}
	}
	server->focused_view = view;
	view->impl->set_activated(view, true);
	if (view->deco) {
		deco_update(view->deco);
	}

	/* Keyboard enter carries the keys already held down, so the client
	 * does not see a release for a press it never got. */
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(server->seat, view->surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(server->seat, view->surface, NULL, 0, NULL);
	}
}

static void focus_next(struct gride_server *server) {
	struct gride_view *next;
	wl_list_for_each(next, &server->views, link) {
		if (next->mapped && !next->minimized) {
			view_focus(next);
			return;
		}
	}
	server->focused_view = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

/* ---- placement ---------------------------------------------------------- */

/* Centre the frame on `within`, keeping the 12px margin and shrinking it
 * if it does not fit. */
static void view_center_in(struct gride_view *view, const struct wlr_box *within) {
	const int margin = GRIDE_SPACE_3;
	struct wlr_box frame;
	view_get_frame(view, &frame);
	int max_w = within->width - 2 * margin;
	int max_h = within->height - 2 * margin;
	if (frame.width > max_w || frame.height > max_h) {
		frame.width = frame.width > max_w ? max_w : frame.width;
		frame.height = frame.height > max_h ? max_h : frame.height;
		view_set_content_size(view, frame.width, frame.height - view_titlebar_height(view));
	}
	int x = within->x + (within->width - frame.width) / 2;
	int y = within->y + (within->height - frame.height) / 2;
	view_move_to(view, x, y);
}

static void view_place_floating(struct gride_view *view) {
	struct gride_server *server = view->server;
	struct wlr_output *output = output_at_cursor(server);
	if (!output) {
		return;
	}
	struct wlr_box area;
	grid_area(server, output, &area);
	/* Dialogs open over the window they belong to. */
	if (view->impl->is_transient(view) && server->focused_view &&
			server->focused_view != view) {
		struct wlr_box parent;
		view_get_frame(server->focused_view, &parent);
		struct wlr_box frame;
		view_get_frame(view, &frame);
		int x = parent.x + (parent.width - frame.width) / 2;
		int y = parent.y + (parent.height - frame.height) / 2;
		if (x < area.x) {
			x = area.x;
		}
		if (y < area.y) {
			y = area.y;
		}
		view_move_to(view, x, y);
		return;
	}
	view_center_in(view, &area);
}

static void view_save_floating(struct gride_view *view) {
	if (view->maximized || view->fullscreen || !view->mapped) {
		return;
	}
	view_get_frame(view, &view->saved);
	view->has_saved = true;
	view->saved_snapped = view->snapped;
	view->saved_range = view->range;
}

static void view_apply_output_box(struct gride_view *view, struct wlr_output *output) {
	if (!output) {
		return;
	}
	struct wlr_box box;
	if (view->fullscreen) {
		output_get_box(view->server, output, &box);
	} else {
		grid_area(view->server, output, &box);
	}
	if (wlr_box_empty(&box)) {
		return;
	}
	view_configure_frame(view, &box);
}

static void view_restore_floating(struct gride_view *view) {
	if (view->saved_snapped) {
		view->saved_snapped = false;
		grid_snap_view(view, view_output(view), &view->saved_range, false);
	} else if (view->has_saved) {
		view_configure_frame(view, &view->saved);
	} else {
		/* Mapped maximised: no floating size known yet. Let the client
		 * choose, and centre it once it has. */
		view->center_pending = true;
		view_set_content_size(view, 0, 0);
	}
	view->has_saved = false;
}

void view_set_maximized(struct gride_view *view, bool maximized) {
	if (view->maximized == maximized) {
		view->impl->set_maximized(view, maximized);
		return;
	}
	struct wlr_output *output = view_output(view);
	if (maximized) {
		view_save_floating(view);
		view->snapped = false;
	}
	view->maximized = maximized;
	view->impl->set_maximized(view, maximized);
	if (view->deco) {
		deco_update(view->deco); /* the maximise glyph changes */
	}
	if (view->fullscreen) {
		return; /* fullscreen wins; applied again when it ends */
	}
	if (maximized) {
		view_apply_output_box(view, output);
	} else {
		view_restore_floating(view);
	}
}

void view_set_fullscreen(struct gride_view *view, bool fullscreen, struct wlr_output *output) {
	if (view->fullscreen == fullscreen) {
		view->impl->set_fullscreen(view, fullscreen);
		return;
	}
	struct wlr_output *current = output ? output : view_output(view);
	if (fullscreen) {
		view_save_floating(view);
		view->snapped = false;
	}
	view->fullscreen = fullscreen;
	view->impl->set_fullscreen(view, fullscreen);
	view_update_layout(view); /* title bar hides or returns */
	if (fullscreen) {
		view_apply_output_box(view, current);
		view_raise(view);
	} else if (view->maximized) {
		view_apply_output_box(view, current);
	} else {
		view_restore_floating(view);
	}
}

void view_set_minimized(struct gride_view *view, bool minimized) {
	if (view->minimized == minimized || !view->mapped) {
		return;
	}
	struct gride_server *server = view->server;
	view->minimized = minimized;
	view->impl->set_minimized(view, minimized);
	wlr_scene_node_set_enabled(&view->scene_tree->node, !minimized);
	wl_list_remove(&view->minimized_link);
	if (minimized) {
		wl_list_insert(&server->minimized, &view->minimized_link);
		if (server->grabbed_view == view) {
			cursor_end_interactive(server);
		}
		if (server->focused_view == view) {
			view->impl->set_activated(view, false);
			server->focused_view = NULL;
			focus_next(server);
		}
	} else {
		wl_list_init(&view->minimized_link);
		view_focus(view);
	}
	cursor_rebase(server, 0);
}

void view_restore_last_minimized(struct gride_server *server) {
	if (wl_list_empty(&server->minimized)) {
		return;
	}
	struct gride_view *view = wl_container_of(server->minimized.next, view, minimized_link);
	view_set_minimized(view, false);
}

void view_close(struct gride_view *view) {
	view->impl->close(view);
}

/* ---- map, unmap, commit ------------------------------------------------- */

void view_map(struct gride_view *view, struct wlr_surface *surface) {
	struct gride_server *server = view->server;
	view->surface = surface;
	view->mapped = true;
	view_update_layout(view);

	struct wlr_output *output = output_at_cursor(server);
	if (view->fullscreen || view->maximized) {
		view_apply_output_box(view, output);
	} else if (!(server->opts->grid_mode == GRIDE_GRID_AUTO_TILE &&
			view_is_snappable(view) && grid_auto_place(view, output))) {
		view_place_floating(view);
	}

	wl_list_insert(&server->views, &view->link);
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	view_focus(view);
	cursor_rebase(server, 0);

	struct wlr_box frame;
	view_get_frame(view, &frame);
	wlr_log(WLR_DEBUG, "Mapped \"%s\" (%s) at %d,%d %dx%d%s", view_title(view),
		view_app_id(view), frame.x, frame.y, frame.width, frame.height,
		view->ssd ? " ssd" : "");
}

void view_unmap(struct gride_view *view) {
	struct gride_server *server = view->server;
	if (server->grabbed_view == view) {
		cursor_end_interactive(server);
	}
	if (view->anim.active) {
		view->anim.active = false;
		server->animating--;
	}
	wlr_log(WLR_DEBUG, "Unmapped \"%s\"", view_title(view));
	view->mapped = false;
	view->minimized = false;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	wl_list_remove(&view->minimized_link);
	wl_list_init(&view->minimized_link);

	if (server->focused_view == view) {
		server->focused_view = NULL;
		focus_next(server);
	}
	view->surface = NULL;
	cursor_rebase(server, 0);
}

void view_content_committed(struct gride_view *view, int width, int height) {
	if (width == view->content_width && height == view->content_height) {
		return;
	}
	view->content_width = width;
	view->content_height = height;
	if (!view->mapped) {
		return;
	}

	/*
	 * While resizing from the left or top edge, the opposite edge stays put.
	 * The new x/y is applied here, when the client commits the new size,
	 * rather than when we ask for it: moving early makes the window jitter.
	 */
	struct gride_server *server = view->server;
	struct wlr_box frame;
	view_get_frame(view, &frame);
	if (server->cursor_mode == GRIDE_CURSOR_RESIZE && server->grabbed_view == view) {
		int x = view->x, y = view->y;
		if (server->resize_edges & WLR_EDGE_LEFT) {
			x = server->grab_geobox.x + server->grab_geobox.width - frame.width;
		}
		if (server->resize_edges & WLR_EDGE_TOP) {
			y = server->grab_geobox.y + server->grab_geobox.height - frame.height;
		}
		if (x != view->x || y != view->y) {
			view_move_to(view, x, y);
		}
	} else if (view->center_pending && !view->maximized && !view->fullscreen) {
		/* First commit after leaving a maximise we had no floating size
		 * for: centre the size the client picked. */
		view->center_pending = false;
		view_place_floating(view);
	}
	view_update_layout(view);
}

/* ---- hit-testing -------------------------------------------------------- */

static struct gride_view *view_from_node(struct wlr_scene_node *node) {
	for (struct wlr_scene_tree *tree = node->parent; tree; tree = tree->node.parent) {
		if (tree->node.data) {
			return tree->node.data;
		}
	}
	return NULL;
}

/* Edges of the resize band at (lx, ly), or 0 when outside it. */
static uint32_t resize_edges_at(struct gride_view *view, double lx, double ly) {
	if (!view->ssd || !view->mapped || view->minimized || view->maximized ||
			view->fullscreen) {
		return 0;
	}
	struct wlr_box frame;
	view_get_frame(view, &frame);
	const int b = GRIDE_RESIZE_BORDER;
	if (lx < frame.x - b || lx >= frame.x + frame.width + b ||
			ly < frame.y - b || ly >= frame.y + frame.height + b) {
		return 0;
	}
	uint32_t edges = 0;
	if (lx < frame.x) {
		edges |= WLR_EDGE_LEFT;
	} else if (lx >= frame.x + frame.width) {
		edges |= WLR_EDGE_RIGHT;
	}
	if (ly < frame.y) {
		edges |= WLR_EDGE_TOP;
	} else if (ly >= frame.y + frame.height) {
		edges |= WLR_EDGE_BOTTOM;
	}
	return edges;
}

void view_hit_test(struct gride_server *server, double lx, double ly, struct gride_hit *hit) {
	memset(hit, 0, sizeof(*hit));

	/* X11 menus sit above every window. */
	struct wlr_scene_node *node = wlr_scene_node_at(&server->layers.unmanaged->node,
		lx, ly, &hit->sx, &hit->sy);
	if (!node) {
		node = wlr_scene_node_at(&server->layers.windows->node, lx, ly, &hit->sx, &hit->sy);
	}

	struct gride_view *under = NULL;
	if (node && node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
		under = view_from_node(node);
		if (scene_surface) {
			hit->type = GRIDE_HIT_SURFACE;
			hit->surface = scene_surface->surface;
			hit->view = under;
		} else if (under && under->deco && deco_owns_node(under->deco, node)) {
			hit->type = GRIDE_HIT_DECO;
			hit->view = under;
			hit->part = deco_part_at(under->deco, lx - under->x, ly - under->y);
		}
	} else if (node) {
		under = view_from_node(node); /* a border hairline */
	}

	/* A higher window's invisible resize band wins over what is below it. */
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == hit->view || view == under) {
			break;
		}
		uint32_t edges = resize_edges_at(view, lx, ly);
		if (edges) {
			memset(hit, 0, sizeof(*hit));
			hit->type = GRIDE_HIT_RESIZE;
			hit->view = view;
			hit->edges = edges;
			return;
		}
	}
	if (hit->type == GRIDE_HIT_NONE && under) {
		uint32_t edges = resize_edges_at(under, lx, ly);
		if (edges) {
			hit->type = GRIDE_HIT_RESIZE;
			hit->view = under;
			hit->edges = edges;
		}
	}
}

struct gride_view *view_from_surface(struct gride_server *server, struct wlr_surface *surface) {
	if (!surface) {
		return NULL;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->surface == root) {
			return view;
		}
	}
	return NULL;
}

/* ---- whole-desktop updates ---------------------------------------------- */

/* Re-fit maximised, fullscreen and snapped views after outputs change. */
void views_arrange(struct gride_server *server) {
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->fullscreen || view->maximized) {
			view_apply_output_box(view, view_output(view));
		} else if (view->snapped) {
			struct wlr_output *output = view->snap_output;
			if (!output) {
				struct wlr_box frame;
				view_get_frame(view, &frame);
				output = output_for_box(server, &frame);
			}
			struct gride_range range = view->range;
			grid_snap_view(view, output, &range, false);
		}
	}
}

void views_redraw_decorations(struct gride_server *server) {
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->deco) {
			deco_update(view->deco);
		}
	}
}
