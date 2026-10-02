# Componenti di terze parti

Il codice di Lensyum non contiene codice di terze parti. Il pacchetto installabile include, accanto al plugin, i componenti seguenti, ognuno con la propria licenza:

| File | Componente | Licenza |
|---|---|---|
| `lensyum_ort.dll`, `onnxruntime_providers_shared.dll` | ONNX Runtime (Microsoft), pacchetto `Microsoft.ML.OnnxRuntime.DirectML` | MIT |
| `DirectML.dll` | DirectML (Microsoft), pacchetto `Microsoft.AI.DirectML` | Microsoft DirectML License, ridistribuibile con l'applicazione |
| `lensyum_depth.onnx` | Depth Anything V2 Small, esportazione ONNX `onnx-community/depth-anything-v2-small` | Apache 2.0 |

Solo la variante Small di Depth Anything V2 è sotto Apache 2.0; le varianti Base, Large e Giant hanno licenza non commerciale e non vanno usate al suo posto in una distribuzione.

L'After Effects SDK serve solo per compilare e non viene ridistribuito. Le prescrizioni ottiche Double-Gauss 50 e Wide 22 sono dati pubblicati (Smith, "Modern Lens Design"); le altre sono progetti originali.
