/*
 * The grid engine.
 *
 * Each output's usable area is split into columns x rows cells with the
 * 12px gap (space-3) between cells and as the margin at the screen edges.
 * A window placed on the grid covers an inclusive cell range such as A1:B2
 * (columns are letters, rows numbers) and its frame fills that range,
 * gaps included.
 *
 * While a window is dragged, the overlay shows every cell as a dashed
 * outline with its id, and the cells the window will snap to as an accent
 * box with a chip ("A1:B3 · Left half"). Dropping snaps the window there
 * in 180ms. Holding Ctrl while dropping leaves it floating.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "draw.h"
#include "server.h"

#define GRID_MAX_COLUMNS 8
#define GRID_MAX_ROWS 6
#define ANIM_TICK_MS 10

struct gride_grid_overlay {
	struct wlr_output *output;
	struct gride_view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *cells;
	struct wlr_scene_buffer *target;
	bool has_target;
	struct gride_range shown;
};

static int columns(struct gride_server *server) {
	return server->opts->grid_columns;
}

static int rows(struct gride_server *server) {
	return server->opts->grid_rows;
}

void grid_area(struct gride_server *server, struct wlr_output *output, struct wlr_box *area) {
	/* Phase 4 takes the taskbar's exclusive zone out of this. */
	output_get_box(server, output, area);
}

/* Start and end (exclusive) of cell i along one axis. */
static void cell_span(int start, int length, int n, int i, int *a, int *b) {
	const int gap = GRIDE_SPACE_3, margin = GRIDE_SPACE_3;
	double cell = (double)(length - 2 * margin - (n - 1) * gap) / n;
	*a = start + margin + (int)lround(i * (cell + gap));
	*b = start + margin + (int)lround(i * (cell + gap) + cell);
}

static void clamp_range(struct gride_server *server, struct gride_range *r) {
	int nc = columns(server), nr = rows(server);
	if (r->c0 > r->c1) {
		int t = r->c0; r->c0 = r->c1; r->c1 = t;
	}
	if (r->r0 > r->r1) {
		int t = r->r0; r->r0 = r->r1; r->r1 = t;
	}
	r->c0 = r->c0 < 0 ? 0 : r->c0 >= nc ? nc - 1 : r->c0;
	r->c1 = r->c1 < r->c0 ? r->c0 : r->c1 >= nc ? nc - 1 : r->c1;
	r->r0 = r->r0 < 0 ? 0 : r->r0 >= nr ? nr - 1 : r->r0;
	r->r1 = r->r1 < r->r0 ? r->r0 : r->r1 >= nr ? nr - 1 : r->r1;
}

void grid_range_box(struct gride_server *server, struct wlr_output *output,
		const struct gride_range *range, struct wlr_box *box) {
	struct wlr_box area;
	grid_area(server, output, &area);
	struct gride_range r = *range;
	clamp_range(server, &r);
	int x0, x1, y0, y1, unused;
	cell_span(area.x, area.width, columns(server), r.c0, &x0, &unused);
	cell_span(area.x, area.width, columns(server), r.c1, &unused, &x1);
	cell_span(area.y, area.height, rows(server), r.r0, &y0, &unused);
	cell_span(area.y, area.height, rows(server), r.r1, &unused, &y1);
	*box = (struct wlr_box){ x0, y0, x1 - x0, y1 - y0 };
}

/* ---- names -------------------------------------------------------------- */

static const char *span_name(int a, int b, int n, bool horizontal) {
	int len = b - a + 1;
	if (len == n) {
		return "";
	}
	if (2 * len == n) {
		if (a == 0) {
			return horizontal ? "Left half" : "Top half";
		}
		if (b == n - 1) {
			return horizontal ? "Right half" : "Bottom half";
		}
		if (2 * a == n - len) {
			return horizontal ? "Centre half" : "Middle half";
		}
	}
	if (3 * len == n) {
		if (a == 0) {
			return horizontal ? "Left third" : "Top third";
		}
		if (b == n - 1) {
			return horizontal ? "Right third" : "Bottom third";
		}
		return horizontal ? "Centre third" : "Middle third";
	}
	if (3 * len == 2 * n) {
		if (a == 0) {
			return horizontal ? "Left two-thirds" : "Top two-thirds";
		}
		if (b == n - 1) {
			return horizontal ? "Right two-thirds" : "Bottom two-thirds";
		}
	}
	return NULL;
}

