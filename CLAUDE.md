# php-vio — PHP Video Input Output Extension

## Was ist das?

Eine PHP C-Extension die GPU-Rendering (OpenGL 3.0–4.6, Vulkan, Metal, Direct3D 11/12),
Audio, Video-Recording, Streaming und Input in PHP verfügbar macht. Basis-Infrastruktur
für die PHPolygon Game Engine. Aktuell **v2.8.0**, 134 PHP-Funktionen, 13 Zend-Klassen,
6 Backends, 98 PHPT-Tests. Releases laufen über semantic-release
(`.github/workflows/release.yml`, Conventional Commits → `CHANGELOG.md`).

## Build

### macOS (Homebrew)

Herd bringt PHP 8.2/8.4/8.5 als Binaries mit, aber **kein `phpize`/`php-config`** —
der Build braucht die passende Homebrew-Formel (`php` = 8.5, API 20250925 wie Herd
`php85`; `php@8.4` = API 20240924 wie Herd `php84`). SPIRV-Cross hat **keine
Homebrew-Formel** und wird aus `.deps/SPIRV-Cross` per cmake nach `/opt/homebrew`
installiert (Rezept in `Makefile.macos`, untracked).

**SPIRV-Cross braucht den Patch `deps-patches/spirv-cross-msl-struct-array-stride.patch`**
(Basis-Commit in `deps-patches/spirv-cross-base-commit.txt`, upstream `main` Stand
2026-09-07). Ohne ihn legt der MSL-Backend Struct-Arrays im Default-Uniform-Block
(`uniform SpotLight u_spot_lights[4]` — std140-Stride 64, gepackte MSL-Größe 52) mit
falschem Element-Stride oder 48 Byte zu viel Padding an; **alle Uniforms hinter dem
Array werden auf Metal verschoben gelesen**. Regression: `tests/backends/094`.
Anwenden: `git -C .deps/SPIRV-Cross apply ../../deps-patches/spirv-cross-msl-struct-array-stride.patch`.

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

112 PHPT-Tests, nach Themen in Unterordnern (`run-tests.php` rekursiert):

