# php-vio — PHP Video Input Output Extension

## Was ist das?

Eine PHP C-Extension die GPU-Rendering (OpenGL 4.1, Vulkan, Metal), Audio, Video-Recording, Streaming und Input in PHP verfügbar macht. Basis-Infrastruktur für die PHPolygon Game Engine.

## Build

### macOS (Homebrew)

```bash
# PHP 8.5 (Homebrew)
make clean; phpize --clean; phpize && \
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross --with-vulkan --with-ffmpeg --with-metal --with-harfbuzz && \
make -j$(sysctl -n hw.ncpu)

# PHP 8.4 (Laravel Herd)
make clean; /usr/local/Cellar/php@8.4/8.4.19/bin/phpize --clean
/usr/local/Cellar/php@8.4/8.4.19/bin/phpize && \
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross --with-vulkan --with-ffmpeg --with-metal --with-harfbuzz \
  --with-php-config=/usr/local/Cellar/php@8.4/8.4.19/bin/php-config && \
make -j$(sysctl -n hw.ncpu)
```

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

88 Tests. Auf Windows (D3D11/D3D12): 84 bestehen, 4 skip. Skips sind
plattform-/backend-bedingt (Vulkan auf macOS wegen SIP/DYLD_LIBRARY_PATH, WARP
nicht verfügbar, o.ä.) — ein Skip ist kein Fehler, ein FAIL schon.

## Architektur

### Backend-Dispatch (Vtable-Pattern)

Alle GPU-Operationen gehen durch `vio_backend` Vtable in `include/vio_backend.h`. Backends registrieren sich in MINIT. Auto-Auswahl ist plattformspezifisch:
- macOS: Metal > OpenGL
- Windows: D3D12 > D3D11 > Vulkan > OpenGL
- Linux: Vulkan > OpenGL

```
vio_create("opengl"|"vulkan"|"metal"|"null"|"auto", [...])
  → vio_find_backend(name) → backend->create_surface()
```

### Zend-Objekte (10 Klassen)

| Klasse | Header | Zweck |
|--------|--------|-------|
| VioContext | src/vio_context.h | GPU-Context, Window, Input-State, 2D-State |
| VioMesh | src/vio_mesh.h | VAO/VBO/EBO für 3D-Geometrie |
| VioShader | src/vio_shader.h | Kompiliertes Shader-Programm + SPIR-V |
| VioPipeline | src/vio_pipeline.h | Render-Pipeline (Shader + State) |
| VioTexture | src/vio_texture.h | GPU-Textur (2D) |
| VioBuffer | src/vio_buffer.h | Uniform/Storage Buffer |
| VioFont | src/vio_font.h | TTF-Font (stb_truetype Atlas) |
| VioSound | src/vio_audio.h | Audio-Quelle (miniaudio) |
| VioRecorder | src/vio_recorder.h | Video-Encoder (FFmpeg) |
| VioStream | src/vio_stream.h | Network-Stream (FFmpeg RTMP/SRT) |

Alle folgen dem gleichen Muster: `zend_object std` als letztes Feld, `Z_VIO_*_P()` Macro, `from_obj()` inline-Helper.

### Verzeichnisstruktur

