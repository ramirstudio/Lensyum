# Lensyum

Lensyum is an optical lens blur plug-in for After Effects. It traces rays through real lens prescriptions, so the shape of the bokeh is not a drawn disc: cat-eye toward the corners, soap-bubble edges, chromatic fringes and the difference between foreground and background all follow from the lens you pick.

## How it works

The project has two parts. `core/` is the engine, plain C++17 with no dependencies, buildable on any platform. `ae/` is the SmartFX wrapper for After Effects (8, 16 and 32 bpc, Multi-Frame Rendering). `ARCHITECTURE.md` describes the internals.

The engine works in three steps. First it scales the prescription to the requested focal length, measures the real maximum aperture and focuses the sensor at the chosen distance (`Lens.cpp`). Then, for a series of positions along the frame radius, it traces tens to hundreds of thousands of rays at six wavelengths and records where they land on the sensor at different levels of defocus, in front of and behind focus (`PsfAtlas.cpp`). For every point the atlas keeps the light that arrives, mechanical vignetting included, and which part of the pupil it came from. The iris (blades, custom aperture, central obstruction, rings, glass texture) is applied at render time in its fixed orientation: an asymmetric shape stays upright across the frame and flips between background and foreground, as it does on a real lens. Finally the renderer (`Renderer.cpp`) splits the image into depth slices, spreads the PSF that matches each pixel's position and blur, and composites the slices from back to front, so a blurred foreground correctly covers what is behind it. For large blurs pixels are aggregated at reduced resolution; highlights stay at full resolution up to the quality threshold, so the edges of bokeh discs stay sharp.

The blur can be uniform (in pixels) or computed from a depth map. In that case each pixel's blur radius comes from the real lens: focal length, aperture, focus distance and sensor size.

## Controls

Camera: lens preset (each preset has its own focal length), sensor format (full frame, Super 35, APS-C, Micro Four Thirds, Super 16, Large Format 65 or a custom width) and F-stop. An aperture wider than the lens maximum is clamped to the maximum.

Focus: the source of the blur is Whole Frame, Depth Map, Focus Region or AI Depth. Focus Region keeps a sharp area around the Focus Point: Region Radius sets its size, Region Falloff how gradual the transition is, Region Aspect its shape, Keep Center Untouched leaves the middle exactly as the source, and Defocus Amount decides how much to blur the rest. The depth layer has polarity and encoding (linear distance or 1/z disparity, the usual format of AI-generated maps), near and far distances in metres, a focus distance or focus on a point sampled from the map, and a blur scale. Depth Edge Clean-up (in pixels, 0 turns it off) removes the sharp outline that appears along the edge between a near and a far subject when the plane of focus sits between them: edge pixels take the blur of the nearer surface. At the edges of the layer the picture is continued by reflection, so bokeh discs neither fade nor snap back to sharp near the borders; the result is still cropped to the layer.

AI Depth (auto) estimates depth directly from the clip with a Depth Anything V2 network running locally on the GPU (DirectML), frame by frame, with no depth layer needed. Depth Detail sets the analysis resolution (Low 392, Medium 518, High 770, Ultra 1022 pixels on the long side), Edge Refine snaps the map to the edges of the image, and Use GPU falls back to the CPU when switched off. To shape the map: Focus On Focus Point focuses the depth under the Focus Point (otherwise Focus Distance applies), Sharp Range keeps a band of depths around focus sharp, Far Cut and Near Cut clip the ends of the map, Depth Contrast and Depth Shift redistribute it, Depth Smooth softens it and Invert Depth flips it. The map is normalised on every frame, so shots with strong movement can flicker slightly.

Two switches under Defocus Source help while you work. Show Depth Map shows the map the render uses (white is near, black is far). Show 3D Focus View shows it as a relief you can rotate with Orbit and Tilt (Relief sets the height): points that stay sharp are green, the green outline is the plane of focus, and the yellow disc is the Focus Point on the depth axis. Both work with Depth Map and AI Depth. The View menu also offers Blur Map, Bokeh Grid and Focus Overlay, which tints the frame orange behind the plane of focus and blue in front of it.

Aperture: shape (iris blades, heart, star, triangle, diamond, cross, ring or crescent), blade count, curvature, rotation, central obstruction (donut bokeh of mirror lenses) and a layer to use as a custom aperture.

Creative: Impression (positive gives a sharp bright edge, negative a soft disc), Coma, Astigmatism in millimetres of focus shift between the radial and tangential direction at the corner, Field Curvature in millimetres (corners go out of focus), Zonal Ripple with density (concentric rings), Lobes with count, shape, angle and orientation toward the centre, optional chromatic aberration (Bokeh Fringing for coloured fringes on the discs, Lateral CA for the red/blue shift toward the edges that also shows on sharp areas), Cat-Eye (mechanical vignetting), anamorphic squeeze, Bokeh Imperfections (dust and grain inside the discs) with scale and seed, Lens Coverage and Filmback Offset.

