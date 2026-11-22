#ifndef OSKEL_BINDINGS_SKIA_FACADE_H
#define OSKEL_BINDINGS_SKIA_FACADE_H

// The only hand-written C++ in the Skia bridge. Everything else is generated
// from skia.oskelbind. Keep this header free of state: the generator owns the
// thunks and the Oskel classes.

#include <cstddef>
#include <cstdint>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkColor.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMilestone.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"

namespace skia_bridge {

inline sk_sp<SkSurface> makeRaster(int width, int height) {
#if SK_MILESTONE >= 115
  return SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
#else
  return SkSurface::MakeRasterN32Premul(width, height);
#endif
}

inline void drawText(SkCanvas* canvas, const char* text, float x, float y,
                     const SkFont& font, const SkPaint& paint) {
  if (canvas == nullptr || text == nullptr) {
    return;
  }
  canvas->drawString(text, x, y, font, paint);
}

inline uint32_t readPixel(SkImage* image, int x, int y) {
  if (image == nullptr) {
    return 0;
  }
  uint32_t pixel = 0;
  const SkImageInfo info =
      SkImageInfo::Make(1, 1, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
  if (!image->readPixels(info, &pixel, sizeof(pixel), x, y)) {
    return 0;
  }
  return pixel;
}

inline bool readPixels(SkImage* image, void* dst, int rowBytes) {
  if (image == nullptr || dst == nullptr || rowBytes <= 0) {
    return false;
  }
  const SkImageInfo info = SkImageInfo::Make(
      image->width(), image->height(), kBGRA_8888_SkColorType,
      kUnpremul_SkAlphaType);
  return image->readPixels(info, dst, static_cast<size_t>(rowBytes), 0, 0);
}

}  // namespace skia_bridge

#endif  // OSKEL_BINDINGS_SKIA_FACADE_H
