# D3D-VULKAN-GAP-PLAN — Was D3D11/D3D12/Vulkan nativ können, vio aber nicht verdrahtet

Status: ✅ Phasen 0–4 umgesetzt (2026-09-09, Windows nativ verifiziert: D3D11 + D3D12 auf
Hardware, Vulkan-ICD, OpenGL; Linux/macOS über CI). Phase 5 offen. Ausgangspunkt: php-vio v2.9.0.

Unterwegs gefundene, vorbestehende Bugs (gefixt, nicht Teil der ursprünglichen Analyse):
- **Headless-Surface-Größe auf D3D11/D3D12/Vulkan**: dekorierte versteckte GLFW-Fenster
  haben eine Windows-Mindestbreite (~350 px); `vio_begin` resizte die D3D-Swapchain darauf,
  `vio_read_pixels` lieferte ein 348×32-Bild, das Tests als 32×32 indizierten — jede Zeile
  ab 1 war falsch. Fix: Headless-Fenster undekoriert + kein Resize im Headless-Modus.
- **OpenGL RT-MSAA** war trotz `RENDER_TARGET_MSAA = 1` nie implementiert (`samples` ignoriert).
- **D3D12 `mipmaps => true`** wurde ignoriert (immer `MipLevels = 1`).
- Test 093 (`vio_texture_update`) und 090 (Cube-RT) liefen auf `auto` = D3D12 nie (Skip).
- Beim Bau der Gallery (`examples/gallery.php`) zusätzlich gefunden und gefixt:
  - **2D-Batch Use-after-free** auf D3D11/D3D12: wurde ein `VioFont`/`VioTexture` zwischen
    `vio_text`/`vio_sprite` und `vio_draw_2d` freigegeben, zeigte das Batch-Item auf ein
    zerstörtes Backend-Objekt (Crash). Der Batch hält jetzt eine Referenz (`vio_2d_item.owner`,
    `vio_2d_push_item_owned`) bis `vio_2d_begin`/`vio_2d_shutdown`.
  - **`in mat4`-Vertex-Attribute** (Instancing) waren auf D3D11/D3D12 kaputt: die Reflection
    lieferte nur eine Location, SPIRV-Cross benennt die Spalten `TEXCOORD{loc}_{col}`. Jetzt
    expandiert `vio_pipeline` Matrizen auf `columns` Attribute (`vio_vertex_attrib.matrix_*`)
    und die D3D-Input-Layouts bilden die Semantik nach.
  - **`vio_draw_instanced_from_buffer` + Vertex-Uniform** → Device Removed auf D3D12: der Pfad
    setzte den Root-CBV nicht (`vio_push_shader_cbuffers` jetzt gemeinsam mit `vio_draw`). Test 088
    deckt es ab.

Abweichungen vom Entwurf: Phase 3 deckt zusätzlich OpenGL ab (siehe oben); 2.3 auf D3D12
ist der CPU-Pfad; die Vulkan-`read_pixels`-Optimierung (4.x) bleibt Phase 5.
Ergänzt `API-ROADMAP.md` (R3–R9) — dieser Plan enthält, was die Roadmap **nicht**
abdeckt oder deren Annahmen unterläuft, gegliedert nach den drei Finding-Blöcken
der Analyse vom 2026-09-09:

- **Block 1 — Feature-Flags, die nicht stimmen** (Korrektheit, `supports_feature`)
- **Block 2 — nativ vorhanden, nicht verdrahtet** (Paritätslücken + Roadmap-Vorstufen)
- **Block 3 — Optimierungen** (GPU-Stalls, Descriptor-Churn, Pool-Churn)

Grundregeln (wie API-ROADMAP): Feature-Flag pro Fähigkeit, Eintrag in
`tests/core/074_backend_capability_matrix.phpt`, Pixel-/Readback-Test unter
`tests/render3d/`, keine neuen `strcmp(ctx->backend->name, …)`-Zweige in
`php_vio.c` (Audit-Gate 099 friert den Bestand ein, Abbau erlaubt).

