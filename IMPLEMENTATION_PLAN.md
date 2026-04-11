# Implementierungsplan: php-vio

## Gesamtarchitektur-Zusammenfassung

php-vio ist eine C-Extension für PHP, die als Abstraktionsschicht für grafischen und auditiven I/O dient. Sie dispatcht über Vtables an Backend-Extensions (OpenGL, Vulkan, Metal), die als separate PHP-Extensions existieren. Das Projekt startet bei Null.

---

## Phase 0: Projekt-Skelett und Build-System

**Ziel:** Ein kompilierbares, installierbares PHP-Extension-Skelett, das `phpize && ./configure && make && make install` überlebt und als `extension=vio` geladen werden kann.

### Verzeichnisstruktur anlegen

```
php-vio/
├── config.m4                    # Autotools Build-Definition
├── config.w32                   # Windows Build-Definition
├── CMakeLists.txt               # CMake (für IDE-Support + CI)
├── php_vio.h                    # Extension-Header
├── php_vio.c                    # MINIT/MSHUTDOWN/RINIT/RSHUTDOWN
├── php_vio_arginfo.h            # Generiert aus Stubs
├── vio.stub.php                 # PHP Stubs für Arginfo-Generierung
├── include/
│   ├── vio_backend.h            # Backend Vtable Definition
│   ├── vio_types.h              # Gemeinsame Typen (vio_config, vio_feature, etc.)
│   ├── vio_plugin.h             # Plugin-System Typen
│   └── vio_constants.h          # Alle VIO_* Konstanten
├── src/
│   ├── vio_context.c            # Context-Erstellung und -Verwaltung
│   ├── vio_context.h
│   ├── vio_backend_registry.c   # Backend-Registrierung und -Discovery
│   ├── vio_backend_registry.h
│   ├── vio_resource.c           # Resource Management Basis
│   └── vio_resource.h
├── tests/
│   ├── 001_extension_loaded.phpt
│   └── 002_constants_defined.phpt
└── php-vio-design.md
```

### Dateien und Inhalt

**`config.m4`** – Der zentrale Build-File:
- `PHP_ARG_ENABLE(vio, ...)` zum Aktivieren der Extension
- `PHP_ARG_WITH(glslang, ...)` optional für Shader-Compiler
- `PHP_ARG_WITH(spirv-cross, ...)` optional für Shader-Cross-Compilation
- Source-Files via `PHP_NEW_EXTENSION(vio, php_vio.c src/vio_context.c src/vio_backend_registry.c src/vio_resource.c, ...)`
- `PHP_INSTALL_HEADERS([include/], [vio_backend.h vio_types.h vio_plugin.h vio_constants.h])` – damit Backend-Extensions gegen die Header kompilieren können
- GLFW-Detection via `PKG_CHECK_MODULES` oder manuellen `--with-glfw` Pfad

**`php_vio.h`** – Extension-Header mit:
- `PHP_MINIT_FUNCTION(vio)`, `PHP_MSHUTDOWN_FUNCTION(vio)`, `PHP_RINIT_FUNCTION(vio)`, `PHP_RSHUTDOWN_FUNCTION(vio)`
- `extern zend_module_entry vio_module_entry`
- Module Globals struct: `ZEND_BEGIN_MODULE_GLOBALS(vio)` mit default_backend, debug-Flag, vsync-Flag

**`php_vio.c`** – Extension-Einstiegspunkt:
- Registriert alle VIO_* Konstanten in MINIT (`REGISTER_LONG_CONSTANT`)
- Initialisiert Backend-Registry (`vio_backend_registry_init()`)
- INI-Einträge: `vio.default_backend`, `vio.debug`, `vio.vsync`
- Registriert alle PHP-Funktionen (anfangs nur `vio_create`, `vio_destroy`)

**`include/vio_backend.h`** – Exakt die Vtable-Definition aus dem Design-Dokument. Zusätzlich:
- `VIO_BACKEND_API_VERSION` Makro (Wert: 1)
- `vio_register_backend()` Funktionsdeklaration

