wlroots = dependency('wlroots-0.18')
wayland_server = dependency('wayland-server')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
pixman = dependency('pixman-1')
libdrm = dependency('libdrm').partial_dependency(compile_args: true, includes: true)
math = cc.find_library('m', required: false)

subdir('protocols')

executable(
	'gride-wm',
	files(
		'src/main.c',
		'src/server.c',
		'src/output.c',
		'src/background.c',
		'src/view.c',
		'src/input.c',
		'src/cursor.c',
		'src/log.c',
		'src/spawn.c',
	),
	tokens_h,
	protocol_headers,
	include_directories: include_directories('src'),
	dependencies: [wlroots, wayland_server, xkbcommon, libinput, pixman, libdrm, math],
	install: true,
)
