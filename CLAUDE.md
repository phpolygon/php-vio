# php-vio — PHP Video Input Output Extension

## Was ist das?

Eine PHP C-Extension die GPU-Rendering (OpenGL 4.1, Vulkan, Metal), Audio, Video-Recording, Streaming und Input in PHP verfügbar macht. Basis-Infrastruktur für die PHPolygon Game Engine.

## Build

```bash
# PHP 8.5 (Homebrew)
make clean; phpize --clean; phpize && \
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross --with-vulkan --with-ffmpeg --with-metal && \
make -j$(sysctl -n hw.ncpu)

# PHP 8.4 (Laravel Herd)
make clean; /usr/local/Cellar/php@8.4/8.4.19/bin/phpize --clean
/usr/local/Cellar/php@8.4/8.4.19/bin/phpize && \
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross --with-vulkan --with-ffmpeg --with-metal \
  --with-php-config=/usr/local/Cellar/php@8.4/8.4.19/bin/php-config && \
make -j$(sysctl -n hw.ncpu)
```

## Tests

```bash
NO_INTERACTION=1 TEST_PHP_EXECUTABLE=$(which php) php run-tests.php -d extension=$PWD/modules/vio.so tests/
```

38 Tests, 37 bestehen, 1 skip (Vulkan wegen macOS SIP/DYLD_LIBRARY_PATH).

## Architektur

### Backend-Dispatch (Vtable-Pattern)

Alle GPU-Operationen gehen durch `vio_backend` Vtable in `include/vio_backend.h`. Backends registrieren sich in MINIT. Auto-Auswahl: Vulkan > Metal > OpenGL.

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
  vio_2d.c                  # 2D-Batch-Renderer (z-sortiert, max 4096 Items)
  vio_font.c                # Font-Atlas (stb_truetype, 512x512)
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
  stb/                      # stb_image, stb_truetype, stb_image_write
  vma/                      # Vulkan Memory Allocator
  miniaudio/                # Audio-Engine
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

Vendored (kein Homebrew): GLAD, stb_image/truetype/write, VMA, miniaudio.

## PHP API (71 Funktionen)

### Context & Frame
```php
$ctx = vio_create("opengl", ["width" => 800, "height" => 600, "headless" => true]);
vio_begin($ctx); vio_clear($ctx, 0.1, 0.1, 0.1); /* render... */ vio_end($ctx);
vio_poll_events($ctx);
vio_close($ctx); vio_destroy($ctx);
```

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
```php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
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

## Konventionen

- **Sprache**: Code und Kommentare auf Englisch. Kommunikation auf Deutsch.
- **Funktionsnamen**: `vio_` Prefix für alle PHP-Funktionen.
- **Konstanten**: `VIO_` Prefix, SCREAMING_CASE.
- **Zend-Objekte**: `vio_*_object` Struct, `Z_VIO_*_P()` Accessor-Macro.
- **Bedingte Kompilierung**: `#ifdef HAVE_GLFW`, `HAVE_VULKAN`, `HAVE_METAL`, `HAVE_FFMPEG`, `HAVE_GLSLANG`, `HAVE_SPIRV_CROSS`.
- **Tests**: PHPT-Format, nummeriert (001–038), headless OpenGL für GPU-Tests.
- **2D Farben**: ARGB als uint32 (0xAARRGGBB), z.B. `0xFF0000FF` = rot, alpha=FF.

## Herd-Integration

Extension ist in Laravel Herd (PHP 8.4) geladen:
- Extension: `~/Library/Application Support/Herd/config/php/extensions/vio.so`
- Config: `~/Library/Application Support/Herd/config/php/84/php.ini`
- Muss gegen PHP 8.4 kompiliert werden (API 20240924), nicht PHP 8.5.

## Bekannte Einschränkungen

- `vio_read_pixels()` nur für OpenGL implementiert (Metal/Vulkan: stub)
- Vulkan auf macOS braucht `VK_DRIVER_FILES=/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json` + `DYLD_LIBRARY_PATH=/usr/local/lib` (SIP blockiert letzteres in Subprozessen)
- VideoToolbox-Encoder kann in headless fehlschlagen → Fallback auf libx264
- 2D Line-Rendering erzeugt auf manchen Configs keine sichtbaren Pixel (Rasterization)
- `php_vio.c` ist monolithisch (~3200 Zeilen) — alle PHP-Funktionen in einer Datei
