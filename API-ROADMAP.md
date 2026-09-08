# API-ROADMAP — vio-API über den GL-3.3-Kern hinaus (alle Backends)

Status: 🚧 R1, R2, R7 (inkl. Async) umgesetzt (2026-09-08, php-vio 2.10-Paket); R3–R9 offen. Ergänzt `METALGPU-REPLACEMENT-PLAN.md`
(dessen Phase 1 zuerst läuft — Cube-RT, `depth_write`, RT-Readback, Headless-Größen,
Pipeline-Destruktor). Diese Roadmap enthält die Features, die **kein** vio-Backend heute
exponiert, obwohl Metal, D3D11/12, Vulkan und (meist) OpenGL sie nativ können.

Grundregeln
- Jedes Feature wird für **Metal, OpenGL, D3D11, D3D12** in einem PR umgesetzt; Vulkan
  (3D-Stub) bekommt `VIO_FEATURE_* = 0` + No-op-Slots. Kein Backend läuft der Matrix voraus.
- Jedes Feature hat ein `VIO_FEATURE_*`-Flag, einen Eintrag in
  `tests/core/074_backend_capability_matrix.phpt` und einen Pixel-/Readback-Test unter
  `tests/render3d/` auf `auto` (macOS → Metal, Linux → GL/Mesa, Windows → D3D12/WARP).
- Backend-spezifische Zweige in `php_vio.c` sind verboten; alles läuft über Vtable-Slots
  (Audit-Gate wie `070`: `tests/core/09x_audit_gate_no_backend_strcmp.phpt`, das neue
  `strcmp(ctx->backend->name, …)`-Vorkommen in `php_vio.c` zählt und einfriert).
- Reihenfolge nach PHPolygon-Nutzen. Aufwände sind Netto-Implementierung inkl. Tests.

Nächste freie Testnummer: 099 (090–093 Replacement-Plan, 094/095 Metal-Regressionen,
096 Storage-Images, 097 MRT, 098 Async-Compute).

---

## R1 — Multiple Render Targets (MRT)   ~2 Tage   ✅ umgesetzt

Stand: `vio_render_target(['attachments' => [VIO_FORMAT_*…]])` (1–4), `vio_render_target_texture($rt, $i)`,
`vio_read_render_target($rt, $face, $i)`, `vio_pipeline(['attachments' => […]])` (nur D3D12 braucht es),
`VIO_FEATURE_MRT`, Test `097_mrt`. Metal + OpenGL auf Hardware verifiziert; D3D11/D3D12 blind
(Windows-CI). Formate: RGBA8, RGBA16F, RGBA32F, R11G11B10F, RG16F, R16F, R32F, R8 — Readback über
den gemeinsamen Konverter `vio_rt_convert_to_rgba8()`. Cube-RTs bleiben Single-Attachment; die
`depth`-Option entfällt (Depth wird wie bisher immer angelegt). D3D12-`vio_read_render_target` ist
weiterhin Follow-up. Abweichung vom Entwurf: kein `create_render_target_ex` — die Backends lesen
`rt->attachment_count/formats` direkt aus dem Objekt.

Nutzen: echter G-Buffer für SSAO/SSR (heute mehrere Pässe), Deferred Lighting.

```php
$gb = vio_render_target($ctx, ['width'=>W,'height'=>H,
        'attachments' => [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F], 'depth' => true]);
vio_bind_render_target($ctx, $gb);           // Fragment: layout(location=0..2) out
$albedo = vio_render_target_texture($gb, 0); // 2. Parameter: Attachment-Index (Default 0)
```
- `vio_types.h`: `vio_format_pixel { VIO_FORMAT_RGBA8, RGBA16F, RGBA32F, R11G11B10F, RG16F, R16F, R32F, R8 }`,
  `VIO_FEATURE_MRT = 25`, `VIO_MAX_COLOR_ATTACHMENTS = 4`.
