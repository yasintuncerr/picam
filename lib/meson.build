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

libuvcgadget = shared_library('uvcgadget',
                              libuvcgadget_sources,
                              'dng-writer.cpp',
                              dependencies : [libcamera, libjpeg, threads, libtiff],
                              cpp_args : ['-DHAVE_TIFF','-DSTILL_CAPTURE', '-mfpu=neon-fp-armv8', '-mcpu=cortex-a53', '-mtune=cortex-a53', '-O3', '-DNDEBUG', '-ftree-vectorize'],
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
