/*
 * Server-side decorations: the GriDE title bar, the 1px window edge and the
 * floating shadow.
 *
 * Scene layout inside a view's tree (back to front):
 *   deco->tree        lowered below the content
 *     shadow[8]       9-slice of one shared shadow image (no centre piece)
 *     border[3]       1px `line` down both sides and along the bottom
 *     titlebar        a cairo buffer, redrawn only when what it shows changes
 *   content_tree      the client's surfaces, below the title bar
 *
 * The title bar buffer is 1px larger than the frame on the top, left and
 * right so its rounded edge and hairline are part of the same drawing.
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "draw.h"
#include "icons.h"
#include "server.h"

struct gride_deco {
	struct gride_view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *shadow[8];
	struct wlr_scene_rect *border[3];
	struct wlr_scene_buffer *titlebar;

	enum gride_deco_part hover;
	enum gride_deco_part pressed;

	/* What the current title bar buffer shows, to skip identical redraws. */
	struct {
		int width;
		bool focused, maximized, terminal;
		enum gride_deco_part hover, pressed;
		float scale;
		const struct gride_palette *palette;
		char *title;
	} drawn;
};

/* ---- the shared shadow image -------------------------------------------- */

/*
 * One image of the shadow-float token around a small rounded rectangle,
 * cut into 9 slices. Corners are drawn 1:1; the 2px middle strips stretch
 * along the edges. The window area is cut out, so a translucent window
 * does not show its own shadow through itself.
 */
static struct {
	struct draw_buffer *buf;
	int pad;    /* how far the shadow reaches outside the window */
	int corner; /* slice size of a corner */
	int size;   /* image width = height */
} shadow;

/* Box sizes for 3 box blurs that approximate a gaussian of sigma. */
static void gauss_boxes(double sigma, int sizes[3]) {
	double ideal = sqrt(12.0 * sigma * sigma / 3.0 + 1.0);
	int wl = (int)floor(ideal);
	if (wl % 2 == 0) {
		wl--;
	}
	int wu = wl + 2;
	double m_ideal = (12.0 * sigma * sigma - 3.0 * wl * wl - 12.0 * wl - 9.0) / (-4.0 * wl - 4.0);
	int m = (int)lround(m_ideal);
	for (int i = 0; i < 3; i++) {
		sizes[i] = i < m ? wl : wu;
	}
}

static void box_blur_pass(const float *src, float *dst, int w, int h, int r, bool horizontal) {
	float inv = 1.0f / (2 * r + 1);
	int lines = horizontal ? h : w, len = horizontal ? w : h;
	for (int line = 0; line < lines; line++) {
#define AT(i) (horizontal ? src[line * w + (i)] : src[(i) * w + line])
		float acc = 0;
		/* Values outside the image are 0 (the shadow fades out before it). */
		for (int k = 0; k <= r && k < len; k++) {
			acc += AT(k);
		}
		for (int i = 0; i < len; i++) {
			dst[horizontal ? line * w + i : i * w + line] = acc * inv;
			int add = i + r + 1, sub = i - r;
			if (add < len) {
				acc += AT(add);
			}
			if (sub >= 0) {
				acc -= AT(sub);
			}
		}
#undef AT
	}
}

static void gaussian_blur(float *data, float *tmp, int w, int h, double sigma) {
	if (sigma <= 0) {
		return;
	}
	int boxes[3];
	gauss_boxes(sigma, boxes);
	for (int i = 0; i < 3; i++) {
		int r = (boxes[i] - 1) / 2;
		box_blur_pass(data, tmp, w, h, r, true);
		box_blur_pass(tmp, data, w, h, r, false);
	}
}

