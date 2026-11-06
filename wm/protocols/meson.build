# wlroots' public headers include the server side of a few protocols
# (xdg-shell for wlr_xdg_shell.h). wayland-scanner generates them here.
wayland_protocols = dependency('wayland-protocols', version: '>=1.32')
wl_protocol_dir = wayland_protocols.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

server_protocols = [
	wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
	wl_protocol_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
]

protocol_headers = []
foreach xml : server_protocols
	protocol_headers += custom_target(
		xml.underscorify() + '_server_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach
