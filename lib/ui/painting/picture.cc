// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/painting/picture.h"

#include "flutter/fml/make_copyable.h"
#include "flutter/lib/ui/painting/canvas.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_args.h"
#include "third_party/tonic/dart_binding_macros.h"
#include "third_party/tonic/dart_library_natives.h"
#include "third_party/tonic/dart_persistent_value.h"
#include "third_party/tonic/logging/dart_invoke.h"

namespace blink {

IMPLEMENT_WRAPPERTYPEINFO(ui, Picture);

#define FOR_EACH_BINDING(V) \
  V(Picture, toImage)       \
  V(Picture, dispose)       \
  V(Picture, GetAllocationSize)

DART_BIND_ALL(Picture, FOR_EACH_BINDING)

fml::RefPtr<Picture> Picture::Create(flow::SkiaGPUObject<SkPicture> picture) {
  return fml::MakeRefCounted<Picture>(std::move(picture));
}

Picture::Picture(flow::SkiaGPUObject<SkPicture> picture)
    : picture_(std::move(picture)) {}

Picture::~Picture() = default;

Dart_Handle Picture::toImage(uint32_t width,
                             uint32_t height,
                             Dart_Handle raw_image_callback) {
  if (!picture_.get()) {
    return tonic::ToDart("Picture is null");
  }

  return RasterizeToImage(picture_.get(), width, height, raw_image_callback);
}

void Picture::dispose() {
  ClearDartWrapper();
}

size_t Picture::GetAllocationSize() {
  if (auto picture = picture_.get()) {
    return picture->approximateBytesUsed();
  } else {
    return sizeof(Picture);
  }
}

Dart_Handle Picture::RasterizeToImage(sk_sp<SkPicture> picture,
                                      uint32_t width,
                                      uint32_t height,
                                      Dart_Handle raw_image_callback) {
  if (Dart_IsNull(raw_image_callback) || !Dart_IsClosure(raw_image_callback)) {
    return tonic::ToDart("Image callback was invalid");
  }

  if (width == 0 || height == 0) {
    return tonic::ToDart("Image dimensions for scene were invalid.");
  }

  auto* dart_state = UIDartState::Current();
  auto image_callback = std::make_unique<tonic::DartPersistentValue>(
      dart_state, raw_image_callback);
  auto unref_queue = dart_state->GetSkiaUnrefQueue();
  auto ui_task_runner = dart_state->GetTaskRunners().GetUITaskRunner();
  auto gpu_task_runner = dart_state->GetTaskRunners().GetGPUTaskRunner();
  auto snapshot_delegate = dart_state->GetSnapshotDelegate();

  // We can't create an image on this task runner because we don't have a
  // graphics context. Even if we did, it would be slow anyway. Also, this
  // thread owns the sole reference to the layer tree. So we flatten the layer
  // tree into a picture and use that as the thread transport mechanism.

  auto picture_bounds = SkISize::Make(width, height);

  auto ui_task = fml::MakeCopyable(
      [ui_task_runner, image_callback = std::move(image_callback),
       unref_queue](sk_sp<SkImage> raster_image) mutable {
        // Send the raster image back to the UI thread for submission to the
        // framework.
        fml::TaskRunner::RunNowOrPostTask(
            ui_task_runner,
            fml::MakeCopyable([raster_image,
                               image_callback = std::move(image_callback),
                               unref_queue]() mutable {
              auto dart_state = image_callback->dart_state().lock();
              if (!dart_state) {
                // The root isolate could have died in the meantime.
                return;
              }
              tonic::DartState::Scope scope(dart_state);

              if (!raster_image) {
                tonic::DartInvoke(image_callback->Get(), {Dart_Null()});
                return;
              }

              auto dart_image = CanvasImage::Create();
              dart_image->set_image(
                  {std::move(raster_image), std::move(unref_queue)});
              auto* raw_dart_image = tonic::ToDart(std::move(dart_image));

              // All done!
              tonic::DartInvoke(image_callback->Get(), {raw_dart_image});
            }));
      });

  auto gpu_task = fml::MakeCopyable([gpu_task_runner, picture, picture_bounds,
                                     snapshot_delegate, ui_task]() {
    fml::TaskRunner::RunNowOrPostTask(gpu_task_runner, [snapshot_delegate,
                                                        picture, picture_bounds,
                                                        ui_task]() {
      // Snapshot the picture on the GPU thread. This thread has access to the
      // GPU contexts that may contain the sole references to texture backed
      // images in the picture.
      ui_task(snapshot_delegate->MakeRasterSnapshot(picture, picture_bounds));
    });
  });

  // Kick things off on the GPU.
  gpu_task();

  return Dart_Null();
}

}  // namespace blink