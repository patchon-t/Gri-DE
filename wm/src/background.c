/*
 * The desktop: a `desktop` fill with a 1px `desktop-dot` every 24px.
 *
 * The pattern is baked into one CPU buffer per output at the output's pixel
 * size, so dots stay 1 device pixel sharp at scale 1 and grow with the scale
 * factor on HiDPI. It is redrawn only when the output's size, scale or the
 * palette changes, never per frame.
 */
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "server.h"

#define DOT_SPACING 24 /* logical px between dots, from the design spec */
/* Dots sit on the half-step so that the 12px screen margin and the grid
 * gap (space-3) fall exactly on a dot row and column. */
#define DOT_OFFSET GRIDE_SPACE_3

/* A minimal read-only wlr_buffer over malloc'd XRGB8888 pixels. */
struct pixel_buffer {
	struct wlr_buffer base;
	uint32_t *data;
	size_t stride;
};

static void pixel_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct pixel_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	free(buf->data);
	free(buf);
}

static bool pixel_buffer_begin_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	struct pixel_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = buf->data;
	*format = DRM_FORMAT_XRGB8888;
	*stride = buf->stride;
	return true;
}

static void pixel_buffer_end_access(struct wlr_buffer *wlr_buffer) {
	/* Nothing to unmap: the pixels are plain heap memory. */
}

static const struct wlr_buffer_impl pixel_buffer_impl = {
	.destroy = pixel_buffer_destroy,
	.begin_data_ptr_access = pixel_buffer_begin_access,
	.end_data_ptr_access = pixel_buffer_end_access,
};

static uint32_t pack_xrgb(const float c[4]) {
	uint32_t r = (uint32_t)lroundf(c[0] * 255.0f);
	uint32_t g = (uint32_t)lroundf(c[1] * 255.0f);
	uint32_t b = (uint32_t)lroundf(c[2] * 255.0f);
	return 0xFF000000u | r << 16 | g << 8 | b;
}

static struct pixel_buffer *render_dots(int width, int height, float scale,
		const struct gride_palette *palette) {
	struct pixel_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		return NULL;
	}
	buf->stride = (size_t)width * 4;
	buf->data = malloc(buf->stride * (size_t)height);
	if (!buf->data) {
		free(buf);
		return NULL;
	}
	wlr_buffer_init(&buf->base, &pixel_buffer_impl, width, height);

	uint32_t fill = pack_xrgb(palette->desktop);
	uint32_t dot = pack_xrgb(palette->desktop_dot);
	int spacing = (int)lroundf(DOT_SPACING * scale);
	int offset = (int)lroundf(DOT_OFFSET * scale);
	int size = scale > 1.0f ? (int)lroundf(scale) : 1;
	if (spacing < 2) {
		spacing = 2;
	}

	for (int y = 0; y < height; y++) {
		uint32_t *row = buf->data + (size_t)y * (size_t)width;
		int ry = (y - offset) % spacing;
		bool dot_row = y >= offset && ry < size;
		for (int x = 0; x < width; x++) {
			row[x] = fill;
		}
		if (!dot_row) {
			continue;
		}
		for (int x = offset; x < width; x += spacing) {
			for (int k = 0; k < size && x + k < width; k++) {
				row[x + k] = dot;
			}
		}
	}
	return buf;
}

struct gride_background {
	struct wlr_scene_buffer *node;
	int width, height; /* pixel size of the current buffer */
	float scale;
	const struct gride_palette *palette;
};

struct gride_background *background_create(struct wlr_scene_tree *parent) {
	struct gride_background *bg = calloc(1, sizeof(*bg));
	if (!bg) {
		return NULL;
	}
	/* Starts with no buffer; background_update() gives it one. */
	bg->node = wlr_scene_buffer_create(parent, NULL);
	if (!bg->node) {
		free(bg);
		return NULL;
	}
	return bg;
}

void background_update(struct gride_background *bg, const struct gride_palette *palette,
		struct wlr_output *output, const struct wlr_box *layout_box) {
	wlr_scene_node_set_position(&bg->node->node, layout_box->x, layout_box->y);
	wlr_scene_buffer_set_dest_size(bg->node, layout_box->width, layout_box->height);

	/* Pixel size of the buffer. The output's mode is already in device
	 * pixels; a rotated output swaps them relative to the layout box. */
	int width = (int)lroundf(layout_box->width * output->scale);
	int height = (int)lroundf(layout_box->height * output->scale);
	if (width <= 0 || height <= 0) {
		return;
	}
	if (width == bg->width && height == bg->height && output->scale == bg->scale &&
			palette == bg->palette) {
		return;
	}

	struct pixel_buffer *buf = render_dots(width, height, output->scale, palette);
	if (!buf) {
		wlr_log(WLR_ERROR, "Out of memory drawing the desktop for %s", output->name);
		return;
	}
	/* The scene node takes its own lock on the buffer; dropping ours means
	 * the pixels are freed as soon as the node lets go of them. */
	wlr_scene_buffer_set_buffer(bg->node, &buf->base);
	wlr_buffer_drop(&buf->base);

	bg->width = width;
	bg->height = height;
	bg->scale = output->scale;
	bg->palette = palette;
}

void background_destroy(struct gride_background *bg) {
	if (!bg) {
		return;
	}
	wlr_scene_node_destroy(&bg->node->node);
	free(bg);
}
