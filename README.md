# Lensyum

Lensyum è un effetto di sfocatura ottica per After Effects basato sul ray tracing di prescrizioni di obiettivi reali. La forma del bokeh non è un disco disegnato: nasce tracciando i raggi attraverso le superfici del vetro, per cui cat-eye ai bordi, bordo a bolla di sapone, frange cromatiche e differenze tra primo piano e sfondo sono una conseguenza della lente scelta.

## Come funziona

Il progetto ha due parti. `core/` è il motore in C++17, senza dipendenze, compilabile su qualunque piattaforma. `ae/` è il wrapper SmartFX per After Effects (8, 16 e 32 bpc, Multi-Frame Rendering).

Il motore lavora in tre passaggi. Per prima cosa scala la prescrizione alla focale richiesta, misura l'apertura massima reale e mette a fuoco il sensore alla distanza scelta (`Lens.cpp`). Poi, per una serie di posizioni lungo il raggio del fotogramma, traccia da decine a centinaia di migliaia di raggi in sei lunghezze d'onda e registra dove arrivano sul piano del sensore a diversi livelli di sfocatura, davanti e dietro il fuoco (`PsfAtlas.cpp`). Per ogni punto l'atlante conserva la luce che arriva, vignettatura meccanica compresa, e da quale punto della pupilla proviene. L'iride (lamelle, apertura personalizzata, ostruzione centrale, anelli, texture del vetro) viene applicata al momento del render nella sua orientazione fissa: una forma asimmetrica resta dritta in tutto il fotogramma e si capovolge tra sfondo e primo piano, come succede con un obiettivo vero. Infine il renderer (`Renderer.cpp`) divide l'immagine in fette di profondità, proietta su ogni pixel la PSF corrispondente alla sua posizione e alla sua sfocatura, e ricompone le fette dal fondo verso il davanti, così un primo piano sfocato copre correttamente quello che sta dietro. Per le sfocature grandi i pixel vengono aggregati a risoluzione ridotta; le alte luci restano a piena risoluzione fino alla soglia di qualità, così il bordo dei bokeh resta netto.

La sfocatura può essere uniforme (in pixel) oppure calcolata da una mappa di profondità: in quel caso il raggio di sfocatura di ogni pixel deriva dalla lente vera, cioè da focale, diaframma, distanza di messa a fuoco e dimensione del sensore.

## Controlli

Camera: preset dell'obiettivo (ogni preset ha la sua focale), formato del sensore (full frame, Super 35, APS-C, Micro 4/3, Super 16, Large Format 65 o larghezza personalizzata), diaframma. Se chiedi un diaframma più aperto di quello massimo della lente, viene usato il massimo.

Focus: sorgente della sfocatura (uniforme, mappa di profondità oppure Focus Region, dove scegli il punto nitido con Focus Point, quanto è grande la zona nitida con Region Radius, quanto è graduale il passaggio con Region Falloff e la forma con Region Aspect, mentre Defocus Amount decide quanto sfocare il resto), layer di profondità con polarità e codifica (distanza lineare o disparità 1/z, il formato tipico delle mappe generate da AI), distanze near/far in metri, distanza di messa a fuoco oppure messa a fuoco su un punto campionato dalla mappa, scala della sfocatura. Depth Edge Clean-up (in pixel, 0 lo spegne) toglie il contorno nitido che compare sui bordi tra un soggetto vicino e uno lontano quando il piano di fuoco sta nel mezzo: i pixel di bordo prendono la sfocatura della superficie più vicina. Ai bordi del livello la scena viene continuata per riflessione, quindi i dischi di bokeh non sbiadiscono né tornano nitidi vicino ai margini; il risultato resta comunque ritagliato ai confini del livello.

