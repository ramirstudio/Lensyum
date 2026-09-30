# Architettura di Lensyum

Lensyum sfoca un fotogramma simulando il bokeh di un obiettivo reale. Il motore è una libreria C++17 senza dipendenze (`core/`), usata da un wrapper SmartFX per After Effects (`ae/`) e da una CLI di prova (`tools/lensyum_cli.cpp`). Gira su CPU con thread propri; la GPU serve solo per la stima di profondità AI.

## Ottica

`Lens.cpp` traccia raggi reali attraverso prescrizioni di obiettivi (superfici sferiche, vetri da nd/Abbe con dispersione di Cauchy). Da ogni prescrizione ricava lunghezza focale efficace, f-number misurato, fuoco parassiale per una distanza data e vignettatura meccanica, cioè il cat-eye. `Aperture.cpp` descrive il diaframma come funzione di trasmissione sulla pupilla: lamelle, curvatura, forme speciali, ostruzione centrale morbida, lobi, maschera personalizzata, texture di imperfezioni.

`PsfAtlas.cpp` precalcola, per ogni raggio di campo e per ogni defocus con segno, la mappa pupilla→immagine. Ogni texel contiene 5 float: luce RGB e posizione pupillare pesata dalla luce. L'atlante ha mip, è in cache e la chiave quantizza il fuoco a 24 passi per decade. L'iride non è nell'atlante: si applica al render, nel sistema di riferimento dell'obiettivo, così una forma personalizzata resta dritta nel bokeh di sfondo e ruota solo con il cat-eye. Impression, coma, astigmatismo, curvatura di campo e cromatica entrano nel tracciamento o nella mappa del defocus.

## Render

`Renderer.cpp` assegna a ogni pixel un raggio di sfocatura con segno `s` (positivo sullo sfondo, negativo in primo piano) dalla sorgente scelta: uniforme, regione di fuoco con falloff, oppure mappa di profondità convertita tramite una LUT costruita con la lente reale.

I pixel vengono sparsi, non raccolti. La profondità è divisa in fette con assegnazione dithered, composte da lontano a vicino con un canale di copertura. Sopra una certa soglia di raggio i pixel sono aggregati in blocchi 2^L (fino a L=5) e i blocchi sparsi al posto dei singoli pixel; le alte luci restano a piena risoluzione. Ogni splat legge il kernel dall'atlante: con iride tonda i kernel sono cotti una volta per render, altrimenti l'iride è applicata per pixel. La normalizzazione usa la massa del kernel, misurata per 32 orientazioni se la forma non è tonda, ed è esatta sul disco intero (anche fuori quadro) per footprint piccoli o sottili. Dove la copertura scende sotto 0,3 si mescola la sorgente.

Dopo le fette: rifrazione della pioggia, aberrazione cromatica laterale, centro intatto della regione, Blend Back. Le viste di controllo (Blur Map, Bokeh Grid, Depth Map, Focus Overlay, 3D) sono rami dello stesso renderer.

Il raggio massimo delle sfocature è noto in anticipo, quindi `renderDefocus` estende il livello oltre i suoi bordi per riflessione di quel tanto, renderizza solo i dischi che raggiungono il quadro e ritaglia. Senza questo, ai margini mancherebbe la luce da un lato. Un filtro sul campo di sfocatura elimina i contorni nitidi che compaiono dove la profondità passa per il piano di fuoco.

## Profondità AI

`DepthAI.cpp` carica ONNX Runtime a run time (`LoadLibraryExW`, `OrtGetApiBase`, provider DirectML con fallback su CPU), quindi non c'è dipendenza di link e non ci sono conflitti con il runtime di altre applicazioni. Il modello è Depth Anything V2 Small: ingresso RGB normalizzato con statistiche ImageNet a lati multipli di 14, uscita disparità relativa. La mappa è normalizzata tra il 1° e il 99° percentile, riportata alla risoluzione del livello e raffinata con un filtro guidato. C'è una cache LRU da 6 voci. `adjustDepth` applica livelli, gamma, shift, smooth, inversione e Sharp Range, che appiattisce una fascia attorno alla profondità del Focus Point. La normalizzazione è per fotogramma, senza coerenza temporale.

## Wrapper After Effects

`LensyumAE.cpp` implementa SmartFX (`PreRender`, `SmartRender`) a 8, 16 e 32 bit, con flag per il multi-frame rendering. I parametri hanno indici nell'ordine del pannello (`LensyumParams.h`) e ID su disco stabili, sempre aggiunti in coda. I valori point arrivano alla risoluzione di downsample corrente e vengono riportati a pixel del livello. Il padding richiesto segue la sfocatura effettiva. Il PiPL è pre-generato in `LensyumPiPL.rc` (PiPLtool in build produceva risorse vuote) e va tenuto coerente con i flag di `GlobalSetup`.

In modalità AI Depth la stima gira sull'area del livello prima del render. Se runtime o modello mancano o falliscono, il fotogramma esce rosso pieno e il motivo è scritto in `%TEMP%\lensyum_log.txt`. Il plugin cerca `lensyum_ort.dll` e `lensyum_depth.onnx` accanto al `.aex`.

## Build

CMake costruisce `lensyum_core`, `lensyum_cli` e, con `AE_SDK_DIR`, il `.aex` (solo MSVC/Windows). `ORT_INCLUDE_DIR` attiva la profondità AI; senza, le funzioni ONNX sono stub che restituiscono errore. La CLI accetta quasi tutti i parametri come `chiave=valore` e l'opzione `pad=N` emula il padding trasparente di After Effects.

## Limiti noti

Solo Windows. L'atlante è per raggio di campo, non per pixel, e il campo è interpolato con dithering. La stima AI non ha coerenza temporale. Il rilievo 3D è una nuvola di punti e mostra buchi sulle pareti ripide.
