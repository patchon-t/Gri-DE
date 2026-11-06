wlroots = dependency('wlroots-0.18')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
pixman = dependency('pixman-1')
cairo = dependency('cairo')
pangocairo = dependency('pangocairo')
fontconfig = dependency('fontconfig')
librsvg = dependency('librsvg-2.0', version: '>=2.52')
xcb = dependency('xcb')
xcb_icccm = dependency('xcb-icccm')
jsonc = dependency('json-c')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)
math = cc.find_library('m', required: false)

if wlroots.get_variable('have_xwayland', default_value: 'true') != 'true'
	error('GriDE needs wlroots built with XWayland support')
endif

subdir('protocols')

gride_fontdir = get_option('prefix') / get_option('datadir') / 'fonts' / 'gride'

executable(
	'gride-wm',
	files(
		'src/main.c',
		'src/server.c',
		'src/output.c',
		'src/background.c',
		'src/view.c',
		'src/xdg.c',
		'src/xwayland.c',
		'src/deco.c',
		'src/grid.c',
		'src/workspace.c',
		'src/flyout.c',
		'src/ipc.c',
		'src/draw.c',
		'src/icons.c',
		'src/input.c',
		'src/cursor.c',
		'src/log.c',
		'src/spawn.c',
	),
	tokens_h,
	protocol_headers,
	c_args: [
		'-DGRIDE_FONTDIR="@0@"'.format(gride_fontdir),
		'-DGRIDE_SOURCE_FONTDIR="@0@"'.format(meson.project_source_root() / 'fonts' / 'rubik'),
	],
	include_directories: include_directories('src'),
	dependencies: [
		wlroots, wayland_server, xkbcommon, libinput, pixman, cairo, pangocairo,
		fontconfig, librsvg, xcb, xcb_icccm, jsonc, libdrm, math,
	],
	install: true,
)

install_data(
	'../fonts/rubik/Rubik-Variable.ttf',
	'../fonts/rubik/OFL.txt',
	install_dir: gride_fontdir,
)
