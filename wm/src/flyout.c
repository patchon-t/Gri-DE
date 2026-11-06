/*
 * The snap layouts flyout.
 *
 * Opens below a window's Maximise button after a 400ms hover, or with
 * Super+Z for the focused window. A 380px panel (radius 18, the floating
 * shadow) with:
 *   "Snap layouts"                                      "Grid 4 × 3"
 *   six layout thumbnails; the zone under the pointer fills with accent
 *   "Or drag across cells" and a cell picker (A1..D3) to pick any range
 * While a zone or range is hovered, the desktop previews it with the accent
 * target and chip. Clicking applies it with the usual 180ms snap.
 *
 * Keyboard: arrows move between zones, Enter applies, Escape closes.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "draw.h"
#include "server.h"

#define CLOSE_DELAY_MS 300 /* grace period after the pointer leaves */
#define PAD GRIDE_SPACE_4  /* flyout inner padding */
#define HEADER_H 24        /* subhead line height */
#define THUMB_COLUMNS 3
#define THUMB_H 68
#define CELL_H 28
#define MAX_ZONES 32

/* Layouts as fractions of the grid: x0, y0, x1, y1 per zone. */
struct layout {
	int zones;
	double z[4][4];
};

static const struct layout layouts[] = {
	/* halves */
	{ 2, { { 0, 0, .5, 1 }, { .5, 0, 1, 1 } } },
	/* two-thirds plus one-third */
	{ 2, { { 0, 0, 2.0 / 3, 1 }, { 2.0 / 3, 0, 1, 1 } } },
	/* thirds */
	{ 3, { { 0, 0, 1.0 / 3, 1 }, { 1.0 / 3, 0, 2.0 / 3, 1 }, { 2.0 / 3, 0, 1, 1 } } },
	/* quarters */
	{ 4, { { 0, 0, .5, .5 }, { .5, 0, 1, .5 }, { 0, .5, .5, 1 }, { .5, .5, 1, 1 } } },
	/* main plus two stacked */
	{ 3, { { 0, 0, .5, 1 }, { .5, 0, 1, .5 }, { .5, .5, 1, 1 } } },
	/* centre focus */
	{ 3, { { 0, 0, .25, 1 }, { .25, 0, .75, 1 }, { .75, 0, 1, 1 } } },
};
#define LAYOUTS (int)(sizeof(layouts) / sizeof(layouts[0]))

struct zone {
	int layout;
	struct wlr_box box;      /* panel-local */
	struct gride_range range;
};

struct gride_flyout {
	struct gride_server *server;
	struct gride_view *view;
	struct wlr_output *output;
	bool by_hover;
	bool close_pending; /* the close timer is running */

	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *shadow;
	struct wlr_scene_buffer *panel;
	struct wlr_box box; /* the panel in layout coordinates */

	struct wlr_box thumbs[LAYOUTS]; /* panel-local */
	struct zone zones[MAX_ZONES];
	int zone_count;
	struct wlr_box cells;           /* the cell picker, panel-local */
	int title_y, label_y;

	int hover_zone;                 /* -1 = none */
	bool cell_hover;                /* a cell range is highlighted */
	struct gride_range cell_range;
	int anchor_c, anchor_r;         /* where a cell drag started */
};

/* What the shared timer does when it fires. */
static bool timer_closes;

/* ---- geometry ----------------------------------------------------------- */

static struct gride_range fraction_range(struct gride_server *server, const double f[4]) {
	int nc = server->grid_columns, nr = server->grid_rows;
	struct gride_range r = {
		(int)lround(f[0] * nc), (int)lround(f[1] * nr),
		(int)lround(f[2] * nc) - 1, (int)lround(f[3] * nr) - 1,
	};
	r.c0 = r.c0 >= nc ? nc - 1 : r.c0;
	r.r0 = r.r0 >= nr ? nr - 1 : r.r0;
	r.c1 = r.c1 < r.c0 ? r.c0 : r.c1;
	r.r1 = r.r1 < r.r0 ? r.r0 : r.r1;
	return r;
}