**`include/vio_types.h`** – Basis-Typen:
- `vio_config` Struct (width, height, title, vsync, samples, debug)
- `vio_buffer_desc`, `vio_texture_desc`, `vio_pipeline_desc` Structs (anfangs minimal)
- `vio_draw_cmd`, `vio_draw_indexed_cmd` Structs
- `vio_feature` Enum
- Format-Enums: `VIO_FLOAT2`, `VIO_FLOAT3`, `VIO_FLOAT4`, etc.

**`include/vio_constants.h`** – Alle `VIO_*` Konstanten als Defines:
- Topologie: `VIO_TRIANGLES`, `VIO_LINES`, `VIO_POINTS`
- Cull-Mode: `VIO_CULL_NONE`, `VIO_CULL_BACK`, `VIO_CULL_FRONT`
- Blend: `VIO_BLEND_NONE`, `VIO_BLEND_ALPHA`, `VIO_BLEND_ADDITIVE`
- Shader: `VIO_SHADER_SPIRV`, `VIO_SHADER_GLSL`, `VIO_SHADER_MSL`, `VIO_SHADER_AUTO`
- Keys: `VIO_KEY_ESCAPE`, `VIO_KEY_W`, ... (komplett, angelehnt an GLFW Key-Codes)
- Mouse: `VIO_MOUSE_LEFT`, `VIO_MOUSE_RIGHT`, `VIO_MOUSE_MIDDLE`
- Actions: `VIO_PRESS`, `VIO_RELEASE`, `VIO_REPEAT`
- Features: `VIO_FEATURE_COMPUTE`, `VIO_FEATURE_RAYTRACING`, `VIO_FEATURE_TESSELLATION`

### Testbares Ergebnis Phase 0

```bash
phpize && ./configure && make && make test
php -d extension=vio.so -r "var_dump(extension_loaded('vio'));" # true
php -d extension=vio.so -r "var_dump(VIO_KEY_ESCAPE);"          # int
```

---

## Phase 1: Context, Backend-Dispatch und Fenster

**Ziel:** `vio_create()` öffnet ein Fenster via GLFW, Backend-Registrierung funktioniert, ein einfacher Clear-Loop läuft.

**Abhängigkeit:** Phase 0 abgeschlossen

### 1a: vio_context als zend_object

**`src/vio_context.c`** und **`src/vio_context.h`**:

```c
typedef struct _vio_context_t {
    vio_config          config;
    vio_backend        *backend;        // Zeiger auf aktives Backend
    void               *surface;        // Backend-spezifisches Surface-Handle
    GLFWwindow         *window;         // GLFW Fenster (NULL bei headless)
    int                 should_close;
    zend_object         std;             // MUSS letztes Feld sein
} vio_context_t;
```

- `zend_class_entry *vio_context_ce` registrieren
- Custom Object-Handler: `create_object`, `free_obj`, `get_gc`
- `vio_context_from_obj()` Makro zum Extrahieren aus zend_object

### 1b: Backend-Registry

**`src/vio_backend_registry.c`**:

- Statisches Array `vio_backend *backends[VIO_MAX_BACKENDS]` (VIO_MAX_BACKENDS = 16)
- `vio_register_backend(const vio_backend *backend)` – von Backend-Extensions in deren MINIT aufgerufen
- `vio_find_backend(const char *name)` – sucht nach Name
- `vio_find_best_backend()` – Auto-Auswahl: Vulkan > Metal > OpenGL
- Prüfung der `VIO_BACKEND_API_VERSION` bei Registrierung

### 1c: GLFW-Integration

**`src/vio_window.c`** und **`src/vio_window.h`**:
- `vio_window_init()` – `glfwInit()` einmalig in MINIT
- `vio_window_create(vio_config *cfg)` – erstellt GLFWwindow
- `vio_window_destroy(GLFWwindow *window)`
- `vio_window_should_close(GLFWwindow *window)`
- `vio_window_poll_events()` – `glfwPollEvents()`
- GLFW Callbacks registrieren: Key, Mouse, Resize, Close

### 1d: PHP-Funktionen (erste Charge)