| Ordner | Inhalt |
|---|---|
| `tests/render3d/090–093` | Cube-RT/Mipmaps, Pipeline-State, RT-Readback, Texture-Update + Pipeline-Free (Replacement-Plan Phase 1) |
| `tests/render3d/112` | Blend und Write-Mask je Attachment auf einer MRT-Pipeline: Attachment 0 alpha-blendet, Attachment 1 (Maske 0) bleibt unberührt, Attachment 2 schreibt nur den Rotkanal – der Vertrag für einen Transparent-Pass in ein G-Buffer-Target. |
| `tests/core/117` | `frame_latency => 1`: D3D11/D3D12 melden `waitable` + Latenz 1 in `vio_swapchain_info`, Frames laufen; Backends ohne Feature melden 0 und ignorieren die Option. |
| `tests/core/116` | Shader-Cache: zweiter Kontext im selben Verzeichnis kompiliert denselben Shader aus dem Cache (`stores` > 0 beim ersten, `hits` > 0 beim zweiten Lauf); Vulkan schreibt seine Pipeline-Cache-Datei beim Destroy. |
| `tests/core/115` | `vio_gpu_frame_time`: nach drei gerenderten Frames liefert jedes Backend mit dem Feature eine plausible GPU-Zeit (0 ≤ ms < 5000), ohne Feature −1. |
| `tests/render3d/114` | 16-Bit-Indices: kleines Mesh bekommt 2 Bytes je Index und zeichnet auf jedem Backend korrekt; ein Index ≥ 65536 oder `index_type => VIO_INDEX_UINT32` erzwingt 4 Bytes. |
| `tests/render3d/113` | Stencil: ein Markierungs-Pass (Farbe maskiert, REPLACE ref 1) auf der linken Hälfte, dann EQUAL-/NOTEQUAL-Passes – links grün, rechts rot, keine Farbe aus dem Markierungs-Pass. |
| `tests/render3d/111` | Draw-time-Bind-Tabelle haelt Referenzen: eine als Temporary gebundene RT-Textur (`vio_bind_texture(\, vio_render_target_texture(\), 6)`) ueberlebt bis zum Draw, auch wenn danach weitere Texturobjekte entstehen (D3D11/D3D12/Metal; Regression aus 2.9: recycelter Objektspeicher legte die AO-Karte auf das Schatten-Register). |
| `tests/render3d/096–098` | Storage-Images + 2D-Dispatch (API-Roadmap R2/R7), Multiple Render Targets (R1), Async-Compute im Frame (R7) |
| `tests/backends/108` | OpenGL: `vio_set_uniform()` erreicht UBO-Block-Member, Default-Block-Uniforms und Array-Elemente von SPIR-V-Pfad-Shadern (SPIRV-Cross flacht sie zu `uniform Matrices _19;` ab → GL-Name `_19.uProjection`). |
| `tests/backends/107` | OpenGL-Kontext-Generation: Objekte eines zerstörten Kontexts, die erst freigegeben werden, wenn ein NEUER Kontext current ist, dürfen dessen (wiederverwendete) GL-Namen nicht löschen. |
| `tests/core/099–100`, `render3d/101–105`, `backends/106` | GAP-Plan (`D3D-VULKAN-GAP-PLAN.md`): Audit-Gate für Backend-Zweige in `php_vio.c`, Auto-Backend-Wahl, Sampler-Filter/Wrap, Cube-RT/Mipmaps/Readback auf allen Backends, Mid-Frame-Upload-Ordnung, Anisotropie, RT-MSAA-Resolve, Vulkan-Present-Mode. Die `*_all_backends`-Tests iterieren über `opengl/d3d11/d3d12/metal` und drucken pro Backend `OK` oder `skip (…)`. |
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
| 3D-Pipeline (`vio_mesh`/`vio_shader`/`vio_pipeline`/`vio_draw`) | ✅ | ✅ | ✅ | ❌ stub | ✅ |
| Native 2D-Batch | ✅ | ✅ | ✅ | ✅ | ✅ |
| Render Target (Basis) | ✅ | ✅ | ✅ | ✅ | ✅ |
| Render Target HDR / Depth-only / MSAA | ✅/✅/✅* | ✅/✅/✅* | ✅/✅/✅ (PSO-Sample-Varianten, GAP-PHASE5 1) | ❌/❌/❌ | ✅/✅/✅ |
| Cubemap | ✅ | ✅ | ✅† (seit 2.9: Upload war vorher nicht implementiert) | ❌ | ✅ |
| Compute (`vio_compute_*`) | ✅ (GL ≥ 4.3 → auf macOS nie) | ✅ | ✅ | ✅ | ✅ |
| Vertex-Storage (`vio_draw_instanced_from_buffer`) | ✅ (wenn Compute) | ✅ | ✅ | ❌ | ✅ |
| Texture 3D | ✅ | ✅ | ✅ | ✅ | ✅ |
| read_pixels | ✅ | ✅ | ✅ | ✅ | ✅ |
| Texture Swizzle | ✅ (3.3+) | ❌ (CPU-Expand) | ❌ (CPU-Expand) | ✅ | ✅ |
| Cubemap-RT + `vio_generate_mipmaps` | ✅ | ❌ (Follow-up) | ❌ (Follow-up) | ❌ | ✅ |
| `vio_read_render_target` | ✅ | ✅ | ✅† | ❌ | ✅ |
| `vio_texture_update` | ✅ | ✅ | ❌ (Follow-up) | ❌ | ✅ |
| `depth_write` / `color_mask` / Blend-Modi | ✅ | ✅ | ✅ | — | ✅ |
| MRT (`'attachments' => [VIO_FORMAT_*…]`, bis 4) | ✅ | ✅† | ✅† | ❌ | ✅ |
| Blend/Write-Mask je Attachment (`attachment_blend`, `attachment_color_mask`) | ✅ (GL ≥ 4.0, indexed) | ✅ (IndependentBlend) | ✅ (IndependentBlend) | ❌ | ✅ (per colorAttachment) |
| uint16-Indices (automatisch, `vio_mesh_index_bytes`) | ✅ | ✅ (R16_UINT) | ✅ (R16_UINT) | — | ✅ (MTLIndexTypeUInt16) |
| Shader-/Pipeline-Cache auf Platte (`vio_create(['shader_cache' => dir])`, `vio_shader_cache_stats`) | ✅ (GL ≥ 4.1 Program-Binary) | ✅ (DXBC je Stage) | ✅ (DXBC je Stage) | ✅ (`VkPipelineCache`) | — (Metal cacht selbst) |
| Waitable Swapchain (`vio_create(['frame_latency' => n])`, `vio_swapchain_info`, `VIO_FEATURE_FRAME_LATENCY`) | — | ✅ (`FRAME_LATENCY_WAITABLE_OBJECT`) | ✅ | — (Präsentmodus) | — (3 Drawables) |
| GPU-Zeit je Frame (`vio_gpu_frame_time`, `VIO_FEATURE_GPU_TIMESTAMP`) | ✅ (GL ≥ 3.3 `GL_TIMESTAMP`) | ✅ (TIMESTAMP + DISJOINT) | ✅ (Query-Heap + Readback) | ✅ (`vkCmdWriteTimestamp`) | ✅ (`GPUStartTime/GPUEndTime`) |
| Stencil (`'stencil' => [...]`, `VIO_FEATURE_STENCIL`) | ✅ (DEPTH24_STENCIL8) | ✅ (D24S8) | ✅ (D24S8, `OMSetStencilRef`) | ❌ | ❌ (Depth32Float ohne Stencil-Plane, macOS-Folgearbeit) |
| Storage-Images (`'storage' => true` + `vio_compute_bind_image`) | ✅ (wenn Compute) | ✅† | ✅† | ❌ | ✅ |
| Compute-`local_size` aus Reflection (2D/3D-Dispatch) | ✅ | ✅ | ✅ | ✅ | ✅ |
| Async-Dispatch im Frame (`['async' => true]`, `vio_compute_wait`) | ✅ (Queue in-order) | ✅ (in-order) | ✅† (Frame-List) | sync | ✅ (Frame-Cmd-Buffer) |

