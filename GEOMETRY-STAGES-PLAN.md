# GEOMETRY-STAGES-PLAN — was nach den Geometry-/Tessellation-Stages fehlt

Stand 2026-10-03, nach PR #23 (Geometry + Tessellation für `vio_shader`, Vulkan nativ,
Test 109/110/135). Alle Phasen sind umgesetzt (Tabelle am Ende); offen bleiben nur Hull/Domain aus GLSL auf D3D
(SPIRV-Cross hat kein HLSL-Tessellations-Backend, der HLSL-Override deckt es ab). Dieser Plan sammelt die Funktionen, die der PR bewusst ausgelassen hat, und
ordnet sie nach Nutzen für PHPolygon und nach Testbarkeit auf dem Windows-Entwicklungsrechner
(RTX 2080, Vulkan 1.4, D3D11/D3D12, OpenGL 4.6; Metal nur über die macOS-CI).

## Ausgangslage

| | OpenGL | D3D11 | D3D12 | Vulkan | Metal |
|---|---|---|---|---|---|
| Geometry-Stage | ✅ | ✅ (nur Varying-Eingänge) | ✅ (dto.) | ✅ | ❌ (gibt es in Metal nicht) |
| Tessellation | ✅ | ❌ (SPIRV-Cross: kein Hull/Domain) | ❌ | ✅ | ❌ |
| Layered Rendering (`gl_Layer`) | ❌ | ❌ | ❌ | ❌ | ❌ |
| Mehrere Viewports (`gl_ViewportIndex`) | ❌ | ❌ | ❌ | ❌ | ❌ |
| Render-Targets | 2D, Cube (eine Face je Bind), MRT, MSAA, Depth-only | dto. | dto. | dto. | dto. |

Lücken bei den Render-Targets, an denen das Layered Rendering hängt: **kein 2D-Array-RT**
und **kein Depth-only-Cube** (`vio_render_target` lehnt `cube` + `depth_only` ab). Punktlicht-
Schatten und CSM laufen in PHPolygon deshalb heute über mehrere einzelne Targets und mehrere
Pässe (CSM: Units 6/8/9 = drei RTs).