static void cell_box(struct gride_flyout *fo, int c, int r, struct wlr_box *box) {
	struct gride_server *server = fo->server;
	const int gap = GRIDE_SPACE_1;
	int nc = server->grid_columns;
	double w = (double)(fo->cells.width - (nc - 1) * gap) / nc;
	int x0 = fo->cells.x + (int)lround(c * (w + gap));
	int x1 = fo->cells.x + (int)lround(c * (w + gap) + w);
	*box = (struct wlr_box){ x0, fo->cells.y + r * (CELL_H + gap), x1 - x0, CELL_H };
}

/* Lay out the panel and return its height. */
static int layout_panel(struct gride_flyout *fo) {
	struct gride_server *server = fo->server;
	const int W = GRIDE_SNAP_FLYOUT_WIDTH;
	const int gap = GRIDE_SPACE_3;
	int content_w = W - 2 * PAD;
	int y = PAD;
	fo->title_y = y;
	y += HEADER_H + gap;

	int thumb_w = (content_w - (THUMB_COLUMNS - 1) * gap) / THUMB_COLUMNS;
	fo->zone_count = 0;
	for (int i = 0; i < LAYOUTS; i++) {
		int col = i % THUMB_COLUMNS, row = i / THUMB_COLUMNS;
		struct wlr_box t = { PAD + col * (thumb_w + gap), y + row * (THUMB_H + gap),
			thumb_w, THUMB_H };
		fo->thumbs[i] = t;
		/* Zones inset 4px in the thumbnail, 4px apart. */
		const int inset = GRIDE_SPACE_1, half_gap = GRIDE_SPACE_1 / 2;
		double ix = t.x + inset, iy = t.y + inset;
		double iw = t.width - 2 * inset, ih = t.height - 2 * inset;
		for (int z = 0; z < layouts[i].zones && fo->zone_count < MAX_ZONES; z++) {
			const double *f = layouts[i].z[z];
			int x0 = (int)lround(ix + f[0] * iw) + (f[0] > 0 ? half_gap : 0);
			int y0 = (int)lround(iy + f[1] * ih) + (f[1] > 0 ? half_gap : 0);
			int x1 = (int)lround(ix + f[2] * iw) - (f[2] < 1 ? half_gap : 0);
			int y1 = (int)lround(iy + f[3] * ih) - (f[3] < 1 ? half_gap : 0);
			struct zone *zone = &fo->zones[fo->zone_count++];
			zone->layout = i;
			zone->box = (struct wlr_box){ x0, y0, x1 - x0, y1 - y0 };
			zone->range = fraction_range(server, f);
		}
	}
	int thumb_rows = (LAYOUTS + THUMB_COLUMNS - 1) / THUMB_COLUMNS;
	y += thumb_rows * THUMB_H + (thumb_rows - 1) * gap + GRIDE_SPACE_4;

	fo->label_y = y;
	y += gride_type_group.line_height + GRIDE_SPACE_2;
	int nr = server->grid_rows;
	fo->cells = (struct wlr_box){ PAD, y, content_w,
		nr * CELL_H + (nr - 1) * GRIDE_SPACE_1 };
	y += fo->cells.height + PAD;
	return y;
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_text_at(cairo_t *cr, const struct gride_type_style *style, const char *text,
		const float color[4], double x, double y, double box_h, bool right_align,
		double right_edge) {
	PangoLayout *layout = draw_text_layout(cr, style, text, false, 0);
	int tw, th;
	draw_text_size(layout, &tw, &th);
	draw_set_color(cr, color);
	cairo_move_to(cr, right_align ? right_edge - tw : x, y + (box_h - th) / 2.0);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

static bool range_has_cell(const struct gride_range *r, int c, int row) {
	return c >= r->c0 && c <= r->c1 && row >= r->r0 && row <= r->r1;
}

static void draw_panel(struct gride_flyout *fo) {
	struct gride_server *server = fo->server;
	const struct gride_palette *p = server->palette;
	struct draw_buffer *buf = draw_buffer_create(fo->box.width, fo->box.height,
		fo->output->scale);
	if (!buf) {
		return;
	}
	cairo_t *cr = buf->cr;

	draw_rounded_rect(cr, 0, 0, fo->box.width, fo->box.height, GRIDE_RADIUS_2XL);
	draw_set_color(cr, p->surface);
	cairo_fill(cr);

	/* Header: title left, grid size right. */
	draw_text_at(cr, &gride_type_subhead, "Snap layouts", p->ink, PAD, fo->title_y,
		HEADER_H, false, 0);
	char grid_label[32];
	snprintf(grid_label, sizeof(grid_label), "Grid %d × %d", server->grid_columns,
		server->grid_rows);
	draw_text_at(cr, &gride_type_caption, grid_label, p->muted, 0, fo->title_y, HEADER_H,
		true, fo->box.width - PAD);

	/* Thumbnails and their zones. */
	for (int i = 0; i < LAYOUTS; i++) {
		struct wlr_box *t = &fo->thumbs[i];
		draw_rounded_rect(cr, t->x, t->y, t->width, t->height, GRIDE_RADIUS_MD);
		draw_set_color(cr, p->field);
		cairo_fill(cr);
	}
	for (int z = 0; z < fo->zone_count; z++) {
		struct wlr_box *b = &fo->zones[z].box;
		draw_rounded_rect(cr, b->x + 0.5, b->y + 0.5, b->width - 1, b->height - 1,
			GRIDE_RADIUS_XS);
		if (z == fo->hover_zone) {
			draw_set_color(cr, p->accent);
			cairo_fill(cr);
		} else {
			draw_set_color(cr, p->surface);
			cairo_fill_preserve(cr);
			cairo_set_line_width(cr, 1);
			draw_set_color(cr, p->line);
			cairo_stroke(cr);
		}
	}

	/* The cell picker. */
	draw_text_at(cr, &gride_type_group, "Or drag across cells", p->ink, PAD, fo->label_y,
		gride_type_group.line_height, false, 0);
	for (int r = 0; r < server->grid_rows; r++) {
		for (int c = 0; c < server->grid_columns; c++) {
			struct wlr_box b;
			cell_box(fo, c, r, &b);
			bool lit = fo->cell_hover && range_has_cell(&fo->cell_range, c, r);
			draw_rounded_rect(cr, b.x, b.y, b.width, b.height, GRIDE_RADIUS_XS);
			draw_set_color(cr, lit ? p->accent : p->field);
			cairo_fill(cr);
			char id[16];
			snprintf(id, sizeof(id), "%c%d", 'A' + c, r + 1);
			PangoLayout *layout = draw_text_layout(cr, &gride_type_mono, id, false, 0);
			int tw, th;
			draw_text_size(layout, &tw, &th);
			draw_set_color(cr, lit ? p->on_accent : p->muted);
			cairo_move_to(cr, b.x + (b.width - tw) / 2.0, b.y + (b.height - th) / 2.0);
			pango_cairo_show_layout(cr, layout);
			g_object_unref(layout);
		}
	}

	wlr_scene_buffer_set_buffer(fo->panel, buf->base);
	wlr_scene_buffer_set_dest_size(fo->panel, fo->box.width, fo->box.height);
	draw_buffer_drop(buf);
}

/* Show the hovered zone or range on the desktop, or nothing. */
static void update_preview(struct gride_flyout *fo) {
	struct gride_server *server = fo->server;
	if (fo->hover_zone >= 0) {
		grid_preview_show(server, fo->output, &fo->zones[fo->hover_zone].range);
	} else if (fo->cell_hover) {
		grid_preview_show(server, fo->output, &fo->cell_range);
	} else {
		grid_preview_hide(server);
	}
}

static void set_hover(struct gride_flyout *fo, int zone, bool cell_hover,
		const struct gride_range *cells) {
	bool changed = zone != fo->hover_zone || cell_hover != fo->cell_hover ||
		(cell_hover && memcmp(cells, &fo->cell_range, sizeof(*cells)) != 0);
	if (!changed) {
		return;
	}
	fo->hover_zone = zone;
	fo->cell_hover = cell_hover;
	if (cell_hover) {
		fo->cell_range = *cells;
	}
	draw_panel(fo);
	update_preview(fo);
}

/* ---- timers ------------------------------------------------------------- */

static int handle_timer(void *data) {
	struct gride_server *server = data;
	if (timer_closes) {
		if (server->flyout && server->flyout->by_hover) {
			flyout_close(server);
		}
	} else if (server->flyout_hover_view && !server->flyout) {
		struct gride_view *view = server->flyout_hover_view;
		server->flyout_hover_view = NULL;
		flyout_open(view);
		if (server->flyout) {
			server->flyout->by_hover = true;
		}
	}
	return 0;
}

static void arm_timer(struct gride_server *server, bool closes, int ms) {
	if (!server->flyout_timer) {
		server->flyout_timer = wl_event_loop_add_timer(server->event_loop, handle_timer, server);
		if (!server->flyout_timer) {
			return;
		}
	}
	timer_closes = closes;
	wl_event_source_timer_update(server->flyout_timer, ms);
}

static void disarm_timer(struct gride_server *server) {
	if (server->flyout_timer) {
		wl_event_source_timer_update(server->flyout_timer, 0);
	}
}

/* ---- open and close ----------------------------------------------------- */

void flyout_open(struct gride_view *view) {
	struct gride_server *server = view->server;
	flyout_close(server);
	if (!view->mapped || view->fullscreen || view->minimized || !view_is_snappable(view)) {
		return;
	}
	struct wlr_output *output = view_get_output(view);
	if (!output) {
		return;
	}
	struct gride_flyout *fo = calloc(1, sizeof(*fo));
	if (!fo) {
		return;
	}
	fo->server = server;
	fo->view = view;
	fo->output = output;
	fo->hover_zone = -1;
	int height = layout_panel(fo);

	/* Below the Maximise button, centred on it, kept on the output with the
	 * 12px margin; above the button when there is no room below. */
	struct wlr_box anchor, area;
	view_maximize_button_box(view, &anchor);
	grid_area(server, output, &area);
	const int margin = GRIDE_SPACE_3, W = GRIDE_SNAP_FLYOUT_WIDTH;
	int x = anchor.x + anchor.width / 2 - W / 2;
	int y = anchor.y + anchor.height + GRIDE_SPACE_2;
	if (y + height > area.y + area.height - margin) {
		y = anchor.y - GRIDE_SPACE_2 - height;
	}
	if (x + W > area.x + area.width - margin) {
		x = area.x + area.width - margin - W;
	}
	if (x < area.x + margin) {
		x = area.x + margin;
	}
	if (y < area.y + margin) {
		y = area.y + margin;
	}
	fo->box = (struct wlr_box){ x, y, W, height };

	fo->tree = wlr_scene_tree_create(server->layers.flyouts);
	wlr_scene_node_set_position(&fo->tree->node, x, y);
	/* Flyouts float: the shadow-float token, drawn once per opening. */
	struct draw_buffer *shadow = shadow_render(W, height, GRIDE_RADIUS_2XL, server->shadow);
	fo->shadow = wlr_scene_buffer_create(fo->tree, shadow ? shadow->base : NULL);
	if (shadow) {
		int pad = shadow_reach(server->shadow);
		wlr_scene_node_set_position(&fo->shadow->node, -pad, -pad);
		draw_buffer_drop(shadow);
	}
	fo->panel = wlr_scene_buffer_create(fo->tree, NULL);
	server->flyout = fo;
	draw_panel(fo);
	view_focus(view);
	wlr_log(WLR_DEBUG, "Snap layouts open for \"%s\"", view_title(view));
}

void flyout_close(struct gride_server *server) {
	disarm_timer(server);
	struct gride_flyout *fo = server->flyout;
	if (!fo) {
		return;
	}
	if (server->cursor_mode == GRIDE_CURSOR_FLYOUT) {
		server->cursor_mode = GRIDE_CURSOR_PASSTHROUGH;
	}
	grid_preview_hide(server);
	wlr_scene_node_destroy(&fo->tree->node);
	free(fo);
	server->flyout = NULL;
}

static void apply(struct gride_flyout *fo, const struct gride_range *range) {
	struct gride_server *server = fo->server;
	struct gride_view *view = fo->view;
	struct wlr_output *output = fo->output;
	struct gride_range r = *range;
	flyout_close(server);
	grid_snap_view(view, output, &r, true);
	cursor_rebase(server, 0);
}

/* ---- pointer ------------------------------------------------------------ */

bool flyout_contains(struct gride_server *server, double lx, double ly) {
	struct gride_flyout *fo = server->flyout;
	return fo && wlr_box_contains_point(&fo->box, lx, ly);
}

static int zone_at(struct gride_flyout *fo, double px, double py) {
	for (int z = 0; z < fo->zone_count; z++) {
		if (wlr_box_contains_point(&fo->zones[z].box, px, py)) {
			return z;
		}
	}
	return -1;
}

static bool cell_at(struct gride_flyout *fo, double px, double py, int *c, int *r) {
	for (int row = 0; row < fo->server->grid_rows; row++) {
		for (int col = 0; col < fo->server->grid_columns; col++) {
			struct wlr_box b;
			cell_box(fo, col, row, &b);
			if (wlr_box_contains_point(&b, px, py)) {
				*c = col;
				*r = row;
				return true;
			}
		}
	}
	return false;
}

/* The nearest cell, for a drag that strays into the gaps or off the picker. */
static void nearest_cell(struct gride_flyout *fo, double px, double py, int *c, int *r) {
	struct gride_server *server = fo->server;
	double best = INFINITY;
	for (int row = 0; row < server->grid_rows; row++) {
		for (int col = 0; col < server->grid_columns; col++) {
			struct wlr_box b;
			cell_box(fo, col, row, &b);
			double dx = px - (b.x + b.width / 2.0), dy = py - (b.y + b.height / 2.0);
			double d = dx * dx + dy * dy;
			if (d < best) {
				best = d;
				*c = col;
				*r = row;
			}
		}
	}
}

void flyout_pointer_motion(struct gride_server *server, double lx, double ly) {
	struct gride_flyout *fo = server->flyout;
	if (!fo) {
		return;
	}
	double px = lx - fo->box.x, py = ly - fo->box.y;
	if (server->cursor_mode == GRIDE_CURSOR_FLYOUT) {
		int c = fo->anchor_c, r = fo->anchor_r;
		nearest_cell(fo, px, py, &c, &r);
		struct gride_range range = { fo->anchor_c, fo->anchor_r, c, r };
		if (range.c0 > range.c1) {
			int t = range.c0; range.c0 = range.c1; range.c1 = t;
		}
		if (range.r0 > range.r1) {
			int t = range.r0; range.r0 = range.r1; range.r1 = t;
		}
		set_hover(fo, -1, true, &range);
		return;
	}
	if (!wlr_box_contains_point(&fo->box, lx, ly)) {
		set_hover(fo, -1, false, NULL);
		return;
	}
	if (fo->close_pending) {
		fo->close_pending = false; /* the pointer is back on the flyout */
		disarm_timer(server);
	}
	int c, r;
	if (cell_at(fo, px, py, &c, &r)) {
		struct gride_range range = { c, r, c, r };
		set_hover(fo, -1, true, &range);
	} else {
		set_hover(fo, zone_at(fo, px, py), false, NULL);
	}
}

bool flyout_pointer_button(struct gride_server *server, double lx, double ly, bool pressed) {
	struct gride_flyout *fo = server->flyout;
	if (!fo) {
		return false;
	}
	double px = lx - fo->box.x, py = ly - fo->box.y;
	if (!pressed) {
		if (server->cursor_mode == GRIDE_CURSOR_FLYOUT) {
			server->cursor_mode = GRIDE_CURSOR_PASSTHROUGH;
			struct gride_range range = fo->cell_range;
			apply(fo, &range);
			return true;
		}
		return wlr_box_contains_point(&fo->box, lx, ly);
	}
	if (!wlr_box_contains_point(&fo->box, lx, ly)) {
		return false;
	}
	int c, r;
	if (cell_at(fo, px, py, &c, &r)) {
		/* Start a range; the release applies it. */
		fo->anchor_c = c;
		fo->anchor_r = r;
		server->cursor_mode = GRIDE_CURSOR_FLYOUT;
		struct gride_range range = { c, r, c, r };
		set_hover(fo, -1, true, &range);
		return true;
	}
	int z = zone_at(fo, px, py);
	if (z >= 0) {
		struct gride_range range = fo->zones[z].range;
		apply(fo, &range);
	}
	return true; /* a click on the panel itself does nothing else */
}

/*
 * Called on every pointer motion outside the flyout with the view whose
 * Maximise button is under the pointer (or NULL). Starts the 400ms
 * hover-to-open, and closes a hover-opened flyout once the pointer has
 * left both the flyout and the button for a moment.
 */
void flyout_hover_maximize(struct gride_server *server, struct gride_view *view) {
	struct gride_flyout *fo = server->flyout;
	if (fo) {
		if (fo->by_hover) {
			if (view == fo->view) {
				if (fo->close_pending) {
					fo->close_pending = false;
					disarm_timer(server);
				}
			} else if (!fo->close_pending) {
				fo->close_pending = true;
				arm_timer(server, true, CLOSE_DELAY_MS);
			}
		}
		set_hover(fo, -1, false, NULL);
		return;
	}
	if (view == server->flyout_hover_view) {
		return;
	}
	server->flyout_hover_view = view;
	if (view && view_is_snappable(view)) {
		arm_timer(server, false, GRIDE_SNAP_FLYOUT_HOVER_MS);
	} else {
		disarm_timer(server);
	}
}

/* ---- keyboard ----------------------------------------------------------- */

bool flyout_handle_key(struct gride_server *server, xkb_keysym_t sym) {
	struct gride_flyout *fo = server->flyout;
	if (!fo) {
		return false;
	}
	int z = fo->hover_zone;
	switch (sym) {
	case XKB_KEY_Escape:
		flyout_close(server);
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (z >= 0) {
			struct gride_range range = fo->zones[z].range;
			apply(fo, &range);
		} else if (fo->cell_hover) {
			struct gride_range range = fo->cell_range;
			apply(fo, &range);
		}
		return true;
	case XKB_KEY_Left:
		z = z <= 0 ? fo->zone_count - 1 : z - 1;
		break;
	case XKB_KEY_Right:
		z = z < 0 || z >= fo->zone_count - 1 ? 0 : z + 1;
		break;
	case XKB_KEY_Up:
	case XKB_KEY_Down: {
		/* To the first zone of the layout above or below. */
		int layout = z < 0 ? 0 : fo->zones[z].layout;
		layout += sym == XKB_KEY_Down ? THUMB_COLUMNS : -THUMB_COLUMNS;
		layout = (layout % LAYOUTS + LAYOUTS) % LAYOUTS;
		for (int i = 0; i < fo->zone_count; i++) {
			if (fo->zones[i].layout == layout) {
				z = i;
				break;
			}
		}
		break;
	}
	default:
		return false;
	}
	set_hover(fo, z, false, NULL);
	return true;
}

/* ---- lifetime ----------------------------------------------------------- */

void flyout_view_gone(struct gride_server *server, struct gride_view *view) {
	if (server->flyout && server->flyout->view == view) {
		flyout_close(server);
	}
	if (server->flyout_hover_view == view) {
		server->flyout_hover_view = NULL;
		disarm_timer(server);
	}
}

void flyout_finish(struct gride_server *server) {
	flyout_close(server);
	if (server->flyout_timer) {
		wl_event_source_remove(server->flyout_timer);
		server->flyout_timer = NULL;
	}
}
