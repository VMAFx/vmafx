# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# The native GStreamer element of VMAFx. A standalone project: it consumes the public C API of
# libvmafx (pkg-config), as the FFmpeg filter does, and is built apart from core/.
#
#   meson setup build-gst gstreamer
#   PKG_CONFIG_PATH=<libvmafx build>/meson-uninstalled meson setup build-gst gstreamer

project('gst-vmafx', 'c',
  version : '1.0.0',
  license : 'EUPL-1.2',
  meson_version : '>= 1.1',
  default_options : ['c_std=gnu11', 'warning_level=2', 'werror=true'])

cc = meson.get_compiler('c')
gst_dep = dependency('gstreamer-1.0', version : '>= 1.18')
gstbase_dep = dependency('gstreamer-base-1.0', version : '>= 1.18')
gstvideo_dep = dependency('gstreamer-video-1.0', version : '>= 1.18')
vmafx_dep = dependency('libvmafx')
gmodule_dep = dependency('gmodule-2.0')

deps = [gst_dep, gstbase_dep, gstvideo_dep, vmafx_dep, gmodule_dep]
args = [
  '-DGST_LICENSE_PLACEHOLDER="' + get_option('plugin_license') + '"',
  '-DPACKAGE="gst-vmafx"',
  '-DGST_USE_UNSTABLE_API',
]

# CUDAMemory input needs GStreamer's CUDA library and the CUDA headers it includes.
gstcuda_dep = dependency('gstreamer-cuda-1.0', version : '>= 1.22', required : get_option('cuda'))
cuda_inc = []
if gstcuda_dep.found()
  cuda_dirs = [get_option('toolkit_root'), '/opt/cuda', '/usr/local/cuda', '/usr']
  cuda_found = cc.has_header('cuda.h')
  foreach root : cuda_dirs
    if not cuda_found and root != '' and cc.has_header('cuda.h', args : '-I' + root / 'include')
      cuda_inc = include_directories(root / 'include')
      cuda_found = true
    endif
  endforeach
  if not cuda_found
    if get_option('cuda').enabled()
      error('feature cuda: cuda.h was not found; set -Dtoolkit_root')
    endif
    gstcuda_dep = dependency('', required : false)
  endif
endif
if gstcuda_dep.found()
  deps += gstcuda_dep
  args += '-DHAVE_GST_CUDA=1'
endif

sources = files(
  'src/gstvmafx.c',
  'src/gstvmafx_caps.c',
  'src/gstvmafx_cuda.c',
  'src/gstvmafx_frame.c',
  'src/gstvmafx_opts.c',
  'src/gstvmafx_out.c',
  'src/gstvmafx_run.c',
  'src/gstvmafx_util.c',
  'src/gstvmafx_windows.c',
)

# One option table for the FFmpeg filter and this element (generated, never edited here).
opt_inc = include_directories('../ffmpeg-patches/src')

plugin = shared_library('gstvmafx', sources,
  c_args : args,
  include_directories : [opt_inc, cuda_inc],
  dependencies : deps,
  install : true,
  install_dir : gst_dep.get_variable('pluginsdir', default_value : get_option('libdir') / 'gstreamer-1.0'))

message('plugin directory for tests:', meson.current_build_dir())
