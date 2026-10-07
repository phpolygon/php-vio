# SHADER-FEATURES-PLAN — moderne Shader-Features auf allen Backends

Stand 2026-10-06. Ziel: die Features von Shader Model 6.0–6.9 (D3D12) und ihre Gegenstücke auf
Metal, Vulkan und OpenGL als portable vio-Features — GLSL bleibt die Quelle, jedes Feature hat ein
`VIO_FEATURE_*`-Flag, einen `_all_backends`-Test und läuft auf jedem Backend, das es kann. Arbeitsweise
**TDD**: Test zuerst (rot), dann Implementierung, dann Flag in `074` pinnen.

## Ausgangslage

| Baustein | Stand |
|---|---|
| OpenGL-Kontext-Leiter 4.6 → 3.0, `vio_gl.caps` | ✅ (`vio_window.c`, `vio_opengl.c`) |
| **Metal-Versionsleiter** MSL 4.1 → 2.0, `vio_mtl.caps`, `vio_backend_info()` | ✅ `a17e588` (Tests 150, 151) |
| **D3D12 Shader Model 6** (höchstes 6.x aus Device ∩ DXC, SPIRV-Cross auf dasselbe Profil, DXC-Stage-Probe, `VIO_FEATURE_SUBGROUP`) | 🚧 Branch `feat/d3d12-shader-model-6` (`b0fcfc1`, Test 149, CI erzwingt DXC) |
| Vulkan: Instanz API 1.1, Device-Features einzeln abgefragt | ✅, aber keine Leiter-/Caps-Auskunft |

