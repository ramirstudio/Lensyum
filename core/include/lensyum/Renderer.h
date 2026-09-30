#pragma once

#include "PsfAtlas.h"

#include <vector>

namespace lensyum {

// Linear-light, premultiplied RGBA float image.
struct Image {
    int width = 0, height = 0;
    std::vector<float> rgba;
    void resize(int w, int h) { width = w; height = h; rgba.assign(static_cast<size_t>(w) * h * 4, 0.0f); }
    float* px(int x, int y) { return rgba.data() + (static_cast<size_t>(y) * width + x) * 4; }
    const float* px(int x, int y) const { return rgba.data() + (static_cast<size_t>(y) * width + x) * 4; }
};

struct DefocusSettings {
    enum Mode { kUniform = 0, kDepthMap = 1, kRegion = 2 };
    int mode = kUniform;

    // Uniform: signed blur radius in full-resolution pixels (+ background look, - foreground look).
    double amountPx = 20.0;

    // Depth map, values 0..1, sampled stretched to fit the frame. Row 0 = top.
    const float* depth = nullptr;
    int depthW = 0, depthH = 0;
    bool whiteIsNear = true;
    bool inverseDepth = false; // map stores disparity (1/z) instead of distance
    double nearMm = 500.0, farMm = 20000.0;
    double focusMm = 3000.0;
    bool focusFromPoint = false;
    // Focus region: sharp inside an ellipse around the focus point, blur (amountPx) grows
    // outside it over regionFalloffPx. Sizes in full-resolution pixels.
    double regionRadiusPx = 300.0, regionFalloffPx = 400.0, regionAspect = 1.0;
    double focusPointX = 0, focusPointY = 0; // full-resolution layer pixels

    double scale = 1.0; // artistic multiplier on the computed blur
};

struct HighlightSettings {
    double threshold = 0.85; // luminance where highlights start
    double gain = 0.0;       // extra brightness pushed into highlights before defocus
};

// Where the buffer sits inside the layer, for field position and downsampling.
struct FrameMapping {
    double originX = 0, originY = 0;         // layer coords (current resolution) of buffer pixel (0,0)
    double downsampleX = 1, downsampleY = 1; // buffer pixels per full-resolution pixel
    double layerW = 1920, layerH = 1080;     // full-resolution layer size
    double centerX = 960, centerY = 540;     // optical centre, full-resolution layer pixels
};

struct RenderSettings {
    OpticsSettings optics;
    DefocusSettings defocus;
    HighlightSettings highlights;
    FrameMapping frame;
    double squeeze = 1.0; // anamorphic squeeze: bokeh becomes an upright oval
    double fieldCurvatureMm = 0.0; // focus shift at the frame corner, mm of sensor travel
    double filmbackOffsetMm = 0.0; // sensor moved along the axis: shifts focus everywhere
    double blendBack = 1.0;        // 1 = full effect, 0 = original
    double lateralCaPx = 0.0;      // lateral chromatic aberration: red/blue shift at the corner, px
    int layers = 12;      // depth slices for occlusion
    enum View { kResult = 0, kBlurMap = 1, kBokehGrid = 2 };
    int view = kResult;
};

void renderDefocus(const Image& src, Image& dst, const RenderSettings& rs);

} // namespace lensyum
