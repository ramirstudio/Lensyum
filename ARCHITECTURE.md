# Lensyum architecture

Lensyum blurs a frame by simulating the bokeh of a real lens. The engine is a C++17 library with no dependencies (`core/`), used by an After Effects SmartFX wrapper (`ae/`) and by a test CLI (`tools/lensyum_cli.cpp`). It runs on the CPU with its own threads; the GPU is used only for the AI depth estimate.

## Optics

`Lens.cpp` traces real rays through lens prescriptions (spherical surfaces, glass from nd/Abbe with Cauchy dispersion). From each prescription it derives the effective focal length, the measured f-number, the paraxial focus for a given distance and the mechanical vignetting, that is the cat-eye. `Aperture.cpp` describes the aperture as a transmission function over the pupil: blades, curvature, special shapes, soft central obstruction, lobes, custom mask and an imperfection texture.

`PsfAtlas.cpp` precomputes, for each field radius and each signed defocus, the pupil-to-image mapping. Each texel holds 5 floats: RGB light and the light-weighted pupil position. The atlas has mips and is cached, and the cache key quantises focus to 24 steps per decade. The iris is not in the atlas: it is applied at render time in the lens frame, so a custom shape stays upright in the background bokeh and rotates only with the cat-eye. Impression, coma, astigmatism, field curvature and chromatic aberration enter the tracing or the defocus map. Where the lens's own spot is larger than the geometric blur (an aberrated lens at small defocus, or a small sensor with large pixels) the PSF would be cut off by the square texture frame and render as a square; each entry then stores a `unit` that widens the frame, and the renderer scales the footprint by it.

## Render

`Renderer.cpp` assigns every pixel a signed blur radius `s` (positive in the background, negative in the foreground) from the chosen source: uniform, a focus region with falloff, or a depth map converted through a LUT built with the real lens.

Pixels are scattered, not gathered. Depth is divided into slices with dithered assignment, composited back to front with a coverage channel. Above a radius threshold pixels are aggregated into 2^L blocks (up to L=5) and the blocks are scattered instead of single pixels; highlights stay at full resolution. Each splat reads its kernel from the atlas: with a round iris the kernels are baked once per render, otherwise the iris is applied per pixel. Normalisation uses the kernel mass, measured for 32 orientations when the shape is not round, and is exact over the whole disc (off-frame included) for small or thin footprints. Where coverage falls below 0.3 the source is blended in.

After the slices: rain refraction, lateral chromatic aberration, the untouched centre of the focus region and Blend Back. The check views (Blur Map, Bokeh Grid, Depth Map, Focus Overlay, 3D) are branches of the same renderer.

The maximum blur radius is known in advance, so `renderDefocus` extends the layer past its edges by reflection by that amount, renders only the discs that reach the frame and crops. Without it the margins would lose light from one side. A filter on the blur field removes the sharp outlines that appear where depth crosses the plane of focus.

## AI depth

`DepthAI.cpp` loads ONNX Runtime at run time (`LoadLibraryExW`, `OrtGetApiBase`, DirectML provider with CPU fallback), so there is no link-time dependency and no clash with the runtime of other applications. The model is Depth Anything V2 Small: RGB input normalised with ImageNet statistics at sides that are multiples of 14, relative disparity output. The map is normalised between the 1st and 99th percentile, resampled to the layer resolution and refined with a guided filter. There is an LRU cache of 6 entries. `adjustDepth` applies levels, gamma, shift, smoothing, inversion and Sharp Range, which flattens a band around the depth of the Focus Point. Normalisation is per frame, with no temporal coherence.

## After Effects wrapper

`LensyumAE.cpp` implements SmartFX (`PreRender`, `SmartRender`) at 8, 16 and 32 bit, with the multi-frame rendering flags. Parameters have indices in panel order (`LensyumParams.h`) and stable disk IDs, always appended at the end. Point values arrive at the current downsample and are converted back to layer pixels. The requested padding follows the effective blur. At the top of the panel there is a no-data parameter (`P_BANNER`, flag `PF_PUI_CONTROL`) drawn with Drawbot by the `PF_Cmd_EVENT` handler: the picture is a raw BGRA block embedded as a resource (`LensyumBanner.bin`) and fits the width of the row; for this reason the PiPL also declares `PF_OutFlag_CUSTOM_UI`. The PiPL is pre-generated in `LensyumPiPL.rc` (PiPLtool produced empty resources in the build) and must be kept consistent with the flags in `GlobalSetup`.

In AI Depth mode the estimate runs on the layer area before the render. If the runtime or the model is missing or fails, the frame comes out solid red and the reason is written to `%TEMP%\lensyum_log.txt`. The plug-in looks for `lensyum_ort.dll` and `lensyum_depth.onnx` next to the `.aex`.

## Build

CMake builds `lensyum_core`, `lensyum_cli` and, with `AE_SDK_DIR`, the `.aex` (MSVC/Windows only). `ORT_INCLUDE_DIR` enables AI depth; without it the ONNX functions are stubs that return an error. The CLI takes almost every parameter as `key=value`, and the `pad=N` option emulates After Effects' transparent padding.

## Known limits

Windows only. The atlas is per field radius, not per pixel, and the field is interpolated with dithering. The AI estimate has no temporal coherence. The 3D relief is a point cloud and shows holes on steep walls.
