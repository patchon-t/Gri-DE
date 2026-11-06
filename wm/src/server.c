/* Creating, running and tearing down the compositor. */
#include <signal.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <time.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "draw.h"
#include "icons.h"
#include "server.h"

uint32_t gride_now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int handle_signal(int signal, void *data) {
	struct gride_server *server = data;
	wlr_log(WLR_INFO, "Caught signal %d, shutting down", signal);
	wl_display_terminate(server->wl_display);
	return 0;
}

bool server_init(struct gride_server *server, const struct gride_options *opts) {
	server->opts = opts;
	server->palette = opts->dark ? &gride_palette_dark : &gride_palette_light;
	server->shadow = opts->dark ? gride_shadow_float_dark : gride_shadow_float_light;
	server->deco_scale = 1.0f;
	wl_list_init(&server->minimized);
	/* Before anything draws text: makes the bundled Rubik visible. */
	draw_init_fonts();
	deco_init_shared(server);
	wl_list_init(&server->outputs);
	wl_list_init(&server->views);
	wl_list_init(&server->keyboards);

	server->wl_display = wl_display_create();
	if (!server->wl_display) {
		wlr_log(WLR_ERROR, "Cannot create the Wayland display");
		return false;
	}
	server->event_loop = wl_display_get_event_loop(server->wl_display);
	server->sigint_source = wl_event_loop_add_signal(server->event_loop, SIGINT,
		handle_signal, server);
	server->sigterm_source = wl_event_loop_add_signal(server->event_loop, SIGTERM,
		handle_signal, server);

	/*
	 * The backend picks itself from the environment: a window on the host
	 * when WAYLAND_DISPLAY or DISPLAY is set (nested development), DRM and
	 * libinput through a logind/seatd session on a TTY. WLR_BACKENDS
	 * overrides it (headless for tests).
	 */
	server->backend = wlr_backend_autocreate(server->event_loop, &server->session);
	if (!server->backend) {
		wlr_log(WLR_ERROR, "Cannot create a wlroots backend");
		return false;
	}
	/* The nested backends are connected now. Children must not inherit
	 * the host's X display; xwayland_init() points DISPLAY at ours. */
	unsetenv("DISPLAY");

	/* GLES2 on most hardware, pixman (CPU) as the fallback. WLR_RENDERER
	 * overrides it. */
	server->renderer = wlr_renderer_autocreate(server->backend);
	if (!server->renderer) {
		wlr_log(WLR_ERROR, "Cannot create a renderer");
		return false;
	}
	/* Advertises wl_shm and linux-dmabuf for the formats the renderer reads. */
	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

	/* The allocator makes the buffers outputs are rendered into. */
	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (!server->allocator) {
		wlr_log(WLR_ERROR, "Cannot create an allocator");
		return false;
	}

	/*
	 * Globals clients bind to. Each object is owned by the display and
	 * destroyed with it, so none are kept here unless we use them.
	 */
	server->compositor = wlr_compositor_create(server->wl_display, 6, server->renderer);
	wlr_subcompositor_create(server->wl_display);
	wlr_data_device_manager_create(server->wl_display);
	wlr_primary_selection_v1_device_manager_create(server->wl_display);
	wlr_data_control_manager_v1_create(server->wl_display);
	wlr_viewporter_create(server->wl_display);
	wlr_single_pixel_buffer_manager_v1_create(server->wl_display);
	wlr_presentation_create(server->wl_display, server->backend);
	/* For screenshots (grim, the GriDE screenshot app). */
	wlr_screencopy_manager_v1_create(server->wl_display);

	server->output_layout = wlr_output_layout_create(server->wl_display);
	wlr_xdg_output_manager_v1_create(server->wl_display, server->output_layout);
	server->new_output.notify = output_handle_new;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	/* Scene graph. The creation order of the layer trees is the stacking
	 * order (see struct gride_server). */
	server->scene = wlr_scene_create();
	server->scene_layout = wlr_scene_attach_output_layout(server->scene,
		server->output_layout);
	server->layers.background = wlr_scene_tree_create(&server->scene->tree);
	server->layers.windows = wlr_scene_tree_create(&server->scene->tree);
	server->layers.unmanaged = wlr_scene_tree_create(&server->scene->tree);
	server->layers.overlay = wlr_scene_tree_create(&server->scene->tree);

	/* Registered after the scene exists: the handler draws into it. */
	server->layout_change.notify = output_handle_layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);

	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 6);
	server->new_xdg_toplevel.notify = xdg_handle_new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = xdg_handle_new_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	/* Lets clients hand their title bar to us (foot, Qt and most toolkits
	 * other than GTK). Clients that never ask keep drawing their own. */
	server->xdg_decoration = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	server->new_xdg_decoration.notify = xdg_handle_new_decoration;
	wl_signal_add(&server->xdg_decoration->events.new_toplevel_decoration,
		&server->new_xdg_decoration);

	cursor_init(server);

	server->seat = wlr_seat_create(server->wl_display, "seat0");
	server->new_input.notify = input_handle_new;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->request_cursor.notify = seat_handle_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	server->request_set_selection.notify = seat_handle_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection,
		&server->request_set_selection);
	server->request_set_primary_selection.notify = seat_handle_request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
	input_init(server);

	if (opts->xwayland) {
		xwayland_init(server); /* X11 apps simply fail to start without it */
	}
	return true;
}