SPIRV-Cross upstream hat keine HLSL-Ausgabe für Hull/Domain und keine sichtbare Arbeit daran
(KhronosGroup/SPIRV-Cross#573, #1701). Auf D3D-Tessellation „von selbst" zu warten, ist keine
Option.

## Regeln für alle Phasen

- Jede neue Fähigkeit läuft über einen Vtable-Slot oder Deskriptor-Feld; `php_vio.c` bekommt
  keinen neuen `strcmp(ctx->backend->name, …)`/`#if HAVE_*`-Zweig (Audit-Gate 099).
- Jede Fähigkeit hat ein ehrliches `VIO_FEATURE_*`-Flag. Geräteabhängige Flags werden in 074
  nicht gepinnt, sondern vom jeweiligen Pixel-Test als Vertrag geprüft.
- Tests iterieren über `opengl/d3d11/d3d12/vulkan/metal`, prüfen orientierungsunabhängig
  (symmetrische Motive oder Vergleich zweier Wege) und laufen lokal zusätzlich mit
  `debug => true` (Vulkan-Validation, D3D-Debug-Layer) — je Backend ein eigener Prozess.
- Nächste freie Testnummer: 136.

---

## Phase 1 — Layered Rendering: Cube- und CSM-Schatten in einem Pass (Nutzen: hoch)

Ziel: eine Szene einmal abschicken und in alle 6 Cube-Faces bzw. alle CSM-Kaskaden rendern.
Das spart je Punktlicht 5 und je Sonne 2 Szenen-Durchläufe auf der CPU. Für PHPolygon ist das
der wichtigste Einsatz der Geometry-Stage.

### 1a — Array- und Depth-Cube-Targets

- `vio_render_target(['width', 'height', 'layers' => N])`: 2D-Array-Target (Farbe, HDR,
  `depth_only`, MRT), N ≤ 16.
- `vio_render_target(['cube' => true, 'depth_only' => true])`: Depth-Cube für Punktlichter.
- `vio_render_target_texture($rt)` liefert eine Textur, die als `sampler2DArray` /
  `sampler2DArrayShadow` bzw. `samplerCubeShadow` gesampelt wird (Compare-Sampler wie bei
  heutigen Depth-only-Targets).
- `vio_bind_render_target($ctx, $rt, $layer)` bindet wie bisher einen einzelnen Layer
  (Cube: Face) — die heutige Mehrpass-Variante bleibt, damit es überall einen Fallback gibt.
- Backends: GL `GL_TEXTURE_2D_ARRAY` / Depth-Cubemap; D3D `Texture2D` mit `ArraySize`,
  RTV/DSV/SRV als Array bzw. `TEXTURECUBE` (Depth typeless R24G8 / R32); Vulkan Image mit
  `arrayLayers`, Views `2D_ARRAY` / `CUBE`; Metal `MTLTextureType2DArray` / Cube mit
  `Depth32Float`.
- Vtable: `create_render_target` bekommt die Layerzahl über das RT-Objekt (wie heute
  `is_cube` / `mip_levels`); `bind_render_target_face` wird zu „Layer binden" verallgemeinert.

### 1b — Alle Layer auf einmal binden

- `vio_bind_render_target($ctx, $rt, VIO_RT_ALL_LAYERS)` (Konstante = -2; -1 bleibt „Standard").
- Neuer Slot `bind_render_target_layered(rt, level)`: GL `glFramebufferTexture` (layered
  attachment), D3D RTV/DSV über alle Array-Slices, Vulkan Framebuffer mit `layers = N`,
  Metal `renderTargetArrayLength = N`.
- Flag `VIO_FEATURE_LAYERED_RENDER`.

### 1c — Layer im Shader wählen

- **Über die Geometry-Stage**: `gl_Layer = …` im GS. GL, D3D11/D3D12 (SPIRV-Cross bildet das
  auf `SV_RenderTargetArrayIndex` ab; per Stage-Probe prüfen) und Vulkan.
- **Über den Vertex-Shader** (kein GS nötig, `gl_Layer = gl_InstanceIndex` + 6 Instanzen):
  - Vulkan: `shaderOutputLayer` (1.2) bzw. `VK_EXT_shader_viewport_index_layer`
  - D3D11.3/D3D12: `VPAndRTArrayIndexFromAnyShaderFeedingRasterizer`
  - GL: `GL_ARB_shader_viewport_layer_array`
  - **Metal: `[[render_target_array_index]]` aus der Vertex-Funktion, nativ.** Damit bekommt
    Metal Single-Pass-Cube- und CSM-Rendering, obwohl es keine Geometry-Stage hat.
- Flag `VIO_FEATURE_VERTEX_LAYER` für den Vertex-Weg; PHPolygon nimmt ihn, wenn vorhanden,
  sonst den GS-Weg, sonst Mehrpass.
- Zu prüfen: dass SPIRV-Cross `gl_Layer` im Vertex-Stage für HLSL und MSL korrekt ausgibt.

### 1d — Mehrere Viewports (`gl_ViewportIndex`)

- `vio_viewports($ctx, [[x, y, w, h], …])` (bis 16) plus `gl_ViewportIndex` im GS/VS: CSM in
  ein einzelnes Atlas-Target statt in ein Array.
- GL `glViewportArrayv` (4.1), D3D `RSSetViewports(N)`, Vulkan `multiViewport` (Device-
  Feature aktivieren), Metal `setViewports:count:` + `[[viewport_array_index]]`.
- Flag `VIO_FEATURE_MULTI_VIEWPORT`.

**Tests:** 136 Array-/Depth-Cube-Targets (Readback je Layer, Sampling als Array bzw.
Cube-Shadow); 137 Single-Pass-Cube (jeder Layer eine andere Farbe per `gl_Layer`, über GS und
über VS-Instanzen, Vergleich mit der Mehrpass-Variante); 138 CSM über `gl_ViewportIndex` im
Atlas. **Lokal testbar:** GL, D3D11, D3D12, Vulkan vollständig; Metal nur in der CI.

---

## Phase 2 — GS-Instancing und Adjacency (Nutzen: mittel, Aufwand: klein)

### 2a — `layout(invocations = N)`

- Reine Shader-Fähigkeit: GL ≥ 4.0, Vulkan (`maxGeometryShaderInvocations`, Desktop 32),
  D3D `[instance(N)]`. Zu prüfen, ob SPIRV-Cross das für HLSL ausgibt → Stage-Probe um einen
  Instancing-Kanonshader erweitern, Flag `VIO_FEATURE_GEOMETRY_INSTANCING`.
- Mit Phase 1 kombiniert: Cube in einem Draw mit `invocations = 6`, `gl_Layer = gl_InvocationID`.

### 2b — Topologien mit Adjacency

- `VIO_LINES_ADJACENCY`, `VIO_LINE_STRIP_ADJACENCY`, `VIO_TRIANGLES_ADJACENCY`,
  `VIO_TRIANGLE_STRIP_ADJACENCY` (Werte 7–10) für Silhouetten, Outlines und Shadow Volumes.
- Abbildung: GL `GL_*_ADJACENCY`, D3D `*_ADJ` (PSO-Typ LINE/TRIANGLE), Vulkan
  `*_WITH_ADJACENCY`; Metal ❌ (keine Geometry-Stage). Nur mit `VIO_FEATURE_GEOMETRY`
  erlaubt — ohne GS sind die Nachbarindizes nutzlos.
- Hilfsfunktion: `vio_mesh([… , 'adjacency' => true])` berechnet aus einer indizierten
  Dreiecksliste den Adjacency-Indexpuffer (6 Indizes je Dreieck, Kanten-Map in C, O(n);
  offene Kanten zeigen auf den gegenüberliegenden Vertex des eigenen Dreiecks).

**Tests:** 139 Invocations (ein Punkt → N Quads, je Invocation eine Farbe); 140
Silhouetten-Kanten eines Würfels per `triangles_adjacency`. **Lokal testbar:** GL, D3D, Vulkan.

---

## Phase 3 — Tessellation auf D3D11/D3D12 (Nutzen: mittel, Aufwand: mittel)

**Empfehlung: HLSL-Override je Stage.**

```php
$sh = vio_shader($ctx, [
    'vertex' => $vs, 'fragment' => $fs,
    'tess_control' => $tcsGlsl, 'tess_eval' => $tesGlsl,          // GL / Vulkan
    'hlsl' => ['tess_control' => $hullHlsl, 'tess_eval' => $domainHlsl],   // D3D
]);
```

- D3D nimmt für Hull/Domain die HLSL-Quelle (`hs_5_x`/`ds_5_x`, unter SM 6 DXIL über den
  vorhandenen Cache-Pfad), VS/PS kommen weiter aus GLSL über SPIRV-Cross.
- Vertrag (in der Doku mit Beispiel): Eingänge mit den Semantiken, die SPIRV-Cross für die
  VS-Ausgänge vergibt (`TEXCOORD{location}`); Stage-Uniforms in `cbuffer … : register(b0)` in
  derselben Reihenfolge wie in der GLSL-Stage. vio prüft das: `D3DReflect` auf dem Hull/Domain-
  Blob gegen die Uniform-Offsets aus der GLSL-SPIR-V, bei Abweichung Warning statt stiller
  Fehlbelegung.
- Flags: `VIO_FEATURE_TESSELLATION` bleibt auf D3D 0 (reiner GLSL-Weg geht nicht); neu
  `VIO_FEATURE_TESSELLATION_HLSL = 1` auf D3D11/D3D12. `vio_shader` nimmt Tessellation-Stages
  auf D3D genau dann an, wenn der HLSL-Override dabei ist.
- Verworfen: SPIRV-Cross selbst um Hull/Domain erweitern (großer, unsicherer Upstream-Beitrag —
  wenn er doch kommt, schaltet die vorhandene Stage-Probe das Flag ohne vio-Änderung um);
  Slang als zusätzlicher GLSL→HLSL-Übersetzer (schwere Abhängigkeit für eine Stage-Art).

**Test:** 141 = Disc aus 110 auf D3D mit HLSL-Override, Kantenzahl folgt `u_level`; ein
Override mit falscher Cbuffer-Reihenfolge erzeugt die Warning. **Lokal testbar:** D3D11 und
D3D12 auf der RTX 2080 und auf WARP.

---

## Phase 4 — Tessellation auf Metal (Nutzen: für macOS-Spiele, Aufwand: groß)

- Metal tesselliert über Compute: SPIRV-Cross übersetzt den VS und den TCS in Compute-Kernel
  (`vertex_for_tessellation`), die Tessellation-Faktoren und Control-Points in Puffer
  schreiben; der TES wird zur Post-Tessellation-Vertex-Funktion (`[[patch(quad, N)]]`).
- Draw: Compute-Encoder (VS-Kernel + TCS-Kernel) im Frame-Command-Buffer, danach
  Render-Encoder mit `setTessellationFactorBuffer` + `drawPatches` / `drawIndexedPatches`
  (auch instanziert und indirekt). Betrifft jeden Draw-Pfad (Test 135 als Vorlage).
- **Nicht lokal testbar** — nur die macOS-CI ist der Beleg. Deshalb zuletzt und als eigener PR.
- Geometry-Stage auf Metal bleibt außen vor (der Weg wären Mesh-Shader ab Metal 3); Phase 1c
  deckt den Hauptnutzen (Layered Rendering) auf Metal ohne GS ab.

---

## Bewusst nicht geplant

- **Stream Output / Transform Feedback:** GL und D3D hätten es, Vulkan nur über eine nicht
  überall verfügbare Extension, Metal gar nicht. Compute mit Storage-Buffern deckt die
  Anwendungsfälle (Partikel, GPU-Skinning) auf allen Backends ab.
- **Vulkan-Multiview / VR-Stereo:** eigenes Thema, falls PHPolygon VR braucht.

## Reihenfolge und Testbarkeit

| Phase | Inhalt | Aufwand | Lokal testbar | Stand |
|---|---|---|---|---|
| 1a–1b | Array-/Depth-Cube-Targets, alle Layer binden | M | GL, D3D11, D3D12, Vulkan | ✅ Test 136/137 |
| 1c | `gl_Layer` aus GS und VS | S–M | GL, D3D11, D3D12, Vulkan (Metal: CI) | ✅ Test 137 (Metal: nur Vertex-Stage, `[[render_target_array_index]]`) |
| 1d | Mehrere Viewports | S | GL, D3D11, D3D12, Vulkan | ✅ Test 138 |
| 2 | GS-Instancing, Adjacency + Mesh-Helfer | S | GL, D3D11, D3D12, Vulkan | ✅ Test 139, 143 (D3D: `gl_InvocationID`/`gl_in` über den SPIR-V-Umbau vor SPIRV-Cross) |
| 3 | D3D-Tessellation über HLSL-Override | M | D3D11, D3D12 | ✅ Test 140, als allgemeiner Override für GS/HS/DS (`VIO_FEATURE_HLSL_STAGE_OVERRIDE`) |
| 4 | Metal-Tessellation | L | nur CI | ✅ Test 110/140 auf macOS (VS/TCS als Compute-Kernel, TES nach `drawPatches`) |
| 1 (Metal) | Layered Targets, VS-Layer, mehrere Viewports | M | nur CI | ✅ Test 136–138, 142 auf macOS |

Abweichungen vom Plan: Phase 3 ist kein Tessellation-Sonderfall (`VIO_FEATURE_TESSELLATION_HLSL`),
sondern ein Override für alle drei Extra-Stages - damit bekommt D3D auch GS-Instancing
(`[instance(N)]`). Seit dem SPIR-V-Umbau (`vio_gs_hlsl_rewrite`) braucht GS-Instancing auf D3D den
Override nicht mehr; für Hull/Domain bleibt er der einzige Weg.
