/*
 * xdg-shell windows, their popups, and xdg-decoration.
 *
 * Lifecycle of a toplevel, as wlroots reports it:
 *   new_toplevel  the client created the role; nothing is visible yet
 *   commit        the first commit has initial_commit set. We must answer
 *                 it with a configure, and must not configure before it
 *   map           the surface has a buffer: show it, focus it
 *   unmap         hidden (it may map again later)
 *   destroy       gone; free everything
 */
#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "server.h"

struct gride_xdg_decoration;

struct gride_xdg_view {
	struct gride_view view; /* first, so the generic code's pointer casts back */
	struct wlr_xdg_toplevel *toplevel;
	struct wlr_scene_tree *surface_tree;
	struct gride_xdg_decoration *decoration;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
};

struct gride_xdg_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration;
	struct gride_xdg_view *xview; /* NULL once the window is gone */
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

struct gride_xdg_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
};

static struct gride_xdg_view *xdg_view(struct gride_view *view) {
	return (struct gride_xdg_view *)view;
}

static bool initialized(struct gride_view *view) {
	return xdg_view(view)->toplevel->base->initialized;
}

/* ---- gride_view_impl ---------------------------------------------------- */

static void impl_configure(struct gride_view *view, int x, int y, int width, int height) {
	/* Configuring before the initial commit is a protocol error. */
	if (initialized(view)) {
		wlr_xdg_toplevel_set_size(xdg_view(view)->toplevel, width, height);
	}
}

static void impl_set_activated(struct gride_view *view, bool activated) {
	if (initialized(view)) {
		wlr_xdg_toplevel_set_activated(xdg_view(view)->toplevel, activated);
	}
}

static void impl_set_maximized(struct gride_view *view, bool maximized) {
	if (initialized(view)) {
		wlr_xdg_toplevel_set_maximized(xdg_view(view)->toplevel, maximized);
	}
}

static void impl_set_fullscreen(struct gride_view *view, bool fullscreen) {
	if (initialized(view)) {
		wlr_xdg_toplevel_set_fullscreen(xdg_view(view)->toplevel, fullscreen);
	}
}

static void impl_set_minimized(struct gride_view *view, bool minimized) {
	/* xdg-shell has no minimised state to tell the client about. */
}

static void impl_set_resizing(struct gride_view *view, bool resizing) {
	if (initialized(view)) {
		wlr_xdg_toplevel_set_resizing(xdg_view(view)->toplevel, resizing);
	}
}

static void impl_close(struct gride_view *view) {
	wlr_xdg_toplevel_send_close(xdg_view(view)->toplevel);
}

static const char *impl_get_title(struct gride_view *view) {
	return xdg_view(view)->toplevel->title;
}

static const char *impl_get_app_id(struct gride_view *view) {
	return xdg_view(view)->toplevel->app_id;
}

static void impl_get_constraints(struct gride_view *view, int *min_w, int *min_h,
		int *max_w, int *max_h) {
	struct wlr_xdg_toplevel_state *state = &xdg_view(view)->toplevel->current;
	*min_w = state->min_width;
	*min_h = state->min_height;
	*max_w = state->max_width;
	*max_h = state->max_height;
}

static bool impl_is_transient(struct gride_view *view) {
	return xdg_view(view)->toplevel->parent != NULL;
}

static const struct gride_view_impl xdg_impl = {
	.configure = impl_configure,
	.set_activated = impl_set_activated,
	.set_maximized = impl_set_maximized,
	.set_fullscreen = impl_set_fullscreen,
	.set_minimized = impl_set_minimized,
	.set_resizing = impl_set_resizing,
	.close = impl_close,
	.get_title = impl_get_title,
	.get_app_id = impl_get_app_id,
	.get_constraints = impl_get_constraints,
	.is_transient = impl_is_transient,
};

/* ---- toplevel events ---------------------------------------------------- */

static void content_size(struct gride_xdg_view *xview, int *w, int *h) {
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(xview->toplevel->base, &geo);
	*w = geo.width;
	*h = geo.height;
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, map);
	int w, h;
	content_size(xview, &w, &h);
	xview->view.content_width = w;
	xview->view.content_height = h;
	view_map(&xview->view, xview->toplevel->base->surface);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, unmap);
	view_unmap(&xview->view);
}