```
php_vio.c                   # Alle PHP-Funktionen (~3200 Zeilen)
php_vio.h                   # Module-Globals (default_backend, debug, vsync)
php_vio_arginfo.h           # Arginfo + Funktionstabelle (generiert aus vio.stub.php)
vio.stub.php                # PHP-Stubs für IDE-Support
config.m4                   # Autotools Build-Konfiguration
Makefile.frag               # Extra-Regeln: VMA (C++14), Metal (ObjC+ARC)

include/
  vio_backend.h             # Backend-Vtable (~20 Funktionspointer)
  vio_types.h               # Enums, Structs, Deskriptoren
  vio_constants.h           # Keyboard/Mouse/Gamepad Konstanten (GLFW-kompatibel)
  vio_plugin.h              # Plugin-System (Output/Input/Filter)

src/
  vio_context.c             # VioContext Objekt
  vio_backend_registry.c    # Backend-Registry + Auto-Auswahl
  vio_backend_null.c        # No-Op Backend für Tests
  vio_resource.c            # Resource-Lifecycle
  vio_window.c              # GLFW Fenster-Management
  vio_input.c               # Input-State + Callbacks
  vio_mesh.c                # Mesh-Objekt
  vio_shader.c              # Shader-Objekt
  vio_pipeline.c            # Pipeline-Objekt
  vio_texture.c             # Texture-Objekt
  vio_buffer.c              # Buffer-Objekt
  vio_2d.c                  # 2D-Batch-Renderer (z-sortiert, dynamisch wachsend)
  vio_font.c                # Font-Atlas (stb_truetype, 4096x4096, Multi-Range Unicode via PackFontRanges, Glyph-Hashmap)
  vio_text_shape.c          # Text-Shaping (HarfBuzz + SheenBidi): glyph-index-Atlas, BiDi, RTL/Joining. Gated: HAVE_HARFBUZZ
  vio_shader_compiler.c     # GLSL→SPIR-V (glslang)
  vio_shader_reflect.c      # SPIR-V Reflection (SPIRV-Cross)
  vio_audio.c               # Audio-Engine (miniaudio)
  vio_recorder.c            # Video-Recording (FFmpeg H.264)
  vio_stream.c              # Network-Streaming (FFmpeg RTMP/SRT)
  vio_plugin_registry.c     # Plugin-Registry
  shaders/
    default_shaders.h       # Built-in 3D Shader
    shaders_2d.h            # Built-in 2D Shader

  backends/
    opengl/vio_opengl.c     # OpenGL 4.1 Core (GLAD)
    vulkan/vio_vulkan.c     # Vulkan (VMA, Swapchain, Sync)
    vulkan/vio_vma_wrapper.cpp  # VMA C++ Wrapper
    metal/vio_metal.m       # Metal (ObjC, CAMetalLayer)

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

Vendored (kein Homebrew): GLAD, stb_image/truetype/write/rect_pack, VMA,
miniaudio, **SheenBidi** (BiDi, Apache-2.0, `vendor/sheenbidi/`, UNITY-Build via
`-DSB_CONFIG_UNITY`).

## PHP API (71 Funktionen)

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
| `samples` | 0 | MSAA |
| `debug` | 0 | Validation Layers / Debug Output |
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
Backends ignorieren die Option. Abgedeckt von `tests/071_d3d12_frame_count.phpt`.

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
vio_key_pressed($ctx, VIO_KEY_W)        // bool
vio_mouse_position($ctx)                // [float, float]
vio_mouse_button($ctx, VIO_MOUSE_LEFT)  // bool
vio_on_key($ctx, function($key, $action, $mods) { ... });
vio_inject_key($ctx, VIO_KEY_W, VIO_PRESS);  // für Tests
```

### 3D Rendering
```php
$mesh = vio_mesh($ctx, ["vertices" => [...], "layout" => [VIO_FLOAT3], "topology" => VIO_TRIANGLES]);
$shader = vio_shader($ctx, ["vertex" => $glsl_vs, "fragment" => $glsl_fs]);
$pipeline = vio_pipeline($ctx, ["shader" => $shader]);
vio_bind_pipeline($ctx, $pipeline);
vio_draw($ctx, $mesh);
```

### 2D Rendering
```php
vio_rect($ctx, 10, 10, 100, 50, ["color" => 0xFF0000FF]);
vio_circle($ctx, 200, 200, 30, ["color" => 0x00FF00FF, "outline" => true]);
vio_line($ctx, 0, 0, 100, 100, ["color" => 0xFFFFFFFF]);
vio_sprite($ctx, $texture, ["x" => 50, "y" => 50, "scale_x" => 2.0]);
$font = vio_font($ctx, "/path/to/font.ttf", 24.0);
vio_text($ctx, $font, "Hello", 10, 10, ["color" => 0xFFFFFFFF]);
vio_draw_2d($ctx);  // Flush
```

### Textures & Buffers
```php
$tex = vio_texture($ctx, ["file" => "image.png"]);
$tex = vio_texture($ctx, ["data" => $rgba, "width" => 64, "height" => 64]);
vio_bind_texture($ctx, $tex, 0);

$buf = vio_uniform_buffer($ctx, ["size" => 64, "binding" => 0]);
vio_update_buffer($buf, pack("f4", 1.0, 0.0, 0.0, 1.0));
```

### Audio
```php
$sound = vio_audio_load("music.mp3");
vio_audio_play($sound, ["volume" => 0.8, "loop" => true]);
vio_audio_pause($sound); vio_audio_resume($sound); vio_audio_stop($sound);
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

### Async Texture
```php
$h = vio_texture_load_async("large.png");
// ... andere Arbeit ...
$result = vio_texture_load_poll($h);  // null=laden, false=fehler, array=fertig
// $result = ["width" => int, "height" => int, "data" => string]
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
- **Bedingte Kompilierung**: `#ifdef HAVE_GLFW`, `HAVE_VULKAN`, `HAVE_METAL`, `HAVE_FFMPEG`, `HAVE_GLSLANG`, `HAVE_SPIRV_CROSS`.
- **Tests**: PHPT-Format, nummeriert (001–038), headless OpenGL für GPU-Tests.
- **2D Farben**: ARGB als uint32 (0xAARRGGBB), z.B. `0xFF0000FF` = rot, alpha=FF.

