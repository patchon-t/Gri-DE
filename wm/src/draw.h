/* Drawing helpers: cairo-backed wlr_buffers and Pango text in GriDE type. */
#ifndef GRIDE_DRAW_H
#define GRIDE_DRAW_H

#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>

#include "gride-tokens.h"

struct wlr_buffer;

/*
 * A wlr_buffer that owns a cairo image surface (ARGB32, premultiplied,
 * which is DRM_FORMAT_ARGB8888 in memory). Draw with ->cr, then hand
 * &->base to a scene node and drop your reference with draw_buffer_drop().
 */
struct draw_buffer {
	struct wlr_buffer *base;
	cairo_surface_t *surface;
	cairo_t *cr;
	int width, height; /* pixels */
	float scale;
};

/* Pixel size is ceil(logical * scale); cr is pre-scaled to logical units. */
struct draw_buffer *draw_buffer_create(int logical_width, int logical_height, float scale);
/* Finish drawing and release our reference. The scene node keeps its own. */
void draw_buffer_drop(struct draw_buffer *buf);

/* Load the bundled fonts (Rubik) so they work before `make install`. */
void draw_init_fonts(void);

void draw_set_color(cairo_t *cr, const float color[4]);
void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r);
/* Rounded on the top corners only (title bars). */
void draw_rounded_top_rect(cairo_t *cr, double x, double y, double w, double h, double r);

/*
 * A Pango layout for `text` in the given type style. `markup` selects
 * Pango markup instead of plain text. max_width (logical px, 0 = none)
 * ellipsizes at the end. Free with g_object_unref().
 */
PangoLayout *draw_text_layout(cairo_t *cr, const struct gride_type_style *style,
	const char *text, bool markup, int max_width);
/* Logical size of a layout in px. */
void draw_text_size(PangoLayout *layout, int *width, int *height);

#endif
