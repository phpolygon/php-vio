# GPU-Video-Encoding — OPEN-ITEMS A39

> **Status: ✅ Phasen 1–2 umgesetzt** (Test 209). 1920×1080, RTX 2080, Capture + Kodieren je Frame: libx264 16,3 ms,
> NVENC mit Readback 10,7 (Vulkan) – 16,4 ms (D3D12), NVENC ohne CPU-Kopie (D3D11) 2,9 ms. D3D11VA-Pools kennen kein
> RGBA: der Pool ist BGRA mit `BIND_RENDER_TARGET`, der Backend-Slot zeichnet den Frame per Kopier-Pixelshader hinein.

> Stand 2026-10-08. `vio_recorder` liest heute jeden Frame per `vio_capture_rgba()` auf die CPU, wandelt ihn mit
> swscale nach YUV und kodiert mit `h264_videotoolbox` bzw. `libx264` – auf Windows/Linux also in Software. Die
> FFmpeg-Builds der Releases enthalten die Hardware-Encoder (NVENC, AMF, QSV, Media Foundation; VideoToolbox) und
> die Hardware-Geräte D3D11VA/D3D12VA/CUDA/Vulkan. Ziel: Hardware-Encoder wählen und auf D3D11 den Frame ohne
> CPU-Umweg übergeben (Interop).

## API

```php
$rec = vio_recorder($ctx, ['path' => 'out.mp4', 'fps' => 60, 'encoder' => 'auto']);   // 'auto' | 'hardware' | 'software' | FFmpeg-Name
vio_recorder_info($rec);   // ['encoder' => 'h264_nvenc', 'hardware' => true, 'zero_copy' => true, 'frames' => n]
// zum Prüfen und für Video-Texturen:
vio_video_info('out.mp4');          // ['width', 'height', 'frames', 'fps', 'codec'] | false
vio_video_frame('out.mp4', $i);     // ['width', 'height', 'data' => RGBA] | false
```

- `auto`: Hardware-Encoder der Plattform in fester Reihenfolge (Windows: NVENC, AMF, QSV, Media Foundation; Apple:
  VideoToolbox; Linux: NVENC), der erste, der sich öffnen lässt; sonst `libx264`. `hardware`: wie `auto`, aber
  ohne Software-Rückfall (`false` + Warning). `software`: `libx264`.
- Die Bildgröße bleibt die Kontextgröße (gerade gerundet, wie bisher).

## Phasen

| Phase | Inhalt | Prüfung |
|---|---|---|
| 1 | Encoder-Wahl + `vio_recorder_info`; Hardware-Encoder mit Systemspeicher-Frames (der Readback bleibt, die Kodierung geht auf die GPU); `vio_video_info` / `vio_video_frame` (Demuxer + Decoder + swscale nach RGBA). | Test 209: 24 Frames mit wanderndem Farbbalken auf jedem Backend, mit `software` und `auto`; Rücklesen: Frame-Zahl, Größe, Farben an Stichpunkten (verlustbehaftet, Toleranz), Bewegung des Balkens |
| 2 | **D3D11-Interop**: FFmpeg bekommt vios `ID3D11Device` (`AV_HWDEVICE_TYPE_D3D11VA`), die Frames sind D3D11-Texturen aus einem Hardware-Frame-Pool; je Capture kopiert die GPU den Frame hinein (Vtable-Slot), der Encoder liest ihn direkt (NVENC/AMF nehmen BGRA, sonst NV12-Wandlung auf der GPU über `VideoProcessorBlt`). `zero_copy => true`. | 209 auf D3D11 mit `VIO_REQUIRE_ZERO_COPY` auf dem RTX-Host; derselbe Pixel-Vergleich |
| — | D3D12/Vulkan/GL: Phase 1 (Readback + Hardware-Encoder). Vulkan-Video (`h264_vulkan`) bräuchte eine Video-Queue in vios Gerät – nicht in diesem Paket. Metal: VideoToolbox (wie bisher, jetzt in `info` sichtbar). | — |