AI Depth (auto) stima la profondità direttamente dalla clip con una rete Depth Anything V2 eseguita in locale su GPU (DirectML), fotogramma per fotogramma, senza bisogno di un layer di profondità. Depth Detail sceglie la risoluzione di analisi (Low 392, Medium 518, High 770, Ultra 1022 pixel sul lato lungo), Edge Refine ammorbidisce la mappa seguendo i bordi dell'immagine, Use GPU passa alla CPU se disattivato. La vista Depth Map mostra la profondità usata (bianco vicino, nero lontano). Per regolare la mappa: Focus On Focus Point mette a fuoco la profondità sotto il Focus Point (altrimenti vale Focus Distance), Sharp Range tiene nitida una fascia di profondità attorno al fuoco, Far Cut e Near Cut tagliano gli estremi della mappa, Depth Contrast e Depth Shift la ridistribuiscono, Depth Smooth la ammorbidisce, Invert Depth la capovolge. La vista Focus Overlay mostra il fotogramma con una tinta arancione dietro il piano di fuoco e blu davanti, nitido dove non c'è sfocatura. Show Depth Map è un interruttore che mostra la mappa usata dal render, Show 3D Focus View la mostra come rilievo tridimensionale che puoi ruotare con Orbit e Tilt (Relief regola l'altezza): i punti che restano nitidi sono verdi, il contorno verde è il piano di fuoco, il disco giallo è il Focus Point sull'asse di profondità. Entrambi funzionano con Depth Map e AI Depth. La mappa è normalizzata su ogni fotogramma, quindi su riprese con movimenti forti può oscillare leggermente.

Aperture: forma (iride a lamelle oppure cuore, stella, triangolo, rombo, croce, anello, mezzaluna), numero di lamelle, curvatura, rotazione, ostruzione centrale (bokeh a ciambella degli obiettivi catadiottrici), layer da usare come apertura personalizzata.

Creative: Impression (positivo bordo netto e luminoso, negativo disco morbido), Coma, Astigmatism in millimetri di spostamento del fuoco tra direzione radiale e tangenziale all'angolo, Field Curvature in millimetri (gli angoli vanno fuori fuoco), Zonal Ripple con densità (anelli concentrici), Lobes con numero, forma, angolo e orientamento verso il centro, aberrazione cromatica attivabile (Bokeh Fringing per le frange colorate sui dischi, Lateral CA per lo spostamento rosso/blu verso i bordi che si vede anche sulle zone nitide), Cat-Eye (vignettatura meccanica), squeeze anamorfico, Bokeh Imperfections (polvere e grana dentro i dischi) con scala e seed, Lens Coverage, Filmback Offset.

Layers: Rain Layer (una mappa di gocce: le gocce fuori fuoco compaiono dentro ogni disco di bokeh, quelle a fuoco deformano l'immagine), con distanza, intensità e rifrazione; Shimmer, scintillii dentro i dischi che cambiano a ogni fotogramma, con densità e seed.

Highlights: soglia e boost delle alte luci prima della sfocatura, per recuperare la luminosità che il footage a 8/16 bit ha perso nel clipping.

Render: qualità, raggio massimo, numero di fette di profondità, spazio di lavoro (Auto decodifica sRGB a 8/16 bpc e considera lineare il 32 bpc), centro ottico, vista (risultato, mappa di sfocatura, griglia di bokeh per controllare la lente su tutto il fotogramma).

## Obiettivi

Double-Gauss 50 f/2 e Wide 22 f/2.8 sono prescrizioni pubblicate (brevetto Tronnier e progetto Nakamura, come tabulati in Smith, "Modern Lens Design"). Double-Gauss 58 f/2.2 Swirl usa lo stesso vetro con il gruppo posteriore più chiuso dalla montatura, per un cat-eye e uno swirl più forti. Cooke Triplet 50 f/2.8, Tessar 50 f/3.5 e Petzval 85 f/2.2 sono progetti Lensyum nelle forme classiche: vetri di catalogo, curvature ottimizzate su raggi reali per il campo del full frame, diametri utili ricavati dai fasci tracciati. Il Wide 22 copre Super 35 e APS-C; su full frame gli angoli cadono molto.

## Installare

Il pacchetto `Lensyum-1.0-win64.zip` contiene il plugin, ONNX Runtime, DirectML e il modello di profondità. Va estratto in `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\Lensyum\` con After Effects chiuso; le istruzioni sono anche in `INSTALL.txt` dentro lo zip. Licenze dei componenti inclusi in `THIRD_PARTY.md`.

## Compilare il plugin su Windows

Servono Visual Studio 2022 o successivo con il workload "Sviluppo di applicazioni desktop con C++", CMake 3.20 o successivo (quello incluso in Visual Studio va bene) e l'After Effects SDK dalla Adobe Developer Console, estratto per esempio in `C:\SDK\AfterEffectsSDK` (deve contenere `Examples\Headers`). Per AI Depth servono anche il pacchetto NuGet `Microsoft.ML.OnnxRuntime.DirectML` estratto in `C:\SDK\ort` e `Microsoft.AI.DirectML` estratto in `C:\SDK\dml` (i `.nupkg` sono zip); CMake trova da solo `C:\SDK\ort`, altrimenti si passa `-DORT_INCLUDE_DIR`.

Da "x64 Native Tools Command Prompt for VS", nella cartella del progetto:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DAE_SDK_DIR="C:/SDK/AfterEffectsSDK"
cmake --build build --target Lensyum
```

Il risultato è `build\ae\Lensyum.aex`. `package.bat` ricompila e crea `dist\Lensyum-1.0-win64.zip` con plugin, DLL e modello; il modello (`model.onnx` di `huggingface.co/onnx-community/depth-anything-v2-small`, rinominato `lensyum_depth.onnx`) viene preso dalla variabile `MODEL` o dalla cartella MediaCore se è già installato. Il banner in cima al pannello si disattiva con `-DLENSYUM_BANNER=OFF`.

Se AI Depth non trova runtime o modello, il fotogramma diventa rosso pieno e il motivo è scritto in `%TEMP%\lensyum_log.txt`.

## Provare il motore senza After Effects

```
cmake -S . -B build
cmake --build build --config Release --target lensyum_cli
build/lensyum_cli lenses
build/lensyum_cli grid griglia.ppm amount=40 blades=7 curv=0.3
build/lensyum_cli scene scena.ppm focus=3 N=2 gain=8
build/lensyum_cli image foto.ppm out.ppm depth=profondita.pgm focus=2.5
```

Le opzioni sono coppie `chiave=valore` (`lens`, `N`, `focus` in metri, `sensor`, `blades`, `curv`, `rot`, `obst`, `mask=file.pgm`, `onion`, `lobes`, `lobecount`, `texture`, `cateye`, `ca`, `imp`, `coma`, `astig` e `fc` in mm, `squeeze`, `gain`, `thr`, `amount`, `quality`, `maxblur`, `layers`, `view`; per la profondità AI `aimodel=modello.onnx`, `ort=libreria onnxruntime`, `aires`, `refine`).

## Stato

Motore su CPU, multi-thread. A 1080p su 4 core il tracciamento della lente richiede circa 0,6 s (solo quando cambiano obiettivo, diaframma, formato o i controlli Creative che agiscono sui raggi) e il render di un fotogramma da 0,3 a 0,8 s. La stima AI della profondità gira su GPU tramite DirectML. Limiti noti: solo Windows; la mappa AI è normalizzata per fotogramma e può oscillare nelle riprese con forti cambi di scena; il render resta su CPU, quindi l'anteprima non è in tempo reale ad alte risoluzioni.

Licenza: vedi LICENSE (tutti i diritti riservati). I componenti di terze parti hanno le licenze indicate in THIRD_PARTY.md.