void grid_range_label(struct gride_server *server, const struct gride_range *range,
		char *ids, size_t ids_len, char *name, size_t name_len) {
	const struct gride_range *r = range;
	if (r->c0 == r->c1 && r->r0 == r->r1) {
		snprintf(ids, ids_len, "%c%d", 'A' + r->c0, r->r0 + 1);
	} else {
		snprintf(ids, ids_len, "%c%d:%c%d", 'A' + r->c0, r->r0 + 1, 'A' + r->c1, r->r1 + 1);
	}

	const char *h = span_name(r->c0, r->c1, columns(server), true);
	const char *v = span_name(r->r0, r->r1, rows(server), false);
	int w = r->c1 - r->c0 + 1, hgt = r->r1 - r->r0 + 1;
	if (h && v && !h[0] && !v[0]) {
		snprintf(name, name_len, "Whole grid");
	} else if (h && v && !v[0]) {
		snprintf(name, name_len, "%s", h);
	} else if (h && v && !h[0]) {
		snprintf(name, name_len, "%s", v);
	} else if (h && v && strstr(h, "half") && strstr(v, "half")) {
		snprintf(name, name_len, "%s-%s quarter", r->r0 == 0 ? "Top" : "Bottom",
			r->c0 == 0 ? "left" : "right");
	} else if (h && v) {
		/* "Left half, top two-thirds": sentence case, so the second part
		 * starts lower case. */
		snprintf(name, name_len, "%s, %c%s", h, v[0] - 'A' + 'a', v + 1);
	} else if (w == 1 && hgt == 1) {
		snprintf(name, name_len, "One cell");
	} else {
		snprintf(name, name_len, "%d × %d cells", w, hgt);
	}
}

/* ---- snapping ----------------------------------------------------------- */

void grid_nearest_range(struct gride_server *server, struct wlr_output *output,
		const struct wlr_box *frame, struct gride_range *range) {
	struct wlr_box area;
	grid_area(server, output, &area);
	int nc = columns(server), nr = rows(server);
	int best;
	double best_d;

	/* Left edge to the nearest column start, right edge to the nearest
	 * column end at or after it; the same for rows. */
	best = 0; best_d = INFINITY;
	for (int i = 0; i < nc; i++) {
		int a, b;
		cell_span(area.x, area.width, nc, i, &a, &b);
		double d = fabs((double)a - frame->x);
		if (d < best_d) { best_d = d; best = i; }
	}
	range->c0 = best;
	best = range->c0; best_d = INFINITY;
	for (int i = range->c0; i < nc; i++) {
		int a, b;
		cell_span(area.x, area.width, nc, i, &a, &b);
		double d = fabs((double)b - (frame->x + frame->width));
		if (d < best_d) { best_d = d; best = i; }
	}
	range->c1 = best;

	best = 0; best_d = INFINITY;
	for (int i = 0; i < nr; i++) {
		int a, b;
		cell_span(area.y, area.height, nr, i, &a, &b);
		double d = fabs((double)a - frame->y);
		if (d < best_d) { best_d = d; best = i; }
	}
	range->r0 = best;
	best = range->r0; best_d = INFINITY;
	for (int i = range->r0; i < nr; i++) {
		int a, b;
		cell_span(area.y, area.height, nr, i, &a, &b);
		double d = fabs((double)b - (frame->y + frame->height));
		if (d < best_d) { best_d = d; best = i; }
	}
	range->r1 = best;
}

static void anim_stop_timer_if_idle(struct gride_server *server) {
	if (server->animating <= 0 && server->anim_timer) {
		server->animating = 0;
		wl_event_source_timer_update(server->anim_timer, 0);
	}
}

