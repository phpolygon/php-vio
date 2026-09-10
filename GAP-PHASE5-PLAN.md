# GAP-PHASE5-PLAN — Folgearbeit aus dem D3D-VULKAN-GAP-PLAN, mit Engine-Gegenstücken

Status: umgesetzt (2026-09-10, Ausgangspunkt php-vio v2.11.0, PHPolygon v0.42.0). Alle zwölf Blöcke
sind in php-vio gemergt und per semantic-release veröffentlicht; Block 10 kam in vier PRs – 10a
3D-Pipeline, 10b MRT/MSAA-/Cube-Targets/Cubemaps/Mipmaps, 10c Texture-Arrays/BC/KTX2 + Variable Rate
Shading + `auto`-Rückfall, 10d HDR10-Swapchain. Die Engine-Gegenstücke liegen im PHPolygon-Branch
`feat/gap-phase5-engine`.
Ziel: die in `D3D-VULKAN-GAP-PLAN.md` Phase 5 gelisteten, nativ vorhandenen Fähigkeiten in
vio verdrahten UND in der Engine (`VioRenderer3D`, `GraphicsSettings`, `EngineConfig`,
`PerfProfiler`) einen echten Abnehmer geben. Jeder Block ist ein eigener Branch/PR gegen
`main`, baut lokal (D3D11 + D3D12 auf Hardware, Vulkan-ICD, OpenGL), lässt die volle Suite
grün und bekommt seinen Pixel-/Readback-Test unter `tests/`. Metal wird blind editiert und
über die macOS-CI (`build.yml`) belegt.

Grundregeln wie im GAP-PLAN: Feature-Flag pro Fähigkeit, Eintrag in
`tests/core/074_backend_capability_matrix.phpt`, keine neuen
`strcmp(ctx->backend->name, …)`-Zweige in `php_vio.c` (Audit-Gate 099), Vtable-Slots statt
Backend-Zweigen. Testnummern ab **113**.

