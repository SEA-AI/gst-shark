# Library source files
libgst_shark_src_files = [
  'gstdot.c',
  'gstcpuusagecompute.c',
  'gstproctimecompute.c',
  'gstctf.c',
  'gstparser.c',
  'gstsharktracer.c',
  'gstperiodictracer.c'
]

libgst_shark_c_args = [gst_c_args,
                      '-DGST_USE_UNSTABLE_API',   # Used for usage of GstTracerRecord, GstTracer functions
                      '-Wno-undef']

# Create both libraries.
gst_shark_lib = both_libraries('gstshark',
  libgst_shark_src_files,
  c_args : libgst_shark_c_args,
  version : gst_shark_version_short,
  include_directories : configinc,
  dependencies : [gst_shark_deps],
  install : true,
  install_dir : lib_install_dir,
)

# Define the library as an internal dependency to the tracers library
lib_gst_shark_dep = declare_dependency(link_with: gst_shark_lib, dependencies : [gst_shark_deps])

install_headers(
  [
    'gstsharktracer.h',
    'gstperiodictracer.h',
  ],
  subdir : join_paths('gst', 'shark')
)

pkgconfig = import('pkgconfig')

pkgconfig.generate(
  libraries : gst_shark_lib,
  version : gst_shark_version_short,
  name : 'gstshark',
  filebase : 'gstshark',
  description : 'GstShark tracing core library',
  subdirs : 'gst',
  requires : ['gstreamer-1.0', 'gstreamer-base-1.0'],
  variables : {
    'exec_prefix' : '${prefix}',
  }
)

# Tracers
libgst_shark_tracers_src_files = [
  'gstplugin.c',
  'gstgraphic.c',
  'gstcpuusage.c',
  'gstproctime.c',
  'gstinterlatency.c',
  'gstscheduletime.c',
  'gstframerate.c',
  'gstqueuelevel.c',
  'gstbitrate.c',
  'gstbuffer.c',
  'gstdetectioncount.c',
]

gst_shark_tracers_plugins = both_libraries('gstsharktracers',
  libgst_shark_tracers_src_files,
  c_args : libgst_shark_c_args,
  version : gst_shark_version_short,
  include_directories : configinc,
  dependencies : [gst_shark_tracers_deps, lib_gst_shark_dep, nvds_tracer_dep],
  install : true,
  install_dir : plugins_install_dir,
)

plugins = [gst_shark_tracers_plugins]