/* Advance every running snap animation; ease-out cubic, no overshoot. */
static int anim_tick(void *data) {
	struct gride_server *server = data;
	uint32_t now = gride_now_msec();
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->anim.active) {
			continue;
		}
		double t = (double)(now - view->anim.start_msec) / GRIDE_SNAP_DURATION_MS;
		int x = view->anim.to_x, y = view->anim.to_y;
		if (t >= 1.0) {
			view->anim.active = false;
			server->animating--;
		} else {
			double e = 1.0 - pow(1.0 - t, 3.0);
			x = view->anim.from_x + (int)lround((view->anim.to_x - view->anim.from_x) * e);
			y = view->anim.from_y + (int)lround((view->anim.to_y - view->anim.from_y) * e);
		}
		wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	}
	if (server->animating > 0) {
		wl_event_source_timer_update(server->anim_timer, ANIM_TICK_MS);
	} else {
		anim_stop_timer_if_idle(server);
	}
	return 0;
}

static void anim_start(struct gride_view *view, int from_x, int from_y, int to_x, int to_y) {
	struct gride_server *server = view->server;
	if (from_x == to_x && from_y == to_y) {
		return;
	}
	if (!server->anim_timer) {
		server->anim_timer = wl_event_loop_add_timer(server->event_loop, anim_tick, server);
		if (!server->anim_timer) {
			return;
		}
	}
	if (!view->anim.active) {
		server->animating++;
	}
	view->anim.active = true;
	view->anim.from_x = from_x;
	view->anim.from_y = from_y;
	view->anim.to_x = to_x;
	view->anim.to_y = to_y;
	view->anim.start_msec = gride_now_msec();
	wl_event_source_timer_update(server->anim_timer, ANIM_TICK_MS);
}

void grid_snap_view(struct gride_view *view, struct wlr_output *output,
		const struct gride_range *range, bool animate) {
	struct gride_server *server = view->server;
	if (!output) {
		return;
	}
	struct gride_range r = *range;
	clamp_range(server, &r);
	if (view->fullscreen) {
		view->fullscreen = false;
		view->impl->set_fullscreen(view, false);
	}
	if (view->maximized) {
		view->maximized = false;
		view->impl->set_maximized(view, false);
	}
	view->has_saved = false;
	view->center_pending = false;
	view->snapped = true;
	view->range = r;
	view->snap_output = output;

	struct wlr_box box;
	grid_range_box(server, output, &r, &box);
	char ids[16], name[48];
	grid_range_label(server, &r, ids, sizeof(ids), name, sizeof(name));
	wlr_log(WLR_DEBUG, "Snapped \"%s\" to %s (%s) at %d,%d %dx%d", view_title(view), ids,
		name, box.x, box.y, box.width, box.height);
	int from_x = view->x, from_y = view->y;
	bool run = animate && server->opts->animations && view->mapped;
	if (run) {
		anim_start(view, from_x, from_y, box.x, box.y);
	} else if (view->anim.active) {
		view->anim.active = false;
		server->animating--;
		anim_stop_timer_if_idle(server);
	}
	view_configure_frame(view, &box);
	if (view->deco) {
		deco_update(view->deco);
	}
}

void grid_unsnap_view(struct gride_view *view) {
	view->snapped = false;
}

/* ---- auto tile ---------------------------------------------------------- */

static struct wlr_output *view_grid_output(struct gride_view *view) {
	if (view->snapped && view->snap_output) {
		return view->snap_output;
	}
	struct wlr_box frame;
	view_get_frame(view, &frame);
	return output_for_box(view->server, &frame);
}

