# METALGPU-REPLACEMENT-PLAN — php-metal-gpu durch vio-Metal ersetzen

Status: 📋 Entwurf (2026-09-08). Ziel: PHPolygon braucht auf macOS **nur noch php-vio**;
die Standalone-Extension `php-metal-gpu` (`ext-metal`, ~5.5k Zeilen C) und PHPolygons
Standalone-Renderer `MetalRenderer3D` (+ `MetalCubemapTarget`, `MetalOffscreenTarget`,
`MetalFxaaPass`, ~2.1k Zeilen PHP, eigene MSL-Shader) werden entfernt.

Voraussetzung (✅ erledigt, unreleased auf `main`): vio-Metal hat eine vollständige
3D-Pipeline mit D3D-Parität — Mesh/Shader/Pipeline/Draw, Uniforms, Texturen 2D/3D,
Cubemaps, Instancing (Path A + B), Render Targets (HDR, Depth-only mit `discard`, MSAA),
Swapchain-MSAA, eager `vio_clear`, Mid-Frame-Readback, Recording/Streaming, `gpu_info`,
`debug`, Intel-Storage. Tests `tests/backends/089`, 85/85 grün.

---

## 0. Ist-Analyse: Was ext-metal PHPolygon heute noch bietet, was vio nicht kann

Quelle: `phpolygon/src/Rendering/Metal*.php`, `PostProcess/MetalFxaaPass.php`,
`resources/shaders/source/{mesh3d,sky,fxaa}.metal`.