## Pläne & Roadmap

Größere Umbauten werden vor der Umsetzung als `*-PLAN.md` im Wurzelverzeichnis
festgehalten (deutsch, phasiert, mit Audit-Gate-/Test-Kontrakt). Bestehende:

- `OPENGL-REFACTOR-PLAN.md` — ✅ implementiert. OpenGL als echtes Backend hinter
  der Vtable; erzwungen durch `tests/070_audit_gate_no_gl_outside_backend.php`
  (kein `glXxx()`/`GL_*` außerhalb `src/backends/opengl/`).
- `TEXT-SHAPING-PLAN.md` — HarfBuzz + SheenBidi (siehe „Text Shaping" oben).
- `VULKAN-2D-PLAN.md`, `v2-architecture.md`, `IMPLEMENTATION_PLAN.md` — Kontext.
- **`NATIVE-PLATFORM-PLAN.md` — 📋 Entwurf. GLFW-Ablöse.** GLFW von der
  Pflicht-Dependency zu einer austauschbaren `vio_platform`-Vtable-Impl machen:
  Phase 0 kapselt GLFW nach `src/platform/glfw/` (alle ~81 `glfw*`-Calls raus aus
  `php_vio.c`) und führt einen Audit-Gate `073` ein (analog 070); die Phasen 1–3
  ergänzen native Win32/Cocoa/X11-Layer hinter derselben Vtable. Präzedenzfall:
  der iOS-Backend (`src/backends/ios/`) ist bereits die GLFW-agnostische
  Window+Input-Hälfte. §9 des Plans: Verifikation via lokaler Multi-Plattform-
  Matrix + CI-Parität.

### Verifikation / CI

- `.github/workflows/build.yml` — Tri-Platform Build+Test+Package (Linux
  x86_64/arm64, macOS x86_64/arm64, Windows x64). Linux/macOS fahren die volle
  Test-Suite; Linux headless via **Xvfb + Mesa-Software-GL** (`LIBGL_ALWAYS_SOFTWARE=1`).
- `.github/workflows/ci-permutations.yml` — kompiliert + load-smoket
  Dependency-Subsets (minimal / glfw-only / no-glfw-full / full) und beweist die
  „kompiliert ohne jede Dependency"-Zusage von `config.m4`.
- `docker/` — reproduziert den Linux-CI-Job lokal (`Dockerfile.linux` + `run.sh`);
  `docker build -t php-vio-ci -f docker/Dockerfile.linux . && docker run --rm -v "$PWD":/src php-vio-ci`.
  macOS/Windows sind nicht dockerbar → native Runner/Hosts (siehe `docker/README.md`).

## Herd-Integration

Extension ist in Laravel Herd (PHP 8.4) geladen:
- Extension: `~/Library/Application Support/Herd/config/php/extensions/vio.so`
- Config: `~/Library/Application Support/Herd/config/php/84/php.ini`
- Muss gegen PHP 8.4 kompiliert werden (API 20240924), nicht PHP 8.5.

## Bekannte Einschränkungen

- `vio_read_pixels()` nur für OpenGL implementiert (Metal/Vulkan: stub)
- Vulkan auf macOS braucht `VK_DRIVER_FILES=/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json` + `DYLD_LIBRARY_PATH=/usr/local/lib` (SIP blockiert letzteres in Subprozessen). Auto-Auswahl vermeidet Vulkan auf macOS zugunsten von Metal.
- VideoToolbox-Encoder kann in headless fehlschlagen → Fallback auf libx264
- `php_vio.c` ist monolithisch (~3200 Zeilen) — alle PHP-Funktionen in einer Datei
- Text-Shaping braucht HarfBuzz (`--with-harfbuzz`); ohne es rendern Arabisch/
  Thai/Ligaturen nicht (`VIO_HAS_SHAPING == 0`, Legacy-Codepoint-Pfad). Der
  vcpkg-HarfBuzz (`harfbuzz[core,freetype]`) ist dynamisch — `harfbuzz.dll` +
  Abhängigkeiten (`freetype.dll`, `brotli*`, `bz2`, `libpng`, `zlib`) müssen zur
  Laufzeit neben `php.exe` liegen. Für ein self-contained `vio.dll` wäre
  `harfbuzz[core]:x64-windows-static-md` (ohne FreeType, statisch, /MD) die
  sauberere Deployment-Variante.
- Shaping v1: nur einzeilig (kein Umbruch), horizontal. Zeilenumbruch + Vertikal
  (CJK vertical) sind Folgearbeit.
