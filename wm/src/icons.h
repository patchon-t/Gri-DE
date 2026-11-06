/* App names and icons for title bars (and later the taskbar). */
#ifndef GRIDE_ICONS_H
#define GRIDE_ICONS_H

#include <cairo.h>

#include "gride-tokens.h"

/*
 * Display name for an app id (xdg app_id or X11 class), from the Name= of
 * its .desktop file. Returns NULL when no .desktop file is found.
 * The string lives until icons_finish().
 */
const char *app_display_name(const char *app_id);

/*
 * Draw the app's icon in a size x size box (logical px) at (x, y).
 * Unknown apps get the generic tile: a `chip` square with radius 25% and
 * the first letter of `label` in `chip-ink`.
 */
void app_icon_draw(cairo_t *cr, const char *app_id, const char *label,
	double x, double y, double size, const struct gride_palette *palette);

void icons_finish(void);

#endif
