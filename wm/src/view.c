/*
 * Toplevel windows and popups (xdg-shell).
 *
 * Lifecycle of a toplevel, as wlroots reports it:
 *   new_toplevel  the client created the role; nothing is visible yet
 *   commit        the first commit has initial_commit set. We must answer
 *                 it with a configure, and must not configure before it
 *   map           the surface has a buffer: show it, focus it
 *   unmap         hidden (it may map again later)
 *   destroy       gone; free everything
 * The view struct lives from new_toplevel to destroy.
 */
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "server.h"

#define WM_CAPABILITIES (WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE | \
	WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN)

void view_get_geometry(struct gride_view *view, struct wlr_box *box) {
	wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, box);
}

void view_move_to(struct gride_view *view, int x, int y) {
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->scene_tree->node, x, y);
}

void view_set_size(struct gride_view *view, int width, int height) {
	wlr_xdg_toplevel_set_size(view->xdg_toplevel, width > 0 ? width : 0,
		height > 0 ? height : 0);
}

static struct gride_view *view_from_xdg_surface(struct wlr_xdg_surface *xdg_surface) {
	if (!xdg_surface || xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL ||
			!xdg_surface->data) {
		return NULL;
	}
	/* base->data is the surface subtree; its parent is the view's tree. */
	struct wlr_scene_tree *surface_tree = xdg_surface->data;
	struct wlr_scene_tree *view_tree = surface_tree->node.parent;
	return view_tree ? view_tree->node.data : NULL;
}

void view_focus(struct gride_view *view) {
	if (!view || !view->mapped) {
		return;
	}
	struct gride_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = view->xdg_toplevel->base->surface;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		return;
	}
	if (prev_surface) {
		/* Tell the previously focused toplevel it is no longer active, so
		 * it can draw its inactive state. */
		struct wlr_xdg_toplevel *prev = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev) {
			wlr_xdg_toplevel_set_activated(prev, false);
		}
	}

	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
	server->focused_view = view;

	/* Keyboard enter carries the keys already held down, so the client
	 * does not see a release for a press it never got. */
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
}

struct gride_view *view_at(struct gride_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	*surface = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&server->layers.windows->node,
		lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}
	*surface = scene_surface->surface;
	/* Walk up to the view's own tree, the only node in the windows layer
	 * whose data pointer we set. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) {
		tree = tree->node.parent;
	}
	return tree ? tree->node.data : NULL;
}

static void view_save_floating(struct gride_view *view) {
	if (view->maximized || view->fullscreen || !view->mapped) {
		return;
	}
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	view->saved = (struct wlr_box){ view->x, view->y, geo.width, geo.height };
	view->has_saved = true;
}

static struct wlr_output *view_output(struct gride_view *view) {
	if (!view->mapped) {
		return output_at_cursor(view->server);
	}
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	struct wlr_box box = { view->x, view->y, geo.width, geo.height };
	return output_for_box(view->server, &box);
}

/* Place a maximised or fullscreen view over its output. */
static void view_apply_output_box(struct gride_view *view, struct wlr_output *output) {
	if (!output) {
		return;
	}
	struct wlr_box box;
	output_get_box(view->server, output, &box);
	if (wlr_box_empty(&box)) {
		return;
	}
	/* Phase 4 subtracts the taskbar's exclusive zone from maximised views. */
	view_move_to(view, box.x, box.y);
	view_set_size(view, box.width, box.height);
}

static void view_restore_floating(struct gride_view *view) {
	if (view->has_saved) {
		view_move_to(view, view->saved.x, view->saved.y);
		view_set_size(view, view->saved.width, view->saved.height);
	} else {
		/* Mapped maximised: no floating size known yet. Let the client
		 * choose, and centre it once it has. */
		view->center_pending = true;
		view_set_size(view, 0, 0);
	}
}

void view_set_maximized(struct gride_view *view, bool maximized) {
	if (view->maximized == maximized) {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
		return;
	}
	if (maximized) {
		view_save_floating(view);
	}
	view->maximized = maximized;
	wlr_xdg_toplevel_set_maximized(view->xdg_toplevel, maximized);
	if (view->fullscreen) {
		return; /* fullscreen wins; applied again when it ends */
	}
	if (maximized) {
		view_apply_output_box(view, view_output(view));
	} else {
		view_restore_floating(view);
	}
}

void view_set_fullscreen(struct gride_view *view, bool fullscreen, struct wlr_output *output) {
	if (view->fullscreen == fullscreen) {
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
		return;
	}
	if (fullscreen) {
		view_save_floating(view);
	}
	view->fullscreen = fullscreen;
	wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, fullscreen);
	if (fullscreen) {
		view_apply_output_box(view, output ? output : view_output(view));
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	} else if (view->maximized) {
		view_apply_output_box(view, view_output(view));
	} else {
		view_restore_floating(view);
	}
}

void view_close(struct gride_view *view) {
	wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

/* Re-fit maximised and fullscreen views after outputs change. */
void views_arrange(struct gride_server *server) {
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->fullscreen || view->maximized) {
			view_apply_output_box(view, view_output(view));
		}
	}
}

