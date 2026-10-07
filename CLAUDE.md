# php-vio — PHP Video Input Output Extension

## Was ist das?

Eine PHP C-Extension die GPU-Rendering (OpenGL 3.0–4.6, Vulkan, Metal, Direct3D 11/12),
Audio, Video-Recording, Streaming und Input in PHP verfügbar macht. Basis-Infrastruktur
für die PHPolygon Game Engine. Aktuell **v2.8.0**, 143 PHP-Funktionen, 14 Zend-Klassen,
6 Backends, 98 PHPT-Tests. Releases laufen über semantic-release
(`.github/workflows/release.yml`, Conventional Commits → `CHANGELOG.md`).

## Build

### macOS (Homebrew)

Herd bringt PHP 8.2/8.4/8.5 als Binaries mit, aber **kein `phpize`/`php-config`** —
der Build braucht die passende Homebrew-Formel (`php` = 8.5, API 20250925 wie Herd
`php85`; `php@8.4` = API 20240924 wie Herd `php84`). SPIRV-Cross wird lokal aus
`.deps/SPIRV-Cross` per cmake nach `/opt/homebrew` installiert (Rezept in
`Makefile.macos`, untracked); die CI nimmt die Homebrew-Formel `spirv-cross`.

**SPIRV-Cross ≥ `vulkan-sdk-1.4.363.0`** (Homebrew `spirv-cross` 1.4.363.0, seit 2026-09-29)
enthält den MSL-Struct-Array-Stride-Fix (KhronosGroup/SPIRV-Cross#2678, `94d59e5`). Ältere
Stände legen Struct-Arrays im Default-Uniform-Block (`uniform SpotLight u_spot_lights[4]` —
std140-Stride 64, gepackte MSL-Größe 52) mit falschem Element-Stride an; **alle Uniforms
hinter dem Array werden auf Metal verschoben gelesen**. Regression: `tests/backends/094`.
Ein lokaler `.deps/SPIRV-Cross`-Checkout muss mindestens auf `vulkan-sdk-1.4.363.0` stehen.

```bash
brew install php glfw glslang ffmpeg harfbuzz vulkan-loader vulkan-headers molten-vk cmake

# PHP 8.5 (Homebrew `php` — gleiche API wie Herd php85)
make clean; phpize --clean; phpize && \
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross=/opt/homebrew --with-vulkan --with-ffmpeg --with-metal --with-harfbuzz && \
make -j$(sysctl -n hw.ncpu)

# PHP 8.4 (Homebrew php@8.4 — gleiche API wie Herd php84)
P=/opt/homebrew/opt/php@8.4/bin
make clean; $P/phpize --clean; $P/phpize && \
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross=/opt/homebrew --with-vulkan --with-ffmpeg --with-metal --with-harfbuzz \
  --with-php-config=$P/php-config && \
make -j$(sysctl -n hw.ncpu)
```

`--with-harfbuzz` nicht vergessen — ohne es ist `VIO_HAS_SHAPING == 0` und die
Shaping-Tests 081–084 skippen. `--with-ios` (impliziert Metal, schaltet GLFW/Vulkan/
FFmpeg ab) ist nur für Cross-Builds gegen das iOS-SDK gedacht.

### Linux

```bash
# Dependencies (Ubuntu/Debian)
sudo apt install php-dev libglfw3-dev glslang-dev libvulkan-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  spirv-cross libspirv-cross-c-shared-dev libharfbuzz-dev

# Build (kein --with-metal auf Linux)
phpize && \
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross --with-vulkan --with-ffmpeg --with-harfbuzz && \
make -j$(nproc)
sudo make install
```

### Windows

Benötigt PHP SDK + Visual Studio Build Tools. Dependencies (GLFW, Vulkan SDK, FFmpeg, glslang, SPIRV-Cross) als vorcompilierte Libs in `deps/` oder per `--with-*=C:\path`.

```cmd
:: PHP SDK einrichten (https://wiki.php.net/internals/windows/stepbystepbuild_sdk_2)
cd C:\php-sdk\phpdev\vs17\x64\php-src\ext\vio

:: Minimal-Build (nur OpenGL, kein Vulkan/FFmpeg)
configure --enable-vio --with-glfw=C:\deps\glfw

:: Voll-Build
configure --enable-vio --with-glfw=C:\deps\glfw ^
  --with-vulkan=C:\VulkanSDK\1.3.xxx ^
  --with-glslang=C:\deps\glslang ^
  --with-spirv-cross=C:\deps\spirv-cross ^
  --with-ffmpeg=C:\deps\ffmpeg ^
  --with-harfbuzz=C:\vcpkg\installed\x64-windows

nmake
```

Hinweis: Metal-Backend ist macOS-only und wird auf Windows/Linux nicht kompiliert. Alle Backend-Sources sind in `#ifdef HAVE_*` Guards, daher kompiliert ein Build ohne bestimmte Dependencies problemlos — die Features sind dann einfach nicht verfügbar.

## Tests

```bash
NO_INTERACTION=1 TEST_PHP_EXECUTABLE=$(which php) php run-tests.php -d extension=$PWD/modules/vio.so tests/
```

153 PHPT-Tests, nach Themen in Unterordnern (`run-tests.php` rekursiert):

| Ordner | Inhalt |
|---|---|
| `tests/render3d/165` | Sampler Feedback (`VIO_FEATURE_SAMPLER_FEEDBACK`, nur D3D12, SM 6.5 + Tier 0.9): Fragment-Stage als HLSL-Override (`'hlsl' => ['fragment' => …]`) mit `FeedbackTexture2D<SAMPLER_FEEDBACK_MIN_MIP> vio_feedback : register(u0, space2)` + `WriteSamplerFeedback`; eine 256²-Textur mit Mip-Kette auf ein 32²-Ziel gezeichnet meldet in jeder Region Mip ≈ 3, nach `vio_sampler_feedback_clear` keine; ohne Flag liefern bind/read/clear `false`. `VIO_REQUIRE_SAMPLER_FEEDBACK=d3d12` macht das Backend Pflicht. |
| `tests/render3d/160` | Shading-Rate-Bild (`VIO_FEATURE_SHADING_RATE_IMAGE`, `vio_set_shading_rate_image`, `vio_shading_rate_tile_size`): 2X2-Kacheln links, 1X1 rechts → links grob, rechts fein; Bild klebt über Frames, `null` löscht; falsche Byte-Zahl / Rate 99 → `false`; ohne Feature Kachelgröße 0 und `false`. Nur D3D12 (Tier 2) — in der CI nicht ausführbar (WARP-Tier unbekannt, Diagnose druckt `shading_rate_image`). |
| `tests/render3d/161` | Bindless-Texturtabelle (`VIO_FEATURE_BINDLESS`, `vio_texture_index()`, BINDLESS-PLAN.md): 64 einfarbige Texturen, ein Draw mit 64 Quads, Slot als Vertex-Attribut → nicht-uniformer Index in `vio_textures[]` (Set 1) im Fragment-Shader; Slot stabil und eindeutig, die Tabelle hält die Texturen am Leben (PHP-Variablen vor dem Draw verworfen); ohne Flag `false`. Liest ein Render-Target (MoltenVK-Swapchain-Readback auf Retina ist nicht exakt). |
| `tests/render3d/163` | Inline-Raytracing (`VIO_FEATURE_RAY_QUERY`): `vio_acceleration_structure` (BLAS je Mesh, TLAS über Instanzen mit Transform) + `vio_bind_acceleration_structure`; `rayQueryEXT` im Fragment-Shader (Verdecker links / per Transform rechts / beide) und im Compute-Shader (4 Strahlen → 1 1 0 0); ohne Flag liefert `vio_acceleration_structure` `false`. Metal ab MSL 2.4 (M5: ausgeführt), D3D12 DXR 1.1 + SM 6.5, Vulkan `VK_KHR_ray_query`. |
| `tests/render3d/159` | Shading-Rate pro Primitiv (`VIO_FEATURE_SHADING_RATE_PRIMITIVE`): Vertex-Stage schreibt `gl_PrimitiveShadingRateEXT` (2×2 = 5, auf Vulkan und D3D12 gleich kodiert) und überschreibt `vio_set_shading_rate`; Pipelines ohne den Write behalten die gesetzte Rate. **Nirgends ausführbar** (lavapipe ohne VRS, WARP nur SM 6.2, MoltenVK/Metal ohne VRS) — D3D12-HLSL nur per DXC geprüft. |
| `tests/render3d/158` | Multiview (`VIO_FEATURE_MULTIVIEW`, `vio_shader(['view_count' => N])`): ein Draw rendert jede View in Layer `gl_ViewIndex` eines mit `VIO_RT_ALL_LAYERS` gebundenen Layered-RTs — Fragment- und Vertex-Arbeit je View, Instancing (Instanz-Attribute stepen je Instanz, nicht je (Instanz, View)), 4 Views, indirekter Draw, Optionsvertrag (2..4, ohne Flag abgelehnt). CI-Pflicht auf allen vier Backends: OpenGL (llvmpipe, `GL_OVR_multiview2`), Vulkan (lavapipe), D3D12 (WARP, SM 6.2), Metal (macOS-Runner). |
| `tests/render3d/155` | 16-Bit-Floats (`VIO_FEATURE_SHADER_FLOAT16`): `float16_t` zur Laufzeit gerundet (2049 → 2048, 0.1 → 0.0999755859375) — beweist echte halbe Genauigkeit (HLSL `min16float` wäre nur ein Hinweis). |
| `tests/render3d/156` | Draw-Parameter (`VIO_FEATURE_BASE_VERTEX`): indirekter Draw mit baseVertex 4 / firstInstance 3 liefert `gl_BaseVertex`/`gl_BaseInstance` = (4, 3), `vio_draw` (0, 0); Vertices 0..3 sind degeneriert, ein ignorierter Base Vertex zeichnet nichts. |
| `tests/render3d/157` | Compute-Derivate (`VIO_FEATURE_COMPUTE_DERIVATIVES`): `derivative_group_quadsNV`, `dFdx`/`dFdy` von 3x + 5y ergeben überall (3, 5); Metal muss 0 melden. |
| `tests/render3d/154` | 64-Bit-Atomics (`VIO_FEATURE_ATOMIC64`): 64 Threads rennen mit (Tiefe << 32 \| ID)-Schlüsseln auf vier `uint64_t`-Slots (`atomicMax`, `atomicMin`, `atomicAdd` über 2³⁹, `atomicCompSwap` mit genau einem Gewinner) — jede 32-Bit-Kürzung fällt auf; Metal muss 0 melden. CI-Pflicht auf lavapipe. |
| `tests/render3d/152` | Quad-Operationen im Fragment-Shader (`VIO_FEATURE_SUBGROUP_QUAD`): `subgroupQuadSwapHorizontal/Vertical/Diagonal` und `subgroupQuadBroadcast` liefern für jedes Pixel die 2×2-Nachbarn (gerade ausgerichtete Quads, unabhängig von der Zeilenrichtung); Metal folgt `quad_group` (MSL 2.1). CI-Pflicht auf lavapipe und (über `VIO_REQUIRE_SM6`) auf WARP. |
| `tests/render3d/153` | Baryzentrische Koordinaten (`VIO_FEATURE_BARYCENTRICS`): `gl_BaryCoordEXT` gleich der interpolierten Einheitsvektoren der Vertices (Vertex-Reihenfolge, zwei Dreiecke ohne geteilte Vertices); Metal folgt `barycentrics` (MSL 2.2), D3D12 braucht SM 6.1 + `BarycentricsSupported`. CI-Pflicht auf WARP (`VIO_REQUIRE_BARYCENTRICS=d3d12`); lavapipe hat die Extension nicht, Vulkan ist nur lokal (MoltenVK) belegt. |
| `tests/backends/150` | Metal-Versionsleiter: `vio_backend_info()` (MSL-Version in Benutzung und Maximum, GPU-Familien, Caps), jede Stufe bis zum OS-Maximum per `msl_version` erzwingbar, versionsgebundene Caps folgen der Stufe, Clamping (unter 2.0 → 2.0, über Maximum → Maximum, zwischen Stufen → darunter), `VIO_METAL_MSL_VERSION` greift ohne Option, Option gewinnt; `null` liefert `false`. |
| `tests/render3d/151` | Dieselbe Szene auf jeder MSL-Stufe 2.0 … OS-Maximum: 3D-Draw mit Uniform + Textur in ein RT (Readback), Compute, emulierter GS, Tessellation (ab 2.1, darunter abgelehnt), 2D-Batch. |
| `tests/render3d/149` | Subgroup-Operationen (`VIO_FEATURE_SUBGROUP`): Compute-Shader mit `subgroupAdd`/`subgroupBroadcastFirst`/`gl_SubgroupSize` (Ergebnisse unabhängig von der Wave-Größe konsistent) und Fragment-Shader mit `subgroupAllEqual`/`subgroupAdd`; D3D12 über SM 6 (DXC), Metal muss das Flag melden, wenn `vio_backend_info()` `simd_group` meldet (ab MSL 2.2), Vulkan/GL nach Device-Eigenschaften. Linux-CI: Pflicht auf lavapipe (`VIO_REQUIRE_SUBGROUP=vulkan`); der GL-Pfad läuft dort nicht (llvmpipe ohne `GL_KHR_shader_subgroup`, zink ohne DRI3 unter Xvfb) und ist CI-seitig unbelegt. |
| `tests/render3d/148` | Tessellation `point_mode` (Dreieck/Quad) erzeugt jeden Domain-Punkt genau einmal (Punktzahl nach GL-Regel, additives Blending deckt Duplikate auf), Isolines mit `fractional_odd_spacing`; auf Metal emuliert (siehe „Metal-3D-Pipeline"). |
| `tests/render3d/147` | Stencil in Array-Layer, Cube-Face und depth_only-Target (Schema von 113); vorher hatte Metal dort keine Stencil-Plane. |
| `tests/render3d/146` | `vio_set_uniform("name[i]", …)` setzt ein Element eines Arrays von Matrizen/Vektoren (`uniform mat4 u_m[3]`, `uniform vec4 u_col[2]`) auf jedem Backend. Vorher fanden D3D11/D3D12/Vulkan/Metal das Element nicht (Befund aus Code Rescue). |
| `tests/render3d/145` | `vio_generate_mipmaps` im Frame: Cube-RT rendern, Mips bauen und die kleinste Stufe im selben Frame sampeln (zwei Frames mit verschiedenem Inhalt, keine veraltete Stufe); weiche Zeitgrenze, auf D3D12-Hardware ohne GPU-Drain (vorher ~3–10 ms je Aufruf). |
| `tests/render3d/144` | Tessellation hält die OpenGL-Konventionen auf jedem Backend: welche Kante `outer[1]` unterteilt (Quad, Dreieck), Winding unter `VIO_CULL_BACK`, Patch-Varying mit Uniforms in TCS und TES, `vertices = 3` mit 4-Punkt-Patches (`gl_PatchVerticesIn`, HS-Variante), Isolines (auf D3D nur auf Hardware, WARP verliert tessellierte Linien), D3D12 auch mit Shader Model 6. Farben tragen die Domain-Koordinaten, die Prüfung ist unabhängig von der Readback-Orientierung. |
| `tests/render3d/143` | Geometry-Stage mit `gl_in[0].gl_Position` und `gl_InvocationID` (`layout(invocations = 2)`) aus purem GLSL auf jedem Backend mit GS – auf D3D über den SPIR-V-Umbau vor SPIRV-Cross (`SV_Position`-Eingang, `SV_GSInstanceID` + `[instance(N)]`). |
| `tests/render3d/142` | Erneutes Binden eines Render-Targets behält Farbe **und Tiefe** (plain, HDR, MSAA, Array-Layer, Cube-Face; im nächsten und im selben Frame) – `vio_clear` ist der einzige Clear. Fehlte auf Vulkan (`loadOp CLEAR`). |
| `tests/render3d/141` | Vergleichs-Sampling auf jedem Backend: `sampler2DShadow`, `sampler2DArrayShadow`, `samplerCubeShadow` liefern das Vergleichsergebnis (ref ≤ gespeicherte Tiefe), dieselbe Tiefentextur liest per `sampler2D` weiter roh, ein 2D-Sprite auf Unit 0 danach sampelt normal. Schlägt ohne den GL-Vergleichs-Sampler fehl (GL lieferte die rohe Tiefe). |
| `tests/render3d/139` | GS-Instancing (`layout(invocations = 4)`, Quadranten; Cube in einem Draw mit `invocations = 6` + `gl_Layer`) und Adjacency (`VIO_TRIANGLES_ADJACENCY` über `vio_mesh(['adjacency' => true])` mit pro Dreieck duplizierten Vertices, `VIO_LINES_ADJACENCY`-Reihenfolge, Ablehnung ohne GS). |
| `tests/render3d/140` | HLSL-Stage-Override: Tessellations-Disc aus 110 mit Hull/Domain-HLSL auf D3D (GLSL auf GL/Vulkan), Instanced-GS per `[instance(4)]`, Warnung bei abweichendem cbuffer-Layout, Argument-Vertrag. |
| `tests/render3d/109–110` | Geometry-Stage (`vio_shader(['geometry' => …])`, Punkt → Quad, GS-Uniform, Unbind-Regression) und Tessellation (`tess_control` + `tess_eval`, `VIO_PATCHES`/`patch_vertices`, Quad-Patch → Disc, Kantenzahl folgt dem TCS-Uniform). Iterieren über `opengl/d3d11/d3d12/vulkan/metal`; Backend mit Flag 0 → `skip`. |
| `tests/render3d/138` | Mehrere Viewports (GEOMETRY-STAGES-PLAN 1d): `vio_viewports()` mit linker/rechter Hälfte, `gl_ViewportIndex` aus dem GS (ein Dreieck je Viewport) und aus dem Vertex-Shader (`gl_InstanceIndex`), ohne Index nur Viewport 0, `vio_viewport` stellt einen Viewport wieder her, Argument-Vertrag. |
| `tests/render3d/137` | Layered Rendering (GEOMETRY-STAGES-PLAN 1b/1c): `vio_bind_render_target($ctx, $rt, VIO_RT_ALL_LAYERS)`, `vio_clear` löscht alle Layer, ein Draw durch einen GS mit `gl_Layer` füllt jeden Layer bzw. einen Depth-Cube (Punktlicht-Schatten in einem Pass), `gl_Layer = gl_InstanceIndex` im Vertex-Shader (`VIO_FEATURE_VERTEX_LAYER`). |
| `tests/render3d/136` | Layered Render-Targets (GEOMETRY-STAGES-PLAN 1a): `'layers' => N`-Array (Farbe, Clear + Quad je Layer, Readback je Layer, `sampler2DArray`), Depth-Array (`.r` je Layer), Depth-Cube (`vio_render_target_cubemap`, `samplerCube .r` je Face), Options-Vertrag (kein `layers` + `cube`/`samples`, Layer-Bereich, keine Mips). |
| `tests/render3d/135` | GS-/Tess-Pipelines auf jedem später dazugekommenen Pfad: `vio_submit_batch`, `vio_draw_instanced`, uint16-Indices, `vio_draw_indirect`, `vio_draw_instanced_from_buffer`; in HDR-, MRT-, MSAA- und Stencil-Targets (D3D12-PSO- und Vulkan-Render-Pass-Varianten); Shader-Cache: gleiches VS+FS mit und ohne GS sind verschiedene Programme (kalter und warmer Cache, beide Reihenfolgen). |
| `tests/render3d/090–093` | Cube-RT/Mipmaps, Pipeline-State, RT-Readback, Texture-Update + Pipeline-Free (Replacement-Plan Phase 1) |
| `tests/input/134` | Replay besitzt den Input (Win32): echte `WM_KEYDOWN`/`WM_MOUSEMOVE`/`WM_MOUSEWHEEL` per `PostMessageW` (FFI im Kindprozess) an das versteckte GLFW-Fenster kommen live an, werden während eines Replays verworfen (nur das Replay-Event erreicht `vio_on_key`) und nach `vio_input_replay_stop` wieder zugestellt. |
| `tests/input/133` | Input-Record/Replay: Tick = `vio_poll_events`-Aufruf seit Start, Tick-0-Events sofort, Replay feuert die Callbacks, Gamepads werden je Poll abgetastet und als virtuelle Pads wiedergegeben, JSON-Round-Trip (Floats werden Ints), unsortierte Skripte (gleicher Tick behält Reihenfolge), `ValueError` mit Entry-Index ohne halbstartetes Replay, `vio_destroy`/Free geben Replay-Pads frei. |
| `tests/input/132` | Virtuelle Gamepads: Slot 0–15 überlagert den physischen Joystick für alle `vio_gamepad_*`-Leser, Default-Zustand (Trigger −1), Achsen geclamped, Reconnect setzt zurück, Fehler für ungültige Slots/Buttons/Achsen und nicht verbundene Pads. |
| `tests/input/131` | Input-Injection nimmt den OS-Eventpfad: `vio_inject_key` feuert `vio_on_key` (mit Action + Mods, auch `VIO_REPEAT`), `vio_inject_scroll` akkumuliert bis `vio_begin`, `vio_inject_char` (Codepoint oder UTF-8) füllt `vio_chars_typed` + `vio_on_char`; Steuerzeichen/kaputtes UTF-8 → `ValueError` ohne Teil-Emission; genau eine `just_pressed`-Flanke, wenn zwischen den Frames injiziert. |
| `tests/render3d/112` | Blend und Write-Mask je Attachment auf einer MRT-Pipeline: Attachment 0 alpha-blendet, Attachment 1 (Maske 0) bleibt unberührt, Attachment 2 schreibt nur den Rotkanal – der Vertrag für einen Transparent-Pass in ein G-Buffer-Target. |
| `tests/render3d/123` | 3D-Konventionen auf allen Backends (Vulkan wie D3D): Tiefentest, Back-Face-Culling, Uniforms je Stage, Textur-V, Instancing (gepackt), RT-Readback-Zeile 0 und gesampelte Orientierung, Depth-only-Target. |
| `tests/render3d/124` | MRT mit gemischten Formaten (RGBA16F / R11G11B10F / RGB10A2 / R8): Readback je Attachment und Sampling von Attachment 1; `'mipmaps' => true`-Texturen filtern bei LOD 2 auf den Mittelwert. |
| `tests/render3d/122` | Variable Rate Shading (D3D12, Vulkan): mit 2X2 teilen benachbarte Pixel eines Blocks denselben Fragment-Wert, mit 1X1 nicht; ohne VRS-Tier liefert `vio_set_shading_rate` `false`. |
| `tests/render3d/121` | Texture-Arrays / BC / KTX2: zweischichtiges RGBA8-Array mit expliziter Mip-Kette (`sampler2DArray`, `textureLod`), ein handkodierter BC1-Block, KTX2-Container (BC1 und RGBA8 mit `mip_offset`), defekte Eingabe → `false`. |
| `tests/render3d/120` | Indirect Draw: zwei Argument-Records (instanceCount 1 / 0) aus einem Storage-Buffer zeichnen genau einen Quad; unindiziertes Mesh mit 4-uint32-Records; ein Compute-Pass, der instanceCount schreibt, steuert den Draw ohne Readback. |
| `tests/core/119` | `shader_model => 6` auf D3D12 (dxcompiler/dxil aus `dxc_dir`, dem Windows-SDK oder dem Suchpfad): `vio_swapchain_info` meldet 6 und `shader_model_version` 60..69, Grafik- und Compute-Shader laufen als DXIL; ohne DXC ehrlicher Fallback auf 5 (51) – außer mit `VIO_REQUIRE_SM6=1` (Windows-CI), dann FAIL (auch 144, 149). |
| `tests/core/118` | HDR10-Ausgabe erzwungen (`hdr_output => 2`): D3D11/D3D12/Vulkan/Metal melden Format RGB10A2 + `hdr_output`, ein weisses 2D-Rechteck kommt PQ-kodiert hell zurueck (Readback expandiert 10 Bit), 3D-Draws laufen ueber die PSO-Format-Variante. |
| `tests/core/117` | `frame_latency => 1`: D3D11/D3D12/Metal melden `waitable` + Latenz 1 in `vio_swapchain_info`, Frames laufen; Backends ohne Feature melden 0 und ignorieren die Option. |
| `tests/core/116` | Shader-Cache: zweiter Kontext im selben Verzeichnis kompiliert denselben Shader aus dem Cache (`stores` > 0 beim ersten, `hits` > 0 beim zweiten Lauf); Vulkan schreibt seine Pipeline-Cache-Datei beim Destroy. |
| `tests/core/115` | `vio_gpu_frame_time`: nach drei gerenderten Frames liefert jedes Backend mit dem Feature eine plausible GPU-Zeit (0 ≤ ms < 5000), ohne Feature −1. |
| `tests/render3d/114` | 16-Bit-Indices: kleines Mesh bekommt 2 Bytes je Index und zeichnet auf jedem Backend korrekt; ein Index ≥ 65536 oder `index_type => VIO_INDEX_UINT32` erzwingt 4 Bytes. |
| `tests/render3d/113` | Stencil: ein Markierungs-Pass (Farbe maskiert, REPLACE ref 1) auf der linken Hälfte, dann EQUAL-/NOTEQUAL-Passes – links grün, rechts rot, keine Farbe aus dem Markierungs-Pass. |
| `tests/render3d/111` | Draw-time-Bind-Tabelle haelt Referenzen: eine als Temporary gebundene RT-Textur (`vio_bind_texture(\, vio_render_target_texture(\), 6)`) ueberlebt bis zum Draw, auch wenn danach weitere Texturobjekte entstehen (D3D11/D3D12/Metal; Regression aus 2.9: recycelter Objektspeicher legte die AO-Karte auf das Schatten-Register). |
| `tests/render3d/096–098` | Storage-Images + 2D-Dispatch (API-Roadmap R2/R7), Multiple Render Targets (R1), Async-Compute im Frame (R7) |
| `tests/backends/108` | OpenGL: `vio_set_uniform()` erreicht UBO-Block-Member, Default-Block-Uniforms und Array-Elemente von SPIR-V-Pfad-Shadern (SPIRV-Cross flacht sie zu `uniform Matrices _19;` ab → GL-Name `_19.uProjection`). |
| `tests/backends/107` | OpenGL-Kontext-Generation: Objekte eines zerstörten Kontexts, die erst freigegeben werden, wenn ein NEUER Kontext current ist, dürfen dessen (wiederverwendete) GL-Namen nicht löschen. |
| `tests/core/099–100`, `render3d/101–105`, `backends/106` | GAP-Plan (`D3D-VULKAN-GAP-PLAN.md`): Audit-Gate für Backend-Zweige in `php_vio.c`, Auto-Backend-Wahl, Sampler-Filter/Wrap, Cube-RT/Mipmaps/Readback auf allen Backends, Mid-Frame-Upload-Ordnung, Anisotropie, RT-MSAA-Resolve, Vulkan-Present-Mode. Die `*_all_backends`-Tests iterieren über `opengl/d3d11/d3d12/metal/vulkan` und drucken pro Backend `OK` oder `skip (…)`. |
| `tests/core/` | Laden, Konstanten, Null-Backend, Context-Lifecycle, Plugins, Audit-Gate 070, Capability-Matrix 074, Perf/Memory-Gates |
| `tests/backends/` | Backend-Registrierung + GPU-Kontexte (OpenGL/Vulkan/Metal/D3D11/D3D12), Cross-Backend-Parity 067, Metal-3D 089, Uniform-Layout Struct-Arrays 094, Texture-Bind-Reihenfolge 095, D3D-Spezifika |
| `tests/render3d/` | Mesh/Shader/Pipeline/Texturen/Buffer/RT/Cubemap/Compute/Vertex-Storage, headless GL |
| `tests/render2d/` | Shapes, Sprites, Fonts, Text-Shaping/-Wrapping, 2D-State-Stacks |
| `tests/input/` | Keyboard/Mouse/Gamepad/Touch/IME, Injection |
| `tests/window/` | Fenstergröße, Display-Metrics, Fullscreen/Borderless, Video-Modes |
| `tests/media/` | Audio, Recorder, Streaming |

Die historischen Nummern (001–089) bleiben erhalten; gleiche Nummern liegen nur in
verschiedenen Ordnern (036/071/073/076). `tests/skipif_gl.inc` ist der geteilte
SKIPIF-Guard für headless GL ≥ 3.3 (`require __DIR__ . '/../skipif_gl.inc'`). Skips sind
plattform-/backend-bedingt (kein headless GL ≥ 3.3 auf macOS-Runnern, Vulkan auf macOS
wegen SIP/DYLD_LIBRARY_PATH, WARP nicht verfügbar, o.ä.) — ein Skip ist kein Fehler, ein
FAIL schon. D3D-spezifische Tests (046/047/049/051/071_d3d12) laufen nur auf Windows.

Das `php` im PATH ist Herd (`~/Library/Application Support/Herd/bin/php`) und zeigt
u.U. auf eine andere Version als die, gegen die gebaut wurde — `TEST_PHP_EXECUTABLE`
explizit auf das passende Binary (`php85`, Homebrew `php`) setzen.

## Architektur

### Backend-Dispatch (Vtable-Pattern)

Alle GPU-Operationen gehen durch `vio_backend` Vtable in `include/vio_backend.h`. Backends registrieren sich in MINIT. Auto-Auswahl ist plattformspezifisch:
- macOS: Metal > OpenGL
- Windows: D3D12 > D3D11 > Vulkan > OpenGL
- Linux: Vulkan > OpenGL

`null` wird nie auto-gewählt. iOS (`--with-ios`) ist kein GPU-Backend, sondern
ersetzt die GLFW-Fenster/Input-Hälfte (`src/backends/ios/`) und rendert über Metal.

```
vio_create("opengl"|"vulkan"|"metal"|"d3d11"|"d3d12"|"null"|"auto", [...])
  → vio_find_backend(name) → backend->create_surface()
```

### Backend-Feature-Matrix (Stand v2.8.0 + Metal-3D)

Quelle: die `*_supports_feature()`-Implementierungen. `vio_supports_feature($ctx, VIO_FEATURE_*)`
liefert das zur Laufzeit; `tests/core/074_backend_capability_matrix.phpt` pinnt den Kontrakt.

| Feature | OpenGL | D3D11 | D3D12 | Vulkan | Metal |
|---|---|---|---|---|---|
| 3D-Pipeline (`vio_mesh`/`vio_shader`/`vio_pipeline`/`vio_draw`) | ✅ | ✅ | ✅ | ✅ (SPIR-V → Vulkan-GLSL → SPIR-V, GAP-PHASE5 Block 10) | ✅ |
| Native 2D-Batch | ✅ | ✅ | ✅ | ✅ | ✅ |
| Render Target (Basis) | ✅ | ✅ | ✅ | ✅ | ✅ |
| Render Target HDR / Depth-only / MSAA | ✅/✅/✅ (Multisample-Renderbuffer + Blit) | ✅/✅/✅ (Resolve beim Unbind/Readback) | ✅/✅/✅ (PSO-Sample-Varianten, GAP-PHASE5 1) | ✅/✅/✅ (Resolve-Attachments im Pass, GAP-PHASE5 10b) | ✅/✅/✅ |
| Cubemap | ✅ | ✅ | ✅† (seit 2.9: Upload war vorher nicht implementiert) | ✅ (6-Layer-Image, Block 10b) | ✅ |
| Compute (`vio_compute_*`) | ✅ (GL ≥ 4.3 → auf macOS nie) | ✅ | ✅ | ✅ | ✅ |
| Vertex-Storage (`vio_draw_instanced_from_buffer`) | ✅ (wenn Compute) | ✅ | ✅ | ✅ | ✅ |
| Texture 3D | ✅ | ✅ | ✅ | ✅ | ✅ |
| read_pixels | ✅ | ✅ | ✅ | ✅ | ✅ |
| Texture Swizzle | ✅ (3.3+) | ❌ (CPU-Expand) | ❌ (CPU-Expand) | ✅ | ✅ |
| Layered RTs (`'layers' => N`-Arrays, Depth-Cube; `VIO_FEATURE_RENDER_TARGET_LAYERED`) | ✅ (`GL_TEXTURE_2D_ARRAY` / Depth-Cubemap, `glFramebufferTextureLayer`) | ✅ (RTV/DSV je Slice) | ✅ (RTV/DSV je Slice) | ✅ (Framebuffer je Layer, `2D_ARRAY`/`CUBE`-Views) | ✅ (`MTLTextureType2DArray` Farbe + Tiefe, Depth32Float-Cube; Slice je Bind) |
| Layered Rendering (`VIO_RT_ALL_LAYERS`, `gl_Layer`; `VIO_FEATURE_LAYERED_RENDER` / `_VERTEX_LAYER`) | ✅ (`glFramebufferTexture`; VS-Layer mit `GL_ARB_shader_viewport_layer_array`) | ✅ (RTV/DSV über alle Slices; GS per Probe, VS-Layer per `D3D11_OPTIONS3`) | ✅ (dto.; VS-Layer immer) | ✅ (Framebuffer `layers = N`; VS-Layer mit `VK_EXT_shader_viewport_index_layer`) | ✅ (`renderTargetArrayLength`, `[[render_target_array_index]]` aus VS oder emuliertem GS; Mac2/Apple5) |
| Mehrere Viewports (`vio_viewports`, `gl_ViewportIndex`; `VIO_FEATURE_MULTI_VIEWPORT`) | ✅ (GL 4.1 / `ARB_viewport_array`, `glViewportIndexedf`) | ✅ (`RSSetViewports(n)`, 3D ohne Scissor) | ✅ (Viewport + Scissor je Eintrag) | ✅ (`multiViewport`; 3D-Pipelines tragen immer `max_viewports` Viewports, `vk3d_prepare` setzt alle) | ✅ (`setViewports:count:` + Scissor je Eintrag, `[[viewport_array_index]]` im VS; Mac2/Apple5) |
| GS-Instancing (`layout(invocations = N)`, `VIO_FEATURE_GEOMETRY_INSTANCING`) | ✅ (GL ≥ 4.0 / `ARB_gpu_shader5`) | ✅‡ (GLSL über SPIR-V-Umbau: `SV_GSInstanceID` + `[instance(N)]`) | ✅‡ | ✅ | ✅ (emuliert: Thread je Primitiv × Invocation) |
| Adjacency-Topologien (`VIO_*_ADJACENCY`, `vio_mesh(['adjacency' => true])`) | ✅ | ✅ | ✅ | ✅ | ✅ (emuliert; ohne `TRIANGLE_STRIP_ADJACENCY`) |
| HLSL-Stage-Override (`'hlsl' => [stage => src]`, `VIO_FEATURE_HLSL_STAGE_OVERRIDE`) | — | ✅ | ✅ (auch DXIL / SM 6) | — | — |
| Cubemap-RT + `vio_generate_mipmaps` | ✅ | ✅ (`GenerateMips`) | ✅ (Compute-Downsample, CPU-Fallback) | ✅ (Framebuffer je Face/Level, `vkCmdBlitImage`-Kette) | ✅ |
| `vio_read_render_target` | ✅ | ✅ | ✅† | ✅ (nach `vio_end`, Face und Attachment) | ✅ |
| `vio_texture_update` | ✅ | ✅ | ✅ | ✅ (Staging-Buffer + Transient-Command-Buffer, Level 0) | ✅ |
| `depth_write` / `color_mask` / Blend-Modi | ✅ | ✅ | ✅ | ✅ | ✅ |
| MRT (`'attachments' => [VIO_FORMAT_*…]`, bis 4) | ✅ | ✅† | ✅† | ✅ (Block 10b) | ✅ |
| Blend/Write-Mask je Attachment (`attachment_blend`, `attachment_color_mask`) | ✅ (GL ≥ 4.0, indexed) | ✅ (IndependentBlend) | ✅ (IndependentBlend) | ✅ (`independentBlend`) | ✅ (per colorAttachment) |
| uint16-Indices (automatisch, `vio_mesh_index_bytes`) | ✅ | ✅ (R16_UINT) | ✅ (R16_UINT) | ✅ (`VK_INDEX_TYPE_UINT16`) | ✅ (MTLIndexTypeUInt16) |
| Shader-/Pipeline-Cache auf Platte (`vio_create(['shader_cache' => dir])`, `vio_shader_cache_stats`) | ✅ (GL ≥ 4.1 Program-Binary) | ✅ (DXBC je Stage) | ✅ (DXBC je Stage) | ✅ (`VkPipelineCache`) | — (Metal cacht selbst) |
| Indirect Draw (`vio_draw_indirect`, `vio_storage_buffer(['indirect' => true])`, `VIO_FEATURE_INDIRECT_DRAW`) | ✅ (GL ≥ 4.0 `glDraw*Indirect`) | ✅ (`Draw*InstancedIndirect`) | ✅ (`ExecuteIndirect`) | ✅ (`vkCmdDraw(Indexed)Indirect`, Multi-Draw wenn verfügbar) | ✅ (`indirectBuffer:`) |
| Texture-Arrays + BC + KTX2 (`vio_texture(['layers', 'format' => VIO_FORMAT_BC*, 'mip_levels'])`, `vio_texture_ktx2`, `VIO_FEATURE_TEXTURE_ARRAY` / `_TEXTURE_COMPRESSION_BC`) | ✅ (`GL_TEXTURE_2D_ARRAY`, S3TC/RGTC/BPTC) | ✅ | ✅ | ✅ (2D-Array-Views, `textureCompressionBC`, Block 10c) | ✅ (`MTLTextureType2DArray`, BC-Formate) |
| Variable Rate Shading (`vio_set_shading_rate`, `VIO_SHADING_RATE_*`, `VIO_FEATURE_SHADING_RATE`) | ❌ | ❌ | ✅ (`RSSetShadingRate`, Tier 1+; 4X4 nur mit Additional Rates) | ✅ (`VK_KHR_fragment_shading_rate`, Pipeline-Rate als Dynamic State, Block 10c) | ❌ |
| VRS pro Primitiv (`gl_PrimitiveShadingRateEXT`, `VIO_FEATURE_SHADING_RATE_PRIMITIVE`) | ❌ (SPIRV-Cross: nur Vulkan-GLSL) | ❌ | ✅ (Tier 2 + SM 6.4 `SV_ShadingRate`, Combiner OVERRIDE für Pipelines, deren VS die Rate schreibt) | ✅ (`primitiveFragmentShadingRate`, Combiner REPLACE je Pipeline) | ❌ |
| VRS-Bild (`vio_set_shading_rate_image`, `VIO_FEATURE_SHADING_RATE_IMAGE`) | ❌ | ❌ | ✅ (Tier 2: R8_UINT-Textur in `SHADING_RATE_SOURCE`, `RSSetShadingRateImage`, Combiner MAX; nach jedem Listen-Reset neu gesetzt) | ❌ (bräuchte `vkCreateRenderPass2` + Fragment-Shading-Rate-Attachment in jedem Pass; kein Treiber in CI/lokal) | ❌ (Rasterization Rate Maps sind ein anderes Modell) |
| Bindless-Texturtabelle (`vio_texture_index`, `texture2D vio_textures[]` + `sampler vio_sampler` in Set 1, `VIO_FEATURE_BINDLESS`) | ❌ (GL-GLSL hat keine getrennten Texturen; `ARB_bindless_texture` nicht gewired) | ❌ | ✅ (Resource Binding Tier 2+: Root-Parameter [15] unbegrenzter SRV-Bereich `t0, space1`, statischer Sampler `s1, space1`, 1024 reservierte Deskriptoren oben im shader-sichtbaren SRV-Heap; FXC 5.1 und DXC) | ✅ (`VK_EXT_descriptor_indexing`: globales Set 1, 1024 Sampled Images `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`, unveränderlicher Sampler) | ✅ (SPIRV-Cross-Argument-Buffer nur für Set 1, `device`-Adressraum, `gpuResourceID`s auf `[[buffer(21)]]`, `useResources` je Draw, `constexpr`-Sampler; Cap `bindless`: Metal3 + Tier 2 + MSL 3.0) |
| Sampler Feedback (`vio_sampler_feedback_bind/read/clear`, HLSL-Fragment-Override, `VIO_FEATURE_SAMPLER_FEEDBACK`) | ❌ | ❌ | ✅ (SM 6.5 + `OPTIONS7.SamplerFeedbackTier ≥ 0.9` + `ID3D12Device8` + Bindless-Layout: MinMip-Map je Textur über `CreateCommittedResource2`, UAV-Table `u0, space2` als Root-Parameter [16] (PIXEL), Decode per `ResolveSubresourceRegion(DECODE_SAMPLER_FEEDBACK)` nach R8_UINT) | ❌ (Sparse Residency liefert Residenz, kein Zugriffs-Feedback) | ❌ (Sparse Textures: dto.) |
| Ray Query (`vio_acceleration_structure`, `rayQueryEXT`, `VIO_FEATURE_RAY_QUERY`) | ❌ | ❌ | ✅ (DXR Tier 1.1 + SM 6.5 `RayQuery<>`; TLAS als Root-SRV `t0, space9` — Grafik-Root-Parameter [14], Compute-Parameter [3]; Bau synchron über `ID3D12GraphicsCommandList4`) | ✅ (`VK_KHR_acceleration_structure` + `VK_KHR_ray_query` + Abhängigkeiten als Extensions auf der 1.1-Instanz; Binding 33 im 3D-Set, Compute an der GLSL-Binding; Bau im Transient-Command-Buffer) | ✅ (`MTLPrimitive`/`MTLInstanceAccelerationStructureDescriptor`, `set*AccelerationStructure` + `useResource` der BLAS; MSL 2.4) |
| Shader Model 6 / DXC (`vio_create(['shader_model' => 6, 'dxc_dir' => …])`, `vio_swapchain_info()['shader_model' / 'shader_model_version']`) | — | — (FXC 5.0) | ✅ (DXIL via `dxcompiler.dll` + `dxil.dll`, Profil = höchstes 6.x, das Device **und** DXC/dxil.dll können; SPIRV-Cross übersetzt auf dasselbe Profil; Fallback FXC 5.1) | — | — |
| Subgroups (`GL_KHR_shader_subgroup_*` in Compute + Fragment, `VIO_FEATURE_SUBGROUP`) | ✅ (`GL_KHR_shader_subgroup`, Stages/Features per `glGetIntegerv`; braucht Compute) | ❌ | ✅ (nur mit SM 6 + `OPTIONS1.WaveOps`: Wave-Intrinsics) | ✅ (`VkPhysicalDeviceSubgroupProperties`: Compute + Fragment, basic/vote/ballot/arithmetic/shuffle) | ✅ (`simd_group`: MSL 2.2, Mac2/Apple7) |
| Quad-Ops im Fragment-Shader (`GL_KHR_shader_subgroup_quad`, `VIO_FEATURE_SUBGROUP_QUAD`) | ✅ (`GL_SUBGROUP_FEATURE_QUAD_BIT_KHR` + Fragment-Stage) | ❌ | ✅ (SM 6 + `WaveOps`, `QuadReadAcross*`) | ✅ (`VK_SUBGROUP_FEATURE_QUAD_BIT`) | ✅ (`quad_group`: MSL 2.1, Mac2/Apple4) |
| Barycentrics (`gl_BaryCoordEXT`, `VIO_FEATURE_BARYCENTRICS`) | ✅ (`GL_EXT_fragment_shader_barycentric`) | ❌ | ✅ (SM 6.1 + `OPTIONS3.BarycentricsSupported`, `SV_Barycentrics`) | ✅ (`VK_KHR_fragment_shader_barycentric`, Feature im pNext-Chain) | ✅ (`barycentrics`: MSL 2.2) |
| 64-Bit-Atomics auf Storage-Buffern (`GL_EXT_shader_atomic_int64`, `VIO_FEATURE_ATOMIC64`) | ✅ (Compute + `GL_ARB_gpu_shader_int64` + `GL_NV_shader_atomic_int64`) | ❌ | ✅ (SM 6.6 + `Int64ShaderOps`; vio benennt SPIRV-Cross' `.InterlockedX(` mit 64-Bit-Operand in `.InterlockedX64(` um — sonst kürzt DXC still auf 32 Bit) | ✅ (`shaderInt64` + `VK_KHR_shader_atomic_int64`) | ❌ (Metal nur min/max, SPIRV-Cross lehnt 64-Bit-Atomics für MSL ab) |
| 16-Bit-Floats (`float16_t`, `VIO_FEATURE_SHADER_FLOAT16`) | ✅ (`GL_AMD_gpu_shader_half_float` / `GL_NV_gpu_shader5`) | ❌ | ✅ (SM 6.2 + `Native16BitShaderOpsSupported`: SPIRV-Cross-Option 16-Bit-Typen → `half`, DXC `-enable-16bit-types`; vorher `min16float`) | ✅ (`VK_KHR_shader_float16_int8` `shaderFloat16`) | ✅ (`half`) |
| Draw-Parameter (`gl_BaseVertex`/`gl_BaseInstance`, `VIO_FEATURE_BASE_VERTEX`) | ✅ (4.6 / `ARB_shader_draw_parameters` + Base Instance 4.2) | ❌ | ✅ (SM 6.8 `SV_StartVertexLocation`/`SV_StartInstanceLocation`; darunter bräuchte SPIRV-Cross einen cbuffer, den indirekte Draws nicht füllen können) | ✅ (`shaderDrawParameters` + `drawIndirectFirstInstance`, letzteres war vorher nie aktiviert) | ✅ (`[[base_vertex]]`/`[[base_instance]]`, Mac2/Apple3; nicht in emulierten GS/Tess-Pipelines) |
| Compute-Derivate (`derivative_group_quadsNV`, `VIO_FEATURE_COMPUTE_DERIVATIVES`) | ✅ (`GL_NV_compute_shader_derivatives`; SPIRV-Cross verliert den Ausführungsmodus, `vio_spirv_to_glsl_compute` setzt Extension + Layout wieder ein) | ❌ | ✅ (SM 6.6) | ✅ (`VK_NV`/`VK_KHR_compute_shader_derivatives`) | ❌ (Kernel ohne Derivate) |
| Multiview (`vio_shader(['view_count' => 2..4])`, `gl_ViewIndex`, `VIO_FEATURE_MULTIVIEW`) | ✅ (`GL_OVR_multiview2`: SPIRV-Cross `num_views`, Attachments je Draw per `glFramebufferTextureMultiviewOVR`, View-Zahl je Programm) | ❌ | ✅ (SM 6.1 + `ViewInstancingTier`: PSO über Pipeline-State-Stream mit `VIEW_INSTANCING`, `SetViewInstanceMask` vor jedem Draw) | ✅ (Render-Pass mit `viewMask` je RT lazily, `vk3d_prepare` schaltet zwischen Multiview- und Layered-Pass um) | ✅ (SPIRV-Cross-Instancing-Emulation: Instanzen × N, Instanz-Attribute `stepRate` N, `spvViewMask` auf Buffer 23, indirekte Draws über die CPU; Mac2/Apple5) |
| HDR10-Ausgabe (`vio_create(['hdr_output' => 1])`, RGB10A2 + ST 2084, 2D-Batch PQ-kodiert, `VIO_FEATURE_HDR_OUTPUT`) | — | ✅ | ✅ (PSO-Format-Varianten) | ✅ (10-Bit-Surface-Format + `VK_EXT_swapchain_colorspace` HDR10 ST 2084, Block 10d) | ✅ (`CAMetalLayer` RGB10A2 im BT.2100-PQ-Farbraum, `hdr_output => 1` nur auf EDR-Displays) |
| Waitable Swapchain (`vio_create(['frame_latency' => n])`, `vio_swapchain_info`, `VIO_FEATURE_FRAME_LATENCY`) | — | ✅ (`FRAME_LATENCY_WAITABLE_OBJECT`) | ✅ | — (Präsentmodus) | ✅ (Dispatch-Semaphore über die Frames in Flight, 1..3) |
| GPU-Zeit je Frame (`vio_gpu_frame_time`, `VIO_FEATURE_GPU_TIMESTAMP`) | ✅ (GL ≥ 3.3 `GL_TIMESTAMP`) | ✅ (TIMESTAMP + DISJOINT) | ✅ (Query-Heap + Readback) | ✅ (`vkCmdWriteTimestamp`) | ✅ (`GPUStartTime/GPUEndTime`) |
| Stencil (`'stencil' => [...]`, `VIO_FEATURE_STENCIL`) | ✅ (DEPTH24_STENCIL8) | ✅ (D24S8) | ✅ (D24S8, `OMSetStencilRef`) | ✅ (D32S8 / D24S8) | ✅ (`Depth32Float_Stencil8` auf Swapchain und allen RTs, auch depth_only/Cube/Array; Test 147) |
| Geometry-Stage (`vio_shader(['geometry' => …])`, `VIO_FEATURE_GEOMETRY`) | ✅ (GL ≥ 3.2) | ✅‡ | ✅‡ | ✅ (`geometryShader`) | ✅ (Compute-Emulation, `METAL-GEOMETRY-PLAN.md`) |
| Tessellation (`tess_control` + `tess_eval`, `VIO_PATCHES`, `VIO_FEATURE_TESSELLATION`) | ✅ (GL ≥ 4.0) | ✅‡ (GLSL über vios Hull/Domain-Generator `vio_tess_hlsl.c`, oder HLSL-Override) | ✅‡ (dto., auch DXIL / SM 6) | ✅ (`tessellationShader`, Domain-Ursprung unten links) | ✅ (VS/TCS als Compute-Kernel + `drawPatches`; Isolines/`point_mode` emuliert, Test 148) |
| Storage-Images (`'storage' => true` + `vio_compute_bind_image`) | ✅ (wenn Compute) | ✅† | ✅† | ✅ (`STORAGE_IMAGE` aus der Reflection, Bild in `GENERAL`) | ✅ |
| Compute-`local_size` aus Reflection (2D/3D-Dispatch) | ✅ | ✅ | ✅ | ✅ | ✅ |
| Async-Dispatch im Frame (`['async' => true]`, `vio_compute_wait`) | ✅ (Queue in-order) | ✅ (in-order) | ✅† (Frame-List) | ✅ (Frame-Command-Buffer, Pass wird geschlossen und mit LOAD fortgesetzt) | ✅ (Frame-Cmd-Buffer) |

‡ D3D: das Flag ist nur 1, wenn das gelinkte SPIRV-Cross HLSL für die Stage emittiert **und FXC
es annimmt** (`vio_hlsl_stage_supported()` + `d3d1x_stage_supported()`, Probe einmal pro Prozess;
`VIO_DEBUG_STAGE_PROBE=1` druckt den Grund). Stand SPIRV-Cross 2026-10: **Geometry ja**. Die beiden
GS-Builtins, die das HLSL-Backend ablehnt („Unsupported builtin in HLSL"), schreibt
`vio_gs_hlsl_rewrite()` vorher im SPIR-V um: `gl_in[i].gl_Position` wird ein eigener Eingang mit
Semantik `SV_Position`, `gl_InvocationID` eine private Variable aus `SV_GSInstanceID` mit
`[instance(N)]` (Test 143) – damit laufen GLSL-GS mit `gl_in` und GS-Instancing auf D3D ohne Override.
**Hull/Domain**: SPIRV-Cross hat (noch) kein HLSL-Tessellations-Backend (KhronosGroup/SPIRV-Cross#2693/#2694
fügen es hinzu). Bis dahin baut vio Hull- und Domain-Shader selbst (`vio_tess_hlsl.c`, siehe
„Geometry- und Tessellation-Stages"); die Probe übersetzt dafür ein TCS/TES-Paar, das Flag ist damit mit
jedem SPIRV-Cross 1. Der HLSL-Stage-Override (`'hlsl' => [...]`, Test 140) bleibt und hat Vorrang. Die SPIRV-Cross-Libs in `C:\php-sdk\vio-build-deps` (SDK
1.4.341) können Geometry, die Windows-CI nutzt dasselbe SDK (seit v2.30; mit 1.3.296 waren `GEOMETRY` = 0 und GS-Tests auf D3D übersprungen, auch in den Release-DLLs).

† D3D11/D3D12: implementiert, aber ohne Windows-Build hier nur blind editiert — Windows-CI
(WARP) ist der Beleg (`tests/render3d/096`, `097`). Die frueher dort beobachtete "veraltete
Textur nach GPU-Schreibzugriff" auf D3D12 war KEIN Barrier-Problem, sondern die Pending-Bind-
Tabelle ohne Referenz (Test 111): `vio_render_target_texture()` liefert ein Temporary, dessen
Speicher die naechste VioTexture wiederverwendete. Seit dem Fix laufen die D3D12-Pixel-Checks
in 096/097 wieder mit.

MSAA-Render-Targets (`'samples' => N`) resolven auf jedem Backend; Depth-only-, Cube-, Array- und
(auf GL/D3D11) MRT-Targets bleiben single-sampled.

Vulkan-3D (GAP-PHASE5 Block 10, `src/backends/vulkan/vio_vulkan_3d*.c`): Shader gehen GLSL →
SPIR-V → Vulkan-GLSL (SPIRV-Cross, Bindings umgelegt: Set 0, 0/1 = Default-Uniform-Block VS/FS
als `UNIFORM_BUFFER_DYNAMIC` im Frame-Upload-Ring, 2–17 = Sampler im D3D12-Register-Schema,
18–25 = Storage-Buffer) → SPIR-V; der Vertex-Stage wird `y = -y; z = (z + w) / 2` angehängt,
Varyings bekommen Locations über den Namen. Damit gilt die D3D-Konvention: NDC +Y = RT-Zeile 0,
Tiefe 0..1, `frontFace` CCW (Test `123`). Pipelines sind Varianten je Render-Pass-Signatur und
Vertex-Stride; Freigaben mitten im Frame parken bis zum Fence des Slots. Headless-Kontexte
kopieren jedes präsentierte Bild in einen Host-Buffer (`vio_read_pixels` auch mitten im Frame).
Render-Targets (Block 10b, `vio_vulkan_rt.c`): bis 4 Attachments in jedem `VIO_FORMAT_*` (RGBA8 =
B8G8R8A8, damit der 2D-Batch passt), MSAA mit Resolve-Attachments im Pass, Cube-Targets mit einem
Framebuffer je (Face, Level), Level > 0 ohne Tiefe. Cubemaps und Mip-Ketten (`'mipmaps' => true`,
`vio_generate_mipmaps`) laufen über `vkCmdBlitImage` (`vio_vulkan_cube.c`). `vio_clear` im Frame
löscht die Attachments des offenen Passes (`vkCmdClearAttachments`, wie D3D12). Ein Bind
behält den Inhalt (`loadOp LOAD` für Farbe und Tiefe, Bilder beim Anlegen initialisiert) wie auf
GL/D3D – vorher löschte jeder Bind mit der zuletzt gesetzten Clear-Farbe (Test 142). Freigaben mitten im
Frame parken auch hier bis zum Fence. Compute: das Descriptor-Set-Layout kommt aus der Reflection des Kernels
(Storage-Buffer, Storage-Images, Params-UBO), jeder Dispatch bekommt ein eigenes Set aus Per-Frame-Pools und
einen Params-Snapshot im Upload-Ring; `['async' => true]` im Frame zeichnet in den Frame-Command-Buffer auf
(Pass schließen, Barrieren, mit LOAD fortsetzen), `vio_compute_wait` mitten im Frame submittet den Frame bis dahin
und öffnet ihn wieder (`vio_vk_flush_frame`). Storage-Texturen liegen in `GENERAL`. `vio_texture_update` kopiert
über Staging-Buffer + Transient-Command-Buffer in Level 0. Texture-Arrays, BC-Daten und gespeicherte Mip-Ketten (KTX2) laufen über
`vio_vk_create_texture_ex`, eine Kopie je Level deckt alle Layer ab. Variable Rate Shading nutzt
`VK_KHR_fragment_shading_rate` als Dynamic State jeder 3D-Pipeline; die Einstiegspunkte kommen per
`vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`, die Instanz läuft dafür mit API 1.1 (Block 10c).
HDR10-Swapchain (Block 10d): `hdr_output => 1` nimmt ein 10-Bit-Surface-Format im Farbraum HDR10 ST 2084,
`=> 2` erzwingt 10 Bit auch auf SDR-Desktops; Render-Pass und jede Swapchain-Neuanlage nutzen dieselbe
Formatwahl, der 2D-Batch PQ-kodiert über einen gemeinsamen Push-Constant-Block (mat4 + vec4), und
`vio_read_pixels` expandiert 10 Bit wie D3D.
Geometry-/Tessellation-Stages (Flags folgen den Device-Features `geometryShader` /
`tessellationShader`): Varyings werden über die ganze Kette VS → TCS → TES → GS → FS per Name
verbunden, der Clip-Space-Fixup wandert in die **letzte** Position schreibende Stage (GS: vor jedes
`EmitVertex()`, TES: Ende von `main`), Default-Uniform-Blöcke der Extra-Stages liegen auf Binding
30–32 (dynamisch, Frame-Ring wie 0/1), `VIO_PATCHES` + `VkPipelineTessellationStateCreateInfo`.
Der Shader-Cache-Schlüssel jeder Stage enthält die SPIR-V aller vorigen Stages und das
„letzte Stage"-Flag (gleiches VS mit und ohne GS kompiliert verschieden, Test 135).
Metal: Geometry-Shader gibt es in Metal nicht; vio emuliert sie über Compute-Kernel, die Tessellation
läuft ebenfalls compute-basiert (siehe „Metal-3D-Pipeline").

#### Geometry- und Tessellation-Stages (`vio_shader` `geometry` / `tess_control` + `tess_eval`)

- **API**: `vio_shader($ctx, ['vertex' => …, 'geometry' => $gs, 'fragment' => …])` bzw.
  `['tess_control' => $tcs, 'tess_eval' => $tes]` (immer als Paar). Pipeline mit Tessellation
  zeichnet **immer** `VIO_PATCHES` (`'patch_vertices' => N`, 1..32, Default 3), egal was
  `topology` sagt; `VIO_PATCHES` ohne Tessellation-Stages lehnt `vio_pipeline` ab.
  `vio_shader_reflect()` liefert die Stages unter denselben Keys.
- **HLSL-Override (D3D)**: `'hlsl' => ['tess_control' => $hs, 'tess_eval' => $ds, 'geometry' => $gs]` —
  das Backend kompiliert das HLSL statt der übersetzten GLSL-Stage; die GLSL-Stage bleibt Pflicht
  (GL/Vulkan, Uniform-Layout). Vertrag: Stage-Uniforms in `cbuffer … : register(b0)` mit denselben
  Namen und Offsets wie in GLSL (sonst Warning aus `vio_d3d_check_override_cbuffer`, per
  `D3DReflect`; unter SM 6 entfällt die Prüfung), Eingänge mit den SPIRV-Cross-Semantiken der
  Vorstufe (`SV_Position`, `TEXCOORD<location>`), Ausgabe im D3D-Clipspace (z ∈ [0, w]), kein Fixup.
- **Gate**: `vio_shader()` lehnt die Stage vor jedem Backend-Aufruf ab, wenn
  `VIO_FEATURE_GEOMETRY`/`TESSELLATION` 0 ist (Warning + `false`) — kein Backend-Zweig in
  `php_vio.c`, Audit-Gate 099 bleibt unverändert.
- **OpenGL**: alle Stages hängen im selben Programm (`vio_opengl_compile_program`; der
  Program-Binary-Cache hasht alle fünf Stages). Draw-Mode kommt aus der Pipeline-Topology
  (`gl_draw_mode()`, auch für `glDraw*Indirect`). Ein Uniform, das mehrere Stages deklarieren,
  wird von SPIRV-Cross pro Stage als eigenes Struct-Uniform emittiert; `opengl_set_uniform`
  schreibt alle Treffer (`gl_uniform_location_all`).
- **D3D11/D3D12**: SPIR-V → HLSL `gs/hs/ds_5_0|5_1` über denselben Compile-Pfad wie VS/PS
  (`d3d1x_compile_cached`: DXBC-Cache, unter Shader Model 6 DXIL — eine PSO darf DXBC und DXIL
  nicht mischen). Der GL→D3D-Depth-Fixup läuft nur in der **letzten** Position schreibenden Stage.
  Jede Extra-Stage hat ihren eigenen Constant-Block (`vio_shader_stage_cb`, Vtable-Slot
  `bind_stage_constants`: D3D11 `GS/HS/DSSetConstantBuffers b0`, D3D12 Root-CBV-Slice aus dem
  Frame-Ring). D3D12-Root-Signature: `[5..7]` CBV, `[8..10]` SRV-Table, `[11..13]` Sampler-Table
  mit GEOMETRY/HULL/DOMAIN-Visibility (`VIO_D3D12_RP_*`). Die PSO-Vorlage (`pso_desc`) trägt die
  Stages, MSAA-/Format-Varianten erben sie. `bind_pipeline` setzt fehlende Stages auf NULL (D3D11).
- **Hull/Domain aus GLSL (D3D, `vio_tess_hlsl.c`)**: `vio_tess_to_hlsl` bekommt **beide** Stages. Die SPIR-V
  wird zu einer Vertex-Stage umgebaut (Execution-Model, Tessellations-Modes und im TCS Barrieren weg;
  TCS-Ein-/Ausgänge und TES-Eingänge werden `Private`, die TES-Ausgänge bleiben echte VS-Ausgänge mit
  Depth-Fixup), SPIRV-Cross übersetzt den Körper (`vert_main`, gleiche Optionen/Register wie VS/PS über
  `vio_spirv_to_hlsl_hooked`), vio ersetzt `main`: Hull = Patch-Constant-Funktion (alle Ausgangs-
  Kontrollpunkte laden, Körper je Invocation) + Kontrollpunkt-Funktion; Domain = `[domain]`-Einstieg
  mit `OutputPatch`, Patch-Struct, `SV_DomainLocation`. Kontrollpunkt- und Patch-Struct (`TEXCOORD<loc>`,
  `PATCH<loc>`, Faktoren) entstehen aus der TCS und sind in HS und DS identisch. GL-Konvention: D3D hat
  dieselben Domain-Koordinaten und Faktor-Kanten wie GL, nur die Winding ist umgekehrt → `outputtopology`
  wird gedreht, `gl_TessCoord` nie gespiegelt (auf WARP gemessen, Test 144). Das Shader-Objekt behält
  beide SPIR-V-Module: weicht `patch_vertices` von `layout(vertices = N)` ab, baut `vio_pipeline` eine
  HS-Variante (D3D11 eigenes `ID3D11HullShader`, D3D12 `hs_variant`-Blob der PSO). Grenzen (Warnung,
  Override als Ausweg): Interface-Blöcke, Matrix-/Struct-Varyings, `gl_ClipDistance`, Lesen fremder
  Kontrollpunkte in der Kontrollpunkt-Phase. `VIO_DUMP_TESS_SPV=<prefix>` schreibt das umgebaute SPIR-V.
- **Tiefe im GS (D3D)**: SPIRV-Cross setzt den GL→D3D-Depth-Fixup ans Ende des GS-Einstiegs, hinter
  jedes `Append` — wirkungslos. `vio_spirv_to_hlsl_ex` rechnet deshalb jede ausgegebene Kopie
  (`stage_output.gl_Position`) selbst um (Test 109: Punkt bei z = −0.5).
- **`gl_ViewportIndex` im Vertex-Shader**: SPIRV-Cross lässt die nötige `#extension` weg (bei
  `gl_Layer` nicht); `vio_glsl_require_viewport_layer_ext()` ergänzt sie im GL- und Vulkan-Pfad.
- **Portabler GS**: Positionen als `layout(location = N) out vec4` aus dem VS exportieren und im
  GS über `vPos[i]` lesen statt `gl_in[i].gl_Position` — auf GL/Vulkan geht beides, auf D3D nur
  das Varying (SPIRV-Cross-Limitierung, s. ‡). Test 109 zeigt das Muster.
- **Texturen in Extra-Stages**: Sampler, die nur eine Extra-Stage deklariert, hängen sich hinter
  die Fragment-Sampler an die GL-Unit-Map (`vio_shader_merge_stage_samplers`, Reihenfolge GS,
  TCS, TES; Vulkan spiegelt das in `fs_sampler_binding`). Sampler in allen Stages in derselben
  Reihenfolge deklarieren.
- **Alle Draw-Pfade** (`vio_draw`, `vio_draw_instanced`, `vio_submit_batch`, `vio_draw_indirect`,
  `vio_draw_instanced_from_buffer`) pushen die Extra-Stage-Constants über
  `vio_push_extra_stage_constants` (Test 135).
- **Metal** (kein Hull/Domain): VS → Compute-Kernel (`vertex_for_tessellation`, Stage-Input-
  Deskriptor je Mesh-Stride, Thread je Vertex × Instanz), TCS → Kernel (`multi_patch_workgroup`,
  Threadgroup je Patch, schreibt Kontrollpunkte, Patch-Konstanten und Half-Faktoren), TES →
  `[[patch]]`-Vertex-Funktion (`raw_buffer_tese_input`, liest die Kontrollpunkte aus den
  Puffern). Jeder Draw: beide Kernel in eigenem, sofort committetem Command-Buffer in Ring-Slices,
  dann `drawPatches` auf dem offenen Render-Encoder (Texturen/Viewport bleiben). Indizierte Draws
  werden auf der CPU de-indiziert. Die TCS bekommt die Domain der TES gesetzt (Faktor-Struct), die
  TES die Kontrollpunktzahl der TCS. Metals Tessellator hat GLs Domain-Koordinaten, Faktor-Kanten und
  Winding-Bezeichnung (Test 144, macOS-CI): keine MSL-Ursprungs-Option, keine Winding-Umkehr.
  `vio_draw_indirect` liest die Argument-Records auf der CPU (Shared-Buffer, async Dispatches des
  Frames laufen vorher), `vio_draw_instanced_from_buffer` bindet die SSBO an den Vertex-Kernel.
  **Isolines und `point_mode`** (Metals Tessellator hat beides nicht): nach dem Control-Kernel
  liest vio die Level zurück (diese Draws warten auf ihre Kernel), rundet sie nach GL-Spacing und
  treibt den Tessellator mit exakten Integer-Faktoren (Isolines als Quad-Domain, SPIR-V-
  Execution-Mode `Isolines` → `Quads`). Die TES läuft als Capture-Funktion in einem Pass ohne
  Rasterisierung (`metal_tess_emul_msl`): jeder benötigte Domain-Punkt landet in einem festen Slot,
  `gl_TessCoord` wird auf die Fractional-Regel umgerechnet (die zwei kurzen Segmente liegen an den
  Enden), zusätzliche Punkte der Integer-Tessellation verfallen. Der Draw liest die Slots über
  `vio_pass` (dieselbe Library, dasselbe Output-Struct) als Linien bzw. Punkte. Grenze: innere Ringe
  eines `point_mode`-Dreiecks folgen bei Fractional-Spacing der Integer-Lage. Ein async Compute-Dispatch
  desselben Frames läuft **nach** den Tessellation-Kerneln. Varyings zwischen den Stages müssen
  in Location-Reihenfolge übereinstimmen (die Puffer-Structs werden je Stage gebaut). Der
  Transpiler liegt als reines C in `src/backends/metal/vio_metal_msl.h` und lässt sich ohne Mac
  gegen SPIRV-Cross prüfen.

#### Metal-3D-Pipeline (`src/backends/metal/vio_metal.m`)

- **Shader**: GLSL → SPIR-V (glslang) → MSL (SPIRV-Cross, `metal_gfx_spirv_to_msl`).
  glslangs `AUTO_MAP_BINDINGS` lässt alle Ressourcen bei `(set 0, binding 0)`, deshalb
  werden UBOs/SSBOs/Push-Constants/Sampler **umnummeriert** auf eindeutige Bindings
  `0..N-1` und per `spvc_compiler_msl_add_resource_binding_2` auf `[[buffer(k)]]` /
  `[[texture(k)]]` / `[[sampler(k)]]` gepinnt. Die Tabellen (`vio_metal_stage_res`)
  liest der Draw-Pfad zur Laufzeit. Vertex-Daten liegen fest auf Buffer-Index **30**
  (Mesh) und **29** (Instanz-mat4, Locations 3–6). `VIO_DUMP_MSL=1` dumpt das MSL.
- **PSO-Cache**: `MTLRenderPipelineState` ist monolithisch → pro `vio_pipeline` bis zu
  8 Varianten, Schlüssel = (Ziel-Farbformat, Mesh-Stride, Sample-Count). Swapchain BGRA8,
  HDR-RT RGBA16F, Depth-only-RT mit einer Fragment-Variante ohne Farb-Outputs
  (SPIRV-Cross `frag_output_mask = 0`) — `discard`/Alpha-Test in Shadow-Passes bleibt
  erhalten. Der 2D-Batch hat denselben Varianten-Cache (Format × Samples).
- **MSAA-RTs** (`'samples' => 2|4|8`, auf unterstützte Werte geclamped): 2DMultisample-
  Paar für Farbe+Depth, `StoreAndMultisampleResolve` in die Single-Sample-Textur, die
  Wrapper/2D-Registry/Readback sehen. Depth-only-RTs bleiben single-sampled (Depth wird
  gesampelt). `vio_create_render_target` reicht `samples` ebenfalls durch.
- **`vio_clear` ist eager** wie auf D3D11: im Frame wird der Pass mit Clear-Actions neu
  geöffnet (Swapchain oder gebundenes RT); vor `vio_begin` wird die Farbe gelatcht.
- **Texturen werden erst beim Draw gebunden** (gilt für alle Typed-Register-Backends:
  Metal, D3D11, D3D12): `vio_bind_texture`/`vio_bind_cubemap` merken sich pro GL-Unit
  nur das Objekt (`ctx->pending_tex_*`, pro Frame geleert); `vio_flush_pending_textures()`
  löst die Unit beim Draw über die Sampler-Map des *dann* gebundenen Shaders in den
  `[[texture(n)]]`- bzw. `t#`-Index auf. Damit sind — wie auf OpenGL — „bind vor
  `vio_set_uniform('u_tex', unit)`" und „bind unter anderer Pipeline" korrekt (Test 095). Ein GL-Unit darf dabei nur **einen** Sampler
  tragen; PHPolygon nutzt 0 Albedo, 1 SSAO, 2 SDF-AO, 3–5 Probe-3D, 6/8/9 CSM,
  7 Legacy-Shadow, 10 Environment-Cube.
- **Render-Target-Orientierung**: ein RT, das mit GL-UVs gesampelt wird, ist auf Metal
  V-gespiegelt (Zeile 0 = NDC-oben, wie D3D; Test 089). Engines flippen Clip-Y für
  Shadow-Map-Lookups/Cube-Face-Captures auf Metal genau wie auf D3D
  (PHPolygon `BackendConventions::flipRenderTargetClipY()`).
- Shadow-Map-Wrapper (`vio_render_target_texture` eines Depth-only-RT) sampeln mit
  Border = Opaque White + Compare-Sampler, wie der D3D11-Wrapper.
- **Swapchain-MSAA**: `vio_create(['samples' => 4])` rendert in ein 2DMultisample-Paar
  und resolvt ins Drawable bzw. die vsync-off-Offscreen-Textur (gleicher Mechanismus
  wie MSAA-RTs; Resize legt das Paar neu an).
- **`debug => true`**: setzt vor der Device-Erzeugung `METAL_DEVICE_WRAPPER_TYPE=1`
  (API-Validation-Layer, sofern noch nicht gesetzt) + `METAL_ERROR_MODE=3`, und jeder
  Command-Buffer bekommt `EncoderExecutionStatus` + Completion-Handler, der GPU-Faults
  nach stderr loggt (Metal-Thread → kein Zend-Aufruf dort).
- **Intel-Macs**: CPU-beschriebene Texturen sind `Shared` nur bei `hasUnifiedMemory`,
  sonst `Managed` (`metal_cpu_texture_storage()`); Buffers bleiben überall `Shared`.
- `vio_gpu_info()` fragt den Vtable-Slot `gpu_info` des Backends mit offenem Device (D3D11/D3D12: DXGI-Adapter,
  headless „Microsoft Basic Render Driver“ = WARP; Vulkan: Physical Device; OpenGL: `GL_RENDERER`) und liefert auf Metal `MTLDevice.name` + `recommendedMaxWorkingSetSize`
  als `vram_bytes` (Unified Memory hat kein dediziertes VRAM).
- `vio_recorder_capture` / `vio_stream_push` lesen den Frame über den gemeinsamen
  Helper `vio_capture_rgba()` in `php_vio.c` — damit funktioniert Recording/Streaming
  auf **allen** Backends (vorher nur OpenGL; D3D11/D3D12/Vulkan-Frames werden auf die
  Context-Größe zugeschnitten).
- **Kein Buffer-Renaming**: Uniform-Buffer halten nur einen CPU-Shadow; jeder Draw
  kopiert ihn (und Instanz-Matrizen) in eine 256-Byte-alignte Slice eines
  **Per-Frame-Rings** (3 Frames, wächst dynamisch, Fence = Command-Buffer des Frames) —
  das D3D12-Schema. `vio_bind_pipeline` staged dafür die Shader-Cbuffers
  (`vio_metal_set_shader_cbuffers`).
- **Readback mittendrin**: `vio_read_pixels` innerhalb eines Frames committet den
  Command-Buffer, wartet, liest das aktuelle Farbziel und öffnet einen neuen Encoder
  mit `Load` (D3D-Kontrakt). Depth wird deshalb immer mit `Store` verlassen.
- **Depth-Compare-Sampler**: `sampler2DShadow` (SPIR-V `depth=1`) bekommt lazily einen
  `MTLSamplerState` mit `compareFunction = LessEqual`.
- Winding CCW = Front (wie GL/D3D-`FrontCounterClockwise`), NDC-Z 0..1 (PHPolygon:
  `BackendConventions::depthZeroToOne()`), Render-Target-Origin oben links.
- **Stencil**: Swapchain- und jede RT-Tiefe (auch depth_only/Cube/Array, wie D3Ds D24S8) sind
  `Depth32Float_Stencil8` (Stencil-Plane im Pass, Clear auf 0); Sampling liest die Depth-Plane, der
  Depth-Readback blittet mit `MTLBlitOptionDepthFromDepthStencil`. Jede Pipeline hat einen Depth-State mit und einen ohne
  Stencil (für Ziele ohne Plane); PSO- und 2D-Varianten tragen das Stencil-Format.
- **Layered Targets**: `'layers' => N` = 2DArray für Farbe und Tiefe, depth_only-Cube = Depth32Float-Cube,
  Farb-Cubes haben eine Cube-Tiefe. `VIO_RT_ALL_LAYERS` setzt `renderTargetArrayLength`; `gl_Layer` /
  `gl_ViewportIndex` kommen aus der Vertex-Stage (`[[render_target_array_index]]` / `[[viewport_array_index]]`,
  PSOs setzen dafür `inputPrimitiveTopology`), `vio_viewports` → `setViewports:count:` + Scissor je Eintrag.
  Flags `LAYERED_RENDER`/`VERTEX_LAYER`/`MULTI_VIEWPORT` nur auf Mac2/Apple5.
- **Geometry-Stage (emuliert, `METAL-GEOMETRY-PLAN.md`)**: `vio_shader(['geometry' => …])` baut aus VS
  und GS je einen Compute-Kernel (`vio_metal_kernel.h`, reines C): SPIR-V-Umbau (GLCompute, Interface-
  Variablen `Private`, `EmitVertex`/`EndPrimitive` → Aufrufe von `vio_emit`/`vio_end_primitive`),
  SPIRV-Cross → GLSL mit von vio vergebenen Namen (`vio_i<loc>`, `vio_o<loc>`, `vio_gl_in`, …), die
  Puffer-Logik als GLSL-Text drumherum, glslang → MSL über die normale Umnummerierung. Je Draw:
  CPU-Primitiv-Assembly (Strips/Fans in GL-Reihenfolge, Adjacency-Listen), VS-Kernel (Thread je
  Stream-Vertex × Instanz, Attribute aus dem Mesh-Puffer wie der Vertex-Deskriptor, 3–6 aus dem
  Instanz-Puffer) → Records aus vec4-Slots (0 `gl_Position`, 1 Layer/Viewport/PointSize, dann die
  Outputs nach Location), GS-Kernel (Thread je Primitiv × Invocation × Instanz, fester Bereich von
  `max_vertices` Records, Strips als Indexliste, unbenutzte Indizes zeigen auf den geclippten Record 0
  – die Primitiv-Reihenfolge bleibt wie in GL), dann `drawIndexedPrimitives` über eine generierte
  Durchreich-Vertex-Funktion (liest den Record per `gl_VertexIndex`, schreibt `gl_Layer` /
  `gl_ViewportIndex` / `gl_PointSize`). Uniforms: VS-Default-Block an den VS-Kernel, GS-Block aus
  `bind_stage_constants`; die Plumbing-Bindings 20–25 findet der Draw über die Ressourcen-Tabelle
  (`mk_res_index`, `mk_user_cbuffer`). `VIO_DUMP_GS_GLSL=1` druckt die drei generierten GLSL-Quellen.
- **HDR10 / Frame-Latenz**: `hdr_output => 2` (bzw. `1` auf einem EDR-Display, `NSScreen.
  maximumPotentialExtendedDynamicRangeColorComponentValue > 1`) schaltet den `CAMetalLayer` auf
  `RGB10A2Unorm` im Farbraum `kCGColorSpaceITUR_2100_PQ`; das Swapchain-Format (`vio_mtl.swap_format`)
  bestimmt Offscreen-/MSAA-Texturen, 2D- und PSO-Varianten. Der 2D-Batch PQ-kodiert nur, was auf der
  Swapchain landet (RTs bleiben display-referred), der Readback expandiert 10 Bit wie D3D/Vulkan.
  `frame_latency => n` (1..3) begrenzt die Frames in Flight mit einer Dispatch-Semaphore, die der
  Command-Buffer jedes Frames beim Abschluss signalisiert; `vio_swapchain_info()` meldet beides.
- Tests: `tests/backends/089_metal_3d_pipeline.phpt` (Pixel-Kontrakt), `088` läuft jetzt auch auf Metal.

### Zend-Objekte (14 Klassen)

| Klasse | Header | Zweck |
|--------|--------|-------|
| VioContext | src/vio_context.h | GPU-Context, Window, Input-State, 2D-State, RT-Stack |
| VioMesh | src/vio_mesh.h | VAO/VBO/EBO bzw. backend_vb/ib für 3D-Geometrie |
| VioShader | src/vio_shader.h | Kompiliertes Shader-Programm + SPIR-V |
| VioPipeline | src/vio_pipeline.h | Render-Pipeline (Shader + State) |
| VioTexture | src/vio_texture.h | GPU-Textur (2D und 3D/Volume) |
| VioBuffer | src/vio_buffer.h | Uniform/Storage Buffer |
| VioRenderTarget | src/vio_render_target.h | Offscreen-Target (Color/HDR/Depth-only/MSAA) |
| VioCubemap | src/vio_cubemap.h | 6-Face-Cubemap |
| VioComputePipeline | src/vio_compute_pipeline.h | Compute-Shader + gebundene Storage-Buffer/Params |
| VioAccelerationStructure | src/vio_acceleration_structure.h | Ray Query: BLAS je Mesh + TLAS über die Instanzen (`vio_acceleration_structure`) |
| VioFont | src/vio_font.h | TTF-Font (stb_truetype Atlas, glyph-index-keyed mit HarfBuzz) |
| VioSound | src/vio_audio.h | Audio-Quelle (miniaudio) |
| VioRecorder | src/vio_recorder.h | Video-Encoder (FFmpeg) |
| VioStream | src/vio_stream.h | Network-Stream (FFmpeg RTMP/SRT) |

Alle folgen dem gleichen Muster: `zend_object std` als letztes Feld, `Z_VIO_*_P()` Macro, `from_obj()` inline-Helper.

### Verzeichnisstruktur

```
php_vio.c                   # Alle PHP-Funktionen (~9000 Zeilen, monolithisch)
php_vio.h                   # Module-Globals (default_backend, debug, vsync)
php_vio_arginfo.h           # Arginfo + Funktionstabelle (generiert aus vio.stub.php)
vio.stub.php                # PHP-Stubs für IDE-Support (143 Funktionen)
config.m4 / config.w32      # Autotools- bzw. Windows-Build-Konfiguration
configure.ac                # PHP-freier Autotools-Einstieg (CI-Permutationen)
CMakeLists.txt              # IDE-Support (CLion/PhpStorm), kein Release-Build
Makefile.fragments          # Extra-Regeln: VMA (C++17), Metal (ObjC+ARC)
Makefile.macos              # (untracked) SPIRV-Cross-from-source + Voll-Rebuild

include/
  vio_backend.h             # Backend-Vtable (~60 Funktionspointer, alle optional außer Lifecycle)
  vio_types.h               # Enums, Structs, Deskriptoren, VIO_FEATURE_*
  vio_constants.h           # Keyboard/Mouse/Gamepad Konstanten (GLFW-kompatibel)
  vio_plugin.h              # Plugin-System (Output/Input/Filter)

src/
  vio_context.c             # VioContext Objekt
  vio_backend_registry.c    # Backend-Registry + Auto-Auswahl
  vio_backend_null.c        # No-Op Backend für Tests
  vio_resource.c            # Resource-Lifecycle
  vio_window.c              # GLFW Fenster-Management (GL-Ladder 4.6→3.0, Fullscreen, Monitore)
  vio_input.c               # Input-State + Callbacks (Keys, Mouse, Touch, Chars/IME)
  vio_mesh.c / vio_shader.c / vio_pipeline.c / vio_texture.c / vio_buffer.c
  vio_render_target.c       # Render-Target-Objekt + Stack
  vio_cubemap.c             # Cubemap-Objekt
  vio_compute_pipeline.c    # Compute-Pipeline-Objekt
  vio_2d.c                  # 2D-Batch-Renderer (z-sortiert, dynamisch wachsend), dispatcht an
  vio_2d_d3d11.c / vio_2d_d3d12.c / vio_2d_vulkan.c   # ...die Backend-2D-Pfade
  vio_font.c                # Font-Atlas (stb_truetype, 4096x4096, Multi-Range Unicode)
  vio_text_shape.c          # Text-Shaping (HarfBuzz + SheenBidi) + Wrapping. Gated: HAVE_HARFBUZZ
  vio_shader_compiler.c     # GLSL→SPIR-V (glslang)
  vio_shader_reflect.c      # SPIR-V Reflection (SPIRV-Cross)
  vio_audio.c               # Audio-Engine (miniaudio, 3D-Positional)
  vio_recorder.c            # Video-Recording (FFmpeg H.264)
  vio_stream.c              # Network-Streaming (FFmpeg RTMP/SRT)
  vio_thermal.c             # Thermal-State (macOS/iOS ProcessInfo)
  vio_plugin_registry.c     # Plugin-Registry
  shaders/
    default_shaders.h       # Built-in 3D Shader
    shaders_2d.h            # Built-in 2D Shader

  backends/
    opengl/vio_opengl.c     # OpenGL 3.0–4.6 Core (GLAD), vollständigster Pfad
    opengl/vio_2d_opengl.c  # GL-2D-Batch
    vulkan/vio_vulkan.c     # Vulkan (VMA, Swapchain, Sync, Texturen, Buffer) — 2D + Compute
    vulkan/vio_vulkan_3d*.c # Vulkan-3D: Shader-Rundreise, Pipeline-Varianten, Frame-Ring, Draws (Block 10)
    vulkan/vio_vulkan_rt.c  # Vulkan-Render-Targets: MRT, MSAA-Resolve, Cube-Faces, Readback, Mid-Frame-Clear (Block 10b)
    vulkan/vio_vulkan_cube.c # Vulkan-Cubemaps und Mip-Ketten per Blit (Block 10b)
    vulkan/vio_vma_wrapper.cpp  # VMA C++17 Wrapper
    metal/vio_metal.m       # Metal (ObjC, CAMetalLayer) — 2D, 3D, RT, Compute, Tessellation, emulierte GS
    metal/vio_metal_msl.h   # SPIR-V → MSL (Umnummerierung, Tess-Stages, Isolines/point_mode-Capture), reines C
    metal/vio_metal_kernel.h  # VS/GS als Compute-Kernel (SPIR-V-Umbau → GLSL), reines C
    metal/vio_metal.c       # C-Shim, #include't vio_metal.m (Autotools kennt kein .m)
    d3d11/vio_d3d11.c       # Direct3D 11 (Windows), vollständig
    d3d12/vio_d3d12.c       # Direct3D 12 (Windows), vollständig, frame_count konfigurierbar
    vio_d3d_common.h        # geteilte D3D-Helfer (HLSL-Register-Mapping etc.)
    ios/vio_ios.m           # iOS/iPadOS Window+Input-Layer (UIKit), ersetzt GLFW bei --with-ios

vendor/
  glad/                     # OpenGL Loader
  stb/                      # stb_image, stb_truetype, stb_image_write, stb_rect_pack
  vma/                      # Vulkan Memory Allocator
  miniaudio/                # Audio-Engine
  sheenbidi/                # SheenBidi (Unicode BiDi, Apache-2.0), UNITY-Build
```

### Dependencies (Homebrew)

| Lib | Zweck | config.m4 Flag |
|-----|-------|----------------|
| GLFW 3.4 | Windowing, Input, Gamepad | --with-glfw |
| glslang | GLSL→SPIR-V Kompilierung | --with-glslang |
| SPIRV-Cross | Shader-Reflection + Transpilation | --with-spirv-cross |
| Vulkan Loader | Vulkan API | --with-vulkan |
| FFmpeg | Video-Recording + Streaming | --with-ffmpeg |
| Metal/QuartzCore | macOS GPU (Framework) | --with-metal |
| HarfBuzz | Text-Shaping (Arabisch, Thai, Ligaturen) | --with-harfbuzz |
| Direct3D 11 / 12 (Windows SDK) | Windows GPU | --with-d3d11 / --with-d3d12 (config.w32, default on) |
| UIKit/Metal (iOS SDK) | iOS-Fenster+Input | --with-ios (impliziert Metal, deaktiviert GLFW/Vulkan/FFmpeg) |

Vendored (kein Homebrew): GLAD, stb_image/truetype/write/rect_pack, VMA,
miniaudio, **SheenBidi** (BiDi, Apache-2.0, `vendor/sheenbidi/`, UNITY-Build via
`-DSB_CONFIG_UNITY`).

## PHP API (143 Funktionen)

Vollständige Signaturen in `vio.stub.php`. Die Beispiele hier zeigen die Gruppen.

### Context & Frame
```php
$ctx = vio_create("auto", ["width" => 800, "height" => 600, "headless" => true]);
vio_begin($ctx); vio_clear($ctx, 0.1, 0.1, 0.1); /* render... */ vio_end($ctx);
vio_poll_events($ctx);
vio_close($ctx); vio_destroy($ctx);
```

#### `vio_create()` Optionen

| Key | Default | Wirkung |
|-----|---------|---------|
| `width` / `height` | — | Fenster- bzw. Framebuffer-Größe |
| `title` | — | Fenstertitel |
| `vsync` | php.ini `vio.vsync` | `false` = kein Sync-Interval. Siehe „Kein Frame-Cap" unten. |
| `samples` | 0 | MSAA für den Swapchain-Framebuffer (OpenGL via GLFW-Hint, Metal via Resolve; D3D/Vulkan ignorieren es) |
| `debug` | 0 | Validation Layers / Debug Output (D3D Debug Layer, Vulkan Validation, Metal API-Validation + Command-Buffer-Fault-Log) |
| `headless` | 0 | Offscreen, kein sichtbares Fenster |
| `frame_count` | 2 (**nur D3D12**) | In-Flight-Frames, siehe unten |
| `msl_version` | 0 = Maximum (bzw. `VIO_METAL_MSL_VERSION`) | **nur Metal**: MSL-Stufe festnageln (`21` = MSL 2.1), siehe „Metal-Feature-Ladder" |

##### `frame_count` — Pipeline-Tiefe (D3D12)

Wie viele Frames die CPU der GPU vorauslaufen darf. `begin_frame()` wartet auf
`frames[frame_index].fence_value`, bevor es den Allocator dieses Slots resetten
darf — die Zahl **ist** die Pipeline-Tiefe. Bei 2 blockiert die CPU auf dem
Submit→Present→Fence-Roundtrip des übernächsten Frames; bei billiger Szene ist
diese Latenz, nicht die GPU-Arbeit, der Frame-Zeit-Boden.

```php
$ctx = vio_create('d3d12', ['frame_count' => 3, ...]);   // 1490 -> 2402 fps @1920x1080
```

**Nicht blind auf 3 stellen.** Der Wert teilt auch die Per-Frame-Slices von
SRV-, Constant- und Instance-Heap sowie den 2D-Vertex-Buffer: 2 → 3 verkleinert
jede Slice von ½ auf ⅓ ihres Heaps. Eine Szene, die schon nah am Heap-Limit
liegt, läuft dann über — still, und nur auf manchen Rechnern. Deshalb bleibt der
Default bei 2; opt-in erst, wenn der eigene Headroom geprüft ist.

Werte außerhalb `[2, 3]` werden geclamped (0/fehlend/negativ → Default). Andere
Backends ignorieren die Option. Abgedeckt von `tests/backends/071_d3d12_frame_count.phpt`.

##### Kein Frame-Cap bei `vsync: false`

vio ist mit `vsync: false` **nicht** auf die Bildwiederholrate gedeckelt — auf
144 Hz gemessen: 3000–4500 fps, windowed wie borderless, D3D11 wie D3D12. Eine
FLIP_DISCARD-Swapchain wird von DWM nicht gedrosselt; `Present(0,0)` blockiert
nicht, DWM verwirft nur Frames, die es nicht anzeigt.

`DXGI_ALLOW_TEARING` (seit v2.4.2 gesetzt) ändert daran **nichts** — es ist ein
Korrektheits-Flag: ohne es greift **VRR (G-Sync/FreeSync) gar nicht**, und der
Frame erreicht den Scanout erst am nächsten vblank. Wer künftig einen
vermeintlichen Refresh-Cap meldet: erst messen, nicht zum Tearing-Flag greifen.

### Input
```php
vio_key_pressed($ctx, VIO_KEY_W)        // bool; auch vio_key_just_pressed / vio_key_released
vio_mouse_position($ctx)                // [float, float]; vio_mouse_delta, vio_mouse_scroll
vio_mouse_button($ctx, VIO_MOUSE_LEFT)  // bool
vio_set_cursor_mode($ctx, VIO_CURSOR_DISABLED);
vio_on_key($ctx, function($key, $action, $mods) { ... });
vio_on_char($ctx, fn($cp) => ...); vio_chars_typed($ctx); vio_ime_backspaces($ctx);
vio_touch_count($ctx); vio_touch_get($ctx, 0); vio_touch_inject($ctx, $id, VIO_TOUCH_BEGAN, $x, $y);
vio_keyboard_show($ctx); vio_keyboard_hide($ctx);       // iOS-Softkeyboard, sonst no-op
vio_inject_key($ctx, VIO_KEY_W, VIO_PRESS, VIO_MOD_SHIFT);  // Tests/Bots: gleicher Pfad wie GLFW, feuert vio_on_key
vio_inject_mouse_move($ctx, $x, $y); vio_inject_mouse_button($ctx, VIO_MOUSE_LEFT, VIO_PRESS);  // roher Cursor-Raum (headless 1:1)
vio_inject_scroll($ctx, 0.0, 1.0); vio_inject_char($ctx, "Hallo");  // Text feuert vio_on_char; Enter/Backspace als Key
vio_virtual_gamepad_connect(0, "Bot Pad");                   // prozessglobal, überlagert physischen Joystick 0
vio_inject_gamepad_button(0, VIO_GAMEPAD_A, VIO_PRESS); vio_inject_gamepad_axis(0, VIO_GAMEPAD_AXIS_LEFT_X, 0.7);
vio_input_record_start($ctx); /* ... spielen ... */ $events = vio_input_record_stop($ctx);  // Array, JSON-fähig
vio_input_replay($ctx, $events);   // Tick N = N-ter vio_poll_events; OS-Input wird solange ignoriert
vio_input_replaying($ctx); vio_input_replay_stop($ctx);
vio_gamepads(); vio_gamepad_buttons($id); vio_gamepad_axes($id); vio_gamepad_triggers($id);
```

### Window / Display
```php
vio_window_size($ctx); vio_framebuffer_size($ctx); vio_content_scale($ctx); vio_pixel_ratio($ctx);
vio_set_window_size($ctx, 1280, 720); vio_set_title($ctx, "…"); vio_on_resize($ctx, fn($w, $h) => ...);
vio_set_fullscreen($ctx, $monitor = -1, $w = 0, $h = 0, $refresh = 0);
vio_set_borderless($ctx); vio_set_windowed($ctx); vio_toggle_fullscreen($ctx);
vio_get_auto_iconify($ctx);             // seit v2.7.4 false: Fullscreen minimiert nicht bei Fokusverlust
vio_monitors($ctx); vio_monitor_info($ctx); vio_video_modes($ctx, $monitor = -1);
vio_native_window_handle($ctx);         // NSWindow*/HWND als int — für Standalone-Renderer (php-metal-gpu)
vio_backend_name($ctx); vio_backends(); vio_backend_count();
vio_gpu_info(); vio_gl_info($ctx); vio_thermal_state(); vio_supports_feature($ctx, VIO_FEATURE_COMPUTE);
```

### 3D Rendering
```php
$mesh = vio_mesh($ctx, ["vertices" => [...], "layout" => [VIO_FLOAT3], "topology" => VIO_TRIANGLES]);
$shader = vio_shader($ctx, ["vertex" => $glsl_vs, "fragment" => $glsl_fs]);
$pipeline = vio_pipeline($ctx, ["shader" => $shader]);   // topology, cull, depth_test, depth_func,
                                                          // blend, depth_bias, hdr_output
vio_bind_pipeline($ctx, $pipeline);
vio_set_uniform($ctx, "u_model", $mat4);   // int|float|array; vio_set_uniforms($ctx, [...])
vio_viewport($ctx, 0, 0, $w, $h);
vio_draw($ctx, $mesh);
vio_draw_instanced($ctx, $mesh, $matrices /* array|packed string */, $count);
vio_submit_batch($ctx, $draws);            // mehrere Draws in einem Call
vio_draw_3d($ctx);                          // flush_draw_state nach dem 3D-Pass

// Render Targets (Objekt-API + Stack)
$rt = vio_render_target($ctx, ["width" => 512, "height" => 512, "hdr" => true, "depth_only" => false, "samples" => 4]);
vio_bind_render_target($ctx, $rt); /* ... */ vio_unbind_render_target($ctx);
vio_push_render_target($ctx, $rt); /* ... */ vio_pop_render_target($ctx);
$tex = vio_render_target_texture($rt);     // als Textur weiterverwenden
// Alternativ: vio_create_render_target($ctx, $w, $h, $opts) / vio_set_render_target($ctx, $rt|null) / vio_destroy_render_target($rt)

// Cubemaps
$cm = vio_cubemap($ctx, ["faces" => [$px, $nx, $py, $ny, $pz, $nz], "mipmaps" => true]);   // Pfade oder RGBA-Strings
vio_bind_cubemap($ctx, $cm, 0);

// Cubemap-Render-Target (Environment-Probe): 6 Faces rendern, Mips bauen, per textureLod sampeln
$env = vio_render_target($ctx, ["cube" => true, "size" => 256, "mipmaps" => true]);
for ($f = 0; $f < 6; $f++) { vio_bind_render_target($ctx, $env, $f); /* Sky-Pass */ }
vio_unbind_render_target($ctx);
vio_generate_mipmaps($ctx, $env);                 // auch für VioTexture / VioCubemap
$envCube = vio_render_target_cubemap($env);       // samplerCube, textureLod(dir, roughness * mipMax)

// Readback eines RTs (auch mid-frame): Farbe, HDR (auf 8 Bit), Depth-only (Grau-Rampe), Cube-Face
$rgba = vio_read_render_target($rt);  $face = vio_read_render_target($env, 3);

// Multiple Render Targets (G-Buffer): bis 4 Farb-Attachments, Fragment layout(location = i) out
$gb = vio_render_target($ctx, ["width" => $w, "height" => $h,
        "attachments" => [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
$gbPipe = vio_pipeline($ctx, ["shader" => $s, "attachments" => [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]); // Formate nur für D3D12-PSO nötig
vio_bind_render_target($ctx, $gb); /* ... */ vio_unbind_render_target($ctx);
$normals = vio_render_target_texture($gb, 1);          // Attachment-Index
$rg      = vio_read_render_target($gb, -1, 2);          // (rt, face, attachment) → immer RGBA8
// Formate: VIO_FORMAT_RGBA8, RGBA16F, RGBA32F, R11G11B10F, RG16F, R16F, R32F, R8

// Pipeline-State
vio_pipeline($ctx, ["shader" => $s, "depth_test" => true, "depth_write" => false,   // Sky / Transparenz
                    "blend" => VIO_BLEND_PREMULTIPLIED, "color_mask" => VIO_COLOR_RGB]);
// blend: NONE, ALPHA, ADDITIVE, PREMULTIPLIED, MULTIPLY, SCREEN, MIN, MAX; color_mask: VIO_COLOR_R|G|B|A

// Geometry- / Tessellation-Stages (Gate: VIO_FEATURE_GEOMETRY / VIO_FEATURE_TESSELLATION)
$gsShader = vio_shader($ctx, ["vertex" => $vs, "geometry" => $gs, "fragment" => $fs]);   // z.B. Punkt → Billboard-Quad
$gsPipe   = vio_pipeline($ctx, ["shader" => $gsShader, "topology" => VIO_POINTS]);
$tessShader = vio_shader($ctx, ["vertex" => $vs, "tess_control" => $tcs, "tess_eval" => $tes, "fragment" => $fs]);
$tessPipe   = vio_pipeline($ctx, ["shader" => $tessShader, "patch_vertices" => 4]);        // zeichnet immer VIO_PATCHES
vio_set_uniform($ctx, "u_level", 16.0);     // Uniforms der Extra-Stages wie gewohnt

// Layered Rendering + mehrere Viewports (Single-Pass-Cube / CSM, GEOMETRY-STAGES-PLAN Phase 1)
$shadowCube = vio_render_target($ctx, ["cube" => true, "size" => 512, "depth_only" => true]);
$cascades   = vio_render_target($ctx, ["width" => 2048, "height" => 2048, "layers" => 4, "depth_only" => true]);
vio_bind_render_target($ctx, $shadowCube, VIO_RT_ALL_LAYERS);   // GS / VS schreibt gl_Layer
vio_viewports($ctx, [[0, 0, 1024, 1024], [1024, 0, 1024, 1024]]);  // gl_ViewportIndex wählt
$depthArray = vio_render_target_texture($cascades);            // sampler2DArray
```

### Compute & Storage Buffers
```php
$cp  = vio_compute_pipeline($ctx, ["source" => $glsl_compute]);           // GLSL #version 450
$in  = vio_storage_buffer($ctx, ["size" => $n * 4, "data" => $packed, "stride" => 4]);
$out = vio_storage_buffer($ctx, ["size" => $n * 4]);
vio_compute_bind_buffer($ctx, $cp, $in,  0, VIO_COMPUTE_READ);
vio_compute_bind_buffer($ctx, $cp, $out, 1, VIO_COMPUTE_WRITE);
vio_compute_set_uniforms($ctx, $cp, pack("i4f4", ...));                     // Params-UBO (binding 2)
vio_compute_dispatch($ctx, $cp, $gx, $gy, $gz);                             // synchron
$bytes = vio_storage_buffer_read($ctx, $out);                               // GPU→CPU

// Storage-Images (image2D / image3D): Kernel schreibt in eine Textur, Render-Pass sampelt sie
$img = vio_texture($ctx, ["width" => $w, "height" => $h, "storage" => true]);  // 'data' optional (nullinitialisiert)
vio_compute_bind_image($ctx, $cp, $img, 0, VIO_COMPUTE_WRITE);   // layout(binding = 0, rgba8) uniform image2D
vio_compute_dispatch($ctx, $cp, ceil($w / 8), ceil($h / 8), 1);   // local_size kommt aus der Reflection (2D/3D-Kernel)
vio_bind_texture($ctx, $img, 0);                                  // danach normal sampeln

// Async im Frame: Dispatch landet im Frame-Command-Stream, spätere Draws desselben Frames sehen ihn
vio_begin($ctx);
vio_compute_dispatch($ctx, $cp, $gx, $gy, 1, ['async' => true]);  // kein CPU-Stall
vio_draw($ctx, $mesh);                                            // sampelt $img / liest den Buffer
vio_end($ctx);
vio_compute_wait($ctx);                                           // explizit fencen — vio_storage_buffer_read() tut es implizit

// "Path B": Vertex-Stage liest den Storage Buffer direkt (kein Readback), v2.8.0
if (vio_supports_feature($ctx, VIO_FEATURE_VERTEX_STORAGE)) {
    vio_bind_pipeline($ctx, $pipeline);
    vio_bind_storage_buffer($ctx, $matrices, 3, VIO_COMPUTE_READ);          // gl_InstanceIndex-indiziert
    vio_draw_instanced_from_buffer($ctx, $mesh, $count);
}
```
Backend-Mapping der GLSL-Bindings: OpenGL SSBO-Slots, Vulkan Descriptor-Sets, D3D
`t#/u#/b#`-Register aus der Reflection, Metal `[[buffer(N)]]` via expliziten
SPIRV-Cross-MSL-Bindings (binding N == buffer N). Gates: `VIO_FEATURE_COMPUTE`,
`VIO_FEATURE_VERTEX_STORAGE` — ohne sie fällt PHPolygon still auf CPU/Readback zurück.

### 2D Rendering
```php
vio_rect($ctx, 10, 10, 100, 50, ["color" => 0xFF0000FF]);
vio_circle($ctx, 200, 200, 30, ["color" => 0x00FF00FF, "outline" => true]);
vio_line($ctx, 0, 0, 100, 100, ["color" => 0xFFFFFFFF]);
vio_sprite($ctx, $texture, ["x" => 50, "y" => 50, "scale_x" => 2.0]);
vio_rounded_rect($ctx, 10, 10, 100, 50, 8.0, ["color" => 0xFF336699]);
$font = vio_font($ctx, "/path/to/font.ttf", 24.0);
vio_text($ctx, $font, "Hello", 10, 10, ["color" => 0xFFFFFFFF, "max_width" => 200]);
vio_text_measure($font, "Hello", $opts);   // ['width','height','lines']
vio_font_has_glyph($font, 0x0416);         // Fallback-Font-Auswahl
vio_push_transform($ctx, $a, $b, $c, $d, $e, $f); /* ... */ vio_pop_transform($ctx);
vio_push_scissor($ctx, $x, $y, $w, $h);           /* ... */ vio_pop_scissor($ctx);
vio_draw_2d($ctx);  // Flush
```

### Textures & Buffers
```php
$tex = vio_texture($ctx, ["file" => "image.png"]);
$tex = vio_texture($ctx, ["data" => $rgba, "width" => 64, "height" => 64]);
$vol = vio_texture_3d($ctx, ["data" => $rgba, "width" => 32, "height" => 32, "depth" => 32]); // sampler3D (SDF-Volumen)
vio_bind_texture($ctx, $tex, 0); vio_texture_size($tex);
vio_texture_update($ctx, $tex, $rgbaRegion, $x, $y, $w, $h);   // Sub-Region-Upload (Streaming/Video), ohne Region = ganze Textur

$buf = vio_uniform_buffer($ctx, ["size" => 64, "binding" => 0]);
vio_update_buffer($buf, pack("f4", 1.0, 0.0, 0.0, 1.0));
```

### Audio
```php
$sound = vio_audio_load("music.mp3");
vio_audio_play($sound, ["volume" => 0.8, "loop" => true]);
vio_audio_pause($sound); vio_audio_resume($sound); vio_audio_stop($sound);
vio_audio_volume($sound, 0.5); vio_audio_playing($sound);
vio_audio_position($sound, $x, $y, $z); vio_audio_listener($x, $y, $z, $fx, $fy, $fz);  // 3D-Audio
```

### Video Recording & Streaming
```php
$rec = vio_recorder($ctx, ["path" => "out.mp4", "fps" => 30]);
vio_recorder_capture($rec, $ctx);  // pro Frame
vio_recorder_stop($rec);

$stream = vio_stream($ctx, ["url" => "rtmp://server/live", "fps" => 30]);
vio_stream_push($stream, $ctx);
vio_stream_stop($stream);
```

### Headless / VRT

`vio_read_pixels()` ist auf **allen** Backends implementiert (Metal headless seit
v2.6.0 via Blit in Shared-Buffer, 1:1 ohne Retina-Faktor seit v2.7.2/2.7.3; Vulkan
via re-acquired Swapchain-Image mit TRANSFER_SRC).

> **D3D11-Readback ist seit v2.4.3 on-demand.** `end_frame()` spiegelt den
> Backbuffer weiterhin jedes Frame (FLIP_DISCARD verwirft ihn beim Present, und
> D3D11 kann — anders als D3D12 — den zuletzt präsentierten Buffer nicht mehr
> adressieren), aber **GPU-lokal** (VRAM→VRAM). Die PCIe-Kopie in CPU-lesbaren
> Speicher passiert erst in `vio_read_pixels()`.
>
> Vorher lief sie in *jedem* Frame — auch wenn nie jemand Pixel las. Auf einem
> 3840×1080-Backbuffer waren das 16,6 MB/Frame über PCIe; echte Spielpanels
> wurden dadurch um 23–57 % langsamer. Die Semantik ist unverändert:
> `vio_read_pixels()` liefert weiterhin den Pre-Present-Frame. Aufrufer müssen
> nichts umstellen.

```php
$ctx = vio_create("auto", ["width" => 64, "height" => 64, "headless" => true]);
$pixels = vio_read_pixels($ctx);           // RGBA string
vio_save_screenshot($ctx, "shot.png");
$diff = vio_compare_images("ref.png", "cur.png", ["threshold" => 0.01]);
// $diff = ["passed" => bool, "diff_ratio" => float, "diff_pixels" => int, "diff_data" => string, ...]
vio_save_diff_image($diff, "diff.png");
```

### Shader Reflection
```php
$info = vio_shader_reflect($shader);
// $info["vertex"]["inputs"]  → [{name, location, format}, ...]
// $info["vertex"]["ubos"]    → [{name, set, binding, size}, ...]
// $info["fragment"]["textures"] → [{name, set, binding}, ...]
```

### Plugins
```php
vio_plugins();               // string[] — registrierte Plugin-Namen
vio_plugin_info("name");     // array|false — Details
// Konstanten: VIO_PLUGIN_TYPE_OUTPUT (1), INPUT (2), FILTER (4)
```

### Async Loading
```php
$h = vio_texture_load_async("large.png");
// ... andere Arbeit ...
$result = vio_texture_load_poll($h);  // null=laden, false=fehler, array=fertig
// $result = ["width" => int, "height" => int, "data" => string]

$fh = vio_font_load_async($ctx, "font.ttf", 24.0);
$font = vio_font_load_poll($fh);      // null=laden, false=fehler, VioFont=fertig
```

## Unicode Font Support

The font system uses stbtt_PackFontRanges with 9 Unicode blocks:
- Latin (U+0020-U+00FF), Latin Extended (U+0100-U+01FF)
- Greek (U+0370-U+03FF), Cyrillic (U+0400-U+04FF)
- Vietnamese (U+1E00-U+1EFF)
- CJK Symbols + Hiragana/Katakana (U+3000-U+30FF)
- CJK Unified Ideographs (U+4E00-U+9FFF)
- Hangul Syllables (U+AC00-U+D7A3)
- Fullwidth Forms (U+FF00-U+FFEF)

Atlas size is 4096x4096. Glyphs are stored in a PHP HashTable (codepoint -> packedchar) for O(1) lookup. Fonts that don't contain glyphs for a range skip them automatically (no atlas space wasted). Space characters (zero visual size but non-zero xadvance) are correctly preserved.

## Text Shaping (HarfBuzz + SheenBidi)

When built `--with-harfbuzz` (constant `VIO_HAS_SHAPING == 1`), **all** text goes
through a full shaping pipeline instead of the legacy codepoint-per-glyph path —
this is what makes Arabic (RTL + joining), Thai (clustering), and ligatures
render correctly. Implementation: `src/vio_text_shape.c`.

- **Atlas**: switches from codepoint-keyed to **glyph-index-keyed**. Every glyph
  the font has (0..numGlyphs) is packed once at `vio_font()` creation via
  `stb_rect_pack` + `stbtt_MakeGlyphBitmap`, then uploaded once — so the GPU
  atlas handle is **stable for the font's life** (no runtime re-upload, no
  destroy race on deferred backends). Glyph-index keying is required because
  HarfBuzz emits glyphs (ligatures, positional forms) that no codepoint reaches.
- **Pipeline** per string: SheenBidi resolves BiDi levels and returns runs in
  *visual* order (`SBLine`) → each run is shaped by HarfBuzz with the
  bidi-resolved direction (script/language guessed from content) → shaped glyphs
  are emitted as `VIO_2D_TEXT` quads. Baseline convention matches the legacy path
  (`y` is the baseline), so Latin stays pixel-stable.
- Without HarfBuzz the whole subsystem compiles to nothing and text uses the
  legacy path unchanged — `VIO_HAS_SHAPING == 0`.
- **Line wrapping**: `vio_text` honors `'\n'` (hard break) always, and
  `['max_width' => px]` enables greedy word wrap; `'line_height' => px` overrides
  the natural leading. `vio_text_measure($font, $text, $opts)` takes the same
  options and returns `['width','height','lines']` (widest line / total height /
  line count). Break opportunities use a compact **UAX #14-lite** classifier
  (`lb_class`/`lb_break_between` in vio_text_shape.c): whitespace for Latin,
  between ideographs for **CJK** (with basic kinsoku — no break after opening /
  before closing punctuation), and at **Thai** cluster boundaries (consonant/
  leading-vowel starts a cluster; combining vowels/tones stay attached). Thai is
  dictionary-free, so it breaks at clusters, not true word boundaries; a segment
  wider than `max_width` still overflows (no mid-cluster/mid-word split).
  Vertical text is out of scope.

## PIE Installation

Release zips contain `vio.so` (Linux/macOS) or `php_vio.dll` (Windows) as the filename inside the archive, matching what PIE expects.

```bash
pie install phpolygon/php-vio
```

## OpenGL-Feature-Ladder

Welche `VIO_FEATURE_*`-Flags sicher `1` zurückgeben, hängt von der Core-Version
des erhaltenen GL-Kontexts ab (Fenster verhandelt `4.6 → 3.3`-Ladder). Die
Backend-Caps in `vio_gl.caps` werden einmal in `vio_opengl_setup_context()`
befüllt — entweder weil die Core-Version das Feature deckt oder weil die
ARB/KHR-Extension exportiert ist. `vio_gl_info($ctx)` legt die finale
Auswertung pro Lauf offen.

| Feature                       | Core ab  | Extension-Fallback                | Floor 3.3 |
|-------------------------------|----------|------------------------------------|-----------|
| `VIO_FEATURE_COMPUTE`         | 4.3      | `GL_ARB_compute_shader`            | 0         |
| `VIO_FEATURE_TESSELLATION`    | 4.0      | `GL_ARB_tessellation_shader`       | 0         |
| `VIO_FEATURE_GEOMETRY`        | 3.2      | —                                   | **1**     |
| `VIO_FEATURE_SEPARATE_SHADERS`| 4.1      | `GL_ARB_separate_shader_objects`   | 0         |
| `VIO_FEATURE_DEBUG_OUTPUT`    | 4.3      | `GL_KHR_debug`                     | 0         |
| `VIO_FEATURE_DSA`             | 4.5      | `GL_ARB_direct_state_access`       | 0         |
| `VIO_FEATURE_BUFFER_STORAGE`  | 4.4      | `GL_ARB_buffer_storage`            | 0         |
| `VIO_FEATURE_TEXTURE_STORAGE` | 4.2      | `GL_ARB_texture_storage`           | 0         |
| `VIO_FEATURE_TEXTURE_SWIZZLE` | 3.3      | `GL_ARB_texture_swizzle`           | **1**     |
| `VIO_FEATURE_3D_PIPELINE`     | 3.3      | —                                   | **1**     |
| `VIO_FEATURE_READ_PIXELS`     | 3.0      | —                                   | **1**     |
| `VIO_FEATURE_INSTANCED_DRAW`  | 3.1      | —                                   | **1**     |
| `VIO_FEATURE_RENDER_TARGET`   | 3.0      | —                                   | **1**     |
| `VIO_FEATURE_RENDER_TARGET_HDR` | 3.0    | `GL_ARB_texture_float`             | **1**     |
| `VIO_FEATURE_RENDER_TARGET_DEPTH` | 3.0  | —                                   | **1**     |
| `VIO_FEATURE_RENDER_TARGET_MSAA` | 3.0   | —                                   | **1**     |
| `VIO_FEATURE_CUBEMAP`         | 3.0      | —                                   | **1**     |
| `VIO_FEATURE_DEPTH_BIAS`      | 3.0      | —                                   | **1**     |
| `VIO_FEATURE_SCISSOR`         | 3.0      | —                                   | **1**     |
| `VIO_FEATURE_NATIVE_2D_BATCH` | —        | —                                   | **1** (immer, vio_2d_opengl.c) |
| `VIO_FEATURE_RAYTRACING`      | —        | nur via NV/EXT-Vendor-Ext           | 0         |
| `VIO_FEATURE_MULTIVIEW`       | —        | `GL_OVR_multiview` (nicht gewired)  | 0         |

**macOS-Hinweis:** Apple's Legacy-GL liefert maximal 4.1 Core. Compute /
Tessellation / DSA / Buffer-Storage / Texture-Storage > 4.2 sind dort
nie verfügbar; `gl_has_ext()` greift, falls Apple jemals den ARB-Pfad
nachgeliefert hat (aktuell nicht).

## Metal-Feature-Ladder

Gegenstück zur OpenGL-Leiter: `vio_metal_setup_context_native` probiert die Metal-Shading-Language-
Stufen **4.1 → 4.0 → 3.2 → 3.1 → 3.0 → 2.4 → 2.3 → 2.2 → 2.1 → 2.0** (ein Probe-Kernel je Stufe,
`MTLLanguageVersion` numerisch `(major << 16) | minor`, damit ältere SDKs bauen; das Maximum wird je
Prozess gecacht) und nimmt die erste, die das OS kompiliert. `vio_create(['msl_version' => 21])` bzw.
`VIO_METAL_MSL_VERSION=21` nageln eine niedrigere Stufe fest (nächste Stufe darunter, Boden 2.0) —
so läuft die ganze Suite auf einem neuen Mac auch gegen alte Stufen. **Alle** Shader des Kontexts
nutzen die Stufe: SPIRV-Cross-MSL der Grafik-Stages, Kernel der GS-Emulation, Tessellation (mindestens
2.1), Compute (vorher SPIRV-Cross-Default 1.2) und die handgeschriebenen 2D-Shader
(`metal_compile_options()`, `metal_msl_apply_target()`); auf iOS mit `SPVC_MSL_PLATFORM_IOS`.

`vio_mtl.caps` (`metal_detect_caps`) = Device-Unterstützung **und** Mindest-MSL. `vio_backend_info($ctx)`
(Vtable-Slot `describe`) legt Stufe, Maximum, Familien (`apple1`…`apple11`, `mac1/2`, `common1–3`,
`metal3/4`) und Caps offen:

| Cap | Mindest-MSL | Hardware / Abfrage | Nutzt heute |
|---|---|---|---|
| `tessellation` | 2.1 | jede (Kernel + `drawPatches`) | `VIO_FEATURE_TESSELLATION` |
| `layered_vertex` | — | Mac2 / Apple5 | `LAYERED_RENDER`, `VERTEX_LAYER`, `MULTI_VIEWPORT` |
| `quad_group` | 2.1 | Mac2 / Apple4 | `VIO_FEATURE_SUBGROUP_QUAD` |
| `simd_group` | 2.2 (`threads_per_simdgroup` im Fragment-Shader) | Mac2 / Apple7 | `VIO_FEATURE_SUBGROUP` |
| `barycentrics` | 2.2 | `supportsShaderBarycentricCoordinates` | `VIO_FEATURE_BARYCENTRICS` |
| `vertex_amplification` | 2.2 | `supportsVertexAmplificationCount:2` | — |
| `argument_buffers_tier2` | — | `argumentBuffersSupport` | — |
| `raytracing` / `function_pointers` | 2.3 | `supportsRaytracing` / `supportsFunctionPointers` | `VIO_FEATURE_RAY_QUERY` (mit `raytracing_from_render`) |
| `raytracing_from_render` | 2.4 | `supportsRaytracingFromRender` | `VIO_FEATURE_RAY_QUERY` |
| `mesh_shaders` | 3.0 | Metal3 + (Apple7 / Mac2) | — |
| `atomic64` | 3.1 | Apple9 | — (`VIO_FEATURE_ATOMIC64` bleibt 0: nur min/max, SPIRV-Cross blockt) |
| `tensors` | 4.0 | Metal4 | — |
| `bindless` | 3.0 | Metal3 + `argument_buffers_tier2` | `VIO_FEATURE_BINDLESS` |
| `rasterization_rate_map`, `bc_texture_compression`, `unified_memory` | — | Device-Abfragen | BC: `TEXTURE_COMPRESSION_BC` |

Auf dem M5 (macOS 27, Metal 4) ist das Maximum 4.1, alle Caps 1. Die lokale Suite ist auf 4.1, 3.0, 2.1
und 2.0 für Metal grün (Tests 150/151 fahren jede Stufe). Neue Metal-Features hängen ihr Flag an eine Cap,
nicht an `@available` im Feature-Code.

## Konventionen

- **Sprache**: Code und Kommentare auf Englisch. Kommunikation auf Deutsch.
- **Funktionsnamen**: `vio_` Prefix für alle PHP-Funktionen.
- **Konstanten**: `VIO_` Prefix, SCREAMING_CASE.
- **Zend-Objekte**: `vio_*_object` Struct, `Z_VIO_*_P()` Accessor-Macro.
- **Bedingte Kompilierung**: `#ifdef HAVE_GLFW`, `HAVE_VULKAN`, `HAVE_METAL`, `HAVE_D3D11`, `HAVE_D3D12`, `HAVE_IOS`, `HAVE_FFMPEG`, `HAVE_GLSLANG`, `HAVE_SPIRV_CROSS`, `HAVE_HARFBUZZ`.
- **Tests**: PHPT-Format, `tests/<thema>/NNN_name.phpt` (Nummern fortlaufend über alle Ordner, nächste freie: 160 (109 Geometry-Stage, 110 Tessellation, 111 Bind-Tabelle, 112 Blend je Attachment, 113 Stencil, 114 uint16-Indices, 115 GPU-Zeit, 116 Shader-Cache, 117 Frame-Latenz, 118 HDR10, 119 Shader Model 6, 120 Indirect Draw, 121 Texture-Arrays/BC/KTX2, 122 Variable Rate Shading, 123 Vulkan-3D-Konventionen, 124 MRT-Formate + Textur-Mips, 125 Compute-Buffer: beschreibbare data-Buffer, Slot-Rebind, Update mit Offset, 126 Async-Compute: Params je Dispatch, 127 Text-Bitmap über VioFontFace, 128 vio_submit_batch-Parität, 129 Fenstergröße-Round-Trip, 130 gepackte Uniforms, 131 Input-Injection über den OS-Eventpfad, 132 virtuelle Gamepads, 133 Input-Record/Replay, 134 Replay verwirft OS-Input, 135 GS/Tess auf allen Draw-Pfaden + Cache, 136 Layered Render-Targets, 137 Layered Rendering, 138 mehrere Viewports, 139 GS-Instancing + Adjacency, 140 HLSL-Stage-Override, 141 Vergleichs-Sampler, 142 RT-Rebind behält Inhalt, 143 GS mit `gl_in`/`gl_InvocationID`, 144 Tessellations-Konventionen, 145 Mipmaps im Frame, 146 Uniform-Array-Elemente, 147 Stencil in Layered/depth_only-RTs, 148 point_mode + Fractional-Isolines, 149 Subgroup-Operationen, 150 Metal-Versionsleiter, 151 Rendering je MSL-Stufe, 152 Quad-Operationen, 153 Barycentrics, 154 64-Bit-Atomics, 155 Float16, 156 Draw-Parameter, 157 Compute-Derivate, 158 Multiview, 159 Shading-Rate pro Primitiv)), headless OpenGL für GPU-Tests (`../skipif_gl.inc`), Backend-spezifische Tests skippen sauber wenn das Backend fehlt.
- **Audit-Gate**: `tests/core/070_audit_gate_no_gl_outside_backend.phpt` — kein `glXxx()`/`GL_*` außerhalb `src/backends/opengl/`.
- **Metal-Objekte in C-Structs**: als `CFBridgingRetain`'d `void *` halten, in den destroy-Hooks `CFRelease`n (ARC trackt keine Refs in C-Structs).
- **Commits**: Conventional Commits (`feat(scope):`, `fix(scope):`, …) — semantic-release leitet daraus Version + CHANGELOG ab.
- **2D Farben**: ARGB als uint32 (0xAARRGGBB), z.B. `0xFF0000FF` = rot, alpha=FF.

## Pläne & Roadmap

Größere Umbauten werden vor der Umsetzung als `*-PLAN.md` im Wurzelverzeichnis
festgehalten (deutsch, phasiert, mit Audit-Gate-/Test-Kontrakt). Bestehende:

- `OPENGL-REFACTOR-PLAN.md` — ✅ implementiert. OpenGL als echtes Backend hinter
  der Vtable; erzwungen durch `tests/core/070_audit_gate_no_gl_outside_backend.phpt`
  (kein `glXxx()`/`GL_*` außerhalb `src/backends/opengl/`).
- **`D3D-VULKAN-GAP-PLAN.md` — ✅ Phasen 0–4 umgesetzt (2026-09-09), Phase 5 offen.**
  Ehrliche Feature-Flags, D3D12-Sampler-Heap, D3D11/D3D12-Render-Targets im Backend
  (Cube-RT, Mipmaps, Readback, MSAA auf D3D11 + GL), Anisotropie, Vulkan-Present-Mode,
  D3D12-Upload-Queue; Audit-Gate `099`. Phase 5 listet, was D3D/Vulkan nativ können und
  noch fehlt (Vulkan-3D-Entscheidung, D3D12-RT-MSAA + Stencil als PSO-State-PR, uint16,
  Timestamps, Indirect, Pipeline-Cache, DXC/SM6, HDR-Swapchain, VRS/Multiview).
- **`D3D12-MIPGEN-STALL-PLAN.md` — ✅ umgesetzt (Befund aus Code Rescue, 2026-10-03).**
  `vio_generate_mipmaps` im Frame zeichnet auf D3D12 in die offene Frame-Liste auf (Deskriptor-Ring
  `mipgen_heap`, danach `d3d12_restore_graphics_state_after_compute`), statt zweimal die GPU zu leeren:
  Repro 10,8 → 0,09 ms. Nebenbefunde: `GEOMETRY` auf D3D = 0 kam vom Vulkan SDK 1.3.296 der Windows-CI
  (jetzt 1.4.341), `name[i]` für Matrix-/Vektor-Arrays löst die Array-Schrittweite auf (Tests 145, 146).
- **`SHADER-FEATURES-PLAN.md` — 📋 geplant (2026-10-06).** Shader-Model-6.0–6.9-Features und ihre
  Metal-/Vulkan-/GL-Gegenstücke als portable `VIO_FEATURE_*`: Phase 0 Infrastruktur (Metal-Leiter ✅,
  SM6-Branch, `describe` überall, SM-Pinning, Agility SDK), 1 Intrinsics (Subgroups, Quad, Barycentrics,
  Float16, Base Vertex, Atomic64, Compute-Derivate), 2 Multiview, 3 VRS Tier 2, 4 Bindless, 5 Mesh-Shader,
  6 Raytracing (Ray Query → Pipeline → SER/OMM), 7 Sampler Feedback, 8 Neural Shading, 9 Work Graphs.
- **Shader Model 6 (D3D12)**: das Profil ist das höchste 6.x, das das Device meldet
  (`CheckFeatureSupport(SHADER_MODEL)` von 6.9 abwärts) und DXC + dxil.dll noch kompilieren und
  signieren (`vio_dxc_highest_minor`, Probe-Compute-Shader). `d3d12_hlsl_target()` gibt SPIRV-Cross
  dasselbe Ziel (ab 60 Wave-Intrinsics, ab 62 templated `Load/Store<T>`), `d3d12_stage_supported`
  probt GS/HS/DS mit dem Compiler, der später kompiliert (DXC unter SM 6), Cache je Ziel. GLSL mit
  `GL_KHR_shader_subgroup` kompiliert glslang gegen Vulkan 1.1 / SPIR-V 1.3, alles andere bleibt 1.0.
- `TEXT-SHAPING-PLAN.md` — HarfBuzz + SheenBidi (siehe „Text Shaping" oben).
- `VULKAN-2D-PLAN.md`, `v2-architecture.md`, `IMPLEMENTATION_PLAN.md` — Kontext.
- **`METAL-GEOMETRY-PLAN.md` — ✅ umgesetzt (2026-10-05).** Geometry-Stage auf Metal per Compute-
  Emulation (VS- und GS-Kernel aus SPIR-V-Umbau → SPIRV-Cross-GLSL → glslang, `vio_metal_kernel.h`,
  Durchreich-Vertex-Funktion); Tests 109/135/137–139/143 laufen auf Metal. Upstream-Alternativen
  (SPIRV-Cross #2654 / #2200, Mesh-Stage-basiert, Drafts mit Konflikten) sind nicht absehbar mergebar.
- **`METALGPU-REPLACEMENT-PLAN.md` — 🚧 Phasen 1–3 umgesetzt.** php-metal-gpu (`ext-metal`) und
  PHPolygons Standalone-`MetalRenderer3D` durch vio-Metal ersetzen. Phase 1 erweitert die
  vio-API auf allen Backends (Cubemap-Render-Target + `vio_generate_mipmaps`, `depth_write`/
  `color_mask`/Blend-Modi, `vio_read_render_target`, konsistente Headless-Größen,
  Pipeline-Destruktor); Phase 2 portiert die GPU-Environment-Cubemap nach `VioRenderer3D`;
  Phase 3 entfernt ext-metal aus PHPolygon; Phase 4 archiviert das Repo.
- **`API-ROADMAP.md` — 🚧 R1 MRT, R2 Storage-Images, R7 Dispatch-Geometrie + Async umgesetzt**
  (2.10-Paket, Tests 096–098); offen: R3 Stencil, R4 uint16-Indices/Texture-Arrays/
  Kompression, R5 GPU-Timestamps, R6 Indirect Draw, R8 Pipeline-Cache; gated:
  Mesh-Shader/Ray-Tracing/Upscaling. Immer für Metal+GL+D3D11+D3D12 gleichzeitig.
- Metal-3D-Pipeline: ✅ implementiert, Feature-Parität mit D3D11/D3D12 (siehe
  „Metal-3D-Pipeline" oben). PHPolygons Standalone-`MetalRenderer3D` (ext-metal /
  php-metal-gpu) ist damit auf macOS nicht mehr nötig. Offen: Vulkan-3D, Vulkan-Cubemap,
  Vulkan-HDR/Depth/MSAA-RT; echtes MSAA für OpenGL/D3D (melden 1, tun nichts).

### Verifikation / CI

- `.github/workflows/build.yml` — Tri-Platform Build+Test+Package (Linux
  x86_64/arm64, macOS x86_64/arm64, Windows x64; PHP 8.5). Linux/macOS fahren die volle
  Test-Suite; Linux headless via **Xvfb + Mesa-Software-GL** (`LIBGL_ALWAYS_SOFTWARE=1`);
  Windows testet die D3D-Backends auf **WARP**.
- `.github/workflows/release.yml` — semantic-release auf `main`: ruft build.yml als
  Gate, taggt, schreibt CHANGELOG.md, triggert den Release-Build der Binaries.
- `configure.ac` erlaubt einen PHP-freien Autotools-Durchlauf, um die „kompiliert ohne
  jede Dependency"-Zusage von `config.m4` zu prüfen.

## Herd-Integration

Laravel Herd liefert PHP 8.2/8.4/8.5 (`~/Library/Application Support/Herd/bin/php{82,84,85}`),
das PATH-`php` ist ein Wrapper auf die aktive Version. Herd hat **kein phpize** — bauen
gegen die Homebrew-Formel mit identischer Modul-API und das `.so` dann in Herd einbinden:

| Herd | Modul-API | Homebrew-Formel |
|---|---|---|
| php85 (8.5.x) | 20250925 | `php` |
| php84 (8.4.x) | 20240924 | `php@8.4` |
| php82 (8.2.x) | 20220829 | `php@8.2` |

- Extension: `~/Library/Application Support/Herd/config/php/extensions/vio.so`
- Config: `~/Library/Application Support/Herd/config/php/{82,84,85}/php.ini` → `extension=vio.so`
- PHPolygon verlangt `php >= 8.5` → Ziel ist Herd `php85` + Homebrew `php`.

## D3D11 / D3D12 / Vulkan — Stand nach dem GAP-Plan (2026-09-09)

`D3D-VULKAN-GAP-PLAN.md` (Phasen 0–4 umgesetzt, Phase 5 = Folgearbeit). Was sich für
Aufrufer geändert hat:

- **Feature-Flags sind ehrlich**: Vulkan meldete damals `3D_PIPELINE/INSTANCED_DRAW/DEPTH_BIAS/
  TESSELLATION/GEOMETRY = 0` (heute alle 1, GEOMETRY/TESSELLATION nach Device-Feature),
  D3D11/D3D12 melden `TESSELLATION/GEOMETRY` nur 1, wenn das gelinkte SPIRV-Cross die Stage nach
  HLSL bringt (s. „Geometry- und Tessellation-Stages"), D3D12 `RENDER_TARGET_MSAA = 0` (PSO braucht `SampleDesc`, Phase 5),
  D3D12 `TEXTURE_SWIZZLE = 1`. `074` pinnt jetzt auch d3d11/d3d12/vulkan.
- **`auto` überspringt Backends ohne 3D-Pipeline**, wenn ein späterer Kandidat eine hat
  (Registry-Pass 0 verlangt den vollständigen 3D-Satz – MRT, Cube-Targets, Cubemaps, Texture-Arrays –,
  den Vulkan seit GAP-PHASE5 Block 10c meldet; Linux wählt damit wieder Vulkan vor OpenGL. Kann das
  gewählte Backend kein Device öffnen, versucht `vio_create('auto')` den nächsten Kandidaten). Test `100`.
- **Audit-Gate `099`** friert `strcmp(ctx->backend->name, …)` (66) und `#if HAVE_D3D11/
  D3D12/VULKAN` (46, seit `vio_gpu_info` über den Slot `gpu_info` läuft) in `php_vio.c` ein — neue Backend-Fähigkeiten gehen über Vtable-Slots.
  Render-Target-Erstellung/-Bind/-Unbind/-Readback und Cubemap-Upload für D3D11/D3D12
  liegen jetzt in `src/backends/d3d1x/` (`create_render_target`, `bind_render_target`,
  `unbind_render_target`, `bind_render_target_face`, `render_target_cubemap`,
  `generate_mipmaps`, `read_render_target`, `upload_cubemap`, `update_texture`).
- **D3D12-Sampler**: `filter`/`wrap`/`anisotropy` einer Textur werden honoriert (vorher 8
  statische LINEAR/WRAP-Sampler). Root-Param [4] ist eine Sampler-Table s0–s7, gefüllt aus
  einem Kombi-Heap (`vio_d3d12_sampler_combo()`), Shadow-Comparison-Sampler bleiben statisch
  auf s8–s11. Wer `pending_srvs` schreibt, nimmt `vio_d3d12_bind_srv_slot()`; wer Heaps
  bindet, nimmt `vio_d3d12_bind_graphics_heaps()`.
- **`vio_texture(['anisotropy' => 1..16])`** auf D3D11, D3D12, Vulkan (`samplerAnisotropy`
  wird aktiviert, wenn vorhanden), OpenGL (`GL_TEXTURE_MAX_ANISOTROPY`) und Metal
  (`MTLSamplerDescriptor.maxAnisotropy`, GAP-PHASE5 Block 11). `mipmaps => true` wird auf
  D3D12 umgesetzt (CPU-Kette beim Upload).
- **Cube-Render-Targets + `vio_generate_mipmaps`** auf D3D11 (`GenerateMips`) und D3D12
  (Compute-Downsample pro Level – SRV Level n, UAV Level n+1, bilineares `SampleLevel` =
  2×2-Box; Ressourcen mit Mip-Kette bekommen `ALLOW_UNORDERED_ACCESS`, wenn das Format
  typed UAV-Stores kann, sonst bleibt der CPU-Box-Filter; GAP-PHASE5 Block 11). `vio_read_render_target($rt,
  $face)` auf D3D11. `vio_texture_update` auf D3D12.
- **RT-MSAA** (`'samples' => N`) ist auf D3D11 (Resolve beim Unbind/Readback) und OpenGL
  (Multisample-Renderbuffer + Blit) implementiert; vorher ignorierten beide `samples`
  bei `RENDER_TARGET_MSAA = 1`. Depth-only-/Cube-/MRT-Targets bleiben single-sample.
- **Vulkan `vsync: false`** wählt `IMMEDIATE` (Fallback MAILBOX → FIFO); `true` = FIFO.
- **Headless-Fenster sind undekoriert** und `vio_begin` resized D3D-Swapchains im
  Headless-Modus nicht mehr auf die Fenstergröße: vorher war ein 32×32-Headless-Backbuffer
  auf D3D11/D3D12/Vulkan 348 px breit (Windows-Mindestbreite dekorierter Fenster) und
  `vio_read_pixels` lieferte ab Zeile 1 die falschen Pixel — jeder D3D-Pixeltest mit
  kleinen Größen prüfte Müll. Jetzt sind alle Backends 1:1 (`strlen(read_pixels) == w*h*4`).
- **D3D12-Upload-Queue**: Textur-/Cubemap-/Buffer-Uploads und RT-Initial-Clears laufen
  über einen Allocator-Ring (3) + eine Upload-Command-List mit Fence-Signal statt
  `wait_for_gpu()` pro Ressource; Staging-Buffer werden in `begin_frame` per Fence
  freigegeben. Ordnung ist durch die eine DIRECT-Queue garantiert (Test `103`: mid-frame
  erzeugte/aktualisierte Texturen sind im selben Frame sichtbar). Statische Mesh-VB/IB
  liegen im DEFAULT-Heap. SRV-Tables starten aus einem vorgebauten Null-Block
  (1 `CopyDescriptorsSimple` statt 16 `CreateShaderResourceView`).
- **Vulkan**: persistenter Transient-Pool + Fence für Uploads/Dispatches. **D3D11**:
  `gpu_flush` yieldet statt zu spinnen. **FXC**: `OPTIMIZATION_LEVEL3` (Release).
- Debug: `VIO_DUMP_HLSL=1` druckt das transpilierte Grafik-HLSL (wie `VIO_DUMP_CS_HLSL`).
- **2D-Batch hält Referenzen** auf `VioFont`/`VioTexture` seiner Items (`vio_2d_item.owner`),
  ein Font darf also nach `vio_text` vor `vio_draw_2d` freigegeben werden (vorher Crash auf D3D).
- **`in mat4`-Vertex-Attribute** werden aus der Reflection auf 4 Locations expandiert
  (`vio_vertex_attrib.matrix_columns/matrix_column`); D3D-Input-Layouts nutzen die SPIRV-Cross-
  Semantik `TEXCOORD{loc}_{col}`. `vio_draw_instanced_from_buffer` bindet Uniforms wie `vio_draw`.
- **Gallery**: `examples/gallery.php [backend] [outdir] [scene,…]` rendert alle Feature-Pfade
  headless nach `docs/gallery/*.png` (README-Abschnitt „Gallery"). Auf D3D12 fehlt nur MSAA
  (dort auf D3D11 gerendert).

## Bekannte Einschränkungen

- **Vulkan-3D** (GAP-PHASE5 Block 10a–10c) hat den vollständigen Satz: 3D-Pipeline, Instancing,
  Depth-Bias, Stencil, Vertex-Storage, Indirect Draw, HDR-/Depth-only-/MSAA-/Cube-Targets, MRT,
  Cubemaps, Mipmaps, Texture-Arrays/BC/KTX2 und Variable Rate Shading (wenn das Device
  `VK_KHR_fragment_shading_rate` hat) und die HDR10-Swapchain (Block 10d). Bekannt: Laufen D3D12-Debug-Layer
  und Vulkan-Validation mit HDR10-Swapchains nacheinander im selben Prozess (Test 118 im Debug-Harness),
  stirbt das Zerstören des zweiten Vulkan-HDR-Kontexts; ohne einen der beiden Layer läuft dieselbe Folge.
- **D3D12 RT-MSAA** (GAP-PHASE5 Block 1): jede `vio_d3d12_pipeline` hält ihre PSO-Beschreibung und
  baut beim Binden lazily die Variante für die Sample-Zahl des gebundenen Targets (2/4/8);
  die RT-Farbe liegt in multisampled Ressourcen, die Resolve-Ziele (SRV/Readback) werden beim
  Unbind per `ResolveSubresource` gefüllt. Kein `'samples'` auf der Pipeline nötig.
- **Input-Layout auf D3D/Metal** kommt aus der Shader-Reflection (Attribute dicht gepackt in
  Location-Reihenfolge), nicht aus dem Mesh-Layout: ein Mesh mit Lücken/anderer Reihenfolge
  (`['location' => 7, …]` zwischen 0 und 1) liest auf D3D falsche Offsets; OpenGL nutzt das
  Mesh-VAO. Meshes in Location-Reihenfolge ohne Lücken anlegen.
- `vio_mouse_position`/`vio_mouse_delta` teilen auf Windows durch den GLFW-Content-Scale
  (physische → logische Cursor-Pixel); im Headless-Modus ist der Scale 1 (injizierte
  Koordinaten sind logisch) — vorher schlug `026` auf jedem HiDPI-Windows-Host fehl.
- Metal: max. 8 PSO-Varianten (Zielformat × Mesh-Stride × Samples) pro Pipeline und 8
  2D-Varianten; Texturen sind `MTLStorageModeShared` (Apple Silicon); Geometry- und Tessellation-
  Stages laufen über Compute-Kernel (siehe „Metal-3D-Pipeline"). Grenzen der GS-Emulation (Warning
  bei `vio_shader`): Sampler in VS/GS einer GS-Pipeline, Interface-Blöcke/Struct-Varyings, GS hinter
  Tessellation, `TRIANGLE_STRIP_ADJACENCY`; ein vom GS geschriebenes `gl_PrimitiveID` erreicht den
  Fragment-Shader nicht. Variable Rate Shading gibt es nicht: Metals Rasterization-Rate-Maps
  verkleinern das physische Target und lassen sich nicht transparent auf `vio_set_shading_rate` abbilden.
- `vio_clear`: vor `vio_begin` wird die Farbe gelatcht und beim Frame-Start angewendet; **im
  Frame cleart jedes Backend sofort** das gebundene Ziel (Swapchain oder RT) — OpenGL seit
  Phase 1 des Replacement-Plans wie D3D11/D3D12/Metal. Neue RTs starten mit Depth 1.0 /
  Farbe 0 (GL, Metal), damit „bind + draw ohne clear" depth-testet.
- **Uniforms auf OpenGL (SPIR-V-Pfad)**: `vio_spirv_to_glsl()` lässt SPIRV-Cross UBOs als plain
  uniforms emittieren, und glslang packt lose `uniform mat4 u_mvp;` eines `#version 450`-Shaders in
  `gl_DefaultUniformBlock` — beides wird zu **einem** struct-typisierten Uniform (`uniform Matrices _19;`),
  die GL-Namen lauten `_19.uProjection`. `opengl_set_uniform()` löst deshalb über
  `gl_uniform_location()` auf: exakter Treffer, sonst `<struct>.name`-Suffix-Match über die aktiven
  Uniforms (Array-Indizes werden gegen die `[0]`-Form normalisiert, `u_lights[2].pos` → exaktes
  Element), gecacht pro (Programm, Name), Cache-Einträge sterben mit `glDeleteProgram`. Vorher wurde
  **jedes** `vio_set_uniform` eines nicht-RAW-Shaders auf GL still verworfen (Test 108). Auf den cbuffer-Backends
  hält die Reflection ein Array von Skalaren/Vektoren/Matrizen als einen Eintrag mit Schrittweite
  (`vio_uniform_entry.stride`); `vio_uniform_lookup` löst `name[i]` daraus auf (Test 146). Weiterhin nicht
  portabel: `vio_uniform_buffer` + `vio_bind_buffer` für Grafik-Shader (GL: Block ist geflattet, kein
  UBO; D3D12: Root-CBV wird vom Shader-Cbuffer-Push überschrieben) — Aufrufer nehmen `vio_set_uniform`.
  Sampler-Unit ist auf D3D/Metal der Wert aus `vio_set_uniform('u_tex', unit)` (GL-Konvention), nicht
  das `layout(binding)`; ohne Set gilt Unit 0.
- **Vergleichs-Sampling auf OpenGL**: Tiefentexturen tragen kein `GL_TEXTURE_COMPARE_MODE` (sie werden
  auch roh per `sampler2D` gelesen). Vor jedem 3D-Draw bindet das Backend stattdessen an jede Unit, die
  ein `sampler*Shadow` des aktuellen Programms liest, ein Sampler-Objekt mit `GL_COMPARE_REF_TO_TEXTURE`
  (LEQUAL, linear, Rand weiß) und löst es nach dem Draw wieder (`gl_shadow_begin/end`; Locations je
  Programm gecacht). Vorher war `sampler2DShadow` auf GL undefiniert und lieferte die rohe Tiefe.
  D3D11-Tiefen-Cubes (`vio_render_target_cubemap`) haben dafür einen eigenen Vergleichs-Sampler
  (`d3d11_sampler_cmp`). Test 141.
- **GL-Namen sind per Kontext** (`gl_generation` in jedem vio-Objekt, `vio_opengl_context_generation()`):
  ein VioShader/VioMesh/VioTexture/… aus Kontext 1 wird von PHP oft erst freigegeben, wenn
  Kontext 2 schon current ist (Neuzuweisung in einer Backend-Schleife, GC nach `vio_destroy`);
  die GL-Destruktoren löschen nur Namen der lebenden Generation, sonst wird der Kontext-2-
  Shader mit demselben Namen gekillt (zweiter OpenGL-Kontext rendert schwarz — Test 107).
  Tests, die über `['auto', …, 'opengl']` iterieren, deduplizieren per `vio_backend_name()`.
- **Headless-Größen** sind 1:1: `vio_framebuffer_size`/`vio_window_size` = Config-Größe,
  `vio_content_scale`/`vio_pixel_ratio` = 1 — unabhängig vom Retina-Faktor des versteckten
  GLFW-Fensters; Viewport/2D-Projektion in `vio_begin` folgen dem.
- Pipelines geben ihre Backend-Objekte im Free-Handler frei (`destroy_pipeline`; D3D12 parkt
  PSOs bis zum Fence des aufzeichnenden Frames).
- **Vulkan headless auf macOS (MoltenVK, Retina)** rendert in doppelter Größe; `vio_read_pixels` liefert nur
  das obere linke Viertel (bei Test 153 entdeckt; die CI fährt Vulkan nicht auf macOS). Pixel-Tests mit
  Vulkan lokal auf dem Mac deshalb nur mit symmetrischen/positionsunabhängigen Prüfungen bewerten.
- Vulkan auf macOS braucht `VK_DRIVER_FILES=/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json` + `DYLD_LIBRARY_PATH=/usr/local/lib` (SIP blockiert letzteres in Subprozessen). Auto-Auswahl vermeidet Vulkan auf macOS zugunsten von Metal.
- VideoToolbox-Encoder kann in headless fehlschlagen → Fallback auf libx264
- `php_vio.c` ist monolithisch (~9050 Zeilen) — alle PHP-Funktionen in einer Datei; Audit-Gate
  `099` hält die Zahl der Backend-Zweige darin auf dem heutigen Stand oder darunter.
- SPIRV-Cross hat keine Homebrew-Formel; ohne `--with-spirv-cross` kann Metal kein
  GLSL→MSL übersetzen und jeder Shader scheitert (`Makefile.macos` baut es aus `.deps/`).
- SPIRV-Cross vor `vulkan-sdk-1.4.363.0` hat den Struct-Array-Stride-Bug im MSL-Backend
  (siehe Build); Test 094 schlägt dort auf Metal fehl. Seit Homebrew `spirv-cross` 1.4.363.0
  läuft 094 in der macOS-CI wieder mit (der frühere Skip `VIO_SKIP_SPIRV_CROSS_LAYOUT_TEST`
  und `deps-patches/` sind entfernt). Windows (Vulkan SDK 1.4.341.0) betrifft das nicht:
  094 prüft den Metal-Pfad.
- Text-Shaping braucht HarfBuzz (`--with-harfbuzz`); ohne es rendern Arabisch/
  Thai/Ligaturen nicht (`VIO_HAS_SHAPING == 0`, Legacy-Codepoint-Pfad). Der
  vcpkg-HarfBuzz (`harfbuzz[core,freetype]`) ist dynamisch — `harfbuzz.dll` +
  Abhängigkeiten (`freetype.dll`, `brotli*`, `bz2`, `libpng`, `zlib`) müssen zur
  Laufzeit neben `php.exe` liegen. Für ein self-contained `vio.dll` wäre
  `harfbuzz[core]:x64-windows-static-md` (ohne FreeType, statisch, /MD) die
  sauberere Deployment-Variante.
- Shaping: horizontal only. Vertikaler Text (CJK vertical) ist Folgearbeit.
- **Tessellierte Isolines auf WARP**: Der Software-Rasterizer (vios Headless-D3D11/D3D12-Device und die
  Windows-CI) verliert die Linien-Primitive tessellierter Isolines – ein eigenständiges D3D11-Programm ohne vio
  zeichnet dort nichts, in vio kommen je Frame zufällig Linien an. Der Tessellator selbst stimmt (Punkt-Ausgabe
  liefert exakt Dichte × (Detail + 1) Punkte), gewöhnliche Linien auch. Auf Hardware (RTX 2080) zeichnen
  generiertes und handgeschriebenes HLSL korrekt und stabil. Test 144 prüft Isolines auf D3D deshalb nur über
  einen Fenster-Kontext auf D3D11 und D3D12 mit echtem Adapter (`vio_gpu_info()` ≠ „Microsoft Basic Render Driver“).
- **Tessellations-Domain-Ursprung**: Vulkan setzt `VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT` (vorher oben links →
  umgekehrte Winding, GL-korrekte Patches verschwanden bei Backface-Culling). Metal nutzte die MSL-Option
  `tess_domain_origin_lower_left` plus Winding-Umkehr: das legte `outer[1]`/`outer[3]` von Quads auf die
  Gegenkante und cullte jedes Dreiecks-Patch – Metal braucht keins von beidem (Test 144 auf der macOS-CI).
- **`gl_PatchVerticesIn` auf OpenGL** mit SPIRV-Cross vor `vulkan-sdk-1.3.275` (Ubuntu 24.04: 1.3.268): das
  GLSL-Backend schreibt `gl_BuiltIn_14`; `vio_spirv_to_glsl` ersetzt es (`vio_glsl_fix_patch_vertices`).
