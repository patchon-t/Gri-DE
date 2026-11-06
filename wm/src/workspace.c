/*
 * Workspaces: a 3x3 grid shared by every output.
 *
 * Every view carries the index (0..8, left to right, top to bottom) of the
 * workspace it lives on. Switching hides the views of the old workspace and
 * shows those of the new one; scene nodes that are disabled are skipped by
 * rendering and by hit-testing, so hidden windows cost nothing per frame.
 */
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "server.h"

bool view_on_current_workspace(struct gride_view *view) {
	return view->workspace == view->server->workspace;
}

int workspace_window_count(struct gride_server *server, int index) {
	int count = 0;
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mapped && view->workspace == index) {
			count++;
		}
	}
	return count;
}

const char *workspace_name(struct gride_server *server, int index) {
	if (index < 0 || index >= GRIDE_WORKSPACES) {
		return NULL;
	}
	return server->workspace_names[index];
}

void workspace_set_name(struct gride_server *server, int index, const char *name) {
	if (index < 0 || index >= GRIDE_WORKSPACES) {
		return;
	}
	free(server->workspace_names[index]);
	server->workspace_names[index] = name && name[0] ? strdup(name) : NULL;
	ipc_event_workspace(server);
}

/* Show or hide a view's tree for the current workspace. */
static void view_update_visibility(struct gride_view *view) {
	bool visible = view->mapped && !view->minimized && view_on_current_workspace(view);
	wlr_scene_node_set_enabled(&view->scene_tree->node, visible);
}

void workspace_switch(struct gride_server *server, int index) {
	if (index < 0 || index >= GRIDE_WORKSPACES || index == server->workspace) {
		return;
	}
	/* Drags, held buttons and the flyout belong to the old workspace. */
	if (server->cursor_mode != GRIDE_CURSOR_PASSTHROUGH) {
		cursor_end_interactive(server);
	}
	flyout_close(server);

	struct gride_view *old_focus = server->focused_view;
	server->workspace = index;
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		view_update_visibility(view);
	}

	/* Focus what was focused here last, else the top window here. */
	struct gride_view *target = server->workspace_focus[index];
	if (!target || !target->mapped || target->minimized || target->workspace != index) {
		target = NULL;
		wl_list_for_each(view, &server->views, link) {
			if (view->mapped && !view->minimized && view->workspace == index) {
				target = view;
				break;
			}
		}
	}
	if (old_focus && old_focus != target) {
		old_focus->impl->set_activated(old_focus, false);
		if (old_focus->deco) {
			deco_update(old_focus->deco);
		}
	}
	server->focused_view = NULL;
	if (target) {
		view_focus(target);
	} else {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
	}
	cursor_rebase(server, 0);
	wlr_log(WLR_DEBUG, "Workspace %d", index + 1);
	ipc_event_workspace(server);
}

void workspace_switch_direction(struct gride_server *server, int dx, int dy) {
	int col = server->workspace % GRIDE_WORKSPACE_COLUMNS + dx;
	int row = server->workspace / GRIDE_WORKSPACE_COLUMNS + dy;
	/* The grid has edges: no wrapping, so Super+Left on the left column
	 * does nothing rather than jumping across the screen. */
	if (col < 0 || col >= GRIDE_WORKSPACE_COLUMNS || row < 0 || row >= GRIDE_WORKSPACE_ROWS) {
		return;
	}
	workspace_switch(server, row * GRIDE_WORKSPACE_COLUMNS + col);
}

void workspace_move_view(struct gride_view *view, int index) {
	struct gride_server *server = view->server;
	if (index < 0 || index >= GRIDE_WORKSPACES || index == view->workspace || !view->mapped) {
		return;
	}
	if (server->grabbed_view == view) {
		cursor_end_interactive(server);
	}
	flyout_view_gone(server, view);
	if (server->workspace_focus[view->workspace] == view) {
		server->workspace_focus[view->workspace] = NULL;
	}
	view->workspace = index;
	/* It arrives on top of its new workspace and is focused there when you
	 * go to it. You stay where you are. */
	server->workspace_focus[index] = view;
	view_update_visibility(view);

	if (server->focused_view == view) {
		view->impl->set_activated(view, false);
		if (view->deco) {
			deco_update(view->deco);
		}
		server->focused_view = NULL;
		struct gride_view *next;
		wl_list_for_each(next, &server->views, link) {
			if (next->mapped && !next->minimized && view_on_current_workspace(next)) {
				view_focus(next);
				break;
			}
		}
		if (!server->focused_view) {
			wlr_seat_keyboard_notify_clear_focus(server->seat);
		}
	}
	cursor_rebase(server, 0);
	ipc_event_window(server, view, "workspace");
	ipc_event_workspace(server);
}

void workspace_finish(struct gride_server *server) {
	for (int i = 0; i < GRIDE_WORKSPACES; i++) {
		free(server->workspace_names[i]);
		server->workspace_names[i] = NULL;
	}
}