/* Coverage (0..1) of a rounded rectangle, rasterised by cairo. */
static float *rounded_rect_mask(int w, int h, double x, double y, double rw, double rh,
		double r) {
	cairo_surface_t *a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
	cairo_t *cr = cairo_create(a8);
	draw_rounded_rect(cr, x, y, rw, rh, r);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_flush(a8);
	float *mask = malloc(sizeof(float) * (size_t)w * (size_t)h);
	unsigned char *px = cairo_image_surface_get_data(a8);
	int stride = cairo_image_surface_get_stride(a8);
	for (int j = 0; mask && j < h; j++) {
		for (int i = 0; i < w; i++) {
			mask[j * w + i] = px[j * stride + i] / 255.0f;
		}
	}
	cairo_surface_destroy(a8);
	return mask;
}

int shadow_reach(const struct gride_shadow_layer *layers) {
	double reach = 0;
	for (int i = 0; i < GRIDE_SHADOW_FLOAT_LAYERS; i++) {
		/* CSS blur radius = 2 sigma; a gaussian is invisible past 3 sigma. */
		double out = 1.5 * layers[i].blur + fmax(fabs(layers[i].x), fabs(layers[i].y));
		reach = fmax(reach, out);
	}
	return (int)ceil(reach);
}

struct draw_buffer *shadow_render(int w, int h, double radius,
		const struct gride_shadow_layer *layers) {
	int pad = shadow_reach(layers);
	int bw = w + 2 * pad, bh = h + 2 * pad;
	struct draw_buffer *buf = draw_buffer_create(bw, bh, 1.0f);
	size_t n = (size_t)bw * (size_t)bh;
	float *acc_rgb[3] = { calloc(n, sizeof(float)), calloc(n, sizeof(float)),
		calloc(n, sizeof(float)) };
	float *acc_a = calloc(n, sizeof(float));
	float *tmp = malloc(n * sizeof(float));
	float *window = NULL;
	if (!buf || !acc_a || !acc_rgb[0] || !acc_rgb[1] || !acc_rgb[2] || !tmp) {
		wlr_log(WLR_ERROR, "Out of memory drawing a shadow");
		draw_buffer_drop(buf);
		buf = NULL;
		goto out;
	}

	/* Composite the layers back to front: the first CSS shadow is on top. */
	for (int l = GRIDE_SHADOW_FLOAT_LAYERS - 1; l >= 0; l--) {
		const struct gride_shadow_layer *layer = &layers[l];
		float *mask = rounded_rect_mask(bw, bh, pad + layer->x, pad + layer->y, w, h, radius);
		if (!mask) {
			continue;
		}
		gaussian_blur(mask, tmp, bw, bh, layer->blur / 2.0);
		for (size_t i = 0; i < n; i++) {
			float a = layer->color[3] * mask[i];
			for (int c = 0; c < 3; c++) {
				acc_rgb[c][i] = layer->color[c] * a + acc_rgb[c][i] * (1 - a);
			}
			acc_a[i] = a + acc_a[i] * (1 - a);
		}
		free(mask);
	}

	/* Cut out the shape itself, so a translucent surface on top does not
	 * show its own shadow through it. */
	window = rounded_rect_mask(bw, bh, pad, pad, w, h, radius);
	unsigned char *px = cairo_image_surface_get_data(buf->surface);
	int stride = cairo_image_surface_get_stride(buf->surface);
	for (int j = 0; j < bh; j++) {
		uint32_t *row = (uint32_t *)(px + j * stride);
		for (int i = 0; i < bw; i++) {
			size_t k = (size_t)j * (size_t)bw + (size_t)i;
			float keep = window ? 1.0f - window[k] : 1.0f;
			/* ARGB32 is premultiplied: the colour channels already carry a. */
			uint32_t a = (uint32_t)lroundf(acc_a[k] * keep * 255.0f);
			uint32_t r = (uint32_t)lroundf(acc_rgb[0][k] * keep * 255.0f);
			uint32_t g = (uint32_t)lroundf(acc_rgb[1][k] * keep * 255.0f);
			uint32_t b = (uint32_t)lroundf(acc_rgb[2][k] * keep * 255.0f);
			row[i] = a << 24 | r << 16 | g << 8 | b;
		}
	}
	cairo_surface_mark_dirty(buf->surface);
	cairo_surface_flush(buf->surface);

out:
	free(window);
	free(tmp);
	free(acc_a);
	for (int c = 0; c < 3; c++) {
		free(acc_rgb[c]);
	}
	return buf;
}

