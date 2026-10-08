# BACKEND-SELECTION-PLAN — die Vorteile aller Backends in einem Renderer bündeln

Stand 2026-10-08: **Phasen 0–2 umgesetzt** (OPEN-ITEMS-PLAN Batch 3, Tests 184–187); Phase 3 lebt in PHPolygon,
Phase 4 ist mit `vio_feature_info` (native/emulated) abgedeckt. Ziel: Eine PHPolygon-Anwendung bekommt auf jedem Gerät das Backend und je Effekt die
Technik, die dort am besten läuft — ohne mehrere Grafik-APIs gleichzeitig zu betreiben.

## Entscheidung: kein Multi-API-Rendering

Zwei APIs parallel (Ressourcen per Shared Handle, Fences/Timeline-Semaphores geteilt) kosten Synchronisation
an jeder Übergabe, doppelte Treiber-, Shader- und PSO-Caches und Formatabgleich; Metal und Linux-D3D fallen
ganz heraus. Der Gewinn ist fast immer null, weil moderne APIs auf derselben Hardware dieselben Features
haben — die Unterschiede liegen im Treiber. **Interop nur als eng begrenzte Brücke** (Phase 4).

Gebündelt wird auf drei anderen Ebenen:

| Ebene | Was | Wo |
|---|---|---|
| A. Backend je Gerät | das beste Backend für diese GPU / diesen Treiber wählen | vio (`vio_create('auto')`) |
| B. Technik je Backend | je Effekt die beste Variante, die die Flags erlauben | PHPolygon-Renderer |
| C. Lücken schließen | fehlende Features im Backend emulieren | vio (wie GS / Multiview auf Metal) |

## Ausgangslage

- `vio_get_auto_backend_skip()` (`src/vio_backend_registry.c`): feste Reihenfolge je Plattform
  (Windows d3d12 > d3d11 > vulkan > opengl, Linux vulkan > opengl, macOS metal > opengl); Pass 0 verlangt den
  vollständigen 3D-Satz, ein Backend ohne Device wird übersprungen (Test 100).
- `supports_feature` wird vor dem Device-Open gefragt — gerätegebundene Flags (VRS, Mesh, Raytracing,
  SM-/MSL-Stufe) sind dort noch unbekannt.
- `vio_backend_info()` (`describe`-Slot) gibt es nur für Metal; D3D12/Vulkan/GL fehlen
  (SHADER-FEATURES-PLAN Phase 0b).
- Feature-Flags sind ehrlich; Leitern: Metal-MSL (`msl_version`), D3D12-Shader-Model (`shader_model_version`).

## Phase 0 — Voraussetzungen (S–M)

- **0a** `describe` für D3D12, Vulkan, OpenGL (SHADER-FEATURES-PLAN 0b): Adapter, Treiberversion, Vendor-ID,
  API-/SM-/GLSL-Stufe, Caps. Ein gemeinsamer Vendor-Code (`vendor_id`: 0x10DE NVIDIA, 0x1002 AMD,
  0x8086 Intel, 0x106B Apple, 0x1414 Microsoft/WARP, Mesa-Treiber per Name).
- **0b** Gerätescan ohne Kontext: Vtable-Slot `enumerate_adapters(vio_adapter_info *out, int max)` —
  DXGI `EnumAdapterByGpuPreference`, `vkEnumeratePhysicalDevices`, `MTLCopyAllDevices`, GL nur über den
  bestehenden Kontext. Liefert Vendor, Name, VRAM, diskret/integriert und die gerätegebundenen Flags,
  die ohne Device-Open abfragbar sind (Vulkan: Features2; D3D12: `D3D12CreateDevice` auf dem Adapter
  ist billig, danach `CheckFeatureSupport`).
- Test: `vio_adapters()` (neu) listet je registriertem Backend die Adapter mit Vendor und Caps.

## Phase 1 — Scoring für `auto` (M)

`vio_create('auto', ['prefer' => 'performance' | 'quality' | 'compat', 'require' => [VIO_FEATURE_*...]])`

1. **Filter**: Backends/Adapter, die ein `require`-Feature nicht haben, fallen weg (heute nur der 3D-Satz).
2. **Herstellerprofil** (Tabelle in `vio_backend_registry.c`, per Treiberversion überschreibbar):
   AMD → vulkan, d3d12 · NVIDIA → d3d12, vulkan, d3d11 · Intel Arc → d3d12, vulkan · Intel iGPU alt →
   d3d11 · Apple → metal · Linux → vulkan · WARP/llvmpipe/lavapipe nur als letzter Ausweg.
