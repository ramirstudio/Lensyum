#pragma once

#include <string>
#include <vector>

namespace lensyum {

// Monocular depth estimation with an ONNX model of the Depth Anything family (input: RGB image
// normalised with ImageNet statistics, sides multiple of 14; output: relative inverse depth).
// The ONNX Runtime library is loaded at run time from runtimeLib, so the plug-in has no link-time
// dependency and never collides with a runtime the host application may already have loaded.
struct DepthAIConfig {
    std::string runtimeLib; // path to onnxruntime shared library
    std::string modelPath;  // path to the .onnx model
    bool useGpu = true;     // DirectML on Windows when available
};

// Thread-safe. Returns false and fills err when the runtime or the model cannot be loaded.
bool depthAIInit(const DepthAIConfig& cfg, std::string& err);

// rgb: display-referred (sRGB) values 0..1, 3 floats per pixel, row 0 = top.
// inferLongSide: resolution of the network input along the long side (rounded to 14).
// out: w*h values, 0 = far, 1 = near (robustly normalised), edges refined against the image
// when refine is true.
bool depthAIEstimate(const float* rgb, int w, int h, int inferLongSide, bool refine,
                     std::vector<float>& out, std::string& err);

// Edge-aware refinement of a depth map with the image as guide (guided filter).
void refineDepth(const float* rgb, int w, int h, std::vector<float>& depth, int radius, float eps);

// Shapes an estimated depth map (0 = far, 1 = near) before it drives the blur.
//   farPoint/nearPoint: levels, values at or below farPoint become 0 and at or above nearPoint 1.
//   gamma: > 1 pushes the middle towards far, < 1 towards near. shift: moves all depths.
//   smoothRadius: box blur in map pixels. range: when focusU/focusV are inside 0..1, depths within
//   +-range of the depth under that point are flattened onto it, so a whole band stays sharp.
struct DepthAdjust {
    float farPoint = 0.0f, nearPoint = 1.0f, gamma = 1.0f, shift = 0.0f;
    bool invert = false;
    float smoothRadius = 0.0f;
    float range = 0.0f;
    float focusU = -1.0f, focusV = -1.0f;
};
void adjustDepth(std::vector<float>& depth, int w, int h, const DepthAdjust& a);

} // namespace lensyum
