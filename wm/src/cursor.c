/* The cursor: pointer focus, clicks, scrolling, interactive move/resize. */
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "server.h"

static uint32_t now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Send pointer enter/motion to whatever surface is under the cursor. */
static void process_passthrough(struct gride_server *server, uint32_t time) {
	double sx, sy;
	struct wlr_surface *surface = NULL;
	view_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (!surface) {
		/* Over the desktop: nobody set an image, so we do. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
		wlr_seat_pointer_notify_clear_focus(server->seat);
		return;
	}
	/* enter is a no-op when the surface already has pointer focus. */
	wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
}

static void process_move(struct gride_server *server) {
	struct gride_view *view = server->grabbed_view;
	view_move_to(view, (int)(server->cursor->x - server->grab_x),
		(int)(server->cursor->y - server->grab_y));
}

/*
 * Resize against the box captured at grab start. The edges not being
 * dragged stay fixed. Only the size is sent here; the left/top position is
 * applied when the client commits that size (see view_handle_commit).
 */
static void process_resize(struct gride_server *server) {
	struct gride_view *view = server->grabbed_view;
	const struct wlr_box *g = &server->grab_geobox;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int left = g->x, right = g->x + g->width;
	int top = g->y, bottom = g->y + g->height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		top = (int)border_y;
		if (top >= bottom) {
			top = bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		bottom = (int)border_y;
		if (bottom <= top) {
			bottom = top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		left = (int)border_x;
		if (left >= right) {
			left = right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		right = (int)border_x;
		if (right <= left) {
			right = left + 1;
		}
	}

	int width = right - left, height = bottom - top;
	const struct wlr_xdg_toplevel_state *cur = &view->xdg_toplevel->current;
	if (cur->min_width > 0 && width < cur->min_width) {
		width = cur->min_width;
	}
	if (cur->max_width > 0 && width > cur->max_width) {
		width = cur->max_width;
	}
	if (cur->min_height > 0 && height < cur->min_height) {
		height = cur->min_height;
	}
	if (cur->max_height > 0 && height > cur->max_height) {
		height = cur->max_height;
	}
	view_set_size(view, width, height);
}

static void process_motion(struct gride_server *server, uint32_t time) {
	switch (server->cursor_mode) {
	case GRIDE_CURSOR_MOVE:
		process_move(server);
		break;
	case GRIDE_CURSOR_RESIZE:
		process_resize(server);
		break;
	case GRIDE_CURSOR_PASSTHROUGH:
		process_passthrough(server, time);
		break;
	}
}

void cursor_rebase(struct gride_server *server, uint32_t time_msec) {
	if (server->cursor_mode == GRIDE_CURSOR_PASSTHROUGH) {
		process_passthrough(server, time_msec ? time_msec : now_msec());
	}
}

void cursor_begin_interactive(struct gride_view *view, enum gride_cursor_mode mode,
		uint32_t edges, bool ours) {
	struct gride_server *server = view->server;
	if (view->fullscreen || server->cursor_mode != GRIDE_CURSOR_PASSTHROUGH) {
		return;
	}
	if (mode == GRIDE_CURSOR_RESIZE && view->maximized) {
		return;
	}
	view_focus(view);

	struct wlr_box geo;
	view_get_geometry(view, &geo);
	double cx = server->cursor->x, cy = server->cursor->y;

	if (mode == GRIDE_CURSOR_MOVE && view->maximized) {
		/* Dragging a maximised window restores it under the cursor, at the
		 * same relative position along its width. */
		double rel = geo.width > 0 ? (cx - view->x) / geo.width : 0.5;
		double dy = cy - view->y;
		view_set_maximized(view, false);
		int w = view->has_saved ? view->saved.width : geo.width / 2;
		view_move_to(view, (int)(cx - rel * w), (int)(cy - dy));
		view_get_geometry(view, &geo);
	}

	server->grabbed_view = view;
	server->cursor_mode = mode;
	server->grab_is_ours = ours;

	if (mode == GRIDE_CURSOR_MOVE) {
		server->grab_x = cx - view->x;
		server->grab_y = cy - view->y;
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "grabbing");
	} else {
		double border_x = view->x + ((edges & WLR_EDGE_RIGHT) ? geo.width : 0);
		double border_y = view->y + ((edges & WLR_EDGE_BOTTOM) ? geo.height : 0);
		server->grab_x = cx - border_x;
		server->grab_y = cy - border_y;
		server->grab_geobox = (struct wlr_box){ view->x, view->y, geo.width, geo.height };
		server->resize_edges = edges;
		wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			wlr_xcursor_get_resize_name(edges));
	}
	/* While we drive the window, the client does not see the pointer. */
	wlr_seat_pointer_notify_clear_focus(server->seat);
}

void cursor_end_interactive(struct gride_server *server) {
	struct gride_view *view = server->grabbed_view;
	if (view && server->cursor_mode == GRIDE_CURSOR_RESIZE) {
		wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, false);
	}
	server->cursor_mode = GRIDE_CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
	server->grab_is_ours = false;
}

