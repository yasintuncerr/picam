/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2020, Raspberry Pi Ltd
 * DNG writer
 * * Copyright (C) 2025 Yasin Tunçer
 * * Added Callback support for DNG writing
 * Added Write to Buffer support for DNG writing
 */

#pragma once

#ifdef HAVE_TIFF

#include <functional>
#include <memory> 
#include <vector>
#include <tiffio.h>

#include <libcamera/camera.h>
#include <libcamera/controls.h>
#include <libcamera/framebuffer.h>
#include <libcamera/stream.h>

extern "C" {
#include "still-source.h"
}


struct StillBufferDeleter {
    void operator()(still_buffer* b) const {
        if (b) {
            if (b->mem) {
                free(b->mem);
            }
            delete b;
        }
    }
};

using DngBufferPtr = std::shared_ptr<still_buffer>;
using DngReadyCallback = std::function<void(DngBufferPtr buffer)>;

class DNGWriter
{
public:
    DNGWriter() = default;
    ~DNGWriter() = default;
    
    DNGWriter(const DNGWriter&) = delete;
    DNGWriter& operator=(const DNGWriter&) = delete;
    
    void setCallback(DngReadyCallback callback) { dngReadyCallback_ = std::move(callback); }
    void clearCallback() { dngReadyCallback_ = nullptr; }
    
    bool writeToBuffer(const libcamera::Camera *camera,
                       const libcamera::StreamConfiguration &config,
                       const libcamera::ControlList &metadata,
                       const libcamera::FrameBuffer *buffer, const void *data);

    static int write(const char *filename, const libcamera::Camera *camera,
                     const libcamera::StreamConfiguration &config,
                     const libcamera::ControlList &metadata,
                     const libcamera::FrameBuffer *buffer, const void *data);
  	
private:
    DngReadyCallback dngReadyCallback_;
    
	int writeInternal(TIFF *tif, const libcamera::Camera *camera,
                      const libcamera::StreamConfiguration &config,
                      const libcamera::ControlList &metadata,
                      const void *data);

  };

#endif /* HAVE_TIFF */