| Funktion | Signatur | Verhalten |
|----------|----------|-----------|
| `vio_create` | `vio_create(string $backend = 'auto', array $config = []): VioContext` | Backend finden, GLFW-Fenster öffnen, Backend init aufrufen |
| `vio_destroy` | `vio_destroy(VioContext $ctx): void` | Backend shutdown, Fenster schließen, Ressourcen freigeben |
| `vio_should_close` | `vio_should_close(VioContext $ctx): bool` | GLFW-Abfrage |
| `vio_close` | `vio_close(VioContext $ctx): void` | should_close = true setzen |
| `vio_poll_events` | `vio_poll_events(VioContext $ctx): void` | glfwPollEvents |
| `vio_begin` | `vio_begin(VioContext $ctx): void` | backend->begin_frame() |
| `vio_end` | `vio_end(VioContext $ctx): void` | backend->end_frame() + present() |

### 1e: Dummy-Backend für Tests

**`src/vio_backend_null.c`** – Ein "Null-Backend", das keine echte GPU-Arbeit macht:
- Alle Vtable-Funktionen sind No-Ops
- Wird immer registriert, dient als Fallback und Test-Backend
- Name: `"null"`

### Testbares Ergebnis Phase 1

```php
$ctx = vio_create('auto', ['width' => 800, 'height' => 600, 'title' => 'php-vio Phase 1']);
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_end($ctx);
    vio_poll_events($ctx);
}
vio_destroy($ctx);
```

---

## Phase 2: OpenGL-Backend und erstes Rendering

**Ziel:** Ein Dreieck auf dem Bildschirm zeichnen über die Convenience-API mit OpenGL als erstem echtem Backend.

**Abhängigkeit:** Phase 1 abgeschlossen

### 2a: OpenGL-Backend Extension

Anfangs eingebettet in php-vio, später als eigene Extension ausgliedern.

```
src/backends/
└── opengl/
    ├── vio_opengl.c          # Backend-Registrierung + Init
    ├── vio_opengl.h
    ├── vio_opengl_pipeline.c  # Pipeline/Shader-Verwaltung
    ├── vio_opengl_buffer.c    # VBO/VAO/EBO Management
    ├── vio_opengl_texture.c   # Texture Loading
    └── vio_opengl_draw.c      # Draw Calls
```

- GLAD oder gl3w als OpenGL-Loader einbetten (Header-only)
- OpenGL 4.1 Core Profile
- Vtable-Implementierung: init, create_surface, begin_frame, end_frame, present, create_buffer, create_pipeline, compile_shader, draw, draw_indexed

### 2b: Resource Management

- `vio_resource_list_t`: Verkettete Liste aller Ressourcen eines Contexts
- Jede Ressource hat: `type`, `backend_handle`, `destroy_fn`
- Einzelne Ressourcen als eigene `zend_object` Klassen: `VioBuffer`, `VioTexture`, `VioPipeline`, `VioShader`
- Jede Ressource hält Referenz auf ihren Context

### 2c: Buffer und Mesh API

| Funktion | Verhalten |
|----------|-----------|
| `vio_mesh($ctx, $config)` | Erstellt VBO (+ optional IBO), leitet Layout ab |
| `vio_buffer($ctx, $config)` | Generischer Buffer (Vertex, Index, Uniform) |
| `vio_update_buffer($buffer, $data)` | Buffer-Daten aktualisieren |
| `vio_destroy_buffer($buffer)` | Explizites Destroy |

### 2d: Default-Shader

Eingebettete Default-Shader als C-String-Konstanten:

```
src/shaders/
├── default_vert.glsl    # Einfacher Vertex-Shader (position + color passthrough)
├── default_frag.glsl    # Einfacher Fragment-Shader (color output)
└── embed_shaders.py     # Build-Script: konvertiert .glsl zu C-Header
```

### 2e: Convenience-Layer Basis

- `vio_create()` erstellt automatisch Default-Pipeline mit Default-Shader
- `vio_draw($ctx, $mesh)` nutzt Default-Pipeline wenn keine gebunden

### Testbares Ergebnis Phase 2