/* Centre a new floating window on the output under the cursor, keeping
 * the 12px screen margin and shrinking it if it does not fit. */
static void view_place_new(struct gride_view *view) {
	struct wlr_output *output = output_at_cursor(view->server);
	if (!output) {
		return;
	}
	struct wlr_box area;
	output_get_box(view->server, output, &area);
	const int margin = GRIDE_SPACE_3;
	struct wlr_box geo;
	view_get_geometry(view, &geo);

	int max_w = area.width - 2 * margin;
	int max_h = area.height - 2 * margin;
	int w = geo.width, h = geo.height;
	if (w > max_w || h > max_h) {
		w = w > max_w ? max_w : w;
		h = h > max_h ? max_h : h;
		view_set_size(view, w, h);
	}
	int x = area.x + (area.width - w) / 2;
	int y = area.y + (area.height - h) / 2;
	if (x < area.x + margin) {
		x = area.x + margin;
	}
	if (y < area.y + margin) {
		y = area.y + margin;
	}
	view_move_to(view, x, y);
}

static void view_handle_map(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, map);
	struct gride_server *server = view->server;
	view->mapped = true;

	if (view->fullscreen || view->maximized) {
		view_apply_output_box(view, output_at_cursor(server));
	} else {
		view_place_new(view);
	}
	wl_list_insert(&server->views, &view->link);
	wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	view_focus(view);
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	wlr_log(WLR_DEBUG, "Mapped \"%s\" (%s) at %d,%d %dx%d",
		view->xdg_toplevel->title ? view->xdg_toplevel->title : "",
		view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "",
		view->x, view->y, geo.width, geo.height);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, unmap);
	struct gride_server *server = view->server;

	if (server->grabbed_view == view) {
		cursor_end_interactive(server);
	}
	view->mapped = false;
	wlr_log(WLR_DEBUG, "Unmapped \"%s\"",
		view->xdg_toplevel->title ? view->xdg_toplevel->title : "");
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	wl_list_remove(&view->link);
	wl_list_init(&view->link); /* so a later remove in destroy is safe */

	if (server->focused_view == view) {
		server->focused_view = NULL;
		if (!wl_list_empty(&server->views)) {
			struct gride_view *next = wl_container_of(server->views.next, next, link);
			view_focus(next);
		} else {
			wlr_seat_keyboard_notify_clear_focus(server->seat);
		}
	}
	/* The window under the cursor changed; refresh pointer focus. */
	cursor_rebase(server, 0);
}

static void view_handle_commit(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_toplevel *toplevel = view->xdg_toplevel;

	if (toplevel->base->initial_commit) {
		/* The client has committed its initial state (title, app id, any
		 * maximise or fullscreen request). Answer with the first configure. */
		wlr_xdg_toplevel_set_wm_capabilities(toplevel, WM_CAPABILITIES);
		struct wlr_output *output = output_at_cursor(view->server);
		if (toplevel->requested.fullscreen) {
			view_set_fullscreen(view, true, toplevel->requested.fullscreen_output ?
				toplevel->requested.fullscreen_output : output);
		} else if (toplevel->requested.maximized) {
			view_set_maximized(view, true);
		} else {
			/* 0x0 lets the client pick its own size. */
			view_set_size(view, 0, 0);
		}
		return;
	}
	if (!view->mapped) {
		return;
	}

	/*
	 * While resizing from the left or top edge, the opposite edge stays put.
	 * The new x/y is applied here, when the client commits the new size,
	 * rather than when we ask for it: moving early makes the window jitter.
	 */
	struct gride_server *server = view->server;
	if (server->cursor_mode == GRIDE_CURSOR_RESIZE && server->grabbed_view == view) {
		struct wlr_box geo;
		view_get_geometry(view, &geo);
		int x = view->x, y = view->y;
		if (server->resize_edges & WLR_EDGE_LEFT) {
			x = server->grab_geobox.x + server->grab_geobox.width - geo.width;
		}
		if (server->resize_edges & WLR_EDGE_TOP) {
			y = server->grab_geobox.y + server->grab_geobox.height - geo.height;
		}
		if (x != view->x || y != view->y) {
			view_move_to(view, x, y);
		}
	} else if (view->center_pending && !view->maximized && !view->fullscreen) {
		/* First commit after leaving a maximise we had no floating size
		 * for: centre the size the client picked. */
		view->center_pending = false;
		view_place_new(view);
	}
}

static void view_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, destroy);
	if (view->server->grabbed_view == view) {
		cursor_end_interactive(view->server);
	}
	if (view->server->focused_view == view) {
		view->server->focused_view = NULL;
	}
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->link);
	/*
	 * The surface subtree created by wlr_scene_xdg_surface_create destroys
	 * itself when the xdg surface goes away. Our own tree does not, so it
	 * is destroyed here (taking anything still inside it along).
	 */
	wlr_scene_node_destroy(&view->scene_tree->node);
	free(view);
}

/*
 * Client-initiated move/resize, e.g. dragging a client-side title bar.
 * Only honoured with a valid serial from a button press the client really
 * received, so a client cannot start a grab on its own.
 */
