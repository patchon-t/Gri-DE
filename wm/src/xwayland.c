/*
 * X11 windows through XWayland.
 *
 * XWayland starts lazily: the X server only launches when the first X11
 * client connects, so a session of Wayland apps pays nothing for it.
 *
 * Lifecycle of an X11 window in wlroots 0.18:
 *   new_surface  an X window exists; it may never be shown
 *   associate    it got a wl_surface: map/unmap/commit can be listened to
 *   map/unmap    on that wl_surface
 *   dissociate   the wl_surface is going away
 *   destroy      the X window is gone
 * Override-redirect windows (menus, tooltips, drag icons) place themselves
 * and are not managed: they get struct gride_unmanaged instead of a view.
 */
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>

#include "server.h"

struct gride_xwayland_view {
	struct gride_view view; /* first, so the generic code's pointer casts back */
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *surface_tree; /* while mapped */

	struct wl_listener destroy;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener request_configure;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener request_activate;
	struct wl_listener set_title;
	struct wl_listener set_class;
	struct wl_listener set_decorations;
	struct wl_listener set_override_redirect;
};

struct gride_unmanaged {
	struct gride_server *server;
	struct wlr_xwayland_surface *xsurface;
	struct wlr_scene_tree *surface_tree; /* while mapped */

	struct wl_listener destroy;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener request_configure;
	struct wl_listener set_geometry;
	struct wl_listener set_override_redirect;
};

static void create_view(struct gride_server *server, struct wlr_xwayland_surface *xsurface);
static void create_unmanaged(struct gride_server *server, struct wlr_xwayland_surface *xsurface);

static struct gride_xwayland_view *xw_view(struct gride_view *view) {
	return (struct gride_xwayland_view *)view;
}

/* An X window asks for no title bar (or no frame at all) through Motif
 * hints; Chrome and some games do. */
static bool wants_ssd(struct wlr_xwayland_surface *xsurface) {
	return xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
}

/* ---- gride_view_impl ---------------------------------------------------- */

static void impl_configure(struct gride_view *view, int x, int y, int width, int height) {
	struct wlr_xwayland_surface *xs = xw_view(view)->xsurface;
	/* X11 has no "you choose" size: 0 means keep the current one. */
	if (width <= 0) {
		width = xs->width > 0 ? xs->width : 1;
	}
	if (height <= 0) {
		height = xs->height > 0 ? xs->height : 1;
	}
	wlr_xwayland_surface_configure(xs, (int16_t)x, (int16_t)y,
		(uint16_t)width, (uint16_t)height);
}

static void impl_set_activated(struct gride_view *view, bool activated) {
	struct wlr_xwayland_surface *xs = xw_view(view)->xsurface;
	wlr_xwayland_surface_activate(xs, activated);
	if (activated) {
		/* Keep the X server's stacking order in step with ours, or X11
		 * apps compute the wrong window under the pointer for drags. */
		wlr_xwayland_surface_restack(xs, NULL, XCB_STACK_MODE_ABOVE);
	}
}

static void impl_set_maximized(struct gride_view *view, bool maximized) {
	wlr_xwayland_surface_set_maximized(xw_view(view)->xsurface, maximized);
}

static void impl_set_fullscreen(struct gride_view *view, bool fullscreen) {
	wlr_xwayland_surface_set_fullscreen(xw_view(view)->xsurface, fullscreen);
}

static void impl_set_minimized(struct gride_view *view, bool minimized) {
	wlr_xwayland_surface_set_minimized(xw_view(view)->xsurface, minimized);
}

static void impl_set_resizing(struct gride_view *view, bool resizing) {
	/* X11 has no resizing state. */
}

static void impl_close(struct gride_view *view) {
	wlr_xwayland_surface_close(xw_view(view)->xsurface);
}

static const char *impl_get_title(struct gride_view *view) {
	return xw_view(view)->xsurface->title;
}

static const char *impl_get_app_id(struct gride_view *view) {
	return xw_view(view)->xsurface->class;
}

