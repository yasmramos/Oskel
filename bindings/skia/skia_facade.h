#pragma once

// The only hand-written C++ in the Skia bridge: a facade for the calls whose
// defaults or value arguments would otherwise need a second binding layer.
// Everything else is generated from skia.oskelbind.
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"

namespace skia_bridge {

inline sk_sp<SkSurface> makeRaster(int width, int height) {
  return SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
}

}  // namespace skia_bridge
