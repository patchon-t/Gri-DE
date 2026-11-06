/* Input devices, keyboards, key bindings and seat requests. */
#include <libinput.h>
#include <stdlib.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "server.h"

/* ---- key bindings ------------------------------------------------------- */

/*
 * Bindings use the layout's unshifted keysym (level 0), so Super+Shift+Q
 * still sees "q" and later Super+Shift+1 sees "1", not "!".
 * Returns true when the key was ours and must not reach the client.
 */
static bool handle_binding(struct gride_server *server, uint32_t mods, xkb_keysym_t sym) {
	uint32_t mod = server->opts->mod_mask;
	struct gride_view *focused = server->focused_view;

	if (mods == mod) {
		switch (sym) {
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter: {
			const char *term = find_terminal(server->opts);
			if (term) {
				spawn(term);
			} else {
				wlr_log(WLR_ERROR, "No terminal found: pass -t or set GRIDE_TERMINAL");
			}
			return true;
		}
		case XKB_KEY_q:
			if (focused) {
				view_close(focused);
			}
			return true;
		case XKB_KEY_f:
			if (focused) {
				view_set_fullscreen(focused, !focused->fullscreen, NULL);
			}
			return true;
		default:
			return false;
		}
	}
	if (mods == (mod | WLR_MODIFIER_SHIFT) && sym == XKB_KEY_Escape) {
		/* Leave GriDE. Phase 7 puts a confirmation in front of this. */
		wlr_log(WLR_INFO, "Exit requested from the keyboard");
		wl_display_terminate(server->wl_display);
		return true;
	}
	return false;
}

/* Ctrl+Alt+F1..F12 switch virtual terminals when we own a real session. */
static bool handle_vt_switch(struct gride_server *server, xkb_keysym_t sym) {
	if (sym < XKB_KEY_XF86Switch_VT_1 || sym > XKB_KEY_XF86Switch_VT_12) {
		return false;
	}
	if (server->session) {
		unsigned vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
		wlr_session_change_vt(server->session, vt);
	}
	return true;
}

/* ---- keyboards ---------------------------------------------------------- */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct gride_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	/* A seat has one keyboard at a time as far as clients are concerned;
	 * the most recently used physical keyboard becomes it. */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct gride_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct gride_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	/* libinput keycodes are evdev codes; xkb's are offset by 8. */
	uint32_t keycode = event->keycode + 8;
	bool handled = false;

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		struct xkb_state *state = keyboard->wlr_keyboard->xkb_state;
		/* Caps Lock and Num Lock (Mod2) never change what a binding means. */
		uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard) &
			~(uint32_t)(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);

		const xkb_keysym_t *translated;
		int n = xkb_state_key_get_syms(state, keycode, &translated);
		for (int i = 0; i < n && !handled; i++) {
			handled = handle_vt_switch(server, translated[i]);
		}

		const xkb_keysym_t *raw;
		xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
		n = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap,
			keycode, layout, 0, &raw);
		for (int i = 0; i < n && !handled; i++) {
			handled = handle_binding(server, mods, xkb_keysym_to_lower(raw[i]));
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct gride_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

/* own_keymap: the device brings its own keymap (virtual keyboards). */
static void add_keyboard(struct gride_server *server, struct wlr_input_device *device,
		bool own_keymap) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct gride_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard) {
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/*
	 * NULL rule names make xkbcommon read XKB_DEFAULT_LAYOUT, _VARIANT,
	 * _OPTIONS and friends from the environment. Phase 6 adds [keyboard]
	 * settings that are passed in here.
	 */
	if (!own_keymap) {
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap) {
			wlr_keyboard_set_keymap(wlr_keyboard, keymap);
		} else {
			wlr_log(WLR_ERROR, "Cannot compile the XKB keymap for %s", device->name);
		}
		xkb_keymap_unref(keymap); /* the keyboard holds its own reference */
		xkb_context_unref(context);
		wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);
	}

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	/* The keyboard is destroyed with its input device. */
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

/* ---- pointers ----------------------------------------------------------- */

/* Laptop-friendly touchpad defaults. Phase 6 makes them settings. */
static void configure_libinput_pointer(struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device)) {
		return; /* nested and virtual pointers have no libinput handle */
	}
	struct libinput_device *li = wlr_libinput_get_device_handle(device);
	if (libinput_device_config_tap_get_finger_count(li) > 0) {
		libinput_device_config_tap_set_enabled(li, LIBINPUT_CONFIG_TAP_ENABLED);
		libinput_device_config_dwt_set_enabled(li, LIBINPUT_CONFIG_DWT_ENABLED);
		if (libinput_device_config_scroll_has_natural_scroll(li)) {
			libinput_device_config_scroll_set_natural_scroll_enabled(li, 1);
		}
	}
}

static void add_pointer(struct gride_server *server, struct wlr_input_device *device) {
	configure_libinput_pointer(device);
	wlr_cursor_attach_input_device(server->cursor, device);
}

/* Advertise what the seat can do. We always offer a pointer so the cursor
 * works before any mouse is plugged in. */
static void update_capabilities(struct gride_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

void input_handle_new(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		add_keyboard(server, device, false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		add_pointer(server, device);
		break;
	default:
		break;
	}
	wlr_log(WLR_DEBUG, "New input device: %s", device->name);
	update_capabilities(server);
}

static void handle_new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	/* The client sends its keymap before any key, so we must not replace it. */
	add_keyboard(server, &keyboard->keyboard.base, true);
	update_capabilities(server);
}

static void handle_new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;
	wlr_cursor_attach_input_device(server->cursor, device);
	if (event->suggested_output) {
		wlr_cursor_map_input_to_output(server->cursor, device, event->suggested_output);
	}
	update_capabilities(server);
}

void input_init(struct gride_server *server) {
	/* Clients read the capabilities when they bind wl_seat; set them before
	 * any device exists so a session that starts without one still works. */
	update_capabilities(server);

	/* Both managers are display globals, destroyed with the display. */
	struct wlr_virtual_keyboard_manager_v1 *vk =
		wlr_virtual_keyboard_manager_v1_create(server->wl_display);
	server->new_virtual_keyboard.notify = handle_new_virtual_keyboard;
	wl_signal_add(&vk->events.new_virtual_keyboard, &server->new_virtual_keyboard);

	struct wlr_virtual_pointer_manager_v1 *vp =
		wlr_virtual_pointer_manager_v1_create(server->wl_display);
	server->new_virtual_pointer.notify = handle_new_virtual_pointer;
	wl_signal_add(&vp->events.new_virtual_pointer, &server->new_virtual_pointer);
}

void input_finish(struct gride_server *server) {
	wl_list_remove(&server->new_virtual_keyboard.link);
	wl_list_remove(&server->new_virtual_pointer.link);
}

/* ---- seat requests ------------------------------------------------------ */

void seat_handle_request_cursor(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* Only the client with pointer focus may set the cursor image. */
	if (server->seat->pointer_state.focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

void seat_handle_request_set_selection(struct wl_listener *listener, void *data) {
	struct gride_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

void seat_handle_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct gride_server *server =
		wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}