| ext-metal-Fähigkeit (PHPolygon-Nutzung) | vio heute | Lücke |
|---|---|---|
| **Environment-Cubemap als Render-Target**: Sky in 6 Faces rendern (`setColorAttachmentSlice`), 9 Mip-Level, `generateMipmaps`, Sampling mit `level(roughness * mipMax)` (`MetalCubemapTarget`, `updateEnvironmentCubemap`) | `vio_cubemap` nur CPU-Upload (`CubemapData`), keine Mips, kein Render-in-Face; `VioRenderer3D` nutzt `reflection_probe` per `texture()` ohne LOD | **Kernlücke** — einzige echte Fähigkeit, die dem vio-Pfad fehlt |
| Depth-State „test always / write off" für Sky-Pass (`skyDepthState`) | `depth_test => false` deaktiviert Test **und** Write gemeinsam | `depth_write`-Option fehlt (Sky funktioniert trotzdem, weil er zuerst gezeichnet wird; für Transparenz-Sortierung nötig) |
| MSAA-Offscreen + Resolve, FXAA/Blit-Present (`MetalOffscreenTarget`, `MetalFxaaPass`) | `VioOffscreenTarget` + `VioFxaaPass` existieren; Metal-MSAA-RTs seit heute echt | ✅ keine Lücke mehr |
| Headless `renderToImage()` über Shared-Textur-Readback | `VioRenderer3D::renderToImage` liest nach `vio_end` die **Swapchain**, nicht das RT → schlägt auf **allen** Backends fehl (Baseline OpenGL ebenfalls schwarz) | RT-Readback-API fehlt (`vio_read_render_target`) |
| Retina-korrekte Größen (Metal-Layer direkt) | headless: `vio_framebuffer_size()` = 2× Fensterpixel, Ziel ist 1× → PHPolygon-Viewport rendert ins falsche Quadrat (OpenGL **und** Metal) | Window-Layer-Bug |
| MetalFX / Ray-Tracing / Mesh-Shader | — | von PHPolygon **nicht** genutzt (nur als „future" kommentiert) → kein Blocker |

Alles andere, was `MetalRenderer3D` rendert (Lighting, Fog, Wind, Materials, Proc-Modes),
ist in `VioRenderer3D` eine **Obermenge** (zusätzlich Shadows/CSM, SSAO, SSR, Bloom,
Fieldtracing, Color-Grading). Der Metal-Renderer ist der ältere, kleinere Pfad.

---

## 1. Phase 1 — vio-API-Erweiterungen (alle Backends, Metal zuerst)

Prinzip: jede Erweiterung wird für **Metal, OpenGL, D3D11, D3D12** gleichzeitig
implementiert (Vulkan: Stub + `VIO_FEATURE_* = 0`), damit die Feature-Matrix nicht
wieder auseinanderläuft. Reihenfolge nach Blocker-Grad.

### 1.1 Cubemap-Render-Target + Mipmaps  (**Blocker**)

API:
```php
$rt = vio_render_target($ctx, ['cube' => true, 'size' => 256, 'mipmaps' => true, 'hdr' => false]);
vio_bind_render_target($ctx, $rt, ['face' => 0..5]);     // 3. Parameter neu (optional array)
/* … Sky-Fullscreen-Triangle mit inv(proj*faceView) zeichnen … */
vio_unbind_render_target($ctx);
vio_generate_mipmaps($ctx, $rt);                          // NEU: auch für VioTexture / VioCubemap
$cm = vio_render_target_cubemap($rt);                     // NEU: VioCubemap-Wrapper (borrowed), samplerCube
vio_bind_cubemap($ctx, $cm, $slot);                       // wie bisher
```
GLSL: `textureLod(u_environment_map, R, roughness * u_env_mip_max)` — funktioniert auf
allen Backends, sobald der Cubemap-Sampler mip-fähig ist (SPIRV-Cross übersetzt nach
`sample(level())` / `SampleLevel`).

Änderungen:
- `include/vio_types.h`: `VIO_FEATURE_RENDER_TARGET_CUBE = 23`, `VIO_FEATURE_MIPMAP_GEN = 24`.
- `include/vio_backend.h`: `bind_render_target(void *rt)` → zusätzlicher Slot
  `bind_render_target_face(void *rt, int face, int level)` (optional, NULL = kein Cube-RT);
  neuer Slot `generate_mipmaps(void *texture_or_cubemap_obj, int kind)`.
- `src/vio_render_target.h`: `int is_cube; int mip_levels; int bound_face;` + Backend-Felder
  (Metal: `metal_color_texture` wird `MTLTextureTypeCube` mit `mipmapLevelCount`; GL:
  `GL_TEXTURE_CUBE_MAP` + `glFramebufferTexture2D(face)`; D3D11: `ArraySize=6` + 6 RTVs;
  D3D12: 6 RTV-Descriptors + SRV `TEXTURECUBE`).
- `src/vio_cubemap.h`: `int mipmaps; int borrowed;` — Sampler mit Mip-Filter, wenn `mipmaps`.
  `vio_cubemap($ctx, ['faces'=>…, 'mipmaps'=>true])` erzeugt Mips auch für CPU-Cubemaps.
- Metal: Face-Bind = `colorAttachments[0].slice = face` im Render-Pass (`metal_open_encoder`
  bekommt `current_bound_face`), Depth-Attachment = eine geteilte 2D-Depth-Textur pro RT
  (wie `MetalCubemapTarget`), `generateMipmapsForTexture` per Blit-Encoder (schließt den
  offenen Encoder, blittet, öffnet mit `Load` neu — gleiches Muster wie Mid-Frame-Readback).
- Tests: `tests/render3d/090_cubemap_render_target.phpt` (6 Faces verschieden färben, per
  `textureLod` an einem Fullscreen-Quad je Face + LOD 0 und LOD max prüfen; auf `auto`),
  `074` um die zwei neuen Flags pro Backend.

### 1.2 Pipeline-State: `depth_write`, `color_mask`, erweiterte Blend-Modi  (klein)

```php
vio_pipeline($ctx, ['shader'=>$s, 'depth_test'=>true, 'depth_write'=>false,
                    'blend'=>VIO_BLEND_PREMULTIPLIED, 'color_mask'=>VIO_COLOR_RGB]);
```
- `vio_pipeline_desc`: `int depth_write` (Default 1), `int color_mask` (Default RGBA),
  `vio_blend_mode` um `PREMULTIPLIED`, `MULTIPLY`, `SCREEN`, `MIN`, `MAX`.
- Metal: `MTLDepthStencilDescriptor.depthWriteEnabled`, `colorAttachments[0].writeMask`,
  Blend-Faktoren. GL: `glDepthMask`, `glColorMask`, `glBlendFuncSeparate`. D3D: `DepthWriteMask`,
  `RenderTargetWriteMask`, `BlendDesc`.
- Test: `tests/render3d/091_pipeline_state.phpt` (Depth-Write-off: zweiter Draw hinter dem
  ersten bleibt sichtbar; Color-Mask RGB: Alpha bleibt Clear-Wert).

### 1.3 Render-Target-Readback  (Blocker für 3D-Pixel-VRT auf macOS)

```php
$rgba = vio_read_render_target($rt);            // top-down RGBA8 (HDR: nach 8 Bit gequantelt), auch mid-frame
```
- Vtable-Slot `read_render_target(void *rt, void *out_rgba)`; Metal: Blit der (Resolve-)
  Farbtextur in einen Shared-Buffer (Code aus `vio_metal_read_pixels` generalisieren);
  GL: `glReadPixels` vom FBO; D3D: Staging-Copy.
- `VioRenderer3D::renderToImage()` liest damit **das RT** statt der Swapchain → der
  vorbestehende Fehler (`VioRenderToImageTest` schwarz auf OpenGL und Metal) verschwindet.
- Test: `tests/render3d/092_render_target_readback.phpt` (Clear-Farbe + Dreieck im RT).

### 1.4 Headless-Größen konsistent machen  (Window-Layer)

- Headless-Kontexte: `vio_framebuffer_size()` / `vio_content_scale()` müssen die
  **tatsächliche** Zielgröße (1×, `config.width/height`) liefern — auf allen Backends.
  Heute: 2× vom versteckten GLFW-Fenster (Retina) bei 1×-Ziel → `VioRenderer3D` setzt
  Viewport 512×512 auf ein 256er-Ziel.
- Fix in `php_vio.c` (`vio_framebuffer_size`, `vio_content_scale`, `vio_pixel_ratio`,
  Metal-/GL-Zweig in `vio_begin`): `if (ctx->config.headless) { fb = logical; scale = 1 }`.
- Test: `tests/window/052_display_metrics.phpt` erweitern (headless → fb == window).

### 1.5 Hygiene, die die Migration sauber macht

- **Pipeline-Destruktor**: `vio_pipeline_object` bekommt `backend`; `vio_pipeline_free_object`
  ruft `destroy_pipeline(backend_pipeline)` (heute leaken PSOs auf allen Backends; Metal
  hat den Destruktor bereits, D3D11 auch). Vorsicht: bound-Pipeline beim Destroy → Backend
  setzt `current_pipeline = NULL` (Metal tut das).
- `vio_texture_update($tex, $rgba, ['x','y','w','h'])` — Sub-Region-Upload (Video-/
  Streaming-Texturen). Klein, alle Backends haben `replaceRegion`/`glTexSubImage2D`/
  `UpdateSubresource`. Nicht Blocker; aufnehmen, weil PHPolygon-Editor es braucht.

**Phase-1-Kontrakt**: `074_backend_capability_matrix` pinnt pro Backend die neuen Flags;
`089` bleibt grün; neue Tests 090–092 laufen auf `auto` (macOS → Metal, Linux → GL,
Windows → D3D12/WARP). `.github/workflows/build.yml` Windows-Liste um 090–092 ergänzen.

---

## 2. Phase 2 — PHPolygon: Environment-Cubemap auf vio portieren

- `VioRenderer3D::updateEnvironmentCubemap(SetSky)`: Port von
  `MetalRenderer3D::updateEnvironmentCubemap` auf die neue API (6 × `bind face` +
  Sky-Shader mit `u_inv_vp`; `vio_generate_mipmaps`; Sky-Hash-Cache aus
  `MetalCubemapTarget::needsUpdate`). Shader: `resources/shaders/source/vio/sky_cube.frag.glsl`
  (aus `sky.metal` zurückportieren, GLSL-Variante des bestehenden atmosphärischen Sky).
- `mesh3d.frag.glsl` (vio + GL-Kopie): `texture(u_environment_map, R)` →
  `textureLod(u_environment_map, R, roughness * u_env_mip_max)`; Uniform `u_env_mip_max`.
  `CubemapRegistry`-Pfad (`reflection_probe`, CPU-`CubemapData`) bleibt als Fallback, wenn
  `VIO_FEATURE_RENDER_TARGET_CUBE == 0` (Vulkan).
- `BackendConventions`: nichts Neues nötig (Metal = depthZeroToOne, kein Clip-Y-Flip —
  durch 089/090 empirisch bestätigt → NOTE-Kommentar in `flipRenderTargetClipY()` anpassen).
- Sky-Pipeline: `depth_write => false` statt `depth_test => false`.
- `renderToImage()` auf `vio_read_render_target($rt)` umstellen.
- VRT: `VioRenderToImageTest` bekommt echte Geometrie-Asserts (Box-Mitte ≠ Clear) und eine
  Metal-Baseline (`ScreenshotComparer`, per-Backend wie in `docs/testing.md` vorgesehen).

Kontrakt: `vendor/bin/phpunit tests/Rendering` grün auf macOS mit vio-Metal (heute 34/35,
der eine Fehler ist genau `renderToImage`).

---

## 3. Phase 3 — PHPolygon: ext-metal entfernen

Löschen:
- `src/Rendering/MetalRenderer3D.php`, `MetalCubemapTarget.php`, `MetalOffscreenTarget.php`,
  `src/Rendering/PostProcess/MetalFxaaPass.php`
- `resources/shaders/source/{mesh3d,sky,fxaa}.metal` (+ `compiled/`-Artefakte)
- `stubs/metal.stub.php` + Eintrag in `phpstan.neon`
- `tests/Rendering/MetalRenderToImageTest.php` (+ `-snapshots/`), `MetalCubemapTargetTest.php`
- `examples/metal_{spinning_box,instancing,lighting,materials,shapes}.php` → durch
  `examples/vio_*`-Pendants ersetzen oder streichen (prüfen, was `examples/` heute an vio-
  Beispielen hat)

Ändern:
- `EngineConfig`: `$useNative3D` und `renderBackend3D: 'metal'` deprecaten — `'metal'` mappt
  auf den vio-Pfad mit `vioBackend: 'metal'`; Warnung bei `useNative3D` (ein Release), dann
  entfernen. `Engine.php` Renderer-`match`: `'metal'`-Arm raus.
- `composer.json` `suggest.ext-metal` raus; `.github/workflows/ci.yml` Step „Install
  php-metal-gpu" raus.
- `CLAUDE.md` (Backend-Tabelle, Klassenübersicht), `README.md`, `docs/testing.md`
  (Abschnitt „Native-backend pixel VRT" neu: macOS-3D-VRT läuft über vio-Metal),
  `.claude/skills/vrt/SKILL.md`.
- `BackendConventions`-Doc: `MetalRenderer3D`-Verweise raus.

Kontrakt: `grep -rn 'Metal\\\\|ext-metal|php-metal|MetalRenderer3D' phpolygon/{src,tests,docs,composer.json,.github}`
liefert nichts; PHPStan Level 10 grün ohne Stub; PHPUnit grün auf macOS-Runner.

---

## 4. Phase 4 — php-metal-gpu stilllegen

- Repo `phpolygon/php-metal-gpu`: README-Banner „Deprecated — superseded by php-vio ≥ 2.9
  (Metal backend)", letztes Release taggen, GitHub-Repo archivieren. PIE-Paket bleibt
  installierbar (bestehende Nutzer), kein weiterer Release.
- php-vio README: „Metal" in der Backend-Tabelle als vollständig kennzeichnen; Hinweis, dass
  php-metal-gpu für PHPolygon nicht mehr nötig ist.
- Lokal: `~/PhpstormProjects/php-metal` kann bleiben; Herd-`php.ini` hat kein `extension=metal`.

---

## 5. Reihenfolge, Aufwand, Risiken

| Schritt | Aufwand | Risiko / Bemerkung |
|---|---|---|
| 1.1 Cube-RT + Mipmaps (4 Backends) | 2–3 Tage | D3D12-Descriptor-Handling für 6 RTVs; GL-FBO-Face-Rebind pro Face |
| 1.2 Pipeline-State | ½ Tag | trivial, aber alle vier Backends |
| 1.3 RT-Readback | ½ Tag | D3D12 braucht Barrier RENDER_TARGET→COPY_SOURCE |
| 1.4 Headless-Größen | ½ Tag | Verhaltensänderung für bestehende headless-Nutzer (Tests mit fb==2× prüfen) |
| 1.5 Hygiene | ½ Tag | Pipeline-Destroy: Use-after-free-Gefahr bei gebundener Pipeline → Backend muss `current` nullen |
| 2 Cubemap-Port + Sky-GLSL | 1–2 Tage | Sky-Shader-Portierung MSL→GLSL; Bild-Parität per VRT sichern |
| 3 Entfernen | ½ Tag | mechanisch |
| 4 Stilllegen | ¼ Tag | — |

Releases: Phase 1 → **php-vio 2.9.0** (`feat(metal)`, `feat(rt): cubemap render targets`,
`feat(api): depth_write/color_mask`, `feat(api): vio_read_render_target`, `fix(window):
headless framebuffer size`); Phase 2–3 → PHPolygon-Release mit `ext-vio >= 2.9`;
Phase 4 danach.

Nicht Teil des Plans (bewusst): Stencil, MRT, Storage-Images, Indirect Draw, uint16-Indices,
Texture-Arrays/Kompression, GPU-Timestamps, Pipeline-Cache auf Platte, MetalFX/Ray-Tracing/
Mesh-Shader — nichts davon braucht PHPolygon heute; das gehört in eine eigene API-Roadmap.