```php
$ctx = vio_create('opengl', ['width' => 800, 'height' => 600, 'title' => 'Dreieck']);
$triangle = vio_mesh($ctx, [
    'vertices' => [-0.5, -0.5, 0.0,  0.5, -0.5, 0.0,  0.0, 0.5, 0.0],
    'layout'   => [VIO_FLOAT3],
]);
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw($ctx, $triangle);
    vio_end($ctx);
    vio_poll_events($ctx);
}
vio_destroy($ctx);
```

---

## Phase 3: Input-Handling

**Ziel:** Keyboard, Maus und Fenster-Events funktionieren vollständig.

**Abhängigkeit:** Phase 1 (GLFW-Integration)

### Input-State Management

```c
typedef struct _vio_input_state_t {
    int    keys[VIO_KEY_LAST + 1];          // Current frame state
    int    keys_prev[VIO_KEY_LAST + 1];     // Previous frame state
    double mouse_x, mouse_y;
    double mouse_dx, mouse_dy;
    int    mouse_buttons[VIO_MOUSE_LAST + 1];
    double scroll_x, scroll_y;
    zval   on_key_callback;
    zval   on_resize_callback;
    zval   on_mouse_callback;
} vio_input_state_t;
```

### PHP-Funktionen

| Funktion | Signatur |
|----------|----------|
| `vio_key_pressed` | `(VioContext $ctx, int $key): bool` |
| `vio_key_just_pressed` | `(VioContext $ctx, int $key): bool` |
| `vio_key_released` | `(VioContext $ctx, int $key): bool` |
| `vio_mouse_position` | `(VioContext $ctx): array` |
| `vio_mouse_delta` | `(VioContext $ctx): array` |
| `vio_mouse_button` | `(VioContext $ctx, int $button): bool` |
| `vio_mouse_scroll` | `(VioContext $ctx): array` |
| `vio_on_key` | `(VioContext $ctx, callable $callback): void` |
| `vio_on_resize` | `(VioContext $ctx, callable $callback): void` |
| `vio_toggle_fullscreen` | `(VioContext $ctx): void` |

### Testbares Ergebnis Phase 3

```php
$ctx = vio_create('opengl', ['width' => 800, 'height' => 600]);
while (!vio_should_close($ctx)) {
    vio_poll_events($ctx);
    if (vio_key_pressed($ctx, VIO_KEY_ESCAPE)) vio_close($ctx);
    $mouse = vio_mouse_position($ctx);
    if (vio_mouse_button($ctx, VIO_MOUSE_LEFT)) {
        echo "Klick bei {$mouse[0]}, {$mouse[1]}\n";
    }
    vio_begin($ctx); vio_end($ctx);
}
```

---

## Phase 4: Pipeline Layer und Shader-System

**Ziel:** Explizite Pipeline-Konfiguration, Shader-Laden (GLSL zuerst), Texturen, Uniform Buffers.

**Abhängigkeit:** Phase 2 abgeschlossen

### 4a: Shader-System

```c
typedef struct _vio_shader_t {
    vio_context_t    *ctx;
    void             *backend_handle;
    vio_shader_format format;
    zend_object       std;
} vio_shader_t;
```

- `vio_shader($ctx, $config)` – lädt Vertex + Fragment Shader
- Format-Detection: Prüfen ob Daten mit SPIR-V Magic Number (`0x07230203`) beginnen

### 4b: Pipeline-Objekt

```c
typedef struct _vio_pipeline_t {
    vio_context_t      *ctx;
    void               *backend_handle;
    vio_shader_t       *shader;
    vio_vertex_layout_t vertex_layout;
    vio_topology        topology;
    vio_cull_mode       cull_mode;
    int                 depth_test;
    vio_blend_mode      blend;
    zend_object         std;
} vio_pipeline_t;
```

- `vio_pipeline($ctx, $config)` – erstellt Pipeline
- `vio_bind_pipeline($ctx, $pipeline)` – setzt aktive Pipeline

### 4c: Texturen