Geprüft auf Apple M5 / macOS 27 (Metal 4, Apple10): GLSL → glslang → SPIRV-Cross → MSL → Treiber
kompiliert für Subgroups, Quad-Ops, Barycentrics, `float16_t`, Base Vertex/Instance, Mesh-/Task-Shader,
Ray Query, Bindless (getrennte `texture2D[]` + `sampler`), Tensoren (MSL 4.0, `matmul2d`). **Nicht**:
Compute-Derivate (Treiber: „unsupported builtin"), 64-Bit-Atomics aus GLSL (SPIRV-Cross: „MSL currently
does not support 64-bit atomics" — handgeschriebenes MSL läuft), Raytracing-Pipeline (`traceRayEXT`).

## Grundregeln (gelten für jede Phase)

1. **Flag + Gate**: jedes Feature bekommt `VIO_FEATURE_*` (nächste freie: 46, `SUBGROUP` = 45 auf dem
   SM6-Branch). Das Backend leitet es aus seinen Caps ab: GL aus `vio_gl.caps` (Version ∨ Extension),
   Metal aus `vio_mtl.caps` (Device ∧ MSL-Stufe), D3D12 aus `CheckFeatureSupport` ∧ Shader-Model-Profil,
   Vulkan aus Device-Features/Extensions. Kein `@available`/Versionsvergleich im Feature-Code.
2. **Kein Backend-Zweig in `php_vio.c`** (Audit-Gate 099). Neue Fähigkeiten über Vtable-Slots.
3. **GLSL-Extension als Quelle** (`GL_KHR_shader_subgroup_*`, `GL_EXT_fragment_shader_barycentric`, …).
   glslang bekommt das Target, das die Extension braucht (heute: SPIR-V 1.3 nur für Subgroup-Shader,
   auf dem SM6-Branch), sonst bleibt SPIR-V 1.0 für die SPIR-V-Umbauten.
4. **Test**: `tests/<thema>/NNN_*_all_backends.phpt`, Ergebnis unabhängig von Wave-Größe/Readback-
   Orientierung, `skip (no …)` wo das Flag 0 ist. Metal-Tests laufen zusätzlich über die Leiter
   (`msl_version` unter der Mindeststufe ⇒ Flag 0 ⇒ Stage wird abgelehnt, nicht falsch gerendert).
5. **Doku**: Feature-Matrix in `CLAUDE.md`, Cap-Tabelle der Metal-Leiter, `vio.stub.php`.

Aufwand: **S** ≤ 1 Tag, **M** einige Tage, **L** eigener Sub-Plan.

## Phase 0 — Infrastruktur fertigstellen (S–M)

- **0a** SM6-Branch mergen (CI-Beleg auf WARP: 119/144/149 mit `VIO_REQUIRE_SM6=1`).
- **0b** `describe`-Slot (`vio_backend_info`) für **D3D12** (Feature-Level, SM-Profil, Optionen-Tiers:
  Wave-Ops, VRS, Mesh, Raytracing, Sampler Feedback, Resource Binding), **Vulkan** (API-Version,
  Extensions, Subgroup-Properties) und **OpenGL** (Version, GLSL, Caps — `vio_gl_info` bleibt).
- **0c** D3D12-Leiter festnageln wie Metal: `vio_create(['shader_model' => 62])` / `VIO_D3D12_SHADER_MODEL`
  pinnt das Profil (6.0 … Maximum), damit Feature-Gates je Profil testbar sind.
- **0d** **Agility SDK** (D3D12, optional): `D3D12SDKVersion`/`D3D12SDKPath`-Exporte über eine
  Lader-DLL neben `php.exe`; Voraussetzung für SM 6.9 auf Windows-Builds ohne neueste Runtime.
  Ohne Agility bleibt das Maximum, was die System-Runtime meldet.
- Tests: 150-Muster für D3D12/Vulkan/GL (`vio_backend_info` je Backend), Profil-Pinning wie 151.

## Phase 1 — Shader-Intrinsics über GLSL-Extensions (S je Feature)

Reiner Übersetzungs-/Gate-Kram, keine neue Ressourcen-API. Reihenfolge = Nutzen/Aufwand.

| # | Feature (`VIO_FEATURE_*`) | D3D12 | Vulkan | Metal | OpenGL | Nutzen / Hardware |
|---|---|---|---|---|---|---|
| 1a ✅ | Subgroups `SUBGROUP` (basic/vote/ballot/arithmetic/shuffle, Compute + Fragment) | ✅ SM 6.0 + `WaveOps` | ✅ `VkPhysicalDeviceSubgroupProperties` (Stages + Operations) | ✅ `simd_group` (MSL **2.2** — `threads_per_simdgroup` im Fragment-Shader, per Leiter gefunden; Mac2/Apple7) | ✅ `GL_KHR_shader_subgroup` (Mesa, NV) | Reduktionen/Culling ohne Shared Memory; jede DX12-Klasse-GPU |
| 1b ✅ | Quad-Ops `SUBGROUP_QUAD` (Fragment-Stage) | SM 6.0 | `subgroupQuadOperationsInAllStages` / Quad-Bit | `quad_group` (Mac2/Apple4) | KHR-Ext | Nachbarpixel ohne Derivate (Filter, Post-FX) |
| 1c ✅ | Barycentrics `BARYCENTRICS` | SM 6.1 + `OPTIONS3.BarycentricsSupported` | `VK_KHR_fragment_shader_barycentric` | `barycentrics` (MSL 2.2) | `GL_NV_fragment_shader_barycentric` / AMD | Visibility Buffer, Wireframe ohne GS; Turing+/RDNA2+/Arc/Apple |
| 1d ✅ | 16-Bit-Typen `SHADER_FLOAT16` | SM 6.2 + `OPTIONS4.Native16BitShaderOpsSupported`, DXC `-enable-16bit-types`, SPIRV-Cross-HLSL-Option | `shaderFloat16` + `storageBuffer16BitAccess` | `half` (immer) | `GL_AMD_gpu_shader_half_float` / NV | halber Registerdruck, 2× FP16 auf Turing+/Vega+/Intel; auf Apple mehr Occupancy |
| 1e ✅ | Base Vertex/Instance `BASE_VERTEX` | SM 6.8 `SV_StartVertexLocation`, darunter Root-Konstante je Draw | `shaderDrawParameters` (1.1) | `[[base_vertex]]`/`[[base_instance]]` (Mac2/Apple3) | GL 4.6 / `ARB_shader_draw_parameters` | `gl_BaseVertex` für `vio_draw_indirect` mit Offsets; alle |
| 1f ✅ (ohne Metal) | 64-Bit-Atomics `ATOMIC64` | SM 6.6 + `OPTIONS9`/`OPTIONS11` Int64-Atomics | `shaderBufferInt64Atomics`, `VK_EXT_shader_image_atomic_int64` | `atomic64` (MSL 3.1, Apple9) — **Blocker**: SPIRV-Cross-MSL; Upstream-Patch oder MSL-Textumbau | `GL_NV_shader_atomic_int64` | Software-Rasterisierung (Nanite-Art), Visibility Buffer; Turing+/RDNA2+/M3+ |
| 1g ✅ | Compute-Derivate `COMPUTE_DERIVATIVES` | SM 6.6 | `VK_KHR_compute_shader_derivatives` | ❌ (Treiber lehnt ab) → 0 | `GL_NV_compute_shader_derivatives` | Mip-Auswahl in Compute (Deferred Texturing) |
| 1h ⏸ | 6.7-Texturops: `SampleCmpLevel`, Raw Gather, Integer-Sampling | SM 6.7 + `OPTIONS14` | core / `shaderImageGatherExtended` | MSL `sample_compare(level)` | GL 4.x | Shadow-Sampling in allen Stages |

**1h zurückgestellt (2026-10-07):** vio hat keine Tiefentexturen mit Mip-Kette (depth_only-Targets nie
`mipmaps`, `php_vio.c` 9707/9976), ein explizites LOD/Gradient am Shadow-Sampler wäre ohne sichtbare Wirkung.
Außerdem verwirft SPIRV-Cross das LOD bei Shadow-Samplern für HLSL auf jeder SM-Stufe (`SampleCmpLevelZero`,
mit SPIRV-Cross-CLI geprüft). Voraussetzung wäre zuerst ein Feature „Tiefentexturen mit Mips“ plus ein
SPIRV-Cross-Patch (`SampleCmpLevel` ab SM 6.7, `SampleCmpGrad` ab 6.8).

Tests (ab 152): je Feature ein `_all_backends`-Test mit wave-größen-unabhängiger Prüfung (Muster 149),
Barycentrics über Farbinterpolation an Dreiecksecken, Float16 über Ergebnis- und Präzisionsgrenzen,
Base Vertex über zwei indirekte Draws mit `baseVertex`/`baseInstance`, Atomic64 über `atomicMax` eines
(Tiefe, ID)-Paars.

## Phase 2 — View Instancing / Multiview (M) ✅ (2026-10-07)

Umgesetzt mit `vio_shader(['view_count' => N])` statt einer Pipeline-Option: GL braucht die View-Zahl schon
in der GLSL (`num_views`), alle anderen Backends übernehmen sie aus dem Shader. Metal nutzt SPIRV-Cross'
Instancing-Emulation statt Vertex Amplification (SPIRV-Cross erzeugt kein `[[amplification_id]]`). Test 158.

`VIO_FEATURE_MULTIVIEW` (heute überall 0). API: `vio_pipeline(['view_count' => N])`, Ziel = Layered-RT
(`'layers' => N`), GLSL `GL_EXT_multiview` + `gl_ViewIndex`.
- D3D12: View Instancing (SM 6.1, `OPTIONS3.ViewInstancingTier`), `D3D12_VIEW_INSTANCING_DESC` in der PSO.
- Vulkan: `VkRenderPassMultiviewCreateInfo` (core 1.1).
- Metal: Vertex Amplification (`vertex_amplification`, MSL 2.2) — SPIRV-Cross emuliert sonst über
  Instancing + Layer; Ziel ist die echte Amplification (`setVertexAmplificationCount:`).
- OpenGL: `GL_OVR_multiview2`; sonst Flag 0.
- Nutzen: Stereo/VR, mehrere Shadow-Kaskaden pro Draw. Pascal+ in Hardware, sonst emuliert.
- Test: 2 Views in 2 Layer, `gl_ViewIndex` färbt, Readback je Layer.

## Phase 3 — Variable Rate Shading Tier 2 (M)

Heute: Tier 1 (`vio_set_shading_rate`, D3D12/Vulkan). Neu: Rate je Primitiv (`gl_PrimitiveShadingRateEXT`
↔ `SV_ShadingRate`, SM 6.4) und Rate-Bild (`vio_set_shading_rate_image($ctx, $tex)`).
- D3D12 `VariableShadingRateTier >= 2`; Vulkan `primitiveFragmentShadingRate` / `attachmentFragmentShadingRate`.
- Metal: Rasterization Rate Maps sind ein anderes Modell (physisch kleineres Ziel) — eigenes,
  bewusst getrenntes Feature `VIO_FEATURE_RASTER_RATE_MAP` evaluieren, nicht in VRS pressen.
- GL: `GL_NV_shading_rate_image` → sonst 0.

## Phase 4 — Bindless / dynamische Ressourcen (L, eigener Sub-Plan `BINDLESS-PLAN.md`)

Größter Architekturhebel: ersetzt Pending-Bind-Tabelle und GL-Unit-Mapping für Materialsysteme.
- API-Skizze: `vio_texture_index($tex): int` (stabiler Heap-Index), GLSL `layout(set=1) uniform texture2D
  u_textures[]; uniform sampler u_samplers[];` + `nonuniformEXT`. **Getrennte** Texturen/Sampler —
  SPIRV-Cross-MSL kann kombinierte `sampler2D[]` mit Laufzeitgröße nicht (geprüft).
- D3D12: SM 6.6 `ResourceDescriptorHeap[]` bzw. unbounded SRV-Table (`ResourceBindingTier 3`).
- Vulkan: Descriptor Indexing (1.2), `UPDATE_AFTER_BIND`, variable Descriptor Count.
- Metal: Argument Buffers Tier 2 (`argument_buffers_tier2`), `useResources:` / Residency Sets (Metal 4).
- OpenGL: `ARB_bindless_texture` (NV/AMD) — sonst Flag 0, Fallback bleibt die Bind-Tabelle.
- Tests: 256 Texturen, Material-Index pro Instanz, nicht-uniformer Index im Fragment-Shader.

## Phase 5 — Mesh- und Task-/Amplification-Shader (L)

`VIO_FEATURE_MESH_SHADER`. API: `vio_shader(['task' => …, 'mesh' => …, 'fragment' => …])`,
`vio_draw_mesh_tasks($ctx, $x, $y, $z)` + indirekte Variante über `vio_storage_buffer(['indirect'])`.
- D3D12: SM 6.5, `OPTIONS7.MeshShaderTier`, `D3D12_PIPELINE_STATE_STREAM` (Mesh-PSO), `DispatchMesh`.
- Vulkan: `VK_EXT_mesh_shader`, `vkCmdDrawMeshTasksEXT`.
- Metal: `mesh_shaders` (MSL 3.0, Metal3 + Apple7/Mac2), `MTLMeshRenderPipelineDescriptor`,
  `drawMeshThreadgroups:` — Übersetzung geprüft (Mesh + Task kompilieren auf M5).
- OpenGL: `GL_NV_mesh_shader` (SPIRV-Cross emittiert EXT) → realistisch 0.
- Nutzen: Meshlet-Culling + LOD auf der GPU, passt zu Indirect Draw/GPU-Culling. Turing+/RDNA2+/Arc/M1+.
- Tests: ein Meshlet-Dreieck, Task-Shader verwirft die Hälfte der Gruppen, Payload kommt an.

## Phase 6 — Raytracing (L, eigener Sub-Plan `RAYTRACING-PLAN.md`)

`VIO_FEATURE_RAYTRACING` aufteilen in `RAY_QUERY` (6a) und `RAYTRACING_PIPELINE` (6b).

**6a Inline-Raytracing / Ray Query (zuerst)** — RT-Schatten/-AO ohne eigene Pipeline.
- Neue API: `vio_acceleration_structure($ctx, ['meshes' => [...]])` (BLAS), `vio_tlas($ctx, $instances)`,
  Rebuild/Refit, Bind als Ressource; GLSL `GL_EXT_ray_query`.
- D3D12: DXR 1.1 (SM 6.5, `OPTIONS5.RaytracingTier >= 1_1`), `RayQuery<>`; SPIRV-Cross-HLSL-Unterstützung
  für Ray Query im Prototyp prüfen, sonst HLSL-Override-Weg.
- Vulkan: `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`.
- Metal: `raytracing` (MSL 2.3) / `raytracing_from_render` (2.4), `MTLAccelerationStructure`,
  `intersector` — Übersetzung geprüft. Hardware-RT ab M3, M5 dritte Generation.
- OpenGL: 0.
- Hardware: RTX 20+, RX 6000+, Arc, Apple M3+ (Apple ab M1 per Compute langsamer).

**6b Raytracing-Pipeline (Raygen/Hit/Miss)** — SPIRV-Cross übersetzt `traceRayEXT` nicht (geprüft, MSL),
HLSL-seitig ebenso nicht verlässlich ⇒ HLSL-/MSL-Quellen je Backend (wie der Stage-Override), D3D12
State Objects + Shader Tables, Vulkan RT-Pipeline aus GLSL, Metal Intersection Functions + Visible
Function Tables. Erst nach 6a und nur mit konkretem Bedarf (Path Tracing).

**6c SM 6.9 Shader Execution Reordering + Opacity Micromaps** — nur D3D12 (Agility SDK + DXC ≥ 1.9,
Phase 0d) und Vulkan (`VK_EXT_ray_tracing_invocation_reorder`, `VK_EXT_opacity_micromap`); Metal hat
kein Gegenstück ⇒ Flags dort 0. Nutzen auf RTX 40/50 (Hardware-Reorder, OMM-Traversal).

## Phase 7 — Sampler Feedback / Texture Streaming (L)

`VIO_FEATURE_SAMPLER_FEEDBACK`: welche Mips/Kacheln tatsächlich gelesen wurden.
- D3D12: SM 6.5 + `OPTIONS7.SamplerFeedbackTier` (Turing+, Arc).
- Vulkan: Sparse Residency + Residency-Status im Shader (kein 1:1-Feedback).
- Metal: Sparse Textures (`sparseTileSizeInBytes`) + Zugriffszähler.
- Gemeinsame API erst nach einem Prototyp festlegen (Streaming-Manager in PHPolygon ist der Abnehmer).

## Phase 8 — Neural Shading: Long Vectors / Cooperative Vectors / Tensoren (L, evaluieren)

- D3D12: SM 6.9 Long Vectors; Cooperative Vectors (teils Preview) — RTX 40/50 Tensor-Kerne.
- Vulkan: `VK_NV_cooperative_vector`, `VK_KHR_cooperative_matrix`.
- Metal: `tensors` (MSL 4.0, `MTLTensor`, Metal Performance Primitives `matmul2d`) — M5 mit Neural
  Accelerators je GPU-Kern; geprüft: Pipeline baut.
- Keine GLSL-Quelle ⇒ backend-native Quellen (`'msl' => …`, `'hlsl' => …`) über einen erweiterten
  Stage-Override; Feature-Flag nur, wo eine portable Form existiert (`GL_KHR_cooperative_matrix`).

## Phase 9 — Work Graphs (nur D3D12, zurückgestellt)

SM 6.8, RDNA3/Ada. Kein Metal-/GL-Gegenstück, Vulkan nur `VK_AMDX_shader_enqueue`. Alternativen für
GPU-getriebene Arbeit: Indirect Draw (✅), Metal Indirect Command Buffers, Vulkan Device-Generated
Commands. Erst bei konkretem Bedarf.

## Nicht übernommen

- `[WaveSize(N)]` (6.6/6.8): Apple fest 32, sonst Feintuning — optional als Hint, kein Flag.
- `dot4add`/Int8 (6.4): über Phase 8 abgedeckt.
- 64-Bit-Integer allgemein (6.0): meist emuliert, kein Nutzen ohne Atomics (1f).

## Reihenfolge und Abhängigkeiten

```
0a SM6 mergen ─┬─ 1a Subgroups (D3D12 ✅, Metal, Vulkan, GL) ── 1b Quad
               ├─ 1c Barycentrics ── 1d Float16 ── 1e Base Vertex ── 1h Texturops
               └─ 0c SM-Pinning ── 1f Atomic64 (Metal: SPIRV-Cross-Patch)
0b describe-Slot überall ── 2 Multiview ── 3 VRS Tier 2
4 Bindless ── 5 Mesh-Shader ── 6a Ray Query ── 6b RT-Pipeline ── 6c SER/OMM (0d Agility)
7 Sampler Feedback, 8 Neural Shading, 9 Work Graphs: nach Bedarf
```

Empfehlung: **0a → 1a (Metal/Vulkan nachziehen) → 1c/1d/1e** als erstes Paket (alles auf dem M5 und
in der CI testbar), danach **4 Bindless** oder **5 Mesh-Shader**, dann **6a Ray Query**.