static void shadow_build(const struct gride_shadow_layer *layers) {
	const double radius = GRIDE_RADIUS_MD;
	double inner_reach = 0;
	for (int i = 0; i < GRIDE_SHADOW_FLOAT_LAYERS; i++) {
		inner_reach = fmax(inner_reach, 1.5 * layers[i].blur);
	}
	shadow.pad = shadow_reach(layers);
	int inner_corner = (int)ceil(inner_reach + radius);
	shadow.corner = shadow.pad + inner_corner;
	int inner = 2 * inner_corner + 2; /* a 2px stretchable middle */
	shadow.size = 2 * shadow.pad + inner;
	shadow.buf = shadow_render(inner, inner, radius, layers);
}

void deco_init_shared(struct gride_server *server) {
	shadow_build(server->shadow);
}

void deco_finish_shared(void) {
	/* Scene nodes that still show the image hold their own references. */
	draw_buffer_drop(shadow.buf);
	shadow.buf = NULL;
}

/* Shadows and hairlines are decoration only: clicks pass through them. */
static bool no_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	return false;
}

/* ---- per-window decorations --------------------------------------------- */

struct gride_deco *deco_create(struct gride_view *view) {
	struct gride_deco *deco = calloc(1, sizeof(*deco));
	if (!deco) {
		return NULL;
	}
	deco->view = view;
	deco->tree = wlr_scene_tree_create(view->scene_tree);
	wlr_scene_node_lower_to_bottom(&deco->tree->node);

	for (int i = 0; i < 8; i++) {
		deco->shadow[i] = wlr_scene_buffer_create(deco->tree,
			shadow.buf ? shadow.buf->base : NULL);
		deco->shadow[i]->point_accepts_input = no_input;
	}
	static const float transparent[4] = { 0, 0, 0, 0 };
	for (int i = 0; i < 3; i++) {
		deco->border[i] = wlr_scene_rect_create(deco->tree, 1, 1, transparent);
	}
	deco->titlebar = wlr_scene_buffer_create(deco->tree, NULL);
	deco_update(deco);
	return deco;
}

void deco_destroy(struct gride_deco *deco) {
	if (!deco) {
		return;
	}
	wlr_scene_node_destroy(&deco->tree->node);
	free(deco->drawn.title);
	free(deco);
}

bool deco_owns_node(struct gride_deco *deco, struct wlr_scene_node *node) {
	return node == &deco->titlebar->node;
}

enum gride_deco_part deco_part_at(struct gride_deco *deco, double fx, double fy) {
	int width = deco->view->content_width;
	if (fy < 0 || fy >= GRIDE_TITLEBAR_HEIGHT || fx < 0 || fx >= width) {
		return GRIDE_DECO_NONE;
	}
	int buttons_x = width - 3 * GRIDE_TITLEBAR_BUTTON_WIDTH;
	if (fx < buttons_x) {
		return GRIDE_DECO_TITLE;
	}
	int index = (int)((fx - buttons_x) / GRIDE_TITLEBAR_BUTTON_WIDTH);
	static const enum gride_deco_part parts[] = {
		GRIDE_DECO_MINIMIZE, GRIDE_DECO_MAXIMIZE, GRIDE_DECO_CLOSE,
	};
	return parts[index < 0 ? 0 : index > 2 ? 2 : index];
}

/* Colours for one title bar. Terminal windows get a dark bar: the dark
 * palette's tokens over the `terminal` background. */
struct bar_colors {
	const float *bg, *text, *line, *hover;
};

static struct bar_colors bar_colors(struct gride_view *view, bool focused, bool terminal) {
	const struct gride_palette *p = view->server->palette;
	if (terminal) {
		return (struct bar_colors){
			.bg = p->terminal,
			.text = focused ? p->terminal_ink : p->terminal_muted,
			.line = gride_palette_dark.line,
			.hover = gride_palette_dark.hover,
		};
	}
	return (struct bar_colors){
		/* tokens.json: `desktop` is also the inactive title bar. */
		.bg = focused ? p->surface : p->desktop,
		.text = focused ? p->ink : p->muted,
		.line = p->line,
		.hover = p->hover,
	};
}