bool grid_auto_place(struct gride_view *view, struct wlr_output *output) {
	struct gride_server *server = view->server;
	if (!output) {
		return false;
	}
	int nc = columns(server), nr = rows(server);
	bool used[GRID_MAX_ROWS][GRID_MAX_COLUMNS] = {{false}};
	struct gride_view *other;
	wl_list_for_each(other, &server->views, link) {
		if (other == view || !other->mapped || other->minimized || !other->snapped ||
				other->snap_output != output) {
			continue;
		}
		for (int r = other->range.r0; r <= other->range.r1; r++) {
			for (int c = other->range.c0; c <= other->range.c1; c++) {
				used[r][c] = true;
			}
		}
	}

	/* The largest empty rectangle of cells, first found wins ties. */
	struct gride_range best = {0};
	int best_area = 0;
	for (int r0 = 0; r0 < nr; r0++) {
		for (int c0 = 0; c0 < nc; c0++) {
			for (int r1 = r0; r1 < nr; r1++) {
				for (int c1 = c0; c1 < nc; c1++) {
					int area = (r1 - r0 + 1) * (c1 - c0 + 1);
					if (area <= best_area) {
						continue;
					}
					bool free_cells = true;
					for (int r = r0; r <= r1 && free_cells; r++) {
						for (int c = c0; c <= c1 && free_cells; c++) {
							free_cells = !used[r][c];
						}
					}
					if (free_cells) {
						best = (struct gride_range){ c0, r0, c1, r1 };
						best_area = area;
					}
				}
			}
		}
	}
	if (best_area > 0) {
		grid_snap_view(view, output, &best, false);
		return true;
	}

	/* No empty cells: halve the biggest window that can be halved. */
	struct gride_view *victim = NULL;
	int victim_area = 0;
	wl_list_for_each(other, &server->views, link) {
		if (other == view || !other->mapped || other->minimized || !other->snapped ||
				other->snap_output != output) {
			continue;
		}
		int w = other->range.c1 - other->range.c0 + 1;
		int h = other->range.r1 - other->range.r0 + 1;
		if ((w >= 2 || h >= 2) && w * h > victim_area) {
			victim = other;
			victim_area = w * h;
		}
	}
	if (!victim) {
		return false;
	}
	struct gride_range keep = victim->range, give = victim->range;
	int w = keep.c1 - keep.c0 + 1, h = keep.r1 - keep.r0 + 1;
	struct wlr_box box;
	grid_range_box(server, output, &keep, &box);
	bool split_columns = w >= 2 && (h < 2 || box.width >= box.height);
	if (split_columns) {
		keep.c1 = keep.c0 + (w + 1) / 2 - 1;
		give.c0 = keep.c1 + 1;
	} else {
		keep.r1 = keep.r0 + (h + 1) / 2 - 1;
		give.r0 = keep.r1 + 1;
	}
	grid_snap_view(victim, output, &keep, true);
	grid_snap_view(view, output, &give, false);
	return true;
}

/* ---- keyboard moves ----------------------------------------------------- */

static bool current_range(struct gride_view *view, struct wlr_output **output,
		struct gride_range *range) {
	if (!view || !view->mapped || view->fullscreen || !view_is_snappable(view)) {
		return false;
	}
	*output = view_grid_output(view);
	if (!*output) {
		return false;
	}
	if (view->maximized) {
		*range = (struct gride_range){ 0, 0, columns(view->server) - 1, rows(view->server) - 1 };
	} else if (view->snapped) {
		*range = view->range;
	} else {
		struct wlr_box frame;
		view_get_frame(view, &frame);
		grid_nearest_range(view->server, *output, &frame, range);
	}
	return true;
}

void grid_move_view(struct gride_view *view, int dc, int dr) {
	struct wlr_output *output;
	struct gride_range r;
	if (!current_range(view, &output, &r)) {
		return;
	}
	int nc = columns(view->server), nr = rows(view->server);
	if (r.c0 + dc < 0 || r.c1 + dc >= nc) {
		dc = 0;
	}
	if (r.r0 + dr < 0 || r.r1 + dr >= nr) {
		dr = 0;
	}
	r.c0 += dc; r.c1 += dc;
	r.r0 += dr; r.r1 += dr;
	grid_snap_view(view, output, &r, true);
}

void grid_resize_view(struct gride_view *view, int dc, int dr) {
	struct wlr_output *output;
	struct gride_range r;
	if (!current_range(view, &output, &r)) {
		return;
	}
	/* Grow or shrink from the right and bottom edges, never below one cell. */
	int c1 = r.c1 + dc, r1 = r.r1 + dr;
	if (c1 >= r.c0 && c1 < columns(view->server)) {
		r.c1 = c1;
	}
	if (r1 >= r.r0 && r1 < rows(view->server)) {
		r.r1 = r1;
	}
	grid_snap_view(view, output, &r, true);
}