Layers: Rain Layer (a drop map: out-of-focus drops show up inside every bokeh disc, in-focus drops bend the image) with distance, strength and refraction, and Shimmer, sparkles inside the discs that change on every frame, with density and seed.

Highlights: threshold and boost of the highlights before blurring, to recover the brightness that 8/16 bit footage lost to clipping.

Render: quality, maximum blur radius, number of depth slices, working space (Auto decodes sRGB at 8/16 bpc and treats 32 bpc as linear), optical centre and view (Result, Blur Map, Bokeh Grid, Depth Map, Focus Overlay).

## Lenses

Double-Gauss 50 f/2 and Wide 22 f/2.8 are published prescriptions (Tronnier patent and Nakamura design, as tabulated in Smith, "Modern Lens Design"). Double-Gauss 58 f/2.2 Swirl uses the same glass with the rear group stopped down by the barrel, for stronger cat-eye and swirl. Cooke Triplet 50 f/2.8, Tessar 50 f/3.5 and Petzval 85 f/2.2 are Lensyum designs in the classic forms: catalogue glass, curvatures optimised on real rays for the full frame field, useful diameters taken from the traced beams. The Wide 22 covers Super 35 and APS-C; on full frame the corners fall off sharply.

## Install

The `Lensyum-1.0-win64.zip` package contains the plug-in, ONNX Runtime, DirectML and the depth model. Extract it into `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\Lensyum\` with After Effects closed; the same steps are in `INSTALL.txt` inside the zip. Licenses of the bundled components are in `THIRD_PARTY.md`.

## Build on Windows

You need Visual Studio 2022 or later with the "Desktop development with C++" workload, CMake 3.20 or later (the one bundled with Visual Studio works) and the After Effects SDK from the Adobe Developer Console, extracted for example to `C:\SDK\AfterEffectsSDK` (it must contain `Examples\Headers`). For AI Depth you also need the NuGet package `Microsoft.ML.OnnxRuntime.DirectML` extracted to `C:\SDK\ort` and `Microsoft.AI.DirectML` extracted to `C:\SDK\dml` (`.nupkg` files are zip archives). CMake finds `C:\SDK\ort` by itself; otherwise pass `-DORT_INCLUDE_DIR`.

From "x64 Native Tools Command Prompt for VS", in the project folder:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAE_SDK_DIR="C:/SDK/AfterEffectsSDK"
cmake --build build --target Lensyum
```

The result is `build\ae\Lensyum.aex`. `package.bat` rebuilds and creates `dist\Lensyum-1.0-win64.zip` with the plug-in, the DLLs and the model. The model (`model.onnx` from `huggingface.co/onnx-community/depth-anything-v2-small`, renamed `lensyum_depth.onnx`) is taken from the `MODEL` variable, or from the MediaCore folder if it is already installed. The banner at the top of the panel can be turned off with `-DLENSYUM_BANNER=OFF`.

If AI Depth cannot find the runtime or the model, the frame turns solid red and the reason is written to `%TEMP%\lensyum_log.txt`.

## Test the engine without After Effects

```
cmake -S . -B build
cmake --build build --config Release --target lensyum_cli
build/lensyum_cli lenses
build/lensyum_cli grid grid.ppm amount=40 blades=7 curv=0.3
build/lensyum_cli scene scene.ppm focus=3 N=2 gain=8
build/lensyum_cli image photo.ppm out.ppm depth=depth.pgm focus=2.5
```

Options are `key=value` pairs (`lens`, `N`, `focus` in metres, `sensor`, `blades`, `curv`, `rot`, `obst`, `mask=file.pgm`, `onion`, `lobes`, `lobecount`, `texture`, `cateye`, `ca`, `imp`, `coma`, `astig` and `fc` in mm, `squeeze`, `gain`, `thr`, `amount`, `quality`, `maxblur`, `layers`, `view`, `pad` to emulate After Effects' transparent padding; for AI depth `aimodel=model.onnx`, `ort=onnxruntime library`, `aires`, `refine`).

## Status

The engine runs on the CPU, multi-threaded. At 1080p on 4 cores, tracing the lens takes about 0.6 s (only when the lens, aperture, format or the Creative controls that act on rays change) and rendering a frame takes 0.3 to 0.8 s. AI depth estimation runs on the GPU through DirectML. Known limits: Windows only; the AI map is normalised per frame and can flicker on shots with sharp scene changes; rendering is on the CPU, so the preview is not real time at high resolutions.

License: see LICENSE (all rights reserved). Third-party components keep the licenses listed in THIRD_PARTY.md.