| # | Block | vio-API | Backends | Engine-Gegenstück | Test |
|---|---|---|---|---|---|
| 1 | **PSO-State: RT-MSAA auf D3D12 + Stencil** | `vio_pipeline(['samples' => N, 'stencil' => [...]])`, `VIO_FEATURE_STENCIL`; D3D12 `RENDER_TARGET_MSAA = 1` (MSAA-Farbe+Depth, Resolve beim Unbind/Readback wie D3D11) | D3D12 (MSAA), GL/D3D11/D3D12/Metal (Stencil) | `VioRenderer3D`: jede Pipeline, die ins Offscreen-Target zeichnet, trägt dessen `samples` (D3D12-PSO); `AntiAliasing::Msaa*` wirkt damit erstmals auf D3D12. Stencil: kein Engine-Abnehmer, nur API + Test | 113 (Stencil), 105 läuft auf D3D12 mit |
| 2 | **uint16-Indices** | automatisch in `vio_mesh` (alle Indizes < 65536 → 16-Bit-IB), `VioMesh::indexType()` | GL, D3D11, D3D12, Metal, (Vulkan mit Block 10) | keiner nötig (transparent); Engine-Test pinnt, dass die Standard-Meshes 16-Bit bekommen | 114 |
| 3 | **GPU-Timestamps** | `vio_gpu_frame_time($ctx): float` (ms des letzten abgeschlossenen Frames, Ring über 3 Frames), `VIO_FEATURE_GPU_TIMESTAMP` | D3D11 (`TIMESTAMP` + `DISJOINT`), D3D12, GL (`GL_TIMESTAMP` ≥ 3.3), Vulkan (`vkCmdWriteTimestamp`), Metal (`GPUStartTime/GPUEndTime`) | `PerfProfiler`-Sektion `render3d.gpu` (VioRenderer3D::endFrame), `PerfOverlay` zeigt CPU- und GPU-Zeit → CPU- vs. GPU-gebunden sichtbar | 115 |
| 4 | **Shader-/Pipeline-Cache** | `vio_create(['shader_cache' => dir])`: DXBC/DXIL nach Hash(HLSL, Profil, Flags) auf Platte (D3D11/D3D12), `VkPipelineCache`-Datei (Vulkan), D3D12 `ID3D12PipelineLibrary` | D3D11, D3D12, Vulkan | `EngineConfig::$shaderCachePath` (Default `<saves>/shader-cache`), gesetzt von `VioWindow`; Startzeit-Messung im Engine-Test | 116 |
| 5 | **Waitable Swapchain / Frame-Latenz** | `vio_create(['frame_latency' => 1..3])` → `FRAME_LATENCY_WAITABLE_OBJECT` + `SetMaximumFrameLatency`, Wait in `vio_begin` | D3D11, D3D12 (Vulkan: Präsentmodus bereits) | `GraphicsSettings::$lowLatency` → `frame_latency = 1`; Menü-Eintrag | 117 |
| 6 | **HDR-Swapchain** | `vio_create(['hdr_output' => true])` → `R10G10B10A2` + `SetColorSpace1(HDR10 ST2084)`, wenn Display/Adapter es kann; `vio_swapchain_info()['color_space']`, `VIO_FEATURE_HDR_OUTPUT` | D3D11, D3D12 (Vulkan `HDR10_ST2084_EXT` mit Block 10) | `GraphicsSettings::$hdrOutput`; Resolve-Shader (`passthrough_blit`, `fxaa`, `tonemap`) mit PQ-Kodierung (BT.2020, ST 2084, Paper-White-Nits-Einstellung) | 118 |
| 7 | **DXC / Shader Model 6** | D3D12 kompiliert über `dxcompiler.dll` (`IDxcCompiler3`, `vs_6_0/ps_6_0/cs_6_0`), Fallback FXC 5.1; `vio_create(['shader_model' => 5|6])`, `vio_gpu_info()['shader_model']` | D3D12 | keiner nötig (transparent); Engine-Doku | 119 |
| 8 | **Indirect Draw** | `vio_draw_indirect($ctx, $mesh, VioBuffer $args, int $maxCount = 1, ?VioBuffer $count = null)`; Argument-Layout = `VkDrawIndexedIndirectCommand` (5×uint32), `VIO_FEATURE_INDIRECT_DRAW` | D3D12 (`ExecuteIndirect`), D3D11 (`DrawIndexedInstancedIndirect`), GL 4.0/4.3 (`glDrawElementsIndirect`), Metal (`drawIndexedPrimitives:indirectBuffer`), Vulkan mit Block 10 | `DrawMeshInstanced::$indirectArgs`; `GpuParticleBaker`/Instanz-Compute schreibt `instanceCount` per Compute (GPU-Frustum-Cull) statt CPU-Readback — Fortsetzung Path B | 120 |
| 9 | **Texture-Arrays + BC-Kompression + KTX2** | `vio_texture(['layers' => N])`, Formate `VIO_FORMAT_BC1/BC3/BC4/BC5/BC7`, `vio_texture_ktx2($ctx, string $bytes, array $opts)`, `VIO_FEATURE_TEXTURE_ARRAY`, `VIO_FEATURE_TEXTURE_COMPRESSION_BC` | GL (S3TC/BPTC-Extensions), D3D11, D3D12, Metal (macOS), Vulkan mit Block 10 | `VioTextureManager` lädt `.ktx2` neben `.png` (bevorzugt, wenn vorhanden); `TextureQuality`-Stufen wählen den Mip-Offset | 121 |
| 10 | **Vulkan 3D-Pipeline** | `VIO_FEATURE_3D_PIPELINE/INSTANCED_DRAW/DEPTH_BIAS/RENDER_TARGET_HDR/DEPTH/MSAA/CUBE/MRT/VERTEX_STORAGE = 1` auf Vulkan: Graphics-Pipeline aus SPIR-V, Descriptor-Sets (UBO-Ring, Combined-Image-Sampler), `set_uniform`/`bind_texture`, Draw/Instancing, Depth-Buffer, RT-Varianten, `read_render_target`, Dynamic Rendering wenn ≥ 1.2/1.3 | Vulkan | `BackendConventions` für `vulkan` (Clip-Y, Tiefe 0..1, RT-Orientierung), `auto` darf Vulkan wieder vor OpenGL wählen; Engine-Bildtests laufen in der Linux-CI auf lavapipe | 122–124 |
| 11 | **D3D12 GPU-Mipmaps + Metal-Anisotropie** | `vio_generate_mipmaps` auf D3D12 per Compute-Downsample; Metal liest `desc->anisotropy` | D3D12, Metal (blind) | keiner nötig; 102/104 belegen | bestehende 102/104 |
| 12 | **Variable Rate Shading** | `vio_set_shading_rate($ctx, VIO_SHADING_RATE_1X1|1X2|2X1|2X2|4X4)`, `VIO_FEATURE_SHADING_RATE` (D3D12 Tier 1, `VK_KHR_fragment_shading_rate`) | D3D12, Vulkan mit Block 10 | `GraphicsSettings::$shadingRate`; `AdaptiveTierStack` senkt zuerst die Shading-Rate, bevor Render-Scale fällt | 125 |

**Nicht in diesem Plan:** Multiview / View-Instancing (kein Abnehmer ohne VR/Stereo);
Swapchain-MSAA (Flip-Model-Swapchains sind nicht multisampled — der Engine-Pfad rendert
ohnehin in ein MSAA-Offscreen-Target und blittet); Vulkan-Timeline-Semaphores/Sync2 nur,
soweit Block 10 sie braucht.

## Reihenfolge

1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 11 → 9 → 12 → 10. Blöcke 1–8 und 11 sind je ein Tag oder
weniger; 9 und 10 sind die grossen. Jeder Block: vio-PR (Release per Push auf `main`) →
Engine-PR (Release) → Spiel-Composer-Bump am Ende.

## Verifikation

Windows nativ (dieser Host): D3D11, D3D12, Vulkan (ICD), OpenGL. Linux/macOS: `build.yml`.
Engine: `tests/Rendering/*` auf `auto` (D3D12) + GL-Harness; Spiel: GPU-Bildtests.