3. **Fähigkeiten**: Punkte je verfügbarem Feature, gewichtet nach `prefer` (quality: Raytracing, Mesh,
   VRS, Bindless; performance: Subgroups, Indirect, Bindless, Async-Compute; compat: wenige Features, stabile
   Pfade, d3d11/opengl vorn).
4. **Diskret vor integriert** bei mehreren Adaptern (DXGI `HIGH_PERFORMANCE`, Vulkan `DISCRETE_GPU`).
- `vio_backend_info()` meldet zusätzlich `selected_by` (profile / score / benchmark / explicit) und die
  Punkte je Kandidat — die Wahl bleibt nachvollziehbar.
- Test: Auswahl mit simulierten Kandidaten (Testhook `VIO_TEST_ADAPTERS` = JSON), damit die Logik ohne
  echte GPUs prüfbar ist; `require` ohne erfüllendes Backend → `false` mit Warnung.

## Phase 2 — Kalibrierlauf mit Cache (M)

- `vio_create('auto', ['benchmark' => true])` rendert beim **ersten** Start je Kandidat ~1 s eine
  Kalibrierszene (Mesh-Batch + Post-Effekt + Compute, headless, feste Auflösung) und misst
  `vio_gpu_frame_time` und CPU-Zeit je Frame.
- Ergebnis-Cache je (Adapter-Name, Treiberversion, vio-Version) in einer JSON-Datei
  (`'benchmark_cache' => dir`, Default neben `shader_cache`); Treiber-Update ⇒ neuer Lauf.
- Der Benchmark entscheidet nur zwischen Kandidaten, die Filter und Profil übrig lassen (max. 3).
- `vio_benchmark_backends(array $opts): array` für Tools/Settings-Menüs (Ergebnistabelle, ohne Auswahl).
- Test: Cache-Treffer überspringt den Lauf; Treiberversion im Schlüssel; `benchmark => false` = Phase 1.

## Phase 3 — Technik-Ketten im Renderer (PHPolygon, L)

Jeder Effekt bekommt eine geordnete Kette; der Renderer wählt beim Start die erste Stufe, deren Flags
vorhanden sind **und** deren gemessene Kosten ins Budget passen (Messung aus Phase 2 oder ein Frame-Zeit-
Monitor mit Rückstufung zur Laufzeit).

| Effekt | Kette (beste zuerst) |
|---|---|
| Geometrie / Culling | Mesh-Shader → Compute-Culling + Indirect (+ Draw-Parameter) → CPU-Culling |
| Lichter | Clustered (Compute + Subgroup-Ballot) → Tiled → Forward mit Limit |
| Schatten-Kaskaden | Multiview → Layered + VS-Layer → ein Pass je Kaskade |
| Schatten / AO / GI | Ray Query → SDF-Raymarching → Screen-Space |
| Auflösung | VRS-Bild → VRS je Draw → Effekte in halber Auflösung + Upsampling → TAA-U |
| Materialien | Bindless → Texture-Arrays → Bind je Draw |
| Post-Processing | float16 + Subgroups → float32 |

- vio liefert dafür nur Flags, Leitern und `vio_backend_info`; die Ketten leben in PHPolygon
  (`BackendConventions`/Renderer-Settings), mit einem Debug-Overlay „welche Stufe läuft“.

## Phase 4 — Lücken in vio schließen (Emulation, M je Feature)

Ein Flag heißt „funktioniert“, nicht „in Hardware“; `vio_backend_info()['caps']` unterscheidet
`native` / `emulated`, damit Phase 2/3 Emulationen bewerten können.
- Multiview auf D3D11 / OpenGL ohne `GL_OVR_multiview2`: Instancing-Emulation wie auf Metal.
- Draw-Parameter auf D3D12 < SM 6.8 für direkte Draws: Root-Konstante je Draw (indirekte Draws bleiben 0).
- VRS-Ersatz: kein Flag, aber ein Helfer für „Effekt in halber Auflösung + tiefenbewusstes Upsampling“.

## Phase 5 — Interop als Brücke (L, nur mit konkretem Bedarf)

- GPU-Video-Encoding für `vio_recorder`: Frame per Shared Handle an NVENC/AMF/QuickSync
  (D3D11-Interop) bzw. VideoToolbox (Metal-IOSurface) statt CPU-Readback.
- Weitere Brücken (OpenXR, Upscaler-SDKs, D3D12-only-Features wie Work Graphs neben einem Vulkan-Renderer)
  einzeln planen; kein allgemeines Multi-API-Rendering.

## Reihenfolge

0a/0b → 1 → 2 (vio) parallel zu 3 (PHPolygon, nutzt zunächst nur Flags) → 4 nach Bedarf → 5 nur auf Anfrage.
