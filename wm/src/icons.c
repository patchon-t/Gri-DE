/*
 * App names and icons.
 *
 * Lookup: app id -> .desktop file (by file name, then by StartupWMClass)
 * -> Name= and Icon= -> an icon file in the GriDE theme, then hicolor, then
 * /usr/share/pixmaps. Results are cached for the whole session: a desktop
 * session opens the same few apps over and over, and the lookup touches
 * the disk.
 */
#include <ctype.h>
#include <dirent.h>
#include <librsvg/rsvg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-util.h>
#include <wlr/util/log.h>

#include "draw.h"
#include "icons.h"

struct app_info {
	struct wl_list link;
	char *app_id;
	char *name;          /* Name= from the .desktop file, or NULL */
	RsvgHandle *svg;     /* parsed SVG icon, or NULL */
	cairo_surface_t *png; /* decoded PNG icon, or NULL */
};

static struct wl_list app_cache;
static bool app_cache_ready;

/* $XDG_DATA_HOME then $XDG_DATA_DIRS, as a NULL-terminated list. */
static char **data_dirs(void) {
	static char *dirs[32];
	static bool ready;
	if (ready) {
		return dirs;
	}
	ready = true;
	size_t n = 0;
	char buf[PATH_MAX];
	const char *home = getenv("XDG_DATA_HOME");
	if (home && home[0] == '/') {
		dirs[n++] = strdup(home);
	} else if (getenv("HOME")) {
		snprintf(buf, sizeof(buf), "%s/.local/share", getenv("HOME"));
		dirs[n++] = strdup(buf);
	}
	const char *list = getenv("XDG_DATA_DIRS");
	if (!list || !list[0]) {
		list = "/usr/local/share:/usr/share";
	}
	char *copy = strdup(list);
	for (char *save = NULL, *tok = strtok_r(copy, ":", &save);
			tok && n < sizeof(dirs) / sizeof(dirs[0]) - 1;
			tok = strtok_r(NULL, ":", &save)) {
		if (tok[0] == '/') {
			dirs[n++] = strdup(tok);
		}
	}
	free(copy);
	dirs[n] = NULL;
	return dirs;
}

static bool file_exists(const char *path) {
	return access(path, R_OK) == 0;
}

/* Read Name=, Icon= and StartupWMClass= from the [Desktop Entry] group. */
static bool parse_desktop_file(const char *path, char **name, char **icon, char **wm_class) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[1024];
	bool in_entry = false;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_entry = strcmp(line, "[Desktop Entry]") == 0;
			continue;
		}
		if (!in_entry) {
			continue;
		}
		if (name && !*name && strncmp(line, "Name=", 5) == 0) {
			*name = strdup(line + 5);
		} else if (icon && !*icon && strncmp(line, "Icon=", 5) == 0) {
			*icon = strdup(line + 5);
		} else if (wm_class && !*wm_class && strncmp(line, "StartupWMClass=", 15) == 0) {
			*wm_class = strdup(line + 15);
		}
	}
	fclose(f);
	return true;
}

static char *find_desktop_by_name(const char *app_id) {
	char path[PATH_MAX];
	char lower[256];
	size_t i;
	for (i = 0; app_id[i] && i < sizeof(lower) - 1; i++) {
		lower[i] = (char)tolower((unsigned char)app_id[i]);
	}
	lower[i] = '\0';
	for (char **dir = data_dirs(); *dir; dir++) {
		snprintf(path, sizeof(path), "%s/applications/%s.desktop", *dir, app_id);
		if (file_exists(path)) {
			return strdup(path);
		}
		snprintf(path, sizeof(path), "%s/applications/%s.desktop", *dir, lower);
		if (file_exists(path)) {
			return strdup(path);
		}
	}
	return NULL;
}

/* X11 apps and some Wayland apps only match through StartupWMClass. */
static char *find_desktop_by_wm_class(const char *app_id) {
	char path[PATH_MAX + 256]; /* directory plus a file name (max 255) */
	for (char **dir = data_dirs(); *dir; dir++) {
		char appdir[PATH_MAX];
		snprintf(appdir, sizeof(appdir), "%s/applications", *dir);
		DIR *d = opendir(appdir);
		if (!d) {
			continue;
		}
		struct dirent *ent;
		while ((ent = readdir(d))) {
			size_t len = strlen(ent->d_name);
			if (len < 9 || strcmp(ent->d_name + len - 8, ".desktop") != 0) {
				continue;
			}
			snprintf(path, sizeof(path), "%s/%s", appdir, ent->d_name);
			char *wm_class = NULL;
			parse_desktop_file(path, NULL, NULL, &wm_class);
			bool match = wm_class && strcasecmp(wm_class, app_id) == 0;
			free(wm_class);
			if (match) {
				closedir(d);
				return strdup(path);
			}
		}
		closedir(d);
	}
	return NULL;
}