static void impl_get_constraints(struct gride_view *view, int *min_w, int *min_h,
		int *max_w, int *max_h) {
	xcb_size_hints_t *hints = xw_view(view)->xsurface->size_hints;
	*min_w = *min_h = *max_w = *max_h = 0;
	if (!hints) {
		return;
	}
	if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
		*min_w = hints->min_width > 0 ? hints->min_width : 0;
		*min_h = hints->min_height > 0 ? hints->min_height : 0;
	}
	if (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
		*max_w = hints->max_width > 0 ? hints->max_width : 0;
		*max_h = hints->max_height > 0 ? hints->max_height : 0;
	}
}

static bool impl_is_transient(struct gride_view *view) {
	struct wlr_xwayland_surface *xs = xw_view(view)->xsurface;
	return xs->parent != NULL || xs->modal;
}

static const struct gride_view_impl xwayland_impl = {
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

/* ---- managed windows ---------------------------------------------------- */

static void view_handle_map(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, map);
	struct wlr_xwayland_surface *xs = xv->xsurface;
	struct gride_view *view = &xv->view;

	/* The surface and its subsurfaces, inside our content tree. */
	xv->surface_tree = wlr_scene_subsurface_tree_create(view->content_tree, xs->surface);
	view->content_width = xs->width;
	view->content_height = xs->height;
	view_set_ssd(view, wants_ssd(xs));
	view_map(view, xs->surface);
	/* Tell the client where it ended up (view_map may have moved it). */
	view_set_content_size(view, view->content_width, view->content_height);
}

static void view_handle_unmap(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, unmap);
	view_unmap(&xv->view);
	if (xv->surface_tree) {
		wlr_scene_node_destroy(&xv->surface_tree->node);
		xv->surface_tree = NULL;
	}
}

static void view_handle_commit(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, commit);
	struct wlr_surface *surface = xv->xsurface->surface;
	view_content_committed(&xv->view, surface->current.width, surface->current.height);
}

static void view_handle_associate(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, associate);
	struct wlr_surface *surface = xv->xsurface->surface;
	xv->map.notify = view_handle_map;
	wl_signal_add(&surface->events.map, &xv->map);
	xv->unmap.notify = view_handle_unmap;
	wl_signal_add(&surface->events.unmap, &xv->unmap);
	xv->commit.notify = view_handle_commit;
	wl_signal_add(&surface->events.commit, &xv->commit);
}

static void view_handle_dissociate(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, dissociate);
	wl_list_remove(&xv->map.link);
	wl_list_remove(&xv->unmap.link);
	wl_list_remove(&xv->commit.link);
}

static void view_handle_request_configure(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	struct gride_view *view = &xv->view;
	if (!view->mapped) {
		/* Not shown yet: give it what it asks for; placement happens at map. */
		wlr_xwayland_surface_configure(xv->xsurface, event->x, event->y,
			event->width, event->height);
		return;
	}
	if (view->snapped || view->maximized || view->fullscreen ||
			view->server->grabbed_view == view) {
		/* We decide its geometry: repeat it. */
		view_set_content_size(view, view->content_width, view->content_height);
		return;
	}
	/* A floating window may move and resize itself. */
	view_move_to(view, event->x, event->y - view_titlebar_height(view));
	view_set_content_size(view, event->width, event->height);
}

static bool button_held(struct gride_server *server) {
	return server->seat->pointer_state.button_count > 0;
}

static void view_handle_request_move(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, request_move);
	/* X11 has no grab serials; only start while a button is really down. */
	if (xv->view.mapped && button_held(xv->view.server)) {
		cursor_begin_interactive(&xv->view, GRIDE_CURSOR_MOVE, 0, false);
	}
}

static void view_handle_request_resize(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, request_resize);
	struct wlr_xwayland_resize_event *event = data;
	if (xv->view.mapped && button_held(xv->view.server)) {
		cursor_begin_interactive(&xv->view, GRIDE_CURSOR_RESIZE, event->edges, false);
	}
}