static void draw_glyph(cairo_t *cr, enum gride_deco_part part, bool maximized,
		double cx, double cy) {
	const double g = GRIDE_TITLEBAR_GLYPH;
	const double s = GRIDE_TITLEBAR_GLYPH_STROKE;
	const double h = g / 2.0;
	cairo_set_line_width(cr, s);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (part) {
	case GRIDE_DECO_MINIMIZE:
		cairo_move_to(cr, cx - h + s / 2, cy);
		cairo_line_to(cr, cx + h - s / 2, cy);
		break;
	case GRIDE_DECO_MAXIMIZE:
		if (!maximized) {
			draw_rounded_rect(cr, cx - h + s / 2, cy - h + s / 2, g - s, g - s, 2);
		} else {
			/* Restore: a front square and the corner of one behind it,
			 * offset up and to the right. */
			const double off = 3;
			double fs = g - off - s;
			double L = cx - h + s / 2, T = cy - h + off + s / 2;
			draw_rounded_rect(cr, L, T, fs, fs, 2);
			cairo_move_to(cr, L + off, T);
			cairo_line_to(cr, L + off, T - off);
			cairo_line_to(cr, L + off + fs, T - off);
			cairo_line_to(cr, L + off + fs, T - off + fs);
			cairo_line_to(cr, L + fs, T - off + fs);
		}
		break;
	case GRIDE_DECO_CLOSE:
		cairo_move_to(cr, cx - h + s / 2, cy - h + s / 2);
		cairo_line_to(cr, cx + h - s / 2, cy + h - s / 2);
		cairo_move_to(cr, cx + h - s / 2, cy - h + s / 2);
		cairo_line_to(cr, cx - h + s / 2, cy + h - s / 2);
		break;
	default:
		return;
	}
	cairo_stroke(cr);
}

static bool contains_ignoring_case(const char *haystack, const char *needle) {
	size_t n = strlen(needle);
	for (const char *p = haystack; *p; p++) {
		if (strncasecmp(p, needle, n) == 0) {
			return true;
		}
	}
	return n == 0;
}

/* "config.toml · Editor": the window title, then the app's name unless the
 * title already says it. */
static char *compose_title(struct gride_view *view) {
	const char *title = view_title(view);
	const char *app = app_display_name(view_app_id(view));
	if (!app || !app[0]) {
		return strdup(title[0] ? title : view_app_id(view));
	}
	if (!title[0]) {
		return strdup(app);
	}
	if (contains_ignoring_case(title, app)) {
		return strdup(title);
	}
	size_t len = strlen(title) + strlen(app) + 8;
	char *out = malloc(len);
	if (out) {
		snprintf(out, len, "%s · %s", title, app);
	}
	return out;
}

