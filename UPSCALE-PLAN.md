# Upscaling (`vio_upscale`) — OPEN-ITEMS A21

> **Status: ✅ Phasen 1–3 umgesetzt** (Test 208). Gemessen (32→64, MAE gegen die Szene in 64): bilinear 22,2,
> spatial 12,4, temporal (16 gejitterte Frames, ruhig) 4,3; Schwenk mit 1,37/0,61 Quellpixeln je Frame: mit Bewegungs-
> vektoren 19,4, ohne 66,5 (Schlieren), spatial 13,6 – die Historie wird unter Subpixel-Bewegung bei jedem Frame neu
> abgetastet und weicher. MetalFX (Phase 3) ist nur über die macOS-CI geprüft.

> Stand 2026-10-08. Ein Bild in niedriger Auflösung auf die Zielauflösung bringen. Der Kern ist ein
> **eigener portabler Pfad** aus Fragment-Passes über die öffentliche Draw-API: er läuft auf jedem Backend mit
> 3D-Pipeline (auch GL 3.3 ohne Compute) und braucht keine Bibliothek. MetalFX hängt auf Metal dahinter
> (nur über die macOS-CI prüfbar); DirectSR kommt erst mit einem Retail-SDK (A21 bleibt dort offen).

## API

```php
vio_upscale($ctx, $src, $dst, [
    'mode'      => VIO_UPSCALE_SPATIAL,   // oder VIO_UPSCALE_TEMPORAL
    'sharpness' => 0.25,                  // 0 = aus .. 1 = stark (Kontrast-adaptives Schärfen)
    // nur temporal:
    'motion'    => $motionRt,             // RG: Bewegung in Ziel-UV je Pixel der Quelle (aktuell − vorher)
    'jitter'    => [$jx, $jy],            // Subpixel-Versatz dieses Frames in Quellpixeln (−0.5..0.5)
    'reset'     => false,                 // Historie verwerfen (Kamerasprung)
]);
[$jx, $jy] = vio_upscale_jitter($frameIndex, 8);   // Halton(2,3), zentriert
vio_upscale_info($ctx);   // ['spatial' => 'portable'|'metalfx', 'temporal' => 'portable'|'metalfx']
```

- `$src`: `VioRenderTarget` (Attachment 0) oder `VioTexture`; `$dst`: `VioRenderTarget` oder `null` (das gerade
  gebundene Ziel bzw. die Swapchain). Im Frame aufrufen; die gebundene Pipeline ist danach unbestimmt.
- Temporal hält je Ziel eine Historie (ein RT in Zielgröße, am Kontext); `reset` oder eine neue Zielgröße
  verwirft sie.

## Phasen

| Phase | Inhalt | Prüfung |
|---|---|---|
| 1 | **Spatial, portabel**: Pass 1 kantenadaptives Lanczos-2 (12 Taps, Richtung und Anisotropie aus den Gradienten der 4 inneren Texel, Ringing an den Min/Max der Nachbarschaft geklemmt), Pass 2 kontrastadaptives Schärfen (5 Taps, Gewicht aus lokalem Min/Max, `sharpness`). Shader/Pipelines/Mesh je Kontext einmal angelegt. | Test 208: auf allen Backends Kanten scharfer als bilinear (Gradientenenergie), Flächen unverändert, Farbe stabil; Ziel = Swapchain und RT; Argument-Vertrag |
| 2 | **Temporal, portabel**: Historie per Bewegungsvektor zurückprojizieren (bikubisch), aktuelle Probe mit Jitter-Gewicht (Abstand zum Zielpixelzentrum), Nachbarschafts-Clamp in YCoCg, Mischfaktor nach Konfidenz; danach Schärfen. | 208: statische Szene mit 8 Jitter-Phasen konvergiert gegen das in Zielauflösung gerenderte Bild (deutlich näher als spatial), `reset` verwirft, Bewegung ohne Geisterbilder (bewegtes Rechteck) |
| 3 | **MetalFX** (Metal, `MTLFXSpatialScaler`, macOS 13+, schwach gelinkt): Vtable-Slots `upscale_method` / `upscale_native`, nur in ein Render-Target (das Drawable ist `framebufferOnly`); fehlt dem Ziel die Output-Usage des Scalers, schreibt er in eine Zwischentextur plus Blit; scheitert er, laufen die portablen Passes; `'native' => false` erzwingt sie. `MTLFXTemporalScaler` braucht Tiefe und eigene Bewegungsvektor-Konventionen – temporal bleibt portabel. | 208 auf der macOS-CI mit `vio_upscale_info()['spatial']` = `metalfx` |