static void view_handle_request_maximize(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, request_maximize);
	struct wlr_xwayland_surface *xs = xv->xsurface;
	/* wlroots has already stored the requested _NET_WM_STATE. */
	bool maximized = xs->maximized_vert && xs->maximized_horz;
	if (xv->view.mapped) {
		view_set_maximized(&xv->view, maximized);
	} else {
		xv->view.maximized = maximized;
	}
}

static void view_handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, request_fullscreen);
	if (xv->view.mapped) {
		view_set_fullscreen(&xv->view, xv->xsurface->fullscreen, NULL);
	} else {
		xv->view.fullscreen = xv->xsurface->fullscreen;
	}
}

static void view_handle_request_minimize(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, request_minimize);
	struct wlr_xwayland_minimize_event *event = data;
	view_set_minimized(&xv->view, event->minimize);
}

static void view_handle_request_activate(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, request_activate);
	if (xv->view.mapped) {
		view_focus(&xv->view);
	}
}

static void view_handle_set_title(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, set_title);
	view_title_changed(&xv->view);
}

static void view_handle_set_class(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, set_class);
	view_title_changed(&xv->view);
}

static void view_handle_set_decorations(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, set_decorations);
	if (xv->view.mapped) {
		view_set_ssd(&xv->view, wants_ssd(xv->xsurface));
	}
}

static void view_remove_listeners(struct gride_xwayland_view *xv) {
	if (xv->xsurface->surface) {
		/* Still associated: the map/unmap/commit listeners are attached. */
		wl_list_remove(&xv->map.link);
		wl_list_remove(&xv->unmap.link);
		wl_list_remove(&xv->commit.link);
	}
	wl_list_remove(&xv->destroy.link);
	wl_list_remove(&xv->associate.link);
	wl_list_remove(&xv->dissociate.link);
	wl_list_remove(&xv->request_configure.link);
	wl_list_remove(&xv->request_move.link);
	wl_list_remove(&xv->request_resize.link);
	wl_list_remove(&xv->request_maximize.link);
	wl_list_remove(&xv->request_fullscreen.link);
	wl_list_remove(&xv->request_minimize.link);
	wl_list_remove(&xv->request_activate.link);
	wl_list_remove(&xv->set_title.link);
	wl_list_remove(&xv->set_class.link);
	wl_list_remove(&xv->set_decorations.link);
	wl_list_remove(&xv->set_override_redirect.link);
}

static void view_free(struct gride_xwayland_view *xv) {
	if (xv->view.mapped) {
		view_handle_unmap(&xv->unmap, NULL);
	}
	view_remove_listeners(xv);
	xv->xsurface->data = NULL;
	view_destroy(&xv->view);
	free(xv);
}

static void view_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, destroy);
	view_free(xv);
}

/* Rare: a window switches between managed and override-redirect. */
static void view_handle_set_override_redirect(struct wl_listener *listener, void *data) {
	struct gride_xwayland_view *xv = wl_container_of(listener, xv, set_override_redirect);
	struct gride_server *server = xv->view.server;
	struct wlr_xwayland_surface *xs = xv->xsurface;
	bool associated = xs->surface != NULL;
	bool mapped = associated && xs->surface->mapped;
	view_free(xv);
	create_unmanaged(server, xs);
	if (associated) {
		struct gride_unmanaged *um = xs->data;
		um->associate.notify(&um->associate, NULL);
		if (mapped) {
			um->map.notify(&um->map, NULL);
		}
	}
}