- Externe Abhängigkeit: `stb_image.h` (Header-only, einbetten in `vendor/stb/`)
- `vio_texture($ctx, $config)` – lädt Bild von Datei oder aus Speicher
- Formate: PNG, JPG, BMP, TGA
- `vio_bind_texture($ctx, $texture, $slot)`

### 4d: Uniform Buffers

- `vio_uniform_buffer($ctx, $config)` – erstellt UBO
- `vio_update_buffer($buffer, $data)` – aktualisiert Daten
- `vio_bind_buffer($ctx, $buffer, $binding)`

### Testbares Ergebnis Phase 4

```php
$shader = vio_shader($ctx, [
    'vertex'   => file_get_contents('shaders/test.vert'),
    'fragment' => file_get_contents('shaders/test.frag'),
    'format'   => VIO_SHADER_GLSL,
]);
$pipeline = vio_pipeline($ctx, [
    'shader'       => $shader,
    'vertex_layout' => [
        ['location' => 0, 'format' => VIO_FLOAT3],
        ['location' => 1, 'format' => VIO_FLOAT2],
    ],
    'depth_test' => true,
]);
$texture = vio_texture($ctx, ['file' => 'textures/test.png']);
```

---

## Phase 5: 2D-API

**Ziel:** Komplette 2D-Convenience-API: Shapes, Sprites, Text, Sprite Batching.

**Abhängigkeit:** Phase 4 (Texturen, Pipeline)

### 2D-Render-System

```c
typedef struct _vio_2d_state_t {
    vio_pipeline_t   *pipeline_shapes;
    vio_pipeline_t   *pipeline_sprites;
    vio_pipeline_t   *pipeline_text;
    vio_2d_item_t    *items;
    int               item_count;
    int               item_capacity;
    float             projection[16];     // Orthographic Projection
} vio_2d_state_t;
```

### API-Funktionen

- **Shapes:** `vio_rect`, `vio_circle`, `vio_line` – Farben als 32-Bit ARGB
- **Sprites:** `vio_sprite`, `vio_sprite_position`, `vio_sprite_scale`, `vio_sprite_rotate`
- **Sprite Batching:** `vio_sprite_batch`, `vio_batch_add`, `vio_batch_clear` – Single Draw Call
- **Text:** `vio_font` (TTF via stb_truetype.h), `vio_text` mit Glyph-Atlas
- **Compositing:** `vio_draw_2d($ctx)` – sortiert nach Z-Order, Alpha-Blending

### Testbares Ergebnis Phase 5

```php
$ctx = vio_create('opengl', ['width' => 1920, 'height' => 1080]);
$font = vio_font($ctx, 'assets/roboto.ttf', 24);
$sprite = vio_sprite($ctx, 'assets/player.png');
vio_sprite_position($sprite, 400, 300);
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_rect($ctx, 10, 10, 200, 100, ['fill' => 0xFF3366CC]);
    vio_text($ctx, $font, 'Hello VIO', 20, 40, ['color' => 0xFFFFFFFF]);
    vio_draw_2d($ctx);
    vio_end($ctx);
    vio_poll_events($ctx);
}
```

---

## Phase 6: Shader Cross-Compilation und Reflection

**Ziel:** SPIR-V als Lingua Franca, automatische Reflection, Shader-Cross-Compilation.

**Abhängigkeit:** Phase 4 (Shader-System)

### 6a: glslang Integration

- GLSL zu SPIR-V Kompilierung zur Laufzeit
- Fehlermeldungen mit Zeilennummer an PHP durchreichen

### 6b: SPIRV-Cross Integration

- SPIR-V zu GLSL Transpilation (für OpenGL-Backend)
- SPIR-V zu MSL Transpilation (für Metal-Backend)
- Version-Targeting: GLSL 410 für OpenGL 4.1

### 6c: Shader Reflection

- Nutzt SPIRV-Cross Reflection API
- Extrahiert: Vertex Inputs, Descriptor Set Layouts, Push Constant Ranges
- `vio_shader_reflect($shader)` – gibt Array mit Reflection-Daten zurück
- Auto-Layout: `vio_pipeline()` ohne `vertex_layout` leitet Layout aus Reflection ab

### Testbares Ergebnis Phase 6