/* ---- drag overlay ------------------------------------------------------- */

static void draw_cells(struct gride_server *server, struct gride_grid_overlay *ov) {
	struct wlr_box area;
	grid_area(server, ov->output, &area);
	struct draw_buffer *buf = draw_buffer_create(area.width, area.height, ov->output->scale);
	if (!buf) {
		return;
	}
	cairo_t *cr = buf->cr;
	const struct gride_palette *p = server->palette;
	const double stroke = 1.5;
	const double dash[] = { 4, 4 };
	int nc = columns(server), nr = rows(server);
	for (int r = 0; r < nr; r++) {
		for (int c = 0; c < nc; c++) {
			struct gride_range cell = { c, r, c, r };
			struct wlr_box box;
			grid_range_box(server, ov->output, &cell, &box);
			double x = box.x - area.x, y = box.y - area.y;
			draw_rounded_rect(cr, x + stroke / 2, y + stroke / 2, box.width - stroke,
				box.height - stroke, GRIDE_RADIUS_XS);
			cairo_set_dash(cr, dash, 2, 0);
			cairo_set_line_width(cr, stroke);
			draw_set_color(cr, p->faint);
			cairo_stroke(cr);

			char id[16];
			snprintf(id, sizeof(id), "%c%d", 'A' + c, r + 1);
			PangoLayout *layout = draw_text_layout(cr, &gride_type_mono, id, false, 0);
			int tw, tht;
			draw_text_size(layout, &tw, &tht);
			draw_set_color(cr, p->muted);
			cairo_move_to(cr, x + box.width - GRIDE_SPACE_2 - tw, y + GRIDE_SPACE_2);
			pango_cairo_show_layout(cr, layout);
			g_object_unref(layout);
		}
	}
	wlr_scene_buffer_set_buffer(ov->cells, buf->base);
	wlr_scene_buffer_set_dest_size(ov->cells, area.width, area.height);
	wlr_scene_node_set_position(&ov->cells->node, area.x, area.y);
	draw_buffer_drop(buf);
}

static void draw_target(struct gride_server *server, struct gride_grid_overlay *ov,
		const struct gride_range *range) {
	struct wlr_box box;
	grid_range_box(server, ov->output, range, &box);
	struct draw_buffer *buf = draw_buffer_create(box.width, box.height, ov->output->scale);
	if (!buf) {
		return;
	}
	cairo_t *cr = buf->cr;
	const struct gride_palette *p = server->palette;

	/* 2px accent outline over accent-tint, with the window radius. */
	draw_rounded_rect(cr, 1, 1, box.width - 2, box.height - 2, GRIDE_RADIUS_MD);
	draw_set_color(cr, p->accent_tint);
	cairo_fill_preserve(cr);
	cairo_set_line_width(cr, 2);
	draw_set_color(cr, p->accent);
	cairo_stroke(cr);

	/* The ink chip: cell ids in mono, then the layout name. */
	char ids[16], name[48], markup[160];
	grid_range_label(server, range, ids, sizeof(ids), name, sizeof(name));
	snprintf(markup, sizeof(markup), "<span font_family=\"%s\" font_weight=\"%d\">%s</span>"
		"  ·  %s", gride_type_mono.family, gride_type_mono.weight, ids, name);
	PangoLayout *layout = draw_text_layout(cr, &gride_type_label, markup, true, 0);
	int tw, tht;
	draw_text_size(layout, &tw, &tht);
	double chip_w = tw + 2 * GRIDE_SPACE_3;
	double chip_h = tht + 2 * GRIDE_SPACE_1;
	double cx = (box.width - chip_w) / 2.0, cy = (box.height - chip_h) / 2.0;
	draw_rounded_rect(cr, cx, cy, chip_w, chip_h, chip_h / 2.0); /* radius-pill */
	draw_set_color(cr, p->ink);
	cairo_fill(cr);
	draw_set_color(cr, p->surface);
	cairo_move_to(cr, cx + GRIDE_SPACE_3, cy + GRIDE_SPACE_1);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);

	wlr_scene_buffer_set_buffer(ov->target, buf->base);
	wlr_scene_buffer_set_dest_size(ov->target, box.width, box.height);
	wlr_scene_node_set_position(&ov->target->node, box.x, box.y);
	wlr_scene_node_set_enabled(&ov->target->node, true);
	draw_buffer_drop(buf);
}