- `vio_render_target_object`: `int attachment_count; int formats[4];` + je Backend 4 Farb-Handles
  (`metal_color_texture[4]`, `color_texture[4]` GL, `d3d11_rtv[4]`, `d3d12_rtv_heap` mit 4 Descriptors).
  `hdr` bleibt als Alias für `attachments => [RGBA16F]`.
- Vtable: `create_render_target(rt, w, h, hdr, depth_only)` → `create_render_target_ex(rt)` liest alles aus dem Objekt (alter Slot bleibt für Kompatibilität, ruft intern `_ex`).
- Pipeline-Variante (Metal PSO / D3D12 PSO) muss die Attachment-Formate kennen → Schlüssel um Format-Tupel erweitern; GL/D3D11 binden zur Laufzeit.
- Test `093_mrt.phpt`: 3 Attachments verschieden beschreiben, jedes per `vio_read_render_target($rt, i)` prüfen.

## R2 — Storage-Images (image2D / image3D)   ~2 Tage   ✅ umgesetzt

Stand: `vio_texture(['storage' => true])` / `vio_texture_3d([... 'storage' => true])` (`data` optional →
nullinitialisiert), `vio_compute_bind_image($ctx, $cp, $tex, $slot, $access)`, `VIO_FEATURE_STORAGE_IMAGE`,
Test `096_storage_image`. Nur RGBA8 (`layout(rgba8)`); kein `format`-Parameter. Metal verifiziert,
GL läuft auf Linux-CI (Mesa 4.5), D3D11/D3D12 blind. Alle GL-Texturen nutzen jetzt das sized
`GL_RGBA8` (Voraussetzung für Image-Load/Store). Vulkan: Flag 0.

Nutzen: GPU-Partikel, Post-FX in Compute (Blur/Tonemap), SDF-Bake direkt in eine 3D-Textur.

```php
$tex = vio_texture($ctx, ['width'=>W,'height'=>H,'format'=>VIO_FORMAT_RGBA8,'storage'=>true]);
vio_compute_bind_image($ctx, $cp, $tex, 0, VIO_COMPUTE_WRITE);   // layout(binding=0, rgba8) uniform image2D
vio_compute_dispatch($ctx, $cp, ceil(W/8), ceil(H/8), 1);        // 2D/3D-Dispatch → siehe R7
```
- `VIO_FEATURE_STORAGE_IMAGE = 26`; Textur-Flag `storage` → Metal `MTLTextureUsageShaderWrite`, GL `glBindImageTexture` (4.2, macOS nie), D3D UAV, Vulkan `STORAGE_BIT`.
- Reflection liefert `storage_images` bereits (`vio_shader_reflect`); Metal-Compute pinnt sie wie Buffer per `msl_texture` (Compute-Transpile in `metal_cs_spirv_to_msl` auf die generische Renumbering-Routine umstellen).
- Vtable: `compute_bind_image(pipeline, backend_texture, slot, access)`.
- Test `094_storage_image.phpt`: Compute schreibt Gradient, Fullscreen-Sample liest ihn.

## R3 — Stencil   ~1 Tag

Nutzen: Portale, Outlines, Decal-Masken, Spiegel-Clipping.

```php
vio_pipeline($ctx, ['shader'=>$s, 'stencil' => ['test'=>VIO_STENCIL_EQUAL, 'ref'=>1, 'pass'=>VIO_STENCIL_KEEP,
                    'fail'=>VIO_STENCIL_KEEP, 'zfail'=>VIO_STENCIL_KEEP, 'read_mask'=>0xFF, 'write_mask'=>0xFF]]);
vio_clear($ctx, r,g,b,a, ['stencil' => 0]);      // optionales 6. Argument
```
- Depth-Format überall auf Depth32Float_Stencil8 (Metal) / D24S8 (GL, D3D — D3D11 hat es schon) / D32S8 (Vulkan) — für Swapchain **und** RTs (`'stencil' => true` bei RTs, sonst weiterhin reine Depth).
- `VIO_FEATURE_STENCIL = 27`. Test `095_stencil.phpt`: Maske schreiben, zweiter Draw nur innerhalb.

