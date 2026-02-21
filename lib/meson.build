# SPDX-License-Identifier: CC0-1.0

libuvcgadget_sources = files([
  'configfs.c',
  'events.c',
  'jpg-source.c',
  'slideshow-source.c',
  'stream.c',
  'test-source.c',
  'timer.c',
  'uvc.c',
  'v4l2.c',
  'v4l2-source.c',
  'video-buffers.c',
  'video-source.c',
  'still-source.c',
  'capture.c',
  'dng-writer.cpp',
])

if libcamera.found()
  libuvcgadget_sources += files(['libcamera-source.cpp'])
endif

if libcamera.found() and libjpeg.found() and threads.found()
  libuvcgadget_sources += files(['mjpeg_encoder.cpp'])
endif

if get_option('hw_mjpeg')
    add_project_arguments('-DENABLE_HW_MJPEG', language : 'cpp')
endif

# Add libtiff dependency
libtiff = dependency('libtiff-4', required : true)

# Architecture-specific SIMD/NEON flags
common_cpp_args = ['-DHAVE_TIFF', '-DSTILL_CAPTURE', '-mcpu=cortex-a53', '-mtune=cortex-a53', '-O3', '-DNDEBUG', '-ftree-vectorize']

if host_machine.cpu_family() == 'arm'
  # 32-bit ARM: NEON is optional, must be explicitly enabled
  arch_cpp_args = common_cpp_args + ['-mfpu=neon-fp-armv8']
elif host_machine.cpu_family() == 'aarch64'
  # 64-bit ARM: NEON/Advanced SIMD is always enabled, no -mfpu needed
  arch_cpp_args = common_cpp_args
else
  # Other architectures: just use common flags
  arch_cpp_args = common_cpp_args
endif

libuvcgadget = shared_library('uvcgadget',
                              libuvcgadget_sources,
                              'dng-writer.cpp',
                              dependencies : [libcamera, libjpeg, threads, libtiff],
                              cpp_args : arch_cpp_args,
                              version : uvc_gadget_version,
                              install : true,
                              include_directories : [includes, config_includes])

libuvcgadget_dep = declare_dependency(sources : [
                                          uvcgadget_public_headers,
                                     ],
                                     include_directories : includes,
                                     link_with : libuvcgadget)

pkg_mod = import('pkgconfig')
pkg_mod.generate(libuvcgadget,
                 description : 'UVC Gadget support library',
                 subdirs : 'uvcgadget')

meson.override_dependency('uvcgadget', libuvcgadget_dep)