bool server_start(struct gride_server *server) {
	const char *socket = wl_display_add_socket_auto(server->wl_display);
	if (!socket) {
		wlr_log(WLR_ERROR, "Cannot open a Wayland socket");
		return false;
	}
	/* Starting the backend creates outputs and input devices, firing the
	 * new_output and new_input handlers above. */
	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "Cannot start the backend");
		return false;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "GriDE", true);
	wlr_log(WLR_INFO, "GriDE is running on WAYLAND_DISPLAY=%s", socket);

	if (server->opts->startup_cmd) {
		spawn(server->opts->startup_cmd);
	}
	return true;
}

void server_run(struct gride_server *server) {
	wl_display_run(server->wl_display);
}

/*
 * Teardown order matters. Clients go first so their surfaces and the views
 * wrapping them are destroyed while everything they point at still exists.
 * Our listeners on long-lived wlroots objects are removed before those
 * objects are destroyed (wlroots 0.18 asserts that signals are empty).
 */
void server_finish(struct gride_server *server) {
	if (!server->wl_display) {
		return;
	}
	xwayland_finish(server);
	wl_display_destroy_clients(server->wl_display);
	grid_finish(server);

	if (server->sigint_source) {
		wl_event_source_remove(server->sigint_source);
	}
	if (server->sigterm_source) {
		wl_event_source_remove(server->sigterm_source);
	}
	if (server->seat) {
		input_finish(server);
		wl_list_remove(&server->new_input.link);
		wl_list_remove(&server->request_cursor.link);
		wl_list_remove(&server->request_set_selection.link);
		wl_list_remove(&server->request_set_primary_selection.link);
	}
	if (server->xdg_shell) {
		wl_list_remove(&server->new_xdg_toplevel.link);
		wl_list_remove(&server->new_xdg_popup.link);
	}
	if (server->xdg_decoration) {
		wl_list_remove(&server->new_xdg_decoration.link);
	}
	if (server->cursor) {
		cursor_finish(server);
	}
	if (server->output_layout) {
		wl_list_remove(&server->new_output.link);
	}
	/* Destroying the backend destroys its outputs, which runs our output
	 * destroy handlers (freeing backgrounds) while the scene still exists. */
	if (server->backend) {
		wlr_backend_destroy(server->backend);
	}
	if (server->scene) {
		wl_list_remove(&server->layout_change.link);
		wlr_scene_node_destroy(&server->scene->tree.node);
	}
	if (server->allocator) {
		wlr_allocator_destroy(server->allocator);
	}
	if (server->renderer) {
		wlr_renderer_destroy(server->renderer);
	}
	wl_display_destroy(server->wl_display);
	server->wl_display = NULL;
	deco_finish_shared();
	icons_finish();
}