## R4 — Index-/Vertex-Formate: uint16-Indices, Texture-Arrays, Kompression   ~1½ Tage

- `vio_mesh(['indices'=>…, 'index_format'=>VIO_INDEX_UINT16])` → halbiert Index-Bandbreite bei Mobile/iOS; `vio_mesh_object.index_format`; Metal `MTLIndexTypeUInt16`, GL `GL_UNSIGNED_SHORT`, D3D `R16_UINT`.
- `vio_texture(['layers'=>N, 'data'=>…])` → `sampler2DArray`; Metal `2DArray`, GL `GL_TEXTURE_2D_ARRAY`, D3D `ArraySize`.
- `vio_texture(['file'=>'x.ktx2'])` → BC7 (Desktop) / ASTC (Apple). Erst KTX2-Container-Parser (vendored `libktx`-light oder eigener Minimalparser), Format-Mapping per Backend; `VIO_FEATURE_TEXTURE_COMPRESSION_BC/ASTC`.
- Tests `096_index_uint16.phpt`, `097_texture_array.phpt`.

## R5 — GPU-Timestamps   ~1 Tag

Nutzen: Frame-Time pro Pass für den F3-Overlay und den `GraphicsAutoTuner` (heute nur CPU-Zeit).

```php
vio_gpu_timestamp($ctx, 'shadow_begin');  …  vio_gpu_timestamp($ctx, 'shadow_end');
$t = vio_gpu_timings($ctx);   // ['shadow' => ms, …], Ergebnisse des VORLETZTEN Frames (kein Stall)
```
- Metal `MTLCounterSampleBuffer` (Timestamp) + `sampleCountersInBuffer` am Encoder-Anfang/-Ende (bei Encoder-Neuöffnung Sample setzen); D3D11/12 `TIMESTAMP`-Queries + `TIMESTAMP_DISJOINT`/`GetTimestampFrequency`; GL `glQueryCounter(GL_TIMESTAMP)`; Vulkan `vkCmdWriteTimestamp`.
- Ring über 3 Frames wie der Metal-Uniform-Ring. `VIO_FEATURE_GPU_TIMESTAMPS = 28`. Test `098_gpu_timestamps.phpt` (Werte ≥ 0, Namen vollständig).

## R6 — Indirect / Multi-Draw   ~1½ Tage

Nutzen: GPU-Culling (Compute schreibt Draw-Args), Vegetation/Partikel ohne CPU-Roundtrip — die konsequente Fortsetzung von Path B.

```php
$args = vio_storage_buffer($ctx, ['size'=>N*20, 'stride'=>20]);   // {indexCount, instanceCount, firstIndex, baseVertex, baseInstance}
vio_draw_indexed_indirect($ctx, $mesh, $args, $offset, $drawCount);
```
- Metal `drawIndexedPrimitives:indirectBuffer:` (Multi-Draw per Loop oder ICB), D3D12 `ExecuteIndirect` (Command Signature), D3D11 `DrawIndexedInstancedIndirect` (Loop), GL `glMultiDrawElementsIndirect` (4.3) / `glDrawElementsIndirect` (4.0), Vulkan `vkCmdDrawIndexedIndirect`.
- `VIO_FEATURE_INDIRECT_DRAW = 29`. Test `099_indirect_draw.phpt` (Compute füllt Args, 4 Quads).

## R7 — Compute-Dispatch-Geometrie + Async   ~½ Tag   ✅ umgesetzt

