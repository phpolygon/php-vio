# Offene Punkte ohne externe Abhängigkeit — Umsetzungsplan

> **Auftrag (2026-10-07):** Alles umsetzen, was ohne Upstream-, SDK-, Treiber- oder Hardware-Abhängigkeit geht.
> Grundlage ist die Bestandsaufnahme vom 2026-10-07 (Feature-Matrix in CLAUDE.md, alle `*-PLAN.md`, Quellcode-Marker).
> **Basis:** Branch `fix/windows-stack-findings` (PR #67, auf dem Feature-Stack #50–#65). Jeder Batch ist ein eigener
> Branch und ein eigener PR auf dem vorigen.
> **Prüfbarkeit:** Lokal gibt es nur Windows (RTX 2080, WARP, Vulkan, GL). Reine Metal-, Cocoa- und X11-Teile
> belegt allein die CI; sie sind unten mit „(CI)“ markiert.

Größe: S ≤ ½ Tag, M ≈ 1–2 Tage, L = mehrere Tage. Nummern A1–A41 stammen aus der Bestandsaufnahme.

## Batch 1 — klein und unabhängig
| # | Punkt | Backend | Größe |
|---|---|---|---|
| E/F | Veraltete Statuszeilen (CLAUDE.md, API-ROADMAP, GEOMETRY-STAGES, SHADER-FEATURES); tote GL-Vtable-Stubs, veraltete Kommentare | — | S |
| A40 | Paritätstest Einzel-Draw ↔ `vio_submit_batch` auf allen Backends (BATCH-SUBMIT Ph.0) | alle | S |
| A5 | D3D12-Shader-Model festlegen: `shader_model => 62`… bzw. `VIO_D3D12_SHADER_MODEL` | D3D12 | S |
| A9 | Caps unterscheiden „nativ“ und „emuliert“ (`vio_feature_info()` o. ä.) | alle | S |
| A19 | Benannte GPU-Zeitmarken je Pass (`vio_gpu_timestamp`, `vio_gpu_timings`) | alle | S–M |
| ~~A25~~ | ~~Vulkan-Cube-RT: Tiefe auch für Level > 0~~ — **gestrichen**: keine Vulkan-Lücke, sondern der gemeinsame Vertrag aller fünf Backends (Mip-Level > 0 eines Cube-RTs rendern ohne Tiefe, siehe `d3d11_bind_render_target_face`). Ändern hieße alle Backends umbauen, ohne Bedarf: Mip-Ketten füllt man mit tiefenlosen Vollbild-Passes. | — | — |

## Batch 2 — Korrektheit und Portabilität im Alltag
| # | Punkt | Backend | Größe |
|---|---|---|---|
| A31 ✅ | Input-Layout aus dem Mesh-Layout statt aus der Reflection (Lücken, Reihenfolge) — betraf auch Vulkan (Test 174) | D3D11, D3D12, Vulkan, Metal (CI) | M |
| A32 ✅ | `vio_uniform_buffer` + `vio_bind_buffer` für Grafik-Shader portabel — betraf alle Backends (Test 175) | alle | M |
| A12 ✅ | Bindless 4b: Slot-Freigabe, Sampler-Varianten, Arrays/Cubes, Compute (Tests 176–179); Metal-4-Residency-Sets bleiben offen | D3D12, Vulkan, Metal (CI) | M–L |
| A23 ✅ | Getrennte `texture`/`sampler`-Objekte — Vulkan und GL, Metal (CI) (Test 180) | Vulkan, GL, Metal | M |
| B4/B5* ✅ | GL: Rohquelltext-Fallback, wenn SPIRV-Cross Subgroup-/Quad-Ops ablehnt; `SUBGROUP`/`SUBGROUP_QUAD` folgen dem Treiber (Tests 149/152 laufen auf GL) | GL | M |
| A11 ✅ | D3D12 Draw-Parameter unter SM 6.8 per Root-Konstante, auch FXC und indirekte Multi-Draws (Test 181) | D3D12 | S–M |
| A10 ✅ | Multiview-Emulation per Instancing (Tests 158 / 182) | D3D11, GL ohne OVR | M |
| A27 ✅ | `view_count` zusammen mit GS-/Tess-Stages: Vulkan nativ, D3D12 über eine aus dem VS durchgereichte View; GL (OVR schließt die Stages aus), D3D11 und Metal (View per Instancing im VS) melden 0 (Test 183) | Vulkan, D3D12 | M |

## Batch 3 — Backend-Auswahl (BACKEND-SELECTION-PLAN) ✅
A4 `describe` für D3D12/D3D11/Vulkan/GL (Test 184) ✅ · A6 `vio_adapters()` (Test 185) ✅ · A7 Scoring für `auto`
(`prefer`/`require`, `vio_rank_backends`, `VIO_TEST_ADAPTERS`; Test 186) ✅ · A8 Kalibrierlauf mit Cache (`benchmark`,
`vio_benchmark_backends`, `headless_hardware`; Test 187) ✅.

## Batch 4 — Render-Features
A24 MSAA für Depth-only/Cube/Array/MRT ✅ (Tests 194–196; Nebenbefund: D3D11 zeichnete MSAA-MRT gar nicht) · A26 Tiefentexturen mit Mip-Kette ✅ (Test 193) · A35 KTX2-Cubemaps und -3D ✅ (Test 190)
(Supercompression braucht zstd/Basis → **nicht** in diesem Plan) · A20 ASTC ✅ (Test 191; Dekodierung belegt nur die macOS-CI) ·
A33 Sub-Image-Uploads für den Glyph-Atlas ✅ (Test 189) · A34 Vertikaltext ✅ (Test 192) · A36 Vulkan `read_pixels` ohne
`vkDeviceWaitIdle` ✅ (Test 188; Nebenbefund: D3D11 las mitten im Frame den vorigen Frame).

## Batch 5 — Lücken der Shader-Stages
A29 Hull/Domain-Generator (Interface-Blöcke, Struct-/Matrix-Varyings, `gl_ClipDistance`, fremde Kontrollpunkte) ✅ (Test 198; Nebenbefund: GL schaltete `GL_CLIP_DISTANCEi` nie ein) ·
A28 Metal-GS-Emulation (Sampler, Interface-Blöcke, GS hinter Tess, Strip-Adjacency, `gl_PrimitiveID`) ✅ (Test 205; D3D hatte dieselben Lücken außer Strip-Adjacency – `gl_PrimitiveIDIn`/`gl_PrimitiveID`, GS-Eingangsblöcke, Vertex-Texturen – und ist mit behoben; GS hinter Tess bleibt auf Metal abgelehnt: der Tessellator gibt seine Dreiecke nicht heraus, Metal-Teil nur über die macOS-CI belegt) ·
A30 Texturen in Mesh-/Task-Stages ✅ (Test 197; Nebenbefund: Sampler-Register je Stage folgten der Reihenfolge der ersten Benutzung – jetzt ein shaderweiter Plan nach Namen) · A13 RT-Pipeline (mehrere Gruppen, Callables, Shader-Records, Ressourcen, Trace im Frame) ✅ (Tests 200–202; Ressourcen = Storage-Buffer + Texturen, Uniform-Buffer laufen über Shader-Records) ·
A14 TLAS-Objekt mit Rebuild/Refit ✅ (`vio_acceleration_structure_update`, Test 199; Nebenbefunde: `vio_mesh` hielt die Dreiecke nur bei `RAY_QUERY`, nicht bei `RAYTRACING` – ein DXR-1.0-Gerät konnte keine Struktur bauen; D3D12-Debug-Warnung 1328 bei jedem Build) · A15 Sampler-Feedback-Emulation ✅ (Tests 203/204: zuerst Fragment-Storage-Buffer auf allen Backends, darauf `VIO_SAMPLER_FEEDBACK_GLSL`; auf D3D12 werden Hardware- und GLSL-Karte zusammengeführt) · A18 Vulkan-Shading-Rate-Bild → **verschoben hinter A37** (Batch 6): die RTX 2080 kann es (`attachmentFragmentShadingRate`), aber das Bild ist ein Attachment des Render-Passes; vio legt alle Passes mit `vkCreateRenderPass` (v1) an und baut Pipelines je Pass-Signatur. Mit Dynamic Rendering (A37) ist es ein Feld von `vkCmdBeginRendering` statt einer zweiten Varianten-Achse über alle Passes.

## Batch 6 — große Pakete
A37 Vulkan 1.2+ (Timeline-Semaphores, Dynamic Rendering, Sync2) ✅ (VULKAN-MODERN-PLAN, Test 206; Nebenbefunde: Vulkan lehnte `vio_read_render_target` im Frame ab; der headless Capture-Puffer hatte keine Abhängigkeit zwischen den Frames – Sync-Validierung) ·
A18 Vulkan-Shading-Rate-Bild ✅ (Test 160 auf Vulkan) · A38 Recording über Secondary Command Buffers ✅ (BUNDLE-PLAN, Test 207: `vio_bundle` mit nativer Aufnahme auf Vulkan (Secondary Command Buffer), D3D12 (Bundle-Command-List) und D3D11 (Deferred Context), Abspielen auf GL/Metal; 2000 Draws D3D12 3,9 → 0,75 ms, Vulkan 5,4 → 1,3 ms) ·
A21 Upscaling `vio_upscale` ✅ (UPSCALE-PLAN, Test 208: portable Fragment-Passes statt Compute – laufen auch auf GL 3.3 –, spatial + temporal; MetalFX spatial dahinter, nur über die macOS-CI belegt; DirectSR erst mit Retail-SDK) ·
A39 GPU-Video-Encoding über Interop (FFmpeg-Hardware-Encoder) ✅ (VIDEO-ENCODE-PLAN, Test 209: Encoder-Wahl `auto`/`hardware`/`software`, D3D11 ohne CPU-Kopie über einen D3D11VA-Pool, `vio_video_info`/`vio_video_frame` zum Zurücklesen; 1080p NVENC 2,9 statt 16 ms je Frame) · A22 Metal `MTLBinaryArchive` ✅ (Test 116 jetzt mit Metal: Archiv je Gerät + OS im Cache-Verzeichnis, Treffer über `FailOnBinaryArchiveMiss`; nur die macOS-CI) ·
A16 Rate Maps ✅ (Test 211: eigenes Feature `VIO_FEATURE_RASTER_RATE_MAP`, `'rate_map'` an Render-Targets, Resolve beim Verlassen; nur die macOS-CI) · A17 Metal-Tensoren über `'msl'`-Override ✅ (Test 210: `vio_compute_pipeline(['msl' => …])`, Tensoren per `tensor_inline` ohne Host-Objekte; der Tensor-Teil braucht Metal 4).

## Batch 7 — eigene Plattformschicht (NATIVE-/WIN32-PLATFORM-PLAN)
A1 Phase 0: `vio_platform.h`, GLFW dahinter, Null-Plattform, Audit-Gate · A2 Win32 (Fenster, Input, WGL-Leiter, XInput) ·
A3 Cocoa und X11/Wayland (CI).

## Arbeitsweise je Punkt
Test zuerst (rot auf dem Ausgangsstand), dann Code, dann volle Suite auf WARP **und** RTX
(`VIO_D3D_HEADLESS_HARDWARE=1`, `VIO_REQUIRE_*`) sowie `vio.debug=1`. Conventional Commits je Punkt, CLAUDE.md
und Feature-Matrix im selben Batch nachziehen.

## Stand
- [x] Batch 1 (Branch `feat/open-items-batch1`): A40 Test 169, A5 Test 170, A9 Test 171, A19 Test 172, E/F;
  Nebenbefund beim Schreiben von 172: D3D12 verlor die gebundene Pipeline über die Frame-Grenze und entfernte
  beim nächsten Draw das Device (Fix + Test 173).
- [x] Batch 2 (Branch `feat/open-items-batch2`): A31, A32, A12, A23, B4/B5, A11, A10, A27 — Tests 174–183. · [x] Batch 3 (Branch `feat/open-items-batch3`): A4, A6, A7, A8 — Tests 184–187. · [x] Batch 4 (Branch `feat/open-items-batch4`): A36, A33, A35, A20, A34, A26, A24 — Tests 188–196. · [x] Batch 5 (Branch `feat/open-items-batch5`): A29, A28, A30, A13, A14, A15 — Tests 197–205; die macOS-CI (erstmals mit Homebrew-PHP lauffähig) belegt die Metal-Teile und fand dabei: `vio_submit_batch` ohne Shader-Cbuffer-Bind auf Metal (169), Metal-Frame-Zeit nur über den letzten Command-Buffer (172), in Metal reservierte Funktionsnamen wie `quad` (205), Patch-Block- und ungenutzte Tess-Varyings (198), das fehlende `zend_exceptions.h` (clang). · [ ] Batch 6 · [ ] Batch 7