static void create_view(struct gride_server *server, struct wlr_xwayland_surface *xs) {
	struct gride_xwayland_view *xv = calloc(1, sizeof(*xv));
	if (!xv) {
		return;
	}
	xv->xsurface = xs;
	xs->data = xv;
	view_init(&xv->view, server, GRIDE_VIEW_XWAYLAND, &xwayland_impl);

	xv->destroy.notify = view_handle_destroy;
	wl_signal_add(&xs->events.destroy, &xv->destroy);
	xv->associate.notify = view_handle_associate;
	wl_signal_add(&xs->events.associate, &xv->associate);
	xv->dissociate.notify = view_handle_dissociate;
	wl_signal_add(&xs->events.dissociate, &xv->dissociate);
	xv->request_configure.notify = view_handle_request_configure;
	wl_signal_add(&xs->events.request_configure, &xv->request_configure);
	xv->request_move.notify = view_handle_request_move;
	wl_signal_add(&xs->events.request_move, &xv->request_move);
	xv->request_resize.notify = view_handle_request_resize;
	wl_signal_add(&xs->events.request_resize, &xv->request_resize);
	xv->request_maximize.notify = view_handle_request_maximize;
	wl_signal_add(&xs->events.request_maximize, &xv->request_maximize);
	xv->request_fullscreen.notify = view_handle_request_fullscreen;
	wl_signal_add(&xs->events.request_fullscreen, &xv->request_fullscreen);
	xv->request_minimize.notify = view_handle_request_minimize;
	wl_signal_add(&xs->events.request_minimize, &xv->request_minimize);
	xv->request_activate.notify = view_handle_request_activate;
	wl_signal_add(&xs->events.request_activate, &xv->request_activate);
	xv->set_title.notify = view_handle_set_title;
	wl_signal_add(&xs->events.set_title, &xv->set_title);
	xv->set_class.notify = view_handle_set_class;
	wl_signal_add(&xs->events.set_class, &xv->set_class);
	xv->set_decorations.notify = view_handle_set_decorations;
	wl_signal_add(&xs->events.set_decorations, &xv->set_decorations);
	xv->set_override_redirect.notify = view_handle_set_override_redirect;
	wl_signal_add(&xs->events.set_override_redirect, &xv->set_override_redirect);
}

/* ---- unmanaged windows (menus, tooltips) -------------------------------- */

static void unmanaged_handle_map(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, map);
	struct wlr_xwayland_surface *xs = um->xsurface;
	um->surface_tree = wlr_scene_subsurface_tree_create(um->server->layers.unmanaged,
		xs->surface);
	wlr_scene_node_set_position(&um->surface_tree->node, xs->x, xs->y);
	/* Menus that take keyboard input get it while open. */
	if (wlr_xwayland_or_surface_wants_focus(xs)) {
		struct wlr_seat *seat = um->server->seat;
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
		wlr_seat_keyboard_notify_enter(seat, xs->surface, kb ? kb->keycodes : NULL,
			kb ? kb->num_keycodes : 0, kb ? &kb->modifiers : NULL);
	}
}

static void unmanaged_handle_unmap(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, unmap);
	struct gride_server *server = um->server;
	if (um->surface_tree) {
		wlr_scene_node_destroy(&um->surface_tree->node);
		um->surface_tree = NULL;
	}
	/* Hand the keyboard back to the focused window. */
	if (server->seat->keyboard_state.focused_surface == um->xsurface->surface) {
		struct gride_view *view = server->focused_view;
		server->focused_view = NULL;
		if (view) {
			view_focus(view);
		} else {
			wlr_seat_keyboard_notify_clear_focus(server->seat);
		}
	}
	cursor_rebase(server, 0);
}

static void unmanaged_handle_associate(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, associate);
	struct wlr_surface *surface = um->xsurface->surface;
	um->map.notify = unmanaged_handle_map;
	wl_signal_add(&surface->events.map, &um->map);
	um->unmap.notify = unmanaged_handle_unmap;
	wl_signal_add(&surface->events.unmap, &um->unmap);
}

static void unmanaged_handle_dissociate(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, dissociate);
	wl_list_remove(&um->map.link);
	wl_list_remove(&um->unmap.link);
}

static void unmanaged_handle_request_configure(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	wlr_xwayland_surface_configure(um->xsurface, event->x, event->y,
		event->width, event->height);
}

static void unmanaged_handle_set_geometry(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, set_geometry);
	if (um->surface_tree) {
		wlr_scene_node_set_position(&um->surface_tree->node, um->xsurface->x, um->xsurface->y);
	}
}

