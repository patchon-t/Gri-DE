/*
 * The cursor: pointer focus, clicks, scrolling, title bar buttons and
 * interactive move/resize (which drives the grid overlay).
 */
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "server.h"

#define DOUBLE_CLICK_MS 400
#define DRAG_THRESHOLD 4 /* px the pointer travels before a press becomes a drag */

void cursor_set_image(struct gride_server *server, const char *name) {
	/* Skip redundant sets: each one re-uploads the cursor image. */
	if (server->cursor_image == name) {
		return;
	}
	server->cursor_image = name;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
}

static void process_motion(struct gride_server *server, uint32_t time);
static void begin_interactive_at(struct gride_view *view, enum gride_cursor_mode mode,
	uint32_t edges, bool ours, double cx, double cy);

static void set_hover(struct gride_server *server, struct gride_view *view,
		enum gride_deco_part part) {
	if (server->hover_view && server->hover_view != view && server->hover_view->deco) {
		deco_set_hover(server->hover_view->deco, GRIDE_DECO_NONE);
	}
	server->hover_view = view;
	if (view && view->deco) {
		deco_set_hover(view->deco, part);
	}
}

/* Send pointer enter/motion to whatever is under the cursor. */
static void process_passthrough(struct gride_server *server, uint32_t time) {
	struct gride_hit hit;
	view_hit_test(server, server->cursor->x, server->cursor->y, &hit);
	switch (hit.type) {
	case GRIDE_HIT_SURFACE:
		set_hover(server, NULL, GRIDE_DECO_NONE);
		/* The client sets its own cursor image after enter. */
		if (server->seat->pointer_state.focused_surface != hit.surface) {
			server->cursor_image = NULL;
		}
		wlr_seat_pointer_notify_enter(server->seat, hit.surface, hit.sx, hit.sy);
		wlr_seat_pointer_notify_motion(server->seat, time, hit.sx, hit.sy);
		return;
	case GRIDE_HIT_DECO:
		set_hover(server, hit.view, hit.part);
		cursor_set_image(server, "default");
		break;
	case GRIDE_HIT_RESIZE:
		set_hover(server, NULL, GRIDE_DECO_NONE);
		cursor_set_image(server, wlr_xcursor_get_resize_name(hit.edges));
		break;
	case GRIDE_HIT_NONE:
		set_hover(server, NULL, GRIDE_DECO_NONE);
		cursor_set_image(server, "default");
		break;
	}
	wlr_seat_pointer_notify_clear_focus(server->seat);
}

static void process_move(struct gride_server *server) {
	struct gride_view *view = server->grabbed_view;
	view_move_to(view, (int)(server->cursor->x - server->grab_x),
		(int)(server->cursor->y - server->grab_y));
	grid_drag_update(server);
}

/*
 * Resize against the frame captured at grab start. The edges not being
 * dragged stay fixed. Only the size is sent here; the left/top position is
 * applied when the client commits that size (see view_content_committed).
 */
