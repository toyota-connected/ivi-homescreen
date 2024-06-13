project(
	'libliftoff',
	'c',
	version: '0.6.0-dev',
	license: 'MIT',
	meson_version: '>=0.52.0',
	default_options: [
		'c_std=c11',
		'warning_level=3',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
	'-Wundef',
	'-Wmissing-prototypes',
	'-Walloca',
	'-Wdeclaration-after-statement',
	'-Wfloat-conversion',
	'-Wsign-conversion',

	'-Wno-missing-braces',
	'-Wno-unused-parameter',
]), language: 'c')

liftoff_inc = include_directories('include')

drm = dependency('libdrm', include_type: 'system', version: '>= 2.4.108')

liftoff_deps = [drm]

liftoff_lib = library(
	'liftoff',
	files(
		'alloc.c',
		'device.c',
		'layer.c',
		'list.c',
		'log.c',
		'output.c',
		'plane.c',
	),
	include_directories: liftoff_inc,
	version: meson.project_version().split('-')[0],
	dependencies: liftoff_deps,
	install: true,
)

liftoff = declare_dependency(
	link_with: liftoff_lib,
	include_directories: liftoff_inc,
	dependencies: liftoff_deps,
)

if meson.version().version_compare('>= 0.54.0')
	meson.override_dependency('libliftoff', liftoff)
endif

install_headers('include/libliftoff.h')

pkgconfig = import('pkgconfig')
pkgconfig.generate(
	liftoff_lib,
	filebase: meson.project_name(),
	name: meson.project_name(),
	description: 'KMS plane library',
	url: 'https://gitlab.freedesktop.org/emersion/libliftoff',
)

subdir('example')
subdir('test')