† D3D11/D3D12: implementiert, aber ohne Windows-Build hier nur blind editiert — Windows-CI
(WARP) ist der Beleg (`tests/render3d/096`, `097`). Die frueher dort beobachtete "veraltete
Textur nach GPU-Schreibzugriff" auf D3D12 war KEIN Barrier-Problem, sondern die Pending-Bind-
Tabelle ohne Referenz (Test 111): `vio_render_target_texture()` liefert ein Temporary, dessen
Speicher die naechste VioTexture wiederverwendete. Seit dem Fix laufen die D3D12-Pixel-Checks
in 096/097 wieder mit.

\* OpenGL/D3D melden `RENDER_TARGET_MSAA = 1`, ignorieren `samples` aber (alle RTs
single-sampled); D3D meldet auch `TESSELLATION`/`GEOMETRY = 1` ohne Hull/Geometry-Stage.
Metal ist aktuell das einzige Backend mit echtem MSAA-Resolve.

Vulkan ist in vio 2D-only; 3D lief historisch über die separate php-vulkan-Extension.
Metal, Geometry-/Tessellation-Shader gibt es in Metal nicht (`VIO_FEATURE_GEOMETRY == 0`).

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
- `vio_gpu_info()` liefert auf Metal `MTLDevice.name` + `recommendedMaxWorkingSetSize`
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
- Tests: `tests/backends/089_metal_3d_pipeline.phpt` (Pixel-Kontrakt), `088` läuft jetzt auch auf Metal.

### Zend-Objekte (13 Klassen)

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
vio.stub.php                # PHP-Stubs für IDE-Support (134 Funktionen)
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
    vulkan/vio_vulkan.c     # Vulkan (VMA, Swapchain, Sync) — 2D + Compute, 3D stub
    vulkan/vio_vma_wrapper.cpp  # VMA C++17 Wrapper
    metal/vio_metal.m       # Metal (ObjC, CAMetalLayer) — 2D + RT + Compute, 3D stub
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

## PHP API (134 Funktionen)

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
vio_inject_key($ctx, VIO_KEY_W, VIO_PRESS);  // für Tests; auch vio_inject_mouse_move/_button
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

## Konventionen

- **Sprache**: Code und Kommentare auf Englisch. Kommunikation auf Deutsch.
- **Funktionsnamen**: `vio_` Prefix für alle PHP-Funktionen.
- **Konstanten**: `VIO_` Prefix, SCREAMING_CASE.
- **Zend-Objekte**: `vio_*_object` Struct, `Z_VIO_*_P()` Accessor-Macro.
- **Bedingte Kompilierung**: `#ifdef HAVE_GLFW`, `HAVE_VULKAN`, `HAVE_METAL`, `HAVE_D3D11`, `HAVE_D3D12`, `HAVE_IOS`, `HAVE_FFMPEG`, `HAVE_GLSLANG`, `HAVE_SPIRV_CROSS`, `HAVE_HARFBUZZ`.
- **Tests**: PHPT-Format, `tests/<thema>/NNN_name.phpt` (Nummern fortlaufend über alle Ordner, nächste freie: 118 (109/110 gehören dem Branch feat/geometry-tessellation-stages, 111 Bind-Tabelle, 112 Blend je Attachment, 113 Stencil, 114 uint16-Indices, 115 GPU-Zeit, 116 Shader-Cache, 117 Frame-Latenz)), headless OpenGL für GPU-Tests (`../skipif_gl.inc`), Backend-spezifische Tests skippen sauber wenn das Backend fehlt.
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
- `TEXT-SHAPING-PLAN.md` — HarfBuzz + SheenBidi (siehe „Text Shaping" oben).
- `VULKAN-2D-PLAN.md`, `v2-architecture.md`, `IMPLEMENTATION_PLAN.md` — Kontext.
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

- **Feature-Flags sind ehrlich**: Vulkan meldet `3D_PIPELINE/INSTANCED_DRAW/DEPTH_BIAS/
  TESSELLATION/GEOMETRY = 0`, D3D11/D3D12 melden `TESSELLATION/GEOMETRY = 0` (kein GS/HS/DS
  über `vio_shader`), D3D12 `RENDER_TARGET_MSAA = 0` (PSO braucht `SampleDesc`, Phase 5),
  D3D12 `TEXTURE_SWIZZLE = 1`. `074` pinnt jetzt auch d3d11/d3d12/vulkan.
- **`auto` überspringt Backends ohne 3D-Pipeline**, wenn ein späterer Kandidat eine hat
  (Linux: OpenGL vor Vulkan, solange Vulkan-3D fehlt). Test `100`.
- **Audit-Gate `099`** friert `strcmp(ctx->backend->name, …)` (66) und `#if HAVE_D3D11/
  D3D12/VULKAN` (47) in `php_vio.c` ein — neue Backend-Fähigkeiten gehen über Vtable-Slots.
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
  wird aktiviert, wenn vorhanden) und OpenGL (`GL_TEXTURE_MAX_ANISOTROPY`); Metal ignoriert
  es noch. `mipmaps => true` wird auf D3D12 jetzt umgesetzt (CPU-Kette beim Upload).