static void decoration_apply(struct gride_xdg_decoration *deco) {
	/* GriDE always draws the title bar for clients that let it. */
	wlr_xdg_toplevel_decoration_v1_set_mode(deco->wlr_decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	if (deco->xview) {
		view_set_ssd(&deco->xview->view, true);
	}
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, commit);
	struct gride_view *view = &xview->view;
	struct wlr_xdg_toplevel *toplevel = xview->toplevel;

	if (toplevel->base->initial_commit) {
		/* The client has committed its initial state (title, app id, any
		 * maximise or fullscreen request). Answer with the first configure. */
		wlr_xdg_toplevel_set_wm_capabilities(toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		if (xview->decoration) {
			decoration_apply(xview->decoration);
		}
		if (toplevel->requested.fullscreen) {
			view_set_fullscreen(view, true, toplevel->requested.fullscreen_output);
		} else if (toplevel->requested.maximized) {
			view_set_maximized(view, true);
		} else {
			/* 0x0 lets the client pick its own size. */
			wlr_xdg_toplevel_set_size(toplevel, 0, 0);
		}
		return;
	}
	if (view->mapped) {
		int w, h;
		content_size(xview, &w, &h);
		view_content_committed(view, w, h);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, destroy);
	wl_list_remove(&xview->map.link);
	wl_list_remove(&xview->unmap.link);
	wl_list_remove(&xview->commit.link);
	wl_list_remove(&xview->destroy.link);
	wl_list_remove(&xview->request_move.link);
	wl_list_remove(&xview->request_resize.link);
	wl_list_remove(&xview->request_maximize.link);
	wl_list_remove(&xview->request_fullscreen.link);
	wl_list_remove(&xview->request_minimize.link);
	wl_list_remove(&xview->set_title.link);
	wl_list_remove(&xview->set_app_id.link);
	if (xview->decoration) {
		xview->decoration->xview = NULL;
	}
	/* The surface subtree destroys itself with the xdg surface; the rest
	 * of our scene nodes go with view_destroy(). */
	view_destroy(&xview->view);
	free(xview);
}

/*
 * Client-initiated move/resize, e.g. dragging a client-side title bar.
 * Only honoured with a valid serial from a button press the client really
 * received, so a client cannot start a grab on its own.
 */
static bool grab_allowed(struct gride_xdg_view *xview, uint32_t serial) {
	return xview->view.mapped && wlr_seat_validate_pointer_grab_serial(
		xview->view.server->seat, xview->toplevel->base->surface, serial);
}

static void handle_request_move(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, request_move);
	struct wlr_xdg_toplevel_move_event *event = data;
	if (grab_allowed(xview, event->serial)) {
		cursor_begin_interactive(&xview->view, GRIDE_CURSOR_MOVE, 0, false);
	}
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	if (grab_allowed(xview, event->serial)) {
		cursor_begin_interactive(&xview->view, GRIDE_CURSOR_RESIZE, event->edges, false);
	}
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, request_maximize);
	/* Before the initial commit the request is picked up in commit(). */
	if (xview->toplevel->base->initialized) {
		view_set_maximized(&xview->view, xview->toplevel->requested.maximized);
	}
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, request_fullscreen);
	if (xview->toplevel->base->initialized) {
		struct wlr_xdg_toplevel_requested *req = &xview->toplevel->requested;
		view_set_fullscreen(&xview->view, req->fullscreen, req->fullscreen_output);
	}
}

static void handle_request_minimize(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, request_minimize);
	if (xview->view.mapped) {
		view_set_minimized(&xview->view, true);
	}
}

static void handle_set_title(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, set_title);
	view_title_changed(&xview->view);
}

static void handle_set_app_id(struct wl_listener *listener, void *data) {
	struct gride_xdg_view *xview = wl_container_of(listener, xview, set_app_id);
	view_title_changed(&xview->view);
}

void xdg_handle_new_toplevel(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *toplevel = data;

	struct gride_xdg_view *xview = calloc(1, sizeof(*xview));
	if (!xview) {
		return;
	}
	xview->toplevel = toplevel;
	view_init(&xview->view, server, GRIDE_VIEW_XDG, &xdg_impl);

	/* wlr_scene_xdg_surface keeps the client's surfaces offset by
	 * (-geometry.x, -geometry.y) inside this tree, so the visible content
	 * starts at the content tree's origin. Popups find their parent's tree
	 * through xdg_surface->data. */
	xview->surface_tree = wlr_scene_xdg_surface_create(xview->view.content_tree,
		toplevel->base);
	toplevel->base->data = xview->surface_tree;

	struct wlr_surface *surface = toplevel->base->surface;
	xview->map.notify = handle_map;
	wl_signal_add(&surface->events.map, &xview->map);
	xview->unmap.notify = handle_unmap;
	wl_signal_add(&surface->events.unmap, &xview->unmap);
	xview->commit.notify = handle_commit;
	wl_signal_add(&surface->events.commit, &xview->commit);
	xview->destroy.notify = handle_destroy;
	wl_signal_add(&toplevel->events.destroy, &xview->destroy);
	xview->request_move.notify = handle_request_move;
	wl_signal_add(&toplevel->events.request_move, &xview->request_move);
	xview->request_resize.notify = handle_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &xview->request_resize);
	xview->request_maximize.notify = handle_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &xview->request_maximize);
	xview->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &xview->request_fullscreen);
	xview->request_minimize.notify = handle_request_minimize;
	wl_signal_add(&toplevel->events.request_minimize, &xview->request_minimize);
	xview->set_title.notify = handle_set_title;
	wl_signal_add(&toplevel->events.set_title, &xview->set_title);
	xview->set_app_id.notify = handle_set_app_id;
	wl_signal_add(&toplevel->events.set_app_id, &xview->set_app_id);
}