static void overlay_destroy(struct gride_server *server) {
	struct gride_grid_overlay *ov = server->grid_overlay;
	if (!ov) {
		return;
	}
	wlr_scene_node_destroy(&ov->tree->node);
	free(ov);
	server->grid_overlay = NULL;
}

static bool overlay_show(struct gride_server *server, struct gride_view *view,
		struct wlr_output *output) {
	struct gride_grid_overlay *ov = calloc(1, sizeof(*ov));
	if (!ov) {
		return false;
	}
	ov->output = output;
	ov->view = view;
	ov->tree = wlr_scene_tree_create(server->layers.overlay);
	ov->cells = wlr_scene_buffer_create(ov->tree, NULL);
	ov->target = wlr_scene_buffer_create(ov->tree, NULL);
	wlr_scene_node_set_enabled(&ov->target->node, false);
	server->grid_overlay = ov;
	draw_cells(server, ov);
	return true;
}

void grid_drag_begin(struct gride_server *server, struct gride_view *view) {
	overlay_destroy(server);
	if (!view_is_snappable(view)) {
		return;
	}
	view->snapped = false; /* re-snapped where it is dropped */
	struct wlr_output *output = output_at_cursor(server);
	if (output && overlay_show(server, view, output)) {
		grid_drag_update(server);
	}
}

void grid_drag_update(struct gride_server *server) {
	struct gride_grid_overlay *ov = server->grid_overlay;
	if (!ov) {
		return;
	}
	/* The grid follows the pointer from monitor to monitor. */
	struct wlr_output *output = output_at_cursor(server);
	if (output && output != ov->output) {
		struct gride_view *view = ov->view;
		overlay_destroy(server);
		if (!overlay_show(server, view, output)) {
			return;
		}
		ov = server->grid_overlay;
	}
	bool skip = input_current_modifiers(server) & WLR_MODIFIER_CTRL;
	if (skip) {
		ov->has_target = false;
		wlr_scene_node_set_enabled(&ov->target->node, false);
		return;
	}
	struct wlr_box frame;
	view_get_frame(ov->view, &frame);
	struct gride_range range;
	grid_nearest_range(server, ov->output, &frame, &range);
	if (ov->has_target && memcmp(&range, &ov->shown, sizeof(range)) == 0) {
		return;
	}
	ov->shown = range;
	ov->has_target = true;
	draw_target(server, ov, &range);
}

void grid_drag_end(struct gride_server *server, bool snap) {
	struct gride_grid_overlay *ov = server->grid_overlay;
	if (!ov) {
		return;
	}
	if (snap) {
		/* Re-check: Ctrl may have been pressed since the last motion. */
		grid_drag_update(server);
		ov = server->grid_overlay;
	}
	if (snap && ov && ov->has_target && !(input_current_modifiers(server) & WLR_MODIFIER_CTRL)) {
		struct gride_range range = ov->shown;
		struct wlr_output *output = ov->output;
		struct gride_view *view = ov->view;
		overlay_destroy(server);
		grid_snap_view(view, output, &range, true);
		return;
	}
	overlay_destroy(server);
}

void grid_output_removed(struct gride_server *server, struct wlr_output *output) {
	if (server->grid_overlay && server->grid_overlay->output == output) {
		overlay_destroy(server);
	}
	struct gride_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->snap_output == output) {
			view->snap_output = NULL;
		}
	}
}

void grid_finish(struct gride_server *server) {
	overlay_destroy(server);
	if (server->anim_timer) {
		wl_event_source_remove(server->anim_timer);
		server->anim_timer = NULL;
	}
}
