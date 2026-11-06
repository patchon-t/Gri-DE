project(
	'gride',
	'c',
	version: '0.1.0',
	meson_version: '>=1.3.0',
	default_options: [
		'c_std=c11',
		'warning_level=2',
		'buildtype=debugoptimized',
	],
)

cc = meson.get_compiler('c')
add_project_arguments(
	'-DWLR_USE_UNSTABLE',
	'-D_POSIX_C_SOURCE=200809L',
	cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wno-missing-field-initializers',
		'-Wundef',
		'-Wvla',
	]),
	language: 'c',
)

# Log lines carry the source file; strip the build-relative prefix from it.
fs = import('fs')
relative_dir = fs.relative_to(meson.current_source_dir(), meson.global_build_root()) + '/'
add_project_arguments(
	cc.get_supported_arguments('-fmacro-prefix-map=@0@='.format(relative_dir)),
	language: 'c',
)

python = find_program('python3')

# gride-tokens.h is generated from the design tokens so the compositor
# never carries its own copy of a colour or size.
tokens_h = custom_target(
	'gride-tokens.h',
	input: files('gride-assets/tokens.json'),
	output: 'gride-tokens.h',
	command: [python, '-I', files('tools/gen-tokens-c.py'), '@INPUT@', '@OUTPUT@'],
)

subdir('wm')