static char *find_icon_file(const char *icon) {
	if (icon[0] == '/') {
		return file_exists(icon) ? strdup(icon) : NULL;
	}
	static const char *const png_sizes[] = {
		"256x256", "128x128", "96x96", "64x64", "48x48", "32x32", "24x24", "16x16",
	};
	char path[PATH_MAX];
	for (char **dir = data_dirs(); *dir; dir++) {
		snprintf(path, sizeof(path), "%s/icons/GriDE/scalable/apps/%s.svg", *dir, icon);
		if (file_exists(path)) {
			return strdup(path);
		}
	}
	for (char **dir = data_dirs(); *dir; dir++) {
		snprintf(path, sizeof(path), "%s/icons/hicolor/scalable/apps/%s.svg", *dir, icon);
		if (file_exists(path)) {
			return strdup(path);
		}
		for (size_t i = 0; i < sizeof(png_sizes) / sizeof(png_sizes[0]); i++) {
			snprintf(path, sizeof(path), "%s/icons/hicolor/%s/apps/%s.png",
				*dir, png_sizes[i], icon);
			if (file_exists(path)) {
				return strdup(path);
			}
		}
	}
	snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.svg", icon);
	if (file_exists(path)) {
		return strdup(path);
	}
	snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.png", icon);
	if (file_exists(path)) {
		return strdup(path);
	}
	return NULL;
}

static void load_icon(struct app_info *info, const char *path) {
	size_t len = strlen(path);
	if (len > 4 && strcasecmp(path + len - 4, ".svg") == 0) {
		GError *err = NULL;
		info->svg = rsvg_handle_new_from_file(path, &err);
		if (!info->svg) {
			wlr_log(WLR_DEBUG, "Cannot load icon %s: %s", path, err ? err->message : "?");
			g_clear_error(&err);
		}
	} else if (len > 4 && strcasecmp(path + len - 4, ".png") == 0) {
		cairo_surface_t *png = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(png) == CAIRO_STATUS_SUCCESS) {
			info->png = png;
		} else {
			cairo_surface_destroy(png);
		}
	}
}

static struct app_info *app_info_get(const char *app_id) {
	if (!app_cache_ready) {
		wl_list_init(&app_cache);
		app_cache_ready = true;
	}
	if (!app_id || !app_id[0]) {
		return NULL;
	}
	struct app_info *info;
	wl_list_for_each(info, &app_cache, link) {
		if (strcmp(info->app_id, app_id) == 0) {
			return info;
		}
	}

	info = calloc(1, sizeof(*info));
	if (!info) {
		return NULL;
	}
	info->app_id = strdup(app_id);
	wl_list_insert(&app_cache, &info->link);

	char *desktop = find_desktop_by_name(app_id);
	if (!desktop) {
		desktop = find_desktop_by_wm_class(app_id);
	}
	char *icon = NULL;
	if (desktop) {
		parse_desktop_file(desktop, &info->name, &icon, NULL);
		free(desktop);
	}
	/* No Icon= (or no .desktop file): many apps name the icon after the id. */
	char *icon_path = find_icon_file(icon ? icon : app_id);
	if (!icon_path && icon) {
		icon_path = find_icon_file(app_id);
	}
	if (icon_path) {
		load_icon(info, icon_path);
		free(icon_path);
	}
	free(icon);
	wlr_log(WLR_DEBUG, "App %s: name \"%s\", icon %s", app_id,
		info->name ? info->name : "", (info->svg || info->png) ? "found" : "generic");
	return info;
}

const char *app_display_name(const char *app_id) {
	struct app_info *info = app_info_get(app_id);
	return info ? info->name : NULL;
}

static void draw_letter_tile(cairo_t *cr, const char *label, double x, double y,
		double size, const struct gride_palette *palette) {
	draw_rounded_rect(cr, x, y, size, size, size * 0.25); /* radius-icon: 25% */
	draw_set_color(cr, palette->chip);
	cairo_fill(cr);

	gunichar ch = '?';
	for (const char *p = label; p && *p; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		if (g_unichar_isalnum(c)) {
			ch = g_unichar_toupper(c);
			break;
		}
	}
	char letter[8] = {0};
	g_unichar_to_utf8(ch, letter);

	struct gride_type_style style = gride_type_label;
	style.size = (int)(size * 0.56 + 0.5);
	style.weight = 600;
	PangoLayout *layout = draw_text_layout(cr, &style, letter, false, 0);
	PangoRectangle ink;
	pango_layout_get_pixel_extents(layout, &ink, NULL);
	draw_set_color(cr, palette->chip_ink);
	cairo_move_to(cr, x + (size - ink.width) / 2.0 - ink.x, y + (size - ink.height) / 2.0 - ink.y);
	pango_cairo_show_layout(cr, layout);
	g_object_unref(layout);
}

void app_icon_draw(cairo_t *cr, const char *app_id, const char *label,
		double x, double y, double size, const struct gride_palette *palette) {
	struct app_info *info = app_info_get(app_id);
	if (info && info->svg) {
		RsvgRectangle viewport = { x, y, size, size };
		if (rsvg_handle_render_document(info->svg, cr, &viewport, NULL)) {
			return;
		}
	}
	if (info && info->png) {
		int w = cairo_image_surface_get_width(info->png);
		int h = cairo_image_surface_get_height(info->png);
		if (w > 0 && h > 0) {
			cairo_save(cr);
			cairo_translate(cr, x, y);
			cairo_scale(cr, size / w, size / h);
			cairo_set_source_surface(cr, info->png, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
			return;
		}
	}
	draw_letter_tile(cr, label && label[0] ? label : app_id, x, y, size, palette);
}

void icons_finish(void) {
	if (!app_cache_ready) {
		return;
	}
	struct app_info *info, *tmp;
	wl_list_for_each_safe(info, tmp, &app_cache, link) {
		wl_list_remove(&info->link);
		if (info->svg) {
			g_object_unref(info->svg);
		}
		if (info->png) {
			cairo_surface_destroy(info->png);
		}
		free(info->app_id);
		free(info->name);
		free(info);
	}
}
