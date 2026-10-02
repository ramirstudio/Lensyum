# Third-party components

Lensyum's own code contains no third-party code. The installable package ships the following components next to the plug-in, each under its own license:

| File | Component | License |
|---|---|---|
| `lensyum_ort.dll`, `onnxruntime_providers_shared.dll` | ONNX Runtime (Microsoft), package `Microsoft.ML.OnnxRuntime.DirectML` | MIT |
| `DirectML.dll` | DirectML (Microsoft), package `Microsoft.AI.DirectML` | Microsoft DirectML License, redistributable with the application |
| `lensyum_depth.onnx` | Depth Anything V2 Small, ONNX export `onnx-community/depth-anything-v2-small` | Apache 2.0 |

The full license texts and notices are in the `licenses` folder of the package: `ONNXRUNTIME-LICENSE.txt` and `ONNXRUNTIME-ThirdPartyNotices.txt` (ONNX Runtime and the components built into it), `DEPTH-ANYTHING-V2-LICENSE.txt` (Apache 2.0 text with the authors' attribution) and the DirectML license copied from its package. Keep that folder in any copy you redistribute.

Only the Small variant of Depth Anything V2 is under Apache 2.0; the Base, Large and Giant variants have a non-commercial license and must not be used in its place in a distribution.

The After Effects SDK is needed only to build and is not redistributed. The Double-Gauss 50 and Wide 22 optical prescriptions are published data (Smith, "Modern Lens Design"); the others are original designs.