static void unmanaged_free(struct gride_unmanaged *um) {
	if (um->surface_tree) {
		unmanaged_handle_unmap(&um->unmap, NULL);
	}
	if (um->xsurface->surface) {
		wl_list_remove(&um->map.link);
		wl_list_remove(&um->unmap.link);
	}
	wl_list_remove(&um->destroy.link);
	wl_list_remove(&um->associate.link);
	wl_list_remove(&um->dissociate.link);
	wl_list_remove(&um->request_configure.link);
	wl_list_remove(&um->set_geometry.link);
	wl_list_remove(&um->set_override_redirect.link);
	um->xsurface->data = NULL;
	free(um);
}

static void unmanaged_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, destroy);
	unmanaged_free(um);
}

static void unmanaged_handle_set_override_redirect(struct wl_listener *listener, void *data) {
	struct gride_unmanaged *um = wl_container_of(listener, um, set_override_redirect);
	struct gride_server *server = um->server;
	struct wlr_xwayland_surface *xs = um->xsurface;
	bool associated = xs->surface != NULL;
	bool mapped = associated && xs->surface->mapped;
	unmanaged_free(um);
	create_view(server, xs);
	if (associated) {
		struct gride_xwayland_view *xv = xs->data;
		xv->associate.notify(&xv->associate, NULL);
		if (mapped) {
			xv->map.notify(&xv->map, NULL);
		}
	}
}

static void create_unmanaged(struct gride_server *server, struct wlr_xwayland_surface *xs) {
	struct gride_unmanaged *um = calloc(1, sizeof(*um));
	if (!um) {
		return;
	}
	um->server = server;
	um->xsurface = xs;
	xs->data = um;
	um->destroy.notify = unmanaged_handle_destroy;
	wl_signal_add(&xs->events.destroy, &um->destroy);
	um->associate.notify = unmanaged_handle_associate;
	wl_signal_add(&xs->events.associate, &um->associate);
	um->dissociate.notify = unmanaged_handle_dissociate;
	wl_signal_add(&xs->events.dissociate, &um->dissociate);
	um->request_configure.notify = unmanaged_handle_request_configure;
	wl_signal_add(&xs->events.request_configure, &um->request_configure);
	um->set_geometry.notify = unmanaged_handle_set_geometry;
	wl_signal_add(&xs->events.set_geometry, &um->set_geometry);
	um->set_override_redirect.notify = unmanaged_handle_set_override_redirect;
	wl_signal_add(&xs->events.set_override_redirect, &um->set_override_redirect);
}

/* ---- server ------------------------------------------------------------- */

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xs = data;
	if (xs->override_redirect) {
		create_unmanaged(server, xs);
	} else {
		create_view(server, xs);
	}
}

static void handle_ready(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, xwayland_ready);
	wlr_xwayland_set_seat(server->xwayland, server->seat);

	/* The image X11 shows over the root window and windows that set none. */
	wlr_xcursor_manager_load(server->cursor_mgr, 1);
	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server->cursor_mgr,
		"default", 1);
	if (xcursor && xcursor->image_count > 0) {
		struct wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(server->xwayland, image->buffer, image->width * 4,
			image->width, image->height, (int32_t)image->hotspot_x,
			(int32_t)image->hotspot_y);
	}
	wlr_log(WLR_INFO, "XWayland is ready on DISPLAY=%s", server->xwayland->display_name);
}

bool xwayland_init(struct gride_server *server) {
	server->xwayland = wlr_xwayland_create(server->wl_display, server->compositor, true);
	if (!server->xwayland) {
		wlr_log(WLR_ERROR, "Cannot set up XWayland; X11 apps will not run");
		return false;
	}
	server->xwayland_ready.notify = handle_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
	server->new_xwayland_surface.notify = handle_new_surface;
	wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);
	/* The socket exists already; Xwayland starts on the first connection. */
	setenv("DISPLAY", server->xwayland->display_name, true);
	wlr_log(WLR_INFO, "XWayland starts on demand: DISPLAY=%s", server->xwayland->display_name);
	return true;
}

void xwayland_finish(struct gride_server *server) {
	if (!server->xwayland) {
		return;
	}
	wl_list_remove(&server->xwayland_ready.link);
	wl_list_remove(&server->new_xwayland_surface.link);
	wlr_xwayland_destroy(server->xwayland);
	server->xwayland = NULL;
}