Verifikation: Windows nativ (D3D11 + D3D12 auf Hardware, Vulkan wenn ICD vorhanden,
OpenGL via GLFW); Linux/macOS über die CI-Matrix (`build.yml`). Metal kann hier
**nicht** kompiliert werden — Änderungen an gemeinsamen Structs sind so gemacht,
dass Metal neue Felder ignoriert; Metal-seitige Folgearbeit ist je Punkt markiert.

Nächste freie Testnummer nach diesem Plan: **107** (099–106 hier vergeben).

---

## Phase 0 — Kontrakt ehrlich machen (Block 1)   ✅ umgesetzt

| Nr | Änderung | Datei |
|---|---|---|
| 0.1 | Vulkan: `3D_PIPELINE`, `INSTANCED_DRAW`, `DEPTH_BIAS`, `TESSELLATION`, `GEOMETRY` → **0** (`vulkan_create_pipeline` gibt NULL, `vulkan_draw*` sind No-ops). Vulkan bleibt offiziell **2D + Compute + Offscreen-RT**. | `src/backends/vulkan/vio_vulkan.c` |
| 0.2 | D3D11/D3D12: `TESSELLATION`, `GEOMETRY` → **0** (`vio_shader_desc` kennt nur VS+FS, kein Weg, GS/HS/DS zu liefern). `RENDER_TARGET_MSAA` → **0** auf D3D12 (jedes `SampleDesc.Count == 1`); D3D11 bekommt in Phase 3 die echte Implementierung und geht dann auf 1. | `vio_d3d11.c`, `vio_d3d12.c` |
| 0.3 | D3D12: `TEXTURE_SWIZZLE` → **1** (SRV `Shader4ComponentMapping` wird für den R8-Font-Atlas bereits genutzt). | `vio_d3d12.c` |
| 0.4 | Auto-Auswahl: `vio_get_auto_backend()` überspringt Backends ohne `VIO_FEATURE_3D_PIPELINE`, wenn ein späterer Kandidat der Plattform-Prioritätsliste es hat. Linux `auto` liefert damit OpenGL statt des 3D-losen Vulkan; Windows/macOS unverändert. Sobald Vulkan-3D landet, greift die Regel automatisch nicht mehr. | `src/vio_backend_registry.c` |
| 0.5 | `074` um Blöcke für **d3d11, d3d12, vulkan** erweitern (skip wenn nicht verfügbar) — bisher sind nur opengl/null/metal gepinnt, weshalb 0.1–0.3 nie auffielen. | `tests/core/074_…phpt` |
| 0.6 | **Audit-Gate 099**: zählt `strcmp(ctx->backend->name` / `strcmp(backend->name` und `#ifdef HAVE_D3D11|HAVE_D3D12|HAVE_VULKAN` in `php_vio.c` und friert die Zahl ein (Abbau erlaubt, Zuwachs = FAIL). | `tests/core/099_audit_gate_backend_branches.phpt` |
| 0.7 | Test 100: `vio_create('auto')` liefert ein Backend mit 3D-Pipeline, sobald irgendein registriertes Backend eine hat. | `tests/core/100_auto_backend_prefers_3d.phpt` |

## Phase 1 — D3D12 Sampler-Heap: `filter` / `wrap` / `anisotropy` (Block 1, Bildfehler)   ✅ umgesetzt

Befund: Root-Signature mit 8 **statischen** Samplern s0–s7 (`MIN_MAG_MIP_LINEAR` + `WRAP`),
`desc->filter` / `desc->wrap` werden auf D3D12 nie gelesen → `VIO_FILTER_NEAREST` verwischt,
`VIO_WRAP_CLAMP` bleedet. D3D11 macht es richtig (Sampler pro Textur).