Stand: Metal liest `local_size` aus der SPIR-V-ExecutionMode (`metal_cs_spirv_to_msl`) und dispatcht
damit statt (64,1,1); GL/D3D waren bereits implizit korrekt. `vio_compute_dispatch(…, ['async' => true])`
zeichnet den Dispatch innerhalb von `vio_begin`/`vio_end` in den Frame-Command-Stream auf (Metal:
Compute-Encoder zwischen den Render-Encodern desselben Command-Buffers; D3D12: Frame-Command-List mit
UAV-Barrier, Descriptor-Ring aus 16 Blöcken, Graphics-State wird danach wiederhergestellt; GL/D3D11:
Queue ist ohnehin in-order). `vio_compute_wait($ctx)` und `vio_storage_buffer_read()` fencen. Außerhalb
eines Frames läuft `async` synchron. Test `098_async_compute`.

- `local_size` aus der Reflection lesen statt `(64,1,1)` festzuverdrahten (Metal `threadsPerThreadgroup`; GL/D3D implizit). Erlaubt 2D/3D-Kernel (R2 braucht das).
- Optional `vio_compute_dispatch(…, ['async'=>true])` + `vio_compute_wait($cp)`: Dispatch im Frame-Command-Buffer statt eigenem synchronem Buffer (Metal: gleicher `MTLCommandBuffer`, Compute-Encoder zwischen den Render-Encodern; D3D12: gleiche Command-List + UAV-Barrier).

## R8 — Pipeline-Cache auf Platte   ~1 Tag

Nutzen: Shader-Warmup beim Start (heute jeder Start kompiliert GLSL→SPIR-V→MSL/HLSL→PSO).
- `vio_pipeline_cache_load($ctx, $path)` / `vio_pipeline_cache_save($ctx, $path)`; Schlüssel = Hash(GLSL + Pipeline-Desc + Backend + Treiber-Version).
- Metal `MTLBinaryArchive`, D3D12 `CachedPSO`-Blob (+ `ID3D12PipelineLibrary`), Vulkan `VkPipelineCache`; D3D11/GL: nur SPIR-V→HLSL/GLSL-Transpilat cachen (spart glslang + SPIRV-Cross, den größeren Teil).

## R9 — Gated High-End-Features (nur mit Fallback im Engine-Code)

Nativ nur teilweise verfügbar → `VIO_FEATURE_*` + Engine-Fallback verpflichtend.
- **Mesh-Shader** (Metal 3, D3D12 SM6.5, `VK_EXT_mesh_shader`; **nicht** D3D11/GL): `vio_shader(['mesh'=>…, 'task'=>…])`. Braucht glslang `GL_EXT_mesh_shader` + SPIRV-Cross-MSL-Support (vorhanden).
- **Ray-Tracing** (Metal Apple Silicon, DXR, `VK_KHR_ray_tracing`): Query-Form (`rayQuery` im Fragment-Shader) ist der kleinste gemeinsame Nenner — Acceleration-Structure-Objekt `vio_accel($ctx, [$mesh…])` + `vio_bind_accel`. Ersetzt langfristig Teile des Fieldtracing.
- **Upscaling** (MetalFX / DirectSR / FSR-Compute): `vio_upscale($ctx, $lowResRt, $outRt, ['mode'=>'spatial'|'temporal'])` — FSR-Compute als portable Basis, MetalFX/DirectSR dahinter.

Diese drei erst nach R1–R8 und nur mit konkretem PHPolygon-Bedarf.

---

## Reihenfolge und Releases

| Release | Inhalt |
|---|---|
| php-vio 2.9 | Replacement-Plan Phase 1 (Cube-RT, depth_write, RT-Readback, Headless-Größen, Pipeline-Destruktor) |
| php-vio 2.10 | R1 MRT, R2 Storage-Images, R7 Dispatch-Geometrie |
| php-vio 2.11 | R3 Stencil, R4 uint16/Arrays, R5 Timestamps |
| php-vio 2.12 | R6 Indirect, R8 Pipeline-Cache |
| später | R9 nach Bedarf |

Jeder Schritt: Feature-Matrix in CLAUDE.md nachziehen, `074` erweitern, Windows-CI-Liste
in `build.yml` um die neuen `render3d/`-Tests ergänzen.
