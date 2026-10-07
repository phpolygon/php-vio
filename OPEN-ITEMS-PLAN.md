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
A24 MSAA für Depth-only/Cube/Array/MRT · A26 Tiefentexturen mit Mip-Kette · A35 KTX2-Cubemaps und -3D
(Supercompression braucht zstd/Basis → **nicht** in diesem Plan) · A20 ASTC · A33 Sub-Image-Uploads für den Glyph-Atlas ·
A34 Vertikaltext · A36 Vulkan `read_pixels` ohne `vkDeviceWaitIdle`.

## Batch 5 — Lücken der Shader-Stages
A29 Hull/Domain-Generator (Interface-Blöcke, Struct-/Matrix-Varyings, `gl_ClipDistance`, fremde Kontrollpunkte) ·
A28 Metal-GS-Emulation (Sampler, Interface-Blöcke, GS hinter Tess, Strip-Adjacency, `gl_PrimitiveID`) (CI) ·
A30 Texturen in Mesh-/Task-Stages · A13 RT-Pipeline (mehrere Gruppen, Callables, Shader-Records, Ressourcen, Trace im Frame) ·
A14 TLAS-Objekt mit Rebuild/Refit · A15 Sampler-Feedback-Emulation · A18 Vulkan-Shading-Rate-Bild (nur kompiliert, kein Treiber).

## Batch 6 — große Pakete
A37 Vulkan 1.2+ (Timeline-Semaphores, Dynamic Rendering, Sync2) · A38 Recording über Secondary Command Buffers ·
A21 Upscaling `vio_upscale` (eigener portabler Compute-Pfad; MetalFX dahinter (CI); DirectSR erst mit Retail-SDK) ·
A39 GPU-Video-Encoding über Interop (FFmpeg-Hardware-Encoder) · A22 Metal `MTLBinaryArchive` (CI) ·
A16 Rate Maps (CI) · A17 Metal-Tensoren über `'msl'`-Override (CI, M5-Hardware nicht lokal).

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
- [x] Batch 2 (Branch `feat/open-items-batch2`): A31, A32, A12, A23, B4/B5, A11, A10, A27 — Tests 174–183. · [x] Batch 3 (Branch `feat/open-items-batch3`): A4, A6, A7, A8 — Tests 184–187. · [ ] Batch 4 · [ ] Batch 5 · [ ] Batch 6 · [ ] Batch 7
