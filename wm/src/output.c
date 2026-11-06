/* Outputs (monitors, or the host window when nested). */
#include <stdlib.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "server.h"

/*
 * `frame` fires when the output is ready for a new frame (vblank pacing).
 * The scene graph tracks damage, so wlr_scene_output_commit() renders only
 * what changed and does nothing at all when nothing did. That is what keeps
 * an idle desktop near zero CPU and GPU on a low-end laptop.
 */
static void output_handle_frame(struct wl_listener *listener, void *data) {
	struct gride_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(scene, output->wlr_output);
	if (!scene_output) {
		return;
	}
	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

/*
 * The backend asks for a new state, e.g. when the nested host window is
 * resized. We accept it as is. The output layout notices the new size and
 * emits `change`, which redraws the desktop and re-fits maximised windows.
 */
static void output_handle_request_state(struct wl_listener *listener, void *data) {
	struct gride_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_output *output = wl_container_of(listener, output, destroy);
	wlr_log(WLR_INFO, "Output %s removed", output->wlr_output->name);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	/* The scene output and the layout entry are destroyed by wlroots
	 * itself: both listen to this same wlr_output's destroy signal. */
	background_destroy(output->background);
	free(output);
}

void output_handle_new(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Tell the output which allocator and renderer to draw with. */
	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "Cannot init rendering on output %s", wlr_output->name);
		return;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	/* DRM outputs have modes; nested and headless outputs do not. */
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}
	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR, "Cannot enable output %s", wlr_output->name);
		return;
	}

	struct gride_output *output = calloc(1, sizeof(*output));
	if (!output) {
		return;
	}
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output;

	output->frame.notify = output_handle_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_handle_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_handle_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(server->outputs.prev, &output->link);
	output->background = background_create(server->layers.background);

	/*
	 * Outputs are placed left to right in the order they appear. Phase 6
	 * reads explicit positions from [outputs] in config.toml.
	 *
	 * The scene output is the scene graph's view of this output; linking
	 * it to the layout entry keeps its position in sync with the layout.
	 * Adding to the layout emits `change`, which draws the background.
	 */
	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	wlr_log(WLR_INFO, "Output %s enabled: %dx%d, scale %.2f", wlr_output->name,
		wlr_output->width, wlr_output->height, wlr_output->scale);
}

void output_handle_layout_change(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, layout_change);
	struct gride_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (output->background && !wlr_box_empty(&box)) {
			background_update(output->background, server->palette, output->wlr_output, &box);
		}
	}
	views_arrange(server);
}

void output_get_box(struct gride_server *server, struct wlr_output *output, struct wlr_box *box) {
	wlr_output_layout_get_box(server->output_layout, output, box);
}

struct wlr_output *output_at_cursor(struct gride_server *server) {
	struct wlr_output *output = wlr_output_layout_output_at(server->output_layout,
		server->cursor->x, server->cursor->y);
	if (!output && !wl_list_empty(&server->outputs)) {
		struct gride_output *first = wl_container_of(server->outputs.next, first, link);
		output = first->wlr_output;
	}
	return output;
}

/* The output that holds the centre of `box`, or the nearest one to it. */
struct wlr_output *output_for_box(struct gride_server *server, const struct wlr_box *box) {
	double cx = box->x + box->width / 2.0;
	double cy = box->y + box->height / 2.0;
	struct wlr_output *output = wlr_output_layout_output_at(server->output_layout, cx, cy);
	if (output) {
		return output;
	}
	double nx, ny;
	wlr_output_layout_closest_point(server->output_layout, NULL, cx, cy, &nx, &ny);
	output = wlr_output_layout_output_at(server->output_layout, nx, ny);
	return output ? output : output_at_cursor(server);
}