static void process_resize(struct gride_server *server) {
	struct gride_view *view = server->grabbed_view;
	const struct wlr_box *g = &server->grab_geobox;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int left = g->x, right = g->x + g->width;
	int top = g->y, bottom = g->y + g->height;
	int th = view_titlebar_height(view);

	if (server->resize_edges & WLR_EDGE_TOP) {
		top = (int)border_y;
		if (top > bottom - th - 1) {
			top = bottom - th - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		bottom = (int)border_y;
		if (bottom < top + th + 1) {
			bottom = top + th + 1;
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

	int width = right - left, height = bottom - top - th;
	int min_w, min_h, max_w, max_h;
	view->impl->get_constraints(view, &min_w, &min_h, &max_w, &max_h);
	/* Leave room for the three title bar buttons. */
	if (view->ssd && min_w < 3 * GRIDE_TITLEBAR_BUTTON_WIDTH + GRIDE_TITLEBAR_ICON * 2) {
		min_w = 3 * GRIDE_TITLEBAR_BUTTON_WIDTH + GRIDE_TITLEBAR_ICON * 2;
	}
	if (min_w > 0 && width < min_w) {
		width = min_w;
	}
	if (max_w > 0 && width > max_w) {
		width = max_w;
	}
	if (min_h > 0 && height < min_h) {
		height = min_h;
	}
	if (max_h > 0 && height > max_h) {
		height = max_h;
	}
	view_set_content_size(view, width, height);
	grid_drag_update(server);
}

static void process_motion(struct gride_server *server, uint32_t time) {
	switch (server->cursor_mode) {
	case GRIDE_CURSOR_MOVE:
		process_move(server);
		break;
	case GRIDE_CURSOR_RESIZE:
		process_resize(server);
		break;
	case GRIDE_CURSOR_BUTTON: {
		/* A held title bar button lights up only while the pointer is
		 * still over it. */
		struct gride_view *view = server->grabbed_view;
		struct gride_hit hit;
		view_hit_test(server, server->cursor->x, server->cursor->y, &hit);
		bool over = hit.type == GRIDE_HIT_DECO && hit.view == view &&
			hit.part == server->pressed_part;
		if (view->deco) {
			deco_set_pressed(view->deco, over ? server->pressed_part : GRIDE_DECO_NONE);
		}
		break;
	}
	case GRIDE_CURSOR_PENDING: {
		double dx = server->cursor->x - server->press_x;
		double dy = server->cursor->y - server->press_y;
		if (dx * dx + dy * dy >= DRAG_THRESHOLD * DRAG_THRESHOLD) {
			struct gride_view *view = server->grabbed_view;
			enum gride_cursor_mode mode = server->pending_mode;
			uint32_t edges = server->pending_edges;
			server->cursor_mode = GRIDE_CURSOR_PASSTHROUGH;
			server->grabbed_view = NULL;
			/* Start the drag from where the press happened, so the window
			 * does not jump by the threshold. */
			begin_interactive_at(view, mode, edges, true, server->press_x, server->press_y);
			process_motion(server, time);
		}
		break;
	}
	case GRIDE_CURSOR_PASSTHROUGH:
		process_passthrough(server, time);
		break;
	}
}

/* Arm a compositor-driven move or resize that starts on drag. */
static void begin_pending(struct gride_server *server, struct gride_view *view,
		enum gride_cursor_mode mode, uint32_t edges) {
	if (view->fullscreen || (mode == GRIDE_CURSOR_RESIZE && view->maximized)) {
		return;
	}
	server->cursor_mode = GRIDE_CURSOR_PENDING;
	server->grabbed_view = view;
	server->grab_is_ours = true;
	server->pending_mode = mode;
	server->pending_edges = edges;
	server->press_x = server->cursor->x;
	server->press_y = server->cursor->y;
}

void cursor_rebase(struct gride_server *server, uint32_t time_msec) {
	if (server->cursor_mode == GRIDE_CURSOR_PASSTHROUGH) {
		process_passthrough(server, time_msec ? time_msec : gride_now_msec());
	}
}

/* (cx, cy): the pointer position the drag is measured from. */
static void begin_interactive_at(struct gride_view *view, enum gride_cursor_mode mode,
		uint32_t edges, bool ours, double cx, double cy) {
	struct gride_server *server = view->server;
	if (view->fullscreen || server->cursor_mode != GRIDE_CURSOR_PASSTHROUGH) {
		return;
	}
	if (mode == GRIDE_CURSOR_RESIZE && view->maximized) {
		return;
	}
	view_focus(view);
	set_hover(server, NULL, GRIDE_DECO_NONE);

	struct wlr_box frame;
	view_get_frame(view, &frame);

	if (mode == GRIDE_CURSOR_MOVE && view->maximized) {
		/* Dragging a maximised window restores it under the cursor, at the
		 * same relative position along its width. */
		double rel = frame.width > 0 ? (cx - frame.x) / frame.width : 0.5;
		double dy = cy - frame.y;
		int w = view->has_saved ? view->saved.width : frame.width / 2;
		view->saved_snapped = false; /* dragging out always floats it */
		view_set_maximized(view, false);
		view->center_pending = false; /* the drag decides where it goes */
		view_move_to(view, (int)(cx - rel * w), (int)(cy - dy));
	}

	server->grabbed_view = view;
	server->cursor_mode = mode;
	server->grab_is_ours = ours;

	if (mode == GRIDE_CURSOR_MOVE) {
		server->grab_x = cx - view->x;
		server->grab_y = cy - view->y;
		cursor_set_image(server, "grabbing");
	} else {
		double border_x = frame.x + ((edges & WLR_EDGE_RIGHT) ? frame.width : 0);
		double border_y = frame.y + ((edges & WLR_EDGE_BOTTOM) ? frame.height : 0);
		server->grab_x = cx - border_x;
		server->grab_y = cy - border_y;
		server->grab_geobox = frame;
		server->resize_edges = edges;
		view->impl->set_resizing(view, true);
		cursor_set_image(server, wlr_xcursor_get_resize_name(edges));
	}
	/* While we drive the window, the client does not see the pointer. */
	wlr_seat_pointer_notify_clear_focus(server->seat);
	grid_drag_begin(server, view);
}

void cursor_begin_interactive(struct gride_view *view, enum gride_cursor_mode mode,
		uint32_t edges, bool ours) {
	begin_interactive_at(view, mode, edges, ours, view->server->cursor->x,
		view->server->cursor->y);
}

void cursor_end_interactive(struct gride_server *server) {
	struct gride_view *view = server->grabbed_view;
	if (view && server->cursor_mode == GRIDE_CURSOR_RESIZE) {
		view->impl->set_resizing(view, false);
	}
	if (view && server->cursor_mode == GRIDE_CURSOR_BUTTON && view->deco) {
		deco_set_pressed(view->deco, GRIDE_DECO_NONE);
	}
	/* Ended without a drop (window closed mid-drag): no snap. */
	grid_drag_end(server, false);
	server->cursor_mode = GRIDE_CURSOR_PASSTHROUGH;
	server->grabbed_view = NULL;
	server->grab_is_ours = false;
	server->pressed_part = GRIDE_DECO_NONE;
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

static void title_button_activate(struct gride_view *view, enum gride_deco_part part) {
	switch (part) {
	case GRIDE_DECO_MINIMIZE:
		view_set_minimized(view, true);
		break;
	case GRIDE_DECO_MAXIMIZE:
		view_set_maximized(view, !view->maximized);
		break;
	case GRIDE_DECO_CLOSE:
		view_close(view);
		break;
	default:
		break;
	}
}

static void handle_release(struct gride_server *server, struct wlr_pointer_button_event *event) {
	switch (server->cursor_mode) {
	case GRIDE_CURSOR_BUTTON: {
		struct gride_view *view = server->grabbed_view;
		enum gride_deco_part part = server->pressed_part;
		struct gride_hit hit;
		view_hit_test(server, server->cursor->x, server->cursor->y, &hit);
		bool over = hit.type == GRIDE_HIT_DECO && hit.view == view && hit.part == part;
		cursor_end_interactive(server);
		if (over) {
			title_button_activate(view, part); /* may destroy the view */
		}
		cursor_rebase(server, event->time_msec);
		return;
	}
	case GRIDE_CURSOR_MOVE:
	case GRIDE_CURSOR_RESIZE: {
		bool ours = server->grab_is_ours;
		grid_drag_end(server, true);
		cursor_end_interactive(server);
		cursor_rebase(server, event->time_msec);
		if (ours) {
			return; /* the client never saw the press */
		}
		break;
	}
	case GRIDE_CURSOR_PENDING:
		/* Released before dragging: it was a click, already handled. */
		cursor_end_interactive(server);
		cursor_rebase(server, event->time_msec);
		return;
	case GRIDE_CURSOR_PASSTHROUGH:
		break;
	}
	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void handle_button(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		handle_release(server, event);
		return;
	}
	if (server->cursor_mode != GRIDE_CURSOR_PASSTHROUGH) {
		return; /* a second button during a grab */
	}

	struct gride_hit hit;
	view_hit_test(server, server->cursor->x, server->cursor->y, &hit);
	struct gride_view *view = hit.view;
	bool left = event->button == BTN_LEFT, right = event->button == BTN_RIGHT;

	/* Super+left drag moves, Super+right drag resizes from the nearest corner. */
	if (view && (left || right) && input_current_modifiers(server) == server->opts->mod_mask) {
		view_focus(view);
		if (left) {
			begin_pending(server, view, GRIDE_CURSOR_MOVE, 0);
		} else {
			struct wlr_box frame;
			view_get_frame(view, &frame);
			uint32_t edges = 0;
			edges |= server->cursor->x < frame.x + frame.width / 2.0 ?
				WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
			edges |= server->cursor->y < frame.y + frame.height / 2.0 ?
				WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
			begin_pending(server, view, GRIDE_CURSOR_RESIZE, edges);
		}
		if (server->cursor_mode != GRIDE_CURSOR_PASSTHROUGH) {
			return; /* armed; a refused one falls through to a click */
		}
	}

	switch (hit.type) {
	case GRIDE_HIT_DECO:
		view_focus(view);
		if (!left) {
			return;
		}
		if (hit.part == GRIDE_DECO_TITLE) {
			/* Double-click the title to maximise or restore. */
			if (server->last_title_click_view == view &&
					event->time_msec - server->last_title_click_msec < DOUBLE_CLICK_MS) {
				server->last_title_click_view = NULL;
				view_set_maximized(view, !view->maximized);
				return;
			}
			server->last_title_click_view = view;
			server->last_title_click_msec = event->time_msec;
			begin_pending(server, view, GRIDE_CURSOR_MOVE, 0);
		} else if (hit.part != GRIDE_DECO_NONE) {
			server->cursor_mode = GRIDE_CURSOR_BUTTON;
			server->grabbed_view = view;
			server->pressed_part = hit.part;
			server->grab_is_ours = true;
			deco_set_pressed(view->deco, hit.part);
		}
		return;
	case GRIDE_HIT_RESIZE:
		view_focus(view);
		if (left) {
			begin_pending(server, view, GRIDE_CURSOR_RESIZE, hit.edges);
		}
		return;
	case GRIDE_HIT_SURFACE:
		if (view && view != server->focused_view) {
			view_focus(view); /* click to focus, and raise */
		} else if (view) {
			view_raise(view);
		}
		break;
	case GRIDE_HIT_NONE:
		break;
	}
	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
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
