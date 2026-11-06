/* Drawing helpers: cairo-backed wlr_buffers and Pango text. */
#include <drm_fourcc.h>
#include <fontconfig/fontconfig.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>

#include "draw.h"

struct cairo_wlr_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct cairo_wlr_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	cairo_surface_destroy(buf->surface);
	free(buf);
}

static bool cairo_buffer_begin_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	struct cairo_wlr_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = cairo_image_surface_get_data(buf->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)cairo_image_surface_get_stride(buf->surface);
	return true;
}

static void cairo_buffer_end_access(struct wlr_buffer *wlr_buffer) {
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
	.destroy = cairo_buffer_destroy,
	.begin_data_ptr_access = cairo_buffer_begin_access,
	.end_data_ptr_access = cairo_buffer_end_access,
};

struct draw_buffer *draw_buffer_create(int logical_width, int logical_height, float scale) {
	if (logical_width <= 0 || logical_height <= 0 || scale <= 0.0f) {
		return NULL;
	}
	int width = (int)ceilf(logical_width * scale);
	int height = (int)ceilf(logical_height * scale);

	struct cairo_wlr_buffer *cbuf = calloc(1, sizeof(*cbuf));
	struct draw_buffer *buf = calloc(1, sizeof(*buf));
	if (!cbuf || !buf) {
		free(cbuf);
		free(buf);
		return NULL;
	}
	cbuf->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(cbuf->surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cbuf->surface);
		free(cbuf);
		free(buf);
		return NULL;
	}
	wlr_buffer_init(&cbuf->base, &cairo_buffer_impl, width, height);

	buf->base = &cbuf->base;
	buf->surface = cbuf->surface;
	buf->cr = cairo_create(cbuf->surface);
	buf->width = width;
	buf->height = height;
	buf->scale = scale;
	cairo_scale(buf->cr, scale, scale);
	return buf;
}

void draw_buffer_drop(struct draw_buffer *buf) {
	if (!buf) {
		return;
	}
	cairo_destroy(buf->cr);
	/* Make sure every pixel cairo wrote is in memory before a renderer
	 * reads the buffer. */
	cairo_surface_flush(buf->surface);
	wlr_buffer_drop(buf->base);
	free(buf);
}

static void add_font_dir(const char *dir) {
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return;
	}
	if (FcConfigAppFontAddDir(NULL, (const FcChar8 *)dir)) {
		wlr_log(WLR_DEBUG, "Loaded fonts from %s", dir);
	}
}

void draw_init_fonts(void) {
	/*
	 * Must run before Pango creates its font map. The installed copy wins
	 * when both exist only because fontconfig already sees it; the source
	 * tree copy lets `make dev` render Rubik without installing anything.
	 */
	add_font_dir(GRIDE_FONTDIR);
	add_font_dir(GRIDE_SOURCE_FONTDIR);
}

void draw_set_color(cairo_t *cr, const float color[4]) {
	cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
}

void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r > w / 2) {
		r = w / 2;
	}
	if (r > h / 2) {
		r = h / 2;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void draw_rounded_top_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
	if (r > w / 2) {
		r = w / 2;
	}
	if (r > h) {
		r = h;
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_line_to(cr, x + w, y + h);
	cairo_line_to(cr, x, y + h);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

PangoLayout *draw_text_layout(cairo_t *cr, const struct gride_type_style *style,
		const char *text, bool markup, int max_width) {
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_new();
	pango_font_description_set_family(desc, style->family);
	pango_font_description_set_absolute_size(desc, style->size * PANGO_SCALE);
	pango_font_description_set_weight(desc, (PangoWeight)style->weight);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	/* Grey-scale antialiasing: subpixel text breaks on rotated outputs and
	 * when the buffer is scaled. */
	cairo_font_options_t *opts = cairo_font_options_create();
	cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_GRAY);
	cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_SLIGHT);
	pango_cairo_context_set_font_options(pango_layout_get_context(layout), opts);
	cairo_font_options_destroy(opts);
	pango_layout_context_changed(layout);

	pango_layout_set_single_paragraph_mode(layout, TRUE);
	if (max_width > 0) {
		pango_layout_set_width(layout, max_width * PANGO_SCALE);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	}
	if (markup) {
		pango_layout_set_markup(layout, text, -1);
	} else {
		pango_layout_set_text(layout, text, -1);
	}
	return layout;
}

void draw_text_size(PangoLayout *layout, int *width, int *height) {
	PangoRectangle logical;
	pango_layout_get_pixel_extents(layout, NULL, &logical);
	if (width) {
		*width = logical.width;
	}
	if (height) {
		*height = logical.height;
	}
}
