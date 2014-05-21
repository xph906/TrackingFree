// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/compositor/software_output_device_ozone.h"
#include "third_party/skia/include/core/SkDevice.h"
#include "ui/compositor/compositor.h"
#include "ui/gfx/ozone/surface_factory_ozone.h"
#include "ui/gfx/ozone/surface_ozone_canvas.h"
#include "ui/gfx/skia_util.h"
#include "ui/gfx/vsync_provider.h"

namespace content {

SoftwareOutputDeviceOzone::SoftwareOutputDeviceOzone(ui::Compositor* compositor)
    : compositor_(compositor) {
  gfx::SurfaceFactoryOzone* factory = gfx::SurfaceFactoryOzone::GetInstance();

  if (factory->InitializeHardware() != gfx::SurfaceFactoryOzone::INITIALIZED)
    LOG(FATAL) << "Failed to initialize hardware in OZONE";

  surface_ozone_ = factory->CreateCanvasForWidget(compositor_->widget());

  if (!surface_ozone_)
    LOG(FATAL) << "Failed to initialize canvas";

  vsync_provider_ = surface_ozone_->CreateVSyncProvider();
}

SoftwareOutputDeviceOzone::~SoftwareOutputDeviceOzone() {
}

void SoftwareOutputDeviceOzone::Resize(const gfx::Size& viewport_size) {
  if (viewport_size_ == viewport_size)
    return;

  viewport_size_ = viewport_size;

  surface_ozone_->ResizeCanvas(viewport_size_);
}

SkCanvas* SoftwareOutputDeviceOzone::BeginPaint(const gfx::Rect& damage_rect) {
  DCHECK(gfx::Rect(viewport_size_).Contains(damage_rect));

  // Get canvas for next frame.
  canvas_ = surface_ozone_->GetCanvas();

  return SoftwareOutputDevice::BeginPaint(damage_rect);
}

void SoftwareOutputDeviceOzone::EndPaint(cc::SoftwareFrameData* frame_data) {
  SoftwareOutputDevice::EndPaint(frame_data);

  surface_ozone_->PresentCanvas(damage_rect_);
}

}  // namespace content