Design (spiegelt den bestehenden SRV-Table-Mechanismus):
- Root-Param **[4]**: Sampler-Descriptor-Table s0–s7 (PIXEL). Die statischen Comparison-Sampler
  s8–s11 (Shadow) bleiben statisch — Register überlappen nicht, D3D12 erlaubt die Mischung.
- Ein CPU-Sampler-Heap („Kombi-Heap") mit allen Varianten `filter(2) × wrap(3) × aniso(1,2,4,8,16)`
  = 30 Descriptors, einmal bei Init erzeugt. `vio_d3d12_texture` merkt sich seinen Kombi-Index
  (`sampler_index`); keine Allokation pro Textur.
- Shader-visible Sampler-Heap als Per-Frame-Ring (Slices wie `srv_heap`): `flush_srv_table`
  baut parallel zum SRV-Block einen 8er-Sampler-Block (`pending_samplers[8]`), gleiche
  Content-Dedup-Logik, `SetDescriptorHeaps` bindet beide Heaps (in `bind_pipeline`, im
  Async-Compute-Restore und im 2D-Pfad).
- `mipmaps => true` auf D3D12 Texturen: `MipLevels` = volle Kette (bisher immer 1), Level 0
  hochladen, Rest über Phase 2.3 (`generate_mipmaps`). Sampler-Kombis sind mip-fähig
  (`MIN_MAG_MIP_*`, `MaxLOD = FLOAT32_MAX`), Texturen ohne Mips haben nur Level 0 → identisch.
- Test 101 (`render3d/101_sampler_filter_wrap.phpt`, alle verfügbaren Backends): 2×2-Schachbrett,
  8× vergrößert gezeichnet: NEAREST → harte Kante (Pixel neben der Mitte = reine Farbe),
  LINEAR → Mischwert; UV 1.25 mit REPEAT → Texel der Gegenseite, mit CLAMP → Randtexel.

## Phase 2 — D3D-Parität + kleine native Gewinne (Block 2a/2b)   ✅ umgesetzt

| Nr | Änderung | Backends |
|---|---|---|
| 2.1 | **`create_render_target` als Vtable-Slot** für D3D11 + D3D12: Code aus `php_vio.c` (`vio_render_target`, ~330 Zeilen inline) in die Backends. `php_vio.c` ruft nur noch den Slot. Voraussetzung für 2.2 und der größte Einzelabbau am Audit-Gate-Zähler. | D3D11, D3D12 |
| 2.2 | **Cube-Render-Targets**: `ArraySize = 6` (+ `TEXTURECUBE`-Misc auf D3D11), ein RTV pro Face × Mip, `bind_render_target_face(rt, face, level)`, `render_target_cubemap` (borrowed SRV / Descriptor, `cm->borrowed = 1`), `read_render_target(face)` (Subresource = face). `VIO_FEATURE_RENDER_TARGET_CUBE = 1`. | D3D11, D3D12 |
| 2.3 | **`generate_mipmaps`**: D3D11 `GenerateMips` (Ressourcen mit `MISC_GENERATE_MIPS` + `BIND_RENDER_TARGET`, SRV über alle Mips). D3D12 hat kein natives GenerateMips — **CPU-Box-Filter** (Readback Level n-1 → 2×2-Mittel → Upload Level n), korrekt, nicht schnell; GPU-Pfad (Compute-Downsample) ist Folgearbeit. `VIO_FEATURE_MIPMAP_GEN = 1`. | D3D11, D3D12 |
| 2.4 | **`read_render_target` D3D11** (Staging-Copy, `CopyResource`/`CopySubresourceRegion` für Faces, Depth als Grau-Rampe, MRT-Attachment-Index). | D3D11 |
| 2.5 | **`update_texture` D3D12** über die Upload-Queue aus Phase 4.1 (`CopyTextureRegion` mit Box). Test 093 läuft damit auch auf D3D12. | D3D12 |
| 2.6 | **Anisotrope Filterung**: `vio_texture(['anisotropy' => 1..16])` (Default 1 = unverändert). D3D11 `D3D11_FILTER_ANISOTROPIC` + `MaxAnisotropy`; D3D12 Sampler-Kombi; Vulkan `samplerAnisotropy`-Feature aktivieren, wenn `VkPhysicalDeviceFeatures` es meldet, `maxAnisotropy` clampen; OpenGL `GL_TEXTURE_MAX_ANISOTROPY` bei `GL_ARB/EXT_texture_filter_anisotropic`. Metal: Feld ignoriert (macOS-Folgearbeit: `MTLSamplerDescriptor.maxAnisotropy`). `vio_texture_object.anisotropy` + `vio_texture_desc.anisotropy`. | D3D11, D3D12, Vulkan, GL |
| 2.7 | **Vulkan `vsync: false`** → `VK_PRESENT_MODE_IMMEDIATE_KHR` (Fallback MAILBOX → FIFO); `vsync: true` → FIFO (bisher immer MAILBOX>FIFO, also weder echtes vsync noch uncapped). | Vulkan |
| 2.8 | **`D3DCOMPILE_OPTIMIZATION_LEVEL3`** für VS/PS/CS in Release (Default ist LEVEL1). | D3D11, D3D12 |
| 2.9 | Test 102 (`render3d/102_cube_rt_mipmaps_readback_all_backends.phpt`): pro verfügbarem Backend Cube-RT 6 Faces färben, `vio_generate_mipmaps`, `vio_read_render_target($rt, face)` je Face, `textureLod` Level 0 == Level max. Test 104 (`render3d/104_anisotropy_option.phpt`): Option akzeptiert, Sampling unverändert korrekt. Test 106 (`backends/106_vulkan_present_mode.phpt`): Vulkan headless mit `vsync => false/true` erzeugt Kontext, Frames laufen. | alle |

## Phase 3 — D3D11 (+ OpenGL) Render-Target-MSAA (Block 1 → echte Implementierung)   ✅ umgesetzt

- Farb-/Tiefen-Texturen mit `SampleDesc.Count = samples` (Clamp auf `CheckMultisampleQualityLevels`,
  effektiver Wert nach `rt->samples`), zusätzliche Single-Sample **Resolve-Textur** als SRV-Quelle.
- `ResolveSubresource` beim Unbind und vor `read_render_target`; `vio_render_target_texture`
  liefert die Resolve-Textur. Depth-only + MSAA: erlaubt, aber nicht resolvebar → Warnung, single-sample.
- `VIO_FEATURE_RENDER_TARGET_MSAA` D3D11 → 1. D3D12 bleibt 0: die PSO braucht `SampleDesc` zur
  Erstellung → `vio_pipeline(['samples' => N])` analog zu `attachments`, gehört mit Stencil (R3)
  in einen PSO-State-PR (Phase 5).
- Test 105 (`render3d/105_render_target_msaa_resolve.phpt`, alle Backends mit Flag = 1): Dreieck
  in 4×-RT, Readback: Innenpixel exakt, Kantenpixel zwischen Clear- und Dreiecksfarbe.

## Phase 4 — Optimierungen (Block 3)   ✅ umgesetzt

| Nr | Änderung | Wirkung |
|---|---|---|
| 4.1 | **D3D12 Upload-Queue** (`d3d12_upload_begin/submit`): ein Ring aus 3 persistenten Command-Allocators + eine Upload-Command-List; Submit signalisiert einen Fence-Wert, Staging-Ressourcen wandern in eine **Retire-Liste** und werden in `begin_frame` freigegeben, sobald der Fence durch ist. Kein `wait_for_gpu` mehr pro Textur/Cubemap/RT-Clear/Update. GPU-Ordnung ist durch die eine DIRECT-Queue garantiert (Upload-Liste läuft vor der später submitteten Frame-Liste), also sieht auch ein mid-frame erzeugtes Texture den fertigen Inhalt. Readbacks (`read_buffer`, `readback_subresource`, sync Compute) warten weiterhin — inhärent. | 12 Voll-Stalls → 0 auf dem Ladepfad; mid-frame Uploads drainen den Frame nicht mehr |
| 4.2 | **D3D12 statische VB/IB im DEFAULT-Heap**: `create_buffer(VERTEX/INDEX)` mit `data` → DEFAULT + Staging-Copy über 4.1 (Barrier COPY_DEST → VERTEX_AND_CONSTANT_BUFFER / INDEX_BUFFER). `update_buffer` auf einem DEFAULT-Buffer **migriert einmalig** in einen UPLOAD-Buffer (alter Buffer über die Retire-Liste freigegeben) und ist danach Map-basiert wie bisher — Semantik unverändert. | keine PCIe-Reads pro Draw für statische Meshes auf diskreten GPUs |
| 4.3 | **SRV-Null-Block**: 16 Null-SRVs einmal im CPU-Staging-Heap vorbauen; `flush_srv_table` kopiert sie mit **einem** `CopyDescriptorsSimple(16)` statt 16× `CreateShaderResourceView(NULL)`. | pro SRV-Rebuild 16 Device-Calls → 1 |
| 4.4 | **Vulkan persistenter Transient-Pool**: `vulkan_begin/submit_transient_commands` nutzen einen lazily erzeugten Pool + Fence (`vkResetCommandPool` / `vkResetFences`) statt Create/Destroy pro Aufruf (Textur-Upload, Compute-Dispatch). | kein Pool/Fence-Churn |
| 4.5 | **D3D11 `gpu_flush`**: Spin auf `GetData` mit `SwitchToThread()` statt Hot-Loop. | kein 100 %-Kern beim Warten |
| 4.6 | Test 103 (`render3d/103_midframe_texture_upload_ordering.phpt`, alle Backends): Textur **innerhalb** von `vio_begin/vio_end` erzeugen bzw. per `vio_texture_update` ändern und im selben Frame zeichnen → Readback zeigt den neuen Inhalt (beweist die Upload-vor-Frame-Ordnung aus 4.1). Test 069 (Perf-Gates) bleibt grün. | Kontrakt für 4.1/4.2 |

Nicht umgesetzt (bewusst, siehe Phase 5): Vulkan `read_pixels` ohne `vkDeviceWaitIdle`
(braucht Kopie im Frame-Command-Buffer vor dem Present + Readback-Ring — erst sinnvoll,
wenn Vulkan mehr als 2D rendert).

## Phase 5 — Folgearbeit (nativ vorhanden, hier nicht umgesetzt)

Alles unten ist in D3D11/D3D12/Vulkan nativ verfügbar, braucht aber entweder eine
Grundsatzentscheidung, einen macOS-Build (4-Backend-Regel der Roadmap) oder gehört
in einen eigenen PSO-State-PR.

| Thema | Nativ | Bemerkung |
|---|---|---|
| **Vulkan 3D-Pipeline** | — | Entscheidung: nachziehen (~2–3k Zeilen: Graphics-Pipeline, Descriptor-Sets für UBO/Texturen, `set_uniform`, `bind_texture`, Draw, Instancing) **oder** Vulkan offiziell als 2D/Compute-Backend führen. Phase 0.4 hält `auto` bis dahin korrekt. |
| Stencil (R3) | D24S8 ist auf D3D11/D3D12 schon der Swapchain-/RT-Depth-Format → nur Pipeline-State fehlt | zusammen mit … |
| D3D12 RT-MSAA + Swapchain-`samples` | `SampleDesc` in PSO + RT | … `vio_pipeline(['samples' => N, 'stencil' => …])` in **einem** PSO-State-PR (D3D12 braucht beides zur PSO-Erstellung) |
| uint16-Indices (R4) | `DXGI_FORMAT_R16_UINT`, `VK_INDEX_TYPE_UINT16` | beide D3D-Draws hardcoden `R32_UINT` |
| Texture-Arrays / BC-Kompression (R4) | `ArraySize`, BC1–BC7 | KTX2-Parser zuerst |
| GPU-Timestamps (R5) | `D3D12_QUERY_TYPE_TIMESTAMP`, `D3D11_QUERY_TIMESTAMP(_DISJOINT)`, `vkCmdWriteTimestamp` | Ring über 3 Frames |
| Indirect Draw (R6) | `ExecuteIndirect`, `DrawIndexedInstancedIndirect`, `vkCmdDrawIndexedIndirect` | Fortsetzung von Path B |
| Pipeline-Cache (R8) | `ID3D12PipelineLibrary` / `CachedPSO`, `VkPipelineCache` | Startzeit |
| **DXC / SM6** | `IDxcCompiler3` | Vorbedingung für Wave-Intrinsics, 16-bit, Mesh-Shader, DXR (R9) auf D3D12; heute nur FXC `*_5_1` |
| HDR-Swapchain | `R10G10B10A2` + `IDXGISwapChain3::SetColorSpace1`, `VK_COLOR_SPACE_HDR10_ST2084_EXT` | Tone-Mapping-Pfad in PHPolygon nötig |
| Waitable Swapchain | `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` + `SetMaximumFrameLatency` | Input-Latenz, passt zu `frame_count` |
| VRS / Multiview | `RSSetShadingRate` / `VK_KHR_fragment_shading_rate`; ViewInstancing / `VK_KHR_multiview` | R9-Klasse |
| Vulkan ≥ 1.2 | Timeline-Semaphores, Dynamic Rendering, Sync2 | erst mit Vulkan-3D |
| Metal-Anisotropie | `MTLSamplerDescriptor.maxAnisotropy` | liest `desc->anisotropy` (macOS-Build) |
| D3D12 GPU-Mipmap-Gen | Compute-Downsample | ersetzt den CPU-Box-Filter aus 2.3 |

---

## Test-Kontrakt

| Test | Phase | Backends |
|---|---|---|
| `core/074_backend_capability_matrix` (erweitert) | 0 | opengl, null, metal, **d3d11, d3d12, vulkan** |
| `core/099_audit_gate_backend_branches` | 0 | — (statisch) |
| `core/100_auto_backend_prefers_3d` | 0 | auto |
| `render3d/101_sampler_filter_wrap` | 1 | alle mit 3D-Pipeline |
| `render3d/102_cube_rt_mipmaps_readback_all_backends` | 2 | alle mit `RENDER_TARGET_CUBE` |
| `render3d/103_midframe_texture_upload_ordering` | 4 | alle mit 3D-Pipeline |
| `render3d/104_anisotropy_option` | 2 | alle mit 3D-Pipeline |
| `render3d/105_render_target_msaa_resolve` | 3 | alle mit `RENDER_TARGET_MSAA` |
| `backends/106_vulkan_present_mode` | 2 | vulkan |
| bestehende 090–093, 096–098 | 1–4 | laufen auf `auto` = D3D12 nun ohne Skip (Cube-RT, `update_texture`) |

Windows-CI (`build.yml`): 099–106 in die WARP-Liste aufnehmen. CLAUDE.md: Feature-Matrix,
Testzahl, D3D12-Sampler-/Upload-Hinweise nachziehen.

## Reihenfolge

0 → 1 → 4.3 → 4.1 → 2.1 → 2.2/2.3/2.4 → 2.5 (braucht 4.1) → 2.6/2.7/2.8 → 4.2 → 3 → 4.4/4.5 → Doku/CI.
Jede Phase baut und lässt die volle Suite grün, bevor die nächste beginnt.