static bool view_grab_allowed(struct gride_view *view, uint32_t serial) {
	struct wlr_seat *seat = view->server->seat;
	return view->mapped && wlr_seat_validate_pointer_grab_serial(seat,
		view->xdg_toplevel->base->surface, serial);
}

static void view_handle_request_move(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, request_move);
	struct wlr_xdg_toplevel_move_event *event = data;
	if (view_grab_allowed(view, event->serial)) {
		cursor_begin_interactive(view, GRIDE_CURSOR_MOVE, 0, false);
	}
}

static void view_handle_request_resize(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	if (view_grab_allowed(view, event->serial)) {
		cursor_begin_interactive(view, GRIDE_CURSOR_RESIZE, event->edges, false);
	}
}

static void view_handle_request_maximize(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, request_maximize);
	/* Before the initial commit the request is picked up in commit(). */
	if (!view->xdg_toplevel->base->initialized) {
		return;
	}
	view_set_maximized(view, view->xdg_toplevel->requested.maximized);
}

static void view_handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct gride_view *view = wl_container_of(listener, view, request_fullscreen);
	if (!view->xdg_toplevel->base->initialized) {
		return;
	}
	struct wlr_xdg_toplevel_requested *req = &view->xdg_toplevel->requested;
	view_set_fullscreen(view, req->fullscreen, req->fullscreen_output);
}

void view_handle_new_toplevel(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct gride_view *view = calloc(1, sizeof(*view));
	if (!view) {
		return;
	}
	view->server = server;
	view->xdg_toplevel = xdg_toplevel;
	wl_list_init(&view->link);

	/*
	 * Two trees: ours (positioned at the window geometry, and later the
	 * parent of the title bar) and, inside it, the client's surface tree.
	 * wlr_scene_xdg_surface keeps the surfaces inside that tree offset by
	 * (-geometry.x, -geometry.y) on every commit, so the visible content
	 * lands exactly at (x, y) without any work from us.
	 * Popups look up their parent's tree through xdg_surface->data, so that
	 * points at the surface tree. Ours starts hidden until map.
	 */
	view->scene_tree = wlr_scene_tree_create(server->layers.windows);
	view->scene_tree->node.data = view;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	view->surface_tree = wlr_scene_xdg_surface_create(view->scene_tree, xdg_toplevel->base);
	xdg_toplevel->base->data = view->surface_tree;

	struct wlr_surface *surface = xdg_toplevel->base->surface;
	view->map.notify = view_handle_map;
	wl_signal_add(&surface->events.map, &view->map);
	view->unmap.notify = view_handle_unmap;
	wl_signal_add(&surface->events.unmap, &view->unmap);
	view->commit.notify = view_handle_commit;
	wl_signal_add(&surface->events.commit, &view->commit);
	view->destroy.notify = view_handle_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);

	view->request_move.notify = view_handle_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = view_handle_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = view_handle_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
	view->request_fullscreen.notify = view_handle_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
}

/* ---- popups (menus, tooltips) ------------------------------------------ */

/* Keep a popup inside the output its toplevel is on. */
static void popup_unconstrain(struct gride_popup *popup) {
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	/* Walk up through nested popups to the toplevel that owns them. */
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	while (parent && parent->role == WLR_XDG_SURFACE_ROLE_POPUP && parent->popup) {
		parent = wlr_xdg_surface_try_from_wlr_surface(parent->popup->parent);
	}
	struct gride_view *view = view_from_xdg_surface(parent);
	if (!view) {
		return;
	}
	struct wlr_output *output = view_output(view);
	if (!output) {
		return;
	}
	struct wlr_box out;
	output_get_box(view->server, output, &out);
	struct wlr_box geo;
	view_get_geometry(view, &geo);
	/* The box is in the toplevel's surface coordinates: its surface origin
	 * is at (x - geo.x, y - geo.y) in layout space. */
	struct wlr_box box = {
		.x = out.x - view->x + geo.x,
		.y = out.y - view->y + geo.y,
		.width = out.width,
		.height = out.height,
	};
	wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
}

static void popup_handle_commit(struct wl_listener *listener, void *data) {
	struct gride_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		popup_unconstrain(popup);
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void popup_handle_reposition(struct wl_listener *listener, void *data) {
	struct gride_popup *popup = wl_container_of(listener, popup, reposition);
	popup_unconstrain(popup);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->reposition.link);
	wl_list_remove(&popup->destroy.link);
	/* The popup's scene subtree removes itself with the xdg surface. */
	free(popup);
}

void view_handle_new_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	/* A popup's parent is a toplevel or another popup. Its scene subtree
	 * goes inside the parent's, so it moves and stacks with it. Phase 4
	 * adds layer-shell parents (taskbar menus). */
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent || !parent->data) {
		wlr_log(WLR_DEBUG, "Popup without a known parent; ignoring");
		return;
	}
	struct gride_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		return;
	}
	popup->xdg_popup = xdg_popup;
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = popup_handle_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->reposition.notify = popup_handle_reposition;
	wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
	popup->destroy.notify = popup_handle_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}