/* ---- xdg-decoration ----------------------------------------------------- */

static struct gride_xdg_view *xview_from_toplevel(struct wlr_xdg_toplevel *toplevel) {
	struct wlr_scene_tree *tree = toplevel->base->data;
	for (; tree; tree = tree->node.parent) {
		if (tree->node.data) {
			return tree->node.data;
		}
	}
	return NULL;
}

static void decoration_handle_request_mode(struct wl_listener *listener, void *data) {
	struct gride_xdg_decoration *deco = wl_container_of(listener, deco, request_mode);
	/* Before the initial commit, commit() applies it. */
	if (deco->wlr_decoration->toplevel->base->initialized) {
		decoration_apply(deco);
	}
}

static void decoration_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_xdg_decoration *deco = wl_container_of(listener, deco, destroy);
	if (deco->xview) {
		deco->xview->decoration = NULL;
		/* The client gave up the protocol: it draws its own frame again. */
		view_set_ssd(&deco->xview->view, false);
	}
	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

void xdg_handle_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration = data;
	struct gride_xdg_decoration *deco = calloc(1, sizeof(*deco));
	if (!deco) {
		return;
	}
	deco->wlr_decoration = wlr_decoration;
	deco->xview = xview_from_toplevel(wlr_decoration->toplevel);
	if (deco->xview) {
		deco->xview->decoration = deco;
	}
	deco->request_mode.notify = decoration_handle_request_mode;
	wl_signal_add(&wlr_decoration->events.request_mode, &deco->request_mode);
	deco->destroy.notify = decoration_handle_destroy;
	wl_signal_add(&wlr_decoration->events.destroy, &deco->destroy);

	if (wlr_decoration->toplevel->base->initialized) {
		decoration_apply(deco);
	}
}

/* ---- popups (menus, tooltips) ------------------------------------------ */

/* Keep a popup inside the output its toplevel is on. */
static void popup_unconstrain(struct gride_xdg_popup *popup) {
	struct wlr_xdg_popup *xdg_popup = popup->xdg_popup;
	/* Walk up through nested popups to the toplevel that owns them. */
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	while (parent && parent->role == WLR_XDG_SURFACE_ROLE_POPUP && parent->popup) {
		parent = wlr_xdg_surface_try_from_wlr_surface(parent->popup->parent);
	}
	if (!parent || parent->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || !parent->toplevel) {
		return;
	}
	struct gride_xdg_view *xview = xview_from_toplevel(parent->toplevel);
	if (!xview) {
		return;
	}
	struct gride_view *view = &xview->view;
	struct wlr_box frame;
	view_get_frame(view, &frame);
	struct wlr_output *output = output_for_box(view->server, &frame);
	if (!output) {
		return;
	}
	struct wlr_box out, geo;
	output_get_box(view->server, output, &out);
	wlr_xdg_surface_get_geometry(parent, &geo);
	/* The box is in the toplevel's surface coordinates: its surface origin
	 * is at the content origin minus the geometry offset. */
	int origin_x = view->x - geo.x;
	int origin_y = view->y + view_titlebar_height(view) - geo.y;
	struct wlr_box box = {
		.x = out.x - origin_x,
		.y = out.y - origin_y,
		.width = out.width,
		.height = out.height,
	};
	wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
}

static void popup_handle_commit(struct wl_listener *listener, void *data) {
	struct gride_xdg_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		popup_unconstrain(popup);
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void popup_handle_reposition(struct wl_listener *listener, void *data) {
	struct gride_xdg_popup *popup = wl_container_of(listener, popup, reposition);
	popup_unconstrain(popup);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_xdg_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->reposition.link);
	wl_list_remove(&popup->destroy.link);
	/* The popup's scene subtree removes itself with the xdg surface. */
	free(popup);
}

void xdg_handle_new_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	/* A popup's parent is a toplevel or another popup. Its scene subtree
	 * goes inside the parent's, so it moves and stacks with it. Phase 4
	 * adds layer-shell parents (taskbar menus). */
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (!parent || !parent->data) {
		wlr_log(WLR_DEBUG, "Popup without a known parent; ignoring");
		return;
	}
	struct gride_xdg_popup *popup = calloc(1, sizeof(*popup));
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