```php
$shader = vio_shader($ctx, [
    'vertex'   => file_get_contents('shader.vert'),
    'fragment' => file_get_contents('shader.frag'),
    'format'   => VIO_SHADER_GLSL,
]);
$reflection = vio_shader_reflect($shader);
var_dump($reflection);
$pipeline = vio_pipeline($ctx, ['shader' => $shader]); // Layout automatisch
```

---

## Phase 7: Vulkan-Backend

**Ziel:** Vulkan als zweites Backend, identische API, Backend-Wechsel via `vio_create('vulkan')`.

**Abhängigkeit:** Phase 6 (SPIR-V-Support ist für Vulkan essentiell)

### Struktur

```
src/backends/vulkan/
├── vio_vulkan.c              # Backend-Registrierung + Init
├── vio_vulkan.h
├── vio_vulkan_instance.c     # VkInstance, Physical/Logical Device, Queues
├── vio_vulkan_swapchain.c    # Swapchain, Image Views, Framebuffers
├── vio_vulkan_pipeline.c     # Graphics Pipeline, Render Pass
├── vio_vulkan_buffer.c       # VkBuffer, VkDeviceMemory, Staging
├── vio_vulkan_texture.c      # VkImage, VkImageView, VkSampler
├── vio_vulkan_command.c      # Command Pool, Command Buffers
├── vio_vulkan_sync.c         # Fences, Semaphores
└── vio_vulkan_shader.c       # VkShaderModule (SPIR-V direkt)
```

### Kritische Entscheidungen

- **Memory Allocation**: VMA (Vulkan Memory Allocator, Header-only) einbetten
- **Swapchain Recreation**: Bei Resize automatisch neu erstellen
- **Command Buffer Management**: Pro Frame ein Primary Command Buffer, Pool-Reset pro Frame
- **Synchronisation**: Frames-in-Flight (Double/Triple Buffering) mit Fences + Semaphores

### Testbares Ergebnis Phase 7

```php
// Identischer Code wie Phase 2, aber mit Vulkan
$ctx = vio_create('vulkan', ['width' => 800, 'height' => 600]);
$triangle = vio_mesh($ctx, [
    'vertices' => [-0.5, -0.5, 0.0,  0.5, -0.5, 0.0,  0.0, 0.5, 0.0],
]);
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw($ctx, $triangle);
    vio_end($ctx);
    vio_poll_events($ctx);
}
```

---

## Phase 8: Audio-System

**Ziel:** Audio-Playback (Sound-Effekte + Musik), Volume-Control, Loop.

**Abhängigkeit:** Phase 1 (Context-System), unabhängig vom Rendering

### Audio-Backend Vtable

```c
typedef struct _vio_audio_backend {
    const char *name;
    int   (*init)(vio_audio_config *cfg);
    void  (*shutdown)(void);
    void *(*load_sound)(const char *path);
    void  (*unload_sound)(void *sound);
    int   (*play)(void *sound, vio_audio_play_opts *opts);
    void  (*stop)(int channel);
    void  (*pause)(int channel);
    void  (*resume)(int channel);
    void  (*set_volume)(int channel, float volume);
    void  (*set_listener)(float pos[3], float forward[3]);
    void  (*set_source_position)(int channel, float pos[3]);
} vio_audio_backend;
```

### miniaudio Integration

- **miniaudio** (Header-only, Cross-Platform, BSD-Lizenz) in `vendor/miniaudio/`
- Unterstützt: WAV, MP3, FLAC nativ
- OGG/Vorbis via `stb_vorbis.h`
- Audio-Mixer läuft auf eigenem Thread

### PHP-Funktionen

- `vio_audio_init($ctx, $config)` – initialisiert Audio-Engine
- `vio_audio_load($audio, $path)` – lädt Sound-Datei
- `vio_audio_play($sound, $options)` – spielt ab (volume, loop, etc.)
- `vio_audio_stop/pause/resume/set_volume`
- `vio_audio_set_listener`, `vio_audio_set_source_position` – 3D Spatial Audio

### Testbares Ergebnis Phase 8

