# Lensyum

Lensyum è un effetto di sfocatura ottica per After Effects basato sul ray tracing di prescrizioni di obiettivi reali. La forma del bokeh non è un disco disegnato: nasce tracciando i raggi attraverso le superfici del vetro, per cui cat-eye ai bordi, bordo a bolla di sapone, frange cromatiche e differenze tra primo piano e sfondo sono una conseguenza della lente scelta.

## Come funziona

Il progetto ha due parti. `core/` è il motore in C++17, senza dipendenze, compilabile su qualunque piattaforma. `ae/` è il wrapper SmartFX per After Effects (8, 16 e 32 bpc, Multi-Frame Rendering).

Il motore lavora in tre passaggi. Per prima cosa scala la prescrizione alla focale richiesta, misura l'apertura massima reale e mette a fuoco il sensore alla distanza scelta (`Lens.cpp`). Poi, per una serie di posizioni lungo il raggio del fotogramma, traccia da decine a centinaia di migliaia di raggi in sei lunghezze d'onda e registra dove arrivano sul piano del sensore a diversi livelli di sfocatura, davanti e dietro il fuoco (`PsfAtlas.cpp`). Per ogni punto l'atlante conserva la luce che arriva, vignettatura meccanica compresa, e da quale punto della pupilla proviene. L'iride (lamelle, apertura personalizzata, ostruzione centrale, anelli, texture del vetro) viene applicata al momento del render nella sua orientazione fissa: una forma asimmetrica resta dritta in tutto il fotogramma e si capovolge tra sfondo e primo piano, come succede con un obiettivo vero. Infine il renderer (`Renderer.cpp`) divide l'immagine in fette di profondità, proietta su ogni pixel la PSF corrispondente alla sua posizione e alla sua sfocatura, e ricompone le fette dal fondo verso il davanti, così un primo piano sfocato copre correttamente quello che sta dietro. Per le sfocature grandi i pixel vengono aggregati a risoluzione ridotta; le alte luci restano a piena risoluzione fino alla soglia di qualità, così il bordo dei bokeh resta netto.

La sfocatura può essere uniforme (in pixel) oppure calcolata da una mappa di profondità: in quel caso il raggio di sfocatura di ogni pixel deriva dalla lente vera, cioè da focale, diaframma, distanza di messa a fuoco e dimensione del sensore.

## Controlli

Camera: preset dell'obiettivo (ogni preset ha la sua focale), formato del sensore (full frame, Super 35, APS-C, Micro 4/3, Super 16, Large Format 65 o larghezza personalizzata), diaframma. Se chiedi un diaframma più aperto di quello massimo della lente, viene usato il massimo.

Focus: sorgente della sfocatura (uniforme o mappa di profondità), layer di profondità con polarità e codifica (distanza lineare o disparità 1/z, il formato tipico delle mappe generate da AI), distanze near/far in metri, distanza di messa a fuoco oppure messa a fuoco su un punto campionato dalla mappa, scala della sfocatura.

Aperture: numero di lamelle, curvatura, rotazione, ostruzione centrale (bokeh a ciambella degli obiettivi catadiottrici), layer da usare come apertura personalizzata.

Creative: Impression (positivo bordo netto e luminoso, negativo disco morbido), Coma, Astigmatism in millimetri di spostamento del fuoco tra direzione radiale e tangenziale all'angolo, Field Curvature in millimetri (gli angoli vanno fuori fuoco), Zonal Ripple con densità (anelli concentrici), Lobes con numero di lobi, aberrazione cromatica, Cat-Eye (vignettatura meccanica), squeeze anamorfico, texture del vetro con scala e seed.

Highlights: soglia e boost delle alte luci prima della sfocatura, per recuperare la luminosità che il footage a 8/16 bit ha perso nel clipping.

Render: qualità, raggio massimo, numero di fette di profondità, spazio di lavoro (Auto decodifica sRGB a 8/16 bpc e considera lineare il 32 bpc), centro ottico, vista (risultato, mappa di sfocatura, griglia di bokeh per controllare la lente su tutto il fotogramma).

## Obiettivi

Double-Gauss 50 f/2 e Wide 22 f/2.8 sono prescrizioni pubblicate (brevetto Tronnier e progetto Nakamura, come tabulati in Smith, "Modern Lens Design"). Double-Gauss 58 f/2.2 Swirl usa lo stesso vetro con il gruppo posteriore più chiuso dalla montatura, per un cat-eye e uno swirl più forti. Cooke Triplet 50 f/2.8, Tessar 50 f/3.5 e Petzval 85 f/2 sono progetti Lensyum nelle forme classiche: vetri di catalogo, curvature ottimizzate su raggi reali per il campo del full frame, diametri utili ricavati dai fasci tracciati. Il Wide 22 copre Super 35 e APS-C; su full frame gli angoli cadono molto.

## Compilare il plugin su Windows

Servono Visual Studio 2022 con il workload "Sviluppo di applicazioni desktop con C++", CMake 3.20 o successivo (quello incluso in Visual Studio va bene) e l'After Effects SDK, scaricabile gratis dalla Adobe Developer Console. Estrai l'SDK in una cartella, per esempio `C:\SDK\AfterEffectsSDK`, che deve contenere `Examples\Headers` ed `Examples\Resources\PiPLtool.exe`.

Da "x64 Native Tools Command Prompt for VS 2022":

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DAE_SDK_DIR="C:/SDK/AfterEffectsSDK"
cmake --build build --config Release --target Lensyum
```

Il file `build\ae\Release\Lensyum.aex` va copiato in `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\` (anche in una sottocartella). Aggiungendo `-DLENSYUM_INSTALL_DIR="C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore/Lensyum"` la copia avviene a ogni build (serve il prompt come amministratore). L'effetto compare in Effetti > Lensyum.

## Provare il motore senza After Effects

```
cmake -S . -B build
cmake --build build --config Release --target lensyum_cli
build/lensyum_cli lenses
build/lensyum_cli grid griglia.ppm amount=40 blades=7 curv=0.3
build/lensyum_cli scene scena.ppm focus=3 N=2 gain=8
build/lensyum_cli image foto.ppm out.ppm depth=profondita.pgm focus=2.5
```

Le opzioni sono coppie `chiave=valore` (`lens`, `N`, `focus` in metri, `sensor`, `blades`, `curv`, `rot`, `obst`, `mask=file.pgm`, `onion`, `lobes`, `lobecount`, `texture`, `cateye`, `ca`, `imp`, `coma`, `astig` e `fc` in mm, `squeeze`, `gain`, `thr`, `amount`, `quality`, `maxblur`, `layers`, `view`).

## Stato

Motore su CPU, multi-thread. A 1080p su 4 core il tracciamento della lente richiede circa 0,6 s (solo quando cambiano obiettivo, diaframma, formato o i controlli Creative che agiscono sui raggi) e il render di un fotogramma da 0,3 a 0,8 s. Prossimi passi: porting su GPU (CUDA) per l'anteprima interattiva, layer extra a profondità propria, filtro frontale (sporco, paraluce) e i layer di pioggia e glitter.