static void draw_titlebar(struct gride_deco *deco, int width, bool focused, bool maximized,
		bool terminal, const char *title) {
	struct gride_view *view = deco->view;
	const int th = GRIDE_TITLEBAR_HEIGHT;
	const double radius = maximized ? 0 : GRIDE_RADIUS_MD;
	const int edge = maximized ? 0 : 1; /* hairline outside the frame */
	struct bar_colors col = bar_colors(view, focused, terminal);

	struct draw_buffer *buf = draw_buffer_create(width + 2 * edge, th + edge,
		view->server->deco_scale);
	if (!buf) {
		return;
	}
	cairo_t *cr = buf->cr;
	cairo_translate(cr, edge, edge); /* (0, 0) is now the frame origin */

	/* Background, clipped to the rounded top so hover fills follow it. */
	draw_rounded_top_rect(cr, 0, 0, width, th, radius);
	cairo_save(cr);
	cairo_clip_preserve(cr);
	draw_set_color(cr, col.bg);
	cairo_fill(cr);

	/* Buttons: minimise, maximise, close, each 46x36. */
	static const enum gride_deco_part parts[] = {
		GRIDE_DECO_MINIMIZE, GRIDE_DECO_MAXIMIZE, GRIDE_DECO_CLOSE,
	};
	const float white[4] = { 1, 1, 1, 1 };
	for (int i = 0; i < 3; i++) {
		double bx = width - (3 - i) * GRIDE_TITLEBAR_BUTTON_WIDTH;
		if (bx < 0) {
			continue;
		}
		bool lit = deco->hover == parts[i] || deco->pressed == parts[i];
		const float *glyph = col.text;
		if (lit) {
			const float *fill = parts[i] == GRIDE_DECO_CLOSE ?
				view->server->palette->danger : col.hover;
			cairo_rectangle(cr, bx, 0, GRIDE_TITLEBAR_BUTTON_WIDTH, th);
			draw_set_color(cr, fill);
			cairo_fill(cr);
			if (parts[i] == GRIDE_DECO_CLOSE) {
				glyph = white;
			}
		}
		draw_set_color(cr, glyph);
		draw_glyph(cr, parts[i], maximized, bx + GRIDE_TITLEBAR_BUTTON_WIDTH / 2.0, th / 2.0);
	}
	cairo_restore(cr);

	/* App tile and title. */
	const double pad = GRIDE_SPACE_3;
	const double icon_y = (th - GRIDE_TITLEBAR_ICON) / 2.0;
	app_icon_draw(cr, view_app_id(view), title, pad, icon_y, GRIDE_TITLEBAR_ICON,
		terminal ? &gride_palette_dark : view->server->palette);
	double text_x = pad + GRIDE_TITLEBAR_ICON + GRIDE_SPACE_2;
	int text_room = width - (int)text_x - 3 * GRIDE_TITLEBAR_BUTTON_WIDTH - GRIDE_SPACE_2;
	if (text_room > 8 && title && title[0]) {
		PangoLayout *layout = draw_text_layout(cr, &gride_type_label, title, false, text_room);
		int tw, tht;
		draw_text_size(layout, &tw, &tht);
		draw_set_color(cr, col.text);
		cairo_move_to(cr, text_x, (th - tht) / 2.0);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}

	/* The window edge: up the sides and over the rounded top, not across
	 * the bottom (the content continues there). */
	if (edge) {
		const double e = 0.5; /* centre of the 1px line just outside */
		cairo_new_path(cr);
		cairo_move_to(cr, -e, th);
		cairo_arc(cr, radius, radius, radius + e, M_PI, 3 * M_PI / 2);
		cairo_arc(cr, width - radius, radius, radius + e, 3 * M_PI / 2, 2 * M_PI);
		cairo_line_to(cr, width + e, th);
		cairo_set_line_width(cr, 1);
		draw_set_color(cr, col.line);
		cairo_stroke(cr);
	}

	wlr_scene_buffer_set_buffer(deco->titlebar, buf->base);
	wlr_scene_buffer_set_dest_size(deco->titlebar, width + 2 * edge, th + edge);
	wlr_scene_node_set_position(&deco->titlebar->node, -edge, -edge);
	draw_buffer_drop(buf);
}