```php
$audio = vio_audio_init($ctx);
$sfx = vio_audio_load($audio, 'explosion.wav');
$music = vio_audio_load($audio, 'bgm.ogg');
vio_audio_play($music, ['loop' => true, 'volume' => 0.5]);
```

---

## Phase 9: Gamepad-Support

**Ziel:** Controller-Input über GLFW Gamepad API.

**Abhängigkeit:** Phase 3 (Input-System)

### PHP-Funktionen

- `vio_gamepads($ctx)` – Liste verbundener Gamepads
- `vio_gamepad_connected($ctx, $id)` – prüft ob verbunden
- `vio_gamepad_axes($ctx, $id)` – Achsen-Werte
- `vio_gamepad_buttons($ctx, $id)` – Button-States
- `vio_gamepad_triggers($ctx, $id)` – Trigger-Werte
- `vio_gamepad_set_deadzone($ctx, $id, $config)`
- `vio_on_gamepad_connect`, `vio_on_gamepad_button` – Callbacks
- `vio_gamepad_rumble($ctx, $id, $config)` – Haptic Feedback (plattformspezifisch)

---

## Phase 10: Headless Rendering und VRT

**Ziel:** Offscreen-Rendering ohne Fenster, Screenshot-Export, Bildvergleich.

**Abhängigkeit:** Phase 2 (Rendering funktioniert)

### 10a: Headless Context

- `vio_create('opengl', ['headless' => true])` – Offscreen-Context
- OpenGL: GLFW mit `GLFW_VISIBLE = GLFW_FALSE` + FBO
- Vulkan: Kein Surface, nur VkImage als Render Target
- `vio_read_pixels($ctx)` – liest Framebuffer als RGBA-Buffer
- `vio_save_screenshot($ctx, $path)` – speichert als PNG (via stb_image_write.h)

### 10b: Bildvergleich (VRT)

- `vio_compare_images($reference, $current, $options)` – Pixel-für-Pixel Vergleich
- `vio_save_diff_image($diff, $path)` – Visualisierung der Differenz

### 10c: Simulierte Interaktion

- `vio_inject_key($ctx, $key, $action)`
- `vio_inject_mouse_move($ctx, $x, $y)`
- `vio_inject_mouse_button($ctx, $button, $action)`

### Testbares Ergebnis Phase 10

```php
$ctx = vio_create('opengl', ['width' => 800, 'height' => 600, 'headless' => true]);
vio_begin($ctx);
vio_draw($ctx, $scene);
vio_end($ctx);
vio_save_screenshot($ctx, 'output/frame.png');
$diff = vio_compare_images('reference/frame.png', 'output/frame.png', ['threshold' => 0.01]);
assert($diff['passed']);
```

---

## Phase 11: Video Recording

**Ziel:** Framebuffer-Aufnahme in Videodatei (H.264, H.265, VP9).

**Abhängigkeit:** Phase 10 (Framebuffer Readback)

### Implementierung

- Externe Abhängigkeit: **FFmpeg** (libavcodec, libavformat, libavutil, libswscale)
- Encoder-Thread: Separater pthread, kommuniziert über Lock-Free Ring-Buffer
- Hardware-Encoding: NVENC, VideoToolbox, VA-API (Fallback auf Software)

### Architektur

```
Render Loop
    ├── Swapchain Present (Bildschirm)
    └── vio_recorder_capture_frame()
            ├── Framebuffer Readback (async)
            └── Encoder-Thread (parallel)
                    ├── H.264 / H.265 (libavcodec)
                    ├── VP9 (libvpx)
                    └── Muxer → .mp4 / .mkv / .webm
```

---

## Phase 12: Netzwerk-Streaming

**Ziel:** Live-Streaming über RTMP, SRT.

**Abhängigkeit:** Phase 11 (Encoder-Infrastruktur wiederverwendbar)

- Baut auf der gleichen Encoder-Infrastruktur wie Video Recording auf
- FFmpeg unterstützt RTMP und SRT als Output-Formate nativ
- `vio_stream_create`, `vio_stream_start/stop/push_frame`
- WebRTC und HLS/DASH als spätere Erweiterungen