/* ---- wlr_cursor events -------------------------------------------------- */

static void handle_motion(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	/* wlr_cursor applies output layout bounds and pointer constraints. */
	wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_motion(server, event->time_msec);
}

static void handle_motion_absolute(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	/* Nested backends and tablets report absolute positions. */
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
	process_motion(server, event->time_msec);
}

static uint32_t current_mods(struct gride_server *server) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (!keyboard) {
		return 0;
	}
	return wlr_keyboard_get_modifiers(keyboard) &
		~(uint32_t)(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
}

static void handle_button(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->cursor_mode != GRIDE_CURSOR_PASSTHROUGH) {
			bool ours = server->grab_is_ours;
			cursor_end_interactive(server);
			cursor_rebase(server, event->time_msec);
			if (ours) {
				return; /* the client never saw the press */
			}
		}
		wlr_seat_pointer_notify_button(server->seat, event->time_msec,
			event->button, event->state);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct gride_view *view = view_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy);

	/* Super+left drag moves, Super+right drag resizes from the nearest corner. */
	if (view && current_mods(server) == server->opts->mod_mask &&
			(event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
		if (event->button == BTN_LEFT) {
			cursor_begin_interactive(view, GRIDE_CURSOR_MOVE, 0, true);
		} else {
			struct wlr_box geo;
			view_get_geometry(view, &geo);
			uint32_t edges = 0;
			edges |= server->cursor->x < view->x + geo.width / 2.0 ?
				WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
			edges |= server->cursor->y < view->y + geo.height / 2.0 ?
				WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
			cursor_begin_interactive(view, GRIDE_CURSOR_RESIZE, edges, true);
		}
		if (server->cursor_mode != GRIDE_CURSOR_PASSTHROUGH) {
			return; /* grab started; a refused grab falls through to a click */
		}
	}

	if (view) {
		view_focus(view); /* click to focus, and raise */
	}
	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);
}

static void handle_axis(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
		event->delta, event->delta_discrete, event->source, event->relative_direction);
}

/* A frame groups the pointer events of one hardware report. */
static void handle_frame(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

void cursor_init(struct gride_server *server) {
	/* wlr_cursor is the image on screen plus the logic that moves it
	 * across the output layout. It is fed by all pointer devices. */
	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

	/* XCursor themes are loaded lazily per output scale. NULL = the
	 * default theme ($XCURSOR_THEME); $XCURSOR_SIZE sets the size. */
	const char *size_env = getenv("XCURSOR_SIZE");
	int size = size_env ? atoi(size_env) : 0;
	server->cursor_mgr = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"),
		size > 0 ? (uint32_t)size : 24);

	server->cursor_mode = GRIDE_CURSOR_PASSTHROUGH;
	server->cursor_motion.notify = handle_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = handle_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = handle_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = handle_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = handle_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
}

void cursor_finish(struct gride_server *server) {
	wl_list_remove(&server->cursor_motion.link);
	wl_list_remove(&server->cursor_motion_absolute.link);
	wl_list_remove(&server->cursor_button.link);
	wl_list_remove(&server->cursor_axis.link);
	wl_list_remove(&server->cursor_frame.link);
	wlr_xcursor_manager_destroy(server->cursor_mgr);
	wlr_cursor_destroy(server->cursor);
}