static void place_shadow(struct gride_deco *deco, int w, int h, bool visible) {
	const int P = shadow.pad, C = shadow.corner, S = shadow.size;
	int mid_w = w + 2 * P - 2 * C;
	int mid_h = h + 2 * P - 2 * C;
	struct {
		double sx, sy, sw, sh; /* source box in the shadow image */
		int x, y, w, h;        /* destination, frame-relative */
	} slices[8] = {
		{ 0, 0, C, C, -P, -P, C, C },                               /* top-left */
		{ C, 0, S - 2 * C, C, C - P, -P, mid_w, C },                /* top */
		{ S - C, 0, C, C, w + P - C, -P, C, C },                    /* top-right */
		{ 0, C, C, S - 2 * C, -P, C - P, C, mid_h },                /* left */
		{ S - C, C, C, S - 2 * C, w + P - C, C - P, C, mid_h },     /* right */
		{ 0, S - C, C, C, -P, h + P - C, C, C },                    /* bottom-left */
		{ C, S - C, S - 2 * C, C, C - P, h + P - C, mid_w, C },     /* bottom */
		{ S - C, S - C, C, C, w + P - C, h + P - C, C, C },         /* bottom-right */
	};
	for (int i = 0; i < 8; i++) {
		struct wlr_scene_buffer *node = deco->shadow[i];
		bool show = visible && shadow.buf && slices[i].w > 0 && slices[i].h > 0;
		wlr_scene_node_set_enabled(&node->node, show);
		if (!show) {
			continue;
		}
		struct wlr_fbox src = { slices[i].sx, slices[i].sy, slices[i].sw, slices[i].sh };
		wlr_scene_buffer_set_source_box(node, &src);
		wlr_scene_buffer_set_dest_size(node, slices[i].w, slices[i].h);
		wlr_scene_node_set_position(&node->node, slices[i].x, slices[i].y);
	}
}

void deco_update(struct gride_deco *deco) {
	struct gride_view *view = deco->view;
	struct gride_server *server = view->server;
	int w = view->content_width;
	bool visible = view->ssd && !view->fullscreen && w > 0;
	wlr_scene_node_set_enabled(&deco->tree->node, visible);
	if (!visible) {
		return;
	}
	const int th = GRIDE_TITLEBAR_HEIGHT;
	int h = view->content_height + th;
	bool focused = server->focused_view == view;
	bool maximized = view->maximized;
	bool terminal = view_is_terminal(view);
	struct bar_colors col = bar_colors(view, focused, terminal);

	/* The one shadow goes to the focused window (and so the dragged one).
	 * Maximised windows are docked: no shadow, no edge. */
	place_shadow(deco, w, h, focused && !maximized);

	struct { int x, y, w, h; } edges[3] = {
		{ -1, th, 1, h - th + 1 }, /* left, down to the bottom-left pixel */
		{ w, th, 1, h - th + 1 },  /* right */
		{ 0, h, w, 1 },            /* bottom */
	};
	for (int i = 0; i < 3; i++) {
		wlr_scene_node_set_enabled(&deco->border[i]->node, !maximized);
		wlr_scene_rect_set_size(deco->border[i], edges[i].w, edges[i].h);
		wlr_scene_rect_set_color(deco->border[i], col.line);
		wlr_scene_node_set_position(&deco->border[i]->node, edges[i].x, edges[i].y);
	}

	char *title = compose_title(view);
	bool same = deco->drawn.width == w && deco->drawn.focused == focused &&
		deco->drawn.maximized == maximized && deco->drawn.terminal == terminal &&
		deco->drawn.hover == deco->hover && deco->drawn.pressed == deco->pressed &&
		deco->drawn.scale == server->deco_scale && deco->drawn.palette == server->palette &&
		deco->drawn.title && title && strcmp(deco->drawn.title, title) == 0;
	if (same) {
		free(title);
		return;
	}
	draw_titlebar(deco, w, focused, maximized, terminal, title);
	deco->drawn.width = w;
	deco->drawn.focused = focused;
	deco->drawn.maximized = maximized;
	deco->drawn.terminal = terminal;
	deco->drawn.hover = deco->hover;
	deco->drawn.pressed = deco->pressed;
	deco->drawn.scale = server->deco_scale;
	deco->drawn.palette = server->palette;
	free(deco->drawn.title);
	deco->drawn.title = title;
}

void deco_set_hover(struct gride_deco *deco, enum gride_deco_part part) {
	if (part == GRIDE_DECO_TITLE) {
		part = GRIDE_DECO_NONE; /* only buttons light up */
	}
	if (deco->hover != part) {
		deco->hover = part;
		deco_update(deco);
	}
}

void deco_set_pressed(struct gride_deco *deco, enum gride_deco_part part) {
	if (deco->pressed != part) {
		deco->pressed = part;
		deco_update(deco);
	}
}