---

## Phase 13: Metal-Backend (macOS)

**Ziel:** Drittes Backend, Metal für macOS.

**Abhängigkeit:** Phase 6 (SPIRV-Cross für SPIR-V zu MSL)

```
src/backends/metal/
├── vio_metal.m               # Objective-C Backend
├── vio_metal.h
├── vio_metal_pipeline.m
├── vio_metal_buffer.m
├── vio_metal_texture.m
└── vio_metal_command.m
```

- `-framework Metal -framework MetalKit -framework QuartzCore`
- GLFW unterstützt Metal via `glfwGetCocoaWindow()` + `CAMetalLayer`
- Shader: SPIR-V zu MSL via SPIRV-Cross

---

## Phase 14: Plugin-System und Threading

**Ziel:** Offenes Plugin-System, Multi-Threaded Command Buffer Recording.

**Abhängigkeit:** Alle vorherigen Phasen stabil

### Plugin-System

- `vio_register_plugin(const vio_plugin *plugin)` – registriert Plugin
- Versionsprüfung gegen `VIO_PLUGIN_API_VERSION`
- Output-Plugin-API: `vio_output_create`, `vio_output_start`, `vio_output_push_frame`
- Input-Plugin-API: `vio_on_input($ctx, $plugin_name, $callback)`

### Multi-Threaded Command Buffers (Vulkan)

- Secondary Command Buffers auf Worker-Threads
- Integration mit PHPolygon Thread-Pool
- Async Asset Loading: `vio_texture_load_async($ctx, $path, $callback)`

---

## Abhängigkeits-Graph

```
Phase 0: Skelett + Build-System
    │
Phase 1: Context + Backend-Dispatch + Fenster
    │
    ├──→ Phase 3: Input-Handling
    │        │
    │        └──→ Phase 9: Gamepad
    │
    ├──→ Phase 2: OpenGL-Backend + erstes Rendering
    │        │
    │        ├──→ Phase 4: Pipeline Layer + Shader
    │        │        │
    │        │        ├──→ Phase 5: 2D-API
    │        │        │
    │        │        └──→ Phase 6: Shader Cross-Compilation
    │        │                 │
    │        │                 ├──→ Phase 7: Vulkan-Backend
    │        │                 │
    │        │                 └──→ Phase 13: Metal-Backend
    │        │
    │        └──→ Phase 10: Headless + VRT
    │                 │
    │                 └──→ Phase 11: Video Recording
    │                          │
    │                          └──→ Phase 12: Netzwerk-Streaming
    │
    └──→ Phase 8: Audio (parallel zu Phase 2-6 möglich)

Phase 14: Plugins + Threading (nach Stabilisierung)
```

## Parallelisierbare Arbeit

- **Phase 3** (Input) und **Phase 2** (OpenGL Rendering) – unabhängig nach Phase 1
- **Phase 8** (Audio) – unabhängig nach Phase 1
- **Phase 5** (2D) und **Phase 6** (Shader Cross-Compilation) – unabhängig nach Phase 4
- **Phase 9** (Gamepad) – unabhängig nach Phase 3

## Externe Abhängigkeiten

| Library | Eingebettet (vendor/) | Extern (System) | Verwendung |
|---------|----------------------|-----------------|------------|
| GLFW 3.x | Nein | Ja (pkg-config) | Windowing, Input |
| GLAD | Ja | – | OpenGL Loader |
| stb_image.h | Ja | – | Bild-Laden |
| stb_image_write.h | Ja | – | Screenshot-Export |
| stb_truetype.h | Ja | – | Font-Rendering |
| miniaudio.h | Ja | – | Audio |
| stb_vorbis.h | Ja | – | OGG Support |
| VMA | Ja | – | Vulkan Memory |
| glslang | Nein | Optional | GLSL zu SPIR-V |
| SPIRV-Cross | Nein | Optional | Shader Reflection + Cross-Compile |
| Vulkan SDK | Nein | Optional | Vulkan-Backend |
| FFmpeg libs | Nein | Optional | Video Recording + Streaming |