- **Cube-Render-Targets + `vio_generate_mipmaps`** auf D3D11 (`GenerateMips`) und D3D12
  (CPU-Box-Filter + Re-Upload — korrekt, nicht schnell). `vio_read_render_target($rt,
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

- **Vulkan hat keine 3D-Pipeline** (`VIO_FEATURE_3D_PIPELINE == 0`); 2D, Render-Targets,
  Compute und read_pixels funktionieren dort. Vulkan: kein Cubemap, kein HDR/Depth-only/MSAA-RT.
  `auto` wählt deshalb OpenGL vor Vulkan (siehe GAP-Plan Phase 0.4 / Phase 5).
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
  2D-Varianten; Texturen sind `MTLStorageModeShared` (Apple Silicon); kein Geometry-/
  Tessellation-Stage (Metal-Limitierung).
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
  **jedes** `vio_set_uniform` eines nicht-RAW-Shaders auf GL still verworfen (Test 108). Weiterhin nicht
  portabel: `vio_uniform_buffer` + `vio_bind_buffer` für Grafik-Shader (GL: Block ist geflattet, kein
  UBO; D3D12: Root-CBV wird vom Shader-Cbuffer-Push überschrieben) — Aufrufer nehmen `vio_set_uniform`.
  Sampler-Unit ist auf D3D/Metal der Wert aus `vio_set_uniform('u_tex', unit)` (GL-Konvention), nicht
  das `layout(binding)`; ohne Set gilt Unit 0.
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
- Vulkan auf macOS braucht `VK_DRIVER_FILES=/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json` + `DYLD_LIBRARY_PATH=/usr/local/lib` (SIP blockiert letzteres in Subprozessen). Auto-Auswahl vermeidet Vulkan auf macOS zugunsten von Metal.
- VideoToolbox-Encoder kann in headless fehlschlagen → Fallback auf libx264
- `php_vio.c` ist monolithisch (~9050 Zeilen) — alle PHP-Funktionen in einer Datei; Audit-Gate
  `099` hält die Zahl der Backend-Zweige darin auf dem heutigen Stand oder darunter.
- SPIRV-Cross hat keine Homebrew-Formel; ohne `--with-spirv-cross` kann Metal kein
  GLSL→MSL übersetzen und jeder Shader scheitert (`Makefile.macos` baut es aus `.deps/`).
- Ungepatchtes SPIRV-Cross (auch Homebrew/CI) hat den Struct-Array-Stride-Bug im
  MSL-Backend (siehe Build → `deps-patches/`); Test 094 schlägt dort fehl. Upstream-PR:
  KhronosGroup/SPIRV-Cross#2678. Bis zum Merge setzen die macOS-CI-Jobs
  `VIO_SKIP_SPIRV_CROSS_LAYOUT_TEST=1` (Test 094 skippt) — nach dem Merge Env + SKIPIF entfernen.
- Text-Shaping braucht HarfBuzz (`--with-harfbuzz`); ohne es rendern Arabisch/
  Thai/Ligaturen nicht (`VIO_HAS_SHAPING == 0`, Legacy-Codepoint-Pfad). Der
  vcpkg-HarfBuzz (`harfbuzz[core,freetype]`) ist dynamisch — `harfbuzz.dll` +
  Abhängigkeiten (`freetype.dll`, `brotli*`, `bz2`, `libpng`, `zlib`) müssen zur
  Laufzeit neben `php.exe` liegen. Für ein self-contained `vio.dll` wäre
  `harfbuzz[core]:x64-windows-static-md` (ohne FreeType, statisch, /MD) die
  sauberere Deployment-Variante.
- Shaping: horizontal only. Vertikaler Text (CJK vertical) ist Folgearbeit.
