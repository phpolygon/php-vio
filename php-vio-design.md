# php-vio – Design Document

## Vision

**PHP Video Input Output** – eine einheitliche C-Extension für PHP, die über eine konsistente API Zugriff auf OpenGL, Vulkan und Metal bietet. Philosophie: *Alles kann, nichts muss.*

php-vio ist die vollständige Abstraktionsschicht für grafischen und auditiven Input/Output: 2D/3D-Rendering, Audio-Playback, Video-Recording, Headless-Rendering für VRT, und Input-Handling – alles über ein einheitliches Interface, das gegen die drei Backend-Extensions dispatcht.

---

## Architektur

```
┌─────────────────────────────────────────────────┐
│                  PHP Userland                    │
│         $renderer = vio_create('vulkan');        │
│         $mesh = vio_mesh($vertices, $indices);   │
│         vio_draw($mesh);                         │
└──────────────────────┬──────────────────────────┘
                       │ PHP Extension API (Zend)
┌──────────────────────┴──────────────────────────┐
│                    php-vio                        │
│            Einheitliche C-Extension               │
│                                                   │
│  ┌─────────────┐ ┌──────────────┐ ┌────────────┐ │
│  │  Convenience │ │  Resource    │ │  Shader    │ │
│  │  Layer       │ │  Manager     │ │  Compiler  │ │
│  └──────┬──────┘ └──────┬───────┘ └─────┬──────┘ │
│         │               │               │         │
│  ┌──────┴───────────────┴───────────────┴──────┐ │
│  │          Backend Dispatch (vtable)           │ │
│  └──────┬───────────────┬───────────────┬──────┘ │
└─────────┼───────────────┼───────────────┼────────┘
          │               │               │
    ┌─────┴─────┐   ┌─────┴─────┐   ┌────┴──────┐
    │ php-opengl │   │ php-vulkan│   │ php-metal  │
    │ Extension  │   │ Extension │   │ Extension  │
    └─────┬─────┘   └─────┬─────┘   └─────┬─────┘
          │               │               │
     ┌────┴────┐    ┌─────┴─────┐   ┌─────┴─────┐
     │ OpenGL  │    │  Vulkan   │   │   Metal    │
     │ Driver  │    │  Loader   │   │ Framework  │
     └─────────┘    └───────────┘   └───────────┘
```

### Kernprinzipien

1. **php-vio linkt gegen die Backend-Extensions, bündelt sie nicht.**
   Jedes Backend bleibt eine eigenständige PHP-Extension. php-vio erkennt zur Laufzeit, welche geladen sind.

2. **Schichtenmodell mit durchlässigen Grenzen.**
   Drei Ebenen, jede optional nutzbar:
   - **Convenience Layer** – Sinnvolle Defaults, minimaler Code
   - **Pipeline Layer** – Explizite Kontrolle über Render Passes, Pipelines, Descriptor Sets
   - **Raw Layer** – Direkter Zugriff auf die Backend-Extension-Funktionen

3. **Distribution über PIE.**
   `pie install php-vio` installiert die Abstraktionsschicht. Backend-Extensions werden als Abhängigkeiten oder separat installiert.

---

## Backend-Registrierung

Jede Backend-Extension registriert sich beim Laden über eine Vtable:

```c
// vio_backend.h – gemeinsames Header-File für alle Backends

typedef struct _vio_config {
    int width;
    int height;
    const char *title;
    int vsync;
    int samples;        // MSAA, 0 = aus
    int debug;          // Validation Layers / Debug Output
} vio_config;

typedef struct _vio_backend {
    const char *name;   // "opengl", "vulkan", "metal"

    // Lifecycle
    int   (*init)(vio_config *cfg);
    void  (*shutdown)(void);

    // Surface & Window
    void *(*create_surface)(vio_config *cfg);
    void  (*destroy_surface)(void *surface);
    void  (*resize)(int width, int height);

    // Pipeline
    void *(*create_pipeline)(vio_pipeline_desc *desc);
    void  (*destroy_pipeline)(void *pipeline);
    void  (*bind_pipeline)(void *pipeline);

    // Resources
    void *(*create_buffer)(vio_buffer_desc *desc);
    void  (*update_buffer)(void *buffer, const void *data, size_t size);
    void  (*destroy_buffer)(void *buffer);

    void *(*create_texture)(vio_texture_desc *desc);
    void  (*destroy_texture)(void *texture);

    // Shader
    void *(*compile_shader)(vio_shader_desc *desc);
    void  (*destroy_shader)(void *shader);

    // Drawing
    void  (*begin_frame)(void);
    void  (*end_frame)(void);
    void  (*draw)(vio_draw_cmd *cmd);
    void  (*draw_indexed)(vio_draw_indexed_cmd *cmd);
    void  (*present)(void);

    // Compute (optional)
    void  (*dispatch_compute)(vio_compute_cmd *cmd);

    // Query
    int   (*supports_feature)(vio_feature feature);
} vio_backend;

// Jede Backend-Extension ruft dies in MINIT auf:
void vio_register_backend(const vio_backend *backend);
```

### Laufzeit-Discovery

```c
// php-vio prüft beim ersten Aufruf:
static vio_backend *backends[VIO_MAX_BACKENDS];
static int backend_count = 0;

// Backend-Auswahl in PHP:
//   vio_create('vulkan')   → sucht registriertes Vulkan-Backend
//   vio_create('auto')     → wählt das "beste" verfügbare Backend
//   vio_create()           → Alias für 'auto'
```

**Auto-Auswahl Priorität:**
1. Vulkan (wenn verfügbar, breiteste Plattformunterstützung)
2. Metal (macOS/iOS)
3. OpenGL (Fallback)

---

## API-Design: Die drei Ebenen

### Ebene 1 – Convenience ("nichts muss")

Minimaler Code, maximale Defaults. Zielgruppe: schnelle Prototypen, Lernende, einfache Visualisierungen.

```php
// Fenster + Kontext + Swapchain + Default-Pipeline in einem Aufruf
$ctx = vio_create('auto', [
    'width' => 800,
    'height' => 600,
    'title' => 'Mein Fenster'
]);

// Mesh aus Vertex-Daten, Layout wird inferiert
$triangle = vio_mesh($ctx, [
    'vertices' => [-0.5, -0.5, 0.0,  0.5, -0.5, 0.0,  0.0, 0.5, 0.0],
    'layout'   => [VIO_FLOAT3],  // Optional, kann auch automatisch erkannt werden
]);

// Render-Loop
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw($ctx, $triangle);
    vio_end($ctx);
    vio_poll_events($ctx);
}

vio_destroy($ctx);
```

**Was intern passiert:**
- `vio_create()` erstellt Fenster, Surface, Swapchain, Default Render Pass, Command Pool
- `vio_mesh()` erstellt Vertex Buffer, optional Index Buffer, bindet Default-Shader
- `vio_begin()`/`vio_end()` kapseln Command Buffer Recording + Submit + Present
- Default-Shader wird mitgeliefert (einfacher Vertex + Fragment Shader)

### Ebene 2 – Pipeline ("alles kann")

Explizite Kontrolle über Pipeline-Konfiguration, Shader, Render Passes.

```php
$ctx = vio_create('vulkan', ['width' => 1280, 'height' => 720]);

// Shader laden
$shader = vio_shader($ctx, [
    'vertex'   => file_get_contents('shaders/vert.spv'),
    'fragment' => file_get_contents('shaders/frag.spv'),
    'format'   => VIO_SHADER_SPIRV,  // oder VIO_SHADER_GLSL, VIO_SHADER_MSL
]);

// Pipeline konfigurieren
$pipeline = vio_pipeline($ctx, [
    'shader'       => $shader,
    'vertex_layout' => [
        ['location' => 0, 'format' => VIO_FLOAT3, 'usage' => VIO_POSITION],
        ['location' => 1, 'format' => VIO_FLOAT4, 'usage' => VIO_COLOR],
        ['location' => 2, 'format' => VIO_FLOAT2, 'usage' => VIO_TEXCOORD],
    ],
    'topology'     => VIO_TRIANGLES,
    'cull_mode'    => VIO_CULL_BACK,
    'depth_test'   => true,
    'blend'        => VIO_BLEND_ALPHA,
]);

// Texture laden
$texture = vio_texture($ctx, [
    'file'       => 'textures/wall.png',
    'filter'     => VIO_FILTER_LINEAR,
    'wrap'       => VIO_WRAP_REPEAT,
    'mipmaps'    => true,
]);

// Uniform Buffer
$ubo = vio_uniform_buffer($ctx, [
    'size' => 128,
    'binding' => 0,
]);

// Render-Loop
while (!vio_should_close($ctx)) {
    vio_update_buffer($ubo, pack('f*', ...($mvp_matrix)));

    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipeline);
    vio_bind_texture($ctx, $texture, 0);
    vio_bind_buffer($ctx, $ubo, 0);
    vio_draw_indexed($ctx, $mesh, $index_count);
    vio_end($ctx);

    vio_poll_events($ctx);
}
```

### Ebene 3 – Raw (Backend-Durchgriff)

Direkter Zugriff auf die darunterliegende Backend-Extension, wenn die Abstraktion nicht ausreicht.

```php
$ctx = vio_create('vulkan');

// Zugriff auf das native Backend-Handle
$vk_device = vio_native_handle($ctx, 'device');
$vk_queue  = vio_native_handle($ctx, 'queue');

// Ab hier: direkte Aufrufe an php-vulkan
vk_create_render_pass($vk_device, $render_pass_info);
// ...

// Zurück zur vio-Abstraktion – beides mischbar
vio_begin($ctx);
vio_draw($ctx, $mesh);
vio_end($ctx);
```

---

## Shader-Strategie

Eines der komplexesten Themen bei einer Multi-Backend-Abstraktion.

### Ansatz: SPIR-V als Lingua Franca

```
                    ┌─────────────┐
                    │  GLSL Source │  (Entwickler schreibt)
                    └──────┬──────┘
                           │ glslang / shaderc
                    ┌──────┴──────┐
                    │   SPIR-V    │  (Zwischenformat)
                    └──────┬──────┘
                           │ php-vio Shader Compiler
              ┌────────────┼────────────┐
              │            │            │
        ┌─────┴─────┐ ┌───┴───┐ ┌──────┴──────┐
        │ SPIR-V    │ │ GLSL  │ │     MSL     │
        │ (Vulkan)  │ │(OpenGL│ │   (Metal)   │
        └───────────┘ └───────┘ └─────────────┘
                        SPIRV-Cross
```

### Unterstützte Formate

| Format | Konstante | Verwendung |
|--------|-----------|------------|
| SPIR-V Binary | `VIO_SHADER_SPIRV` | Vorkompiliert, empfohlen |
| GLSL Source | `VIO_SHADER_GLSL` | Wird intern kompiliert |
| MSL Source | `VIO_SHADER_MSL` | Nur Metal, kein Cross-Compile |
| Auto | `VIO_SHADER_AUTO` | php-vio wählt je nach Backend |

### Eingebetteter Shader-Compiler

php-vio bündelt optional `glslang` und `SPIRV-Cross` als Build-Dependencies:
- **GLSL → SPIR-V**: via glslang (zur Laufzeit)
- **SPIR-V → GLSL**: via SPIRV-Cross (für OpenGL-Backend)
- **SPIR-V → MSL**: via SPIRV-Cross (für Metal-Backend)

Wenn nicht verfügbar: nur vorkompilierte SPIR-V Shader nutzbar.

---

## Resource Management

### Ownership-Modell

```
vio_context
 ├── besitzt alle vio_*-Ressourcen (Buffer, Textures, Pipelines, Shader)
 ├── Reference Counting via PHP Zend Engine (zend_object)
 └── Cleanup bei vio_destroy() ODER wenn PHP GC den Context freigibt
```

### Regeln

1. **Jede Ressource gehört einem Context.** Kein Sharing zwischen Contexts (vereinfacht Vulkan-Synchronisation enorm).
2. **Automatisches Cleanup.** Wenn der Context zerstört wird, werden alle Ressourcen freigegeben.
3. **Explizites Destroy optional.** `vio_destroy_buffer($buf)` gibt sofort frei, ist aber nicht nötig.
4. **Frame-lokale Ressourcen.** php-vio verwaltet intern Double/Triple-Buffering. Der Nutzer sieht davon nichts.

---

## Input-Handling

php-vio abstrahiert auch Fenster-Events und Input.

```php
// Polling (Standard)
while (!vio_should_close($ctx)) {
    vio_poll_events($ctx);

    if (vio_key_pressed($ctx, VIO_KEY_ESCAPE)) {
        vio_close($ctx);
    }

    $mouse = vio_mouse_position($ctx);   // [float $x, float $y]
    $click = vio_mouse_button($ctx, VIO_MOUSE_LEFT);  // bool
}

// Callback-basiert (optional)
vio_on_key($ctx, function(int $key, int $action, int $mods) {
    if ($key === VIO_KEY_F11 && $action === VIO_PRESS) {
        vio_toggle_fullscreen($ctx);
    }
});

vio_on_resize($ctx, function(int $width, int $height) {
    // Swapchain wird intern automatisch recreated
    // Callback nur zur Benachrichtigung
});
```

---

## Fehlende Backend-Features

Nicht jedes Backend unterstützt alles. php-vio behandelt das explizit:

```php
// Feature-Query
if (vio_supports($ctx, VIO_FEATURE_COMPUTE)) {
    vio_dispatch_compute($ctx, $compute_cmd);
}

if (vio_supports($ctx, VIO_FEATURE_RAYTRACING)) {
    // ...
}

// Auflistung aller Features
$features = vio_capabilities($ctx);
// → ['compute' => true, 'raytracing' => false, 'tessellation' => true, ...]
```

Wenn ein Feature aufgerufen wird, das nicht unterstützt ist:
- **Convenience Layer**: Stille No-Op oder Fallback wo sinnvoll
- **Pipeline/Raw Layer**: PHP Warning + Rückgabe `false`

---

## Build & Distribution

### PIE-Pakete

```
php-vio          (Abstraktionsschicht, Kern)
 ├── Requires: mindestens eines der Backends
 ├── Optional: glslang, SPIRV-Cross (für Laufzeit-Shader-Kompilierung)
 │
 ├── php-vio-opengl  (OpenGL Backend)
 ├── php-vio-vulkan  (Vulkan Backend)
 └── php-vio-metal   (Metal Backend, nur macOS)
```

### Plattform-Matrix

| Plattform | OpenGL | Vulkan | Metal |
|-----------|--------|--------|-------|
| Linux | ✓ | ✓ | – |
| macOS | ✓ (Legacy) | ✓ (MoltenVK) | ✓ |
| Windows | ✓ | ✓ | – |

### php.ini

```ini
; Backend-Extensions laden
extension=vio_opengl
extension=vio_vulkan

; Abstraktionsschicht laden (nach den Backends!)
extension=vio

; Optionale Konfiguration
vio.default_backend=auto
vio.debug=0
vio.vsync=1
```

---

## 2D-API

php-vio bietet eine vollständige 2D-Convenience-API. 2D ist kein nachrangiger Modus, sondern ein erstklassiger Bestandteil.

```php
$ctx = vio_create('auto', ['width' => 1920, 'height' => 1080]);

// Shapes
$rect = vio_rect($ctx, 100, 100, 200, 150, ['fill' => 0xFF3366CC]);
$circle = vio_circle($ctx, 400, 300, 50, ['fill' => 0xFFCC3333, 'outline' => 0xFFFFFFFF]);

// Sprites
$sprite = vio_sprite($ctx, 'assets/player.png');
vio_sprite_position($sprite, 320, 240);
vio_sprite_scale($sprite, 2.0);
vio_sprite_rotate($sprite, 45.0);

// Text
$font = vio_font($ctx, 'assets/roboto.ttf', 24);
vio_text($ctx, $font, 'Hello VIO', 100, 50, ['color' => 0xFFFFFFFF]);

// Sprite Batching (intern optimiert)
$batch = vio_sprite_batch($ctx, $spritesheet, 1000);  // max 1000 Sprites
for ($i = 0; $i < $particle_count; $i++) {
    vio_batch_add($batch, $particles[$i]);
}

// Render-Loop
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw_2d($ctx);  // Zeichnet alle 2D-Objekte, sortiert nach Z-Order
    vio_end($ctx);
    vio_poll_events($ctx);
}
```

### 2D/3D Mischbetrieb

2D und 3D sind frei kombinierbar. php-vio verwaltet intern getrennte Render Passes und compositet sie zusammen.

```php
vio_begin($ctx);

// 3D-Szene
vio_bind_pipeline($ctx, $pipeline_3d);
vio_draw_indexed($ctx, $world_mesh, $index_count);

// 2D-Overlay (HUD, UI)
vio_draw_2d($ctx);  // Zeichnet über die 3D-Szene

vio_end($ctx);
```

---

## Audio-Output

Audio gehört zum Scope von php-vio als Output-Kanal. Der Name "Video Input Output" umfasst explizit Audio als Teil des Outputs.

```php
// Audio-System initialisieren
$audio = vio_audio_init($ctx, [
    'sample_rate' => 44100,
    'channels'    => 2,
    'buffer_size' => 4096,
]);

// Sound laden und abspielen
$sfx = vio_audio_load($audio, 'assets/explosion.wav');
$music = vio_audio_load($audio, 'assets/bgm.ogg');

vio_audio_play($sfx, ['volume' => 0.8]);
vio_audio_play($music, ['volume' => 0.5, 'loop' => true]);

// Laufzeitkontrolle
vio_audio_set_volume($music, 0.3);
vio_audio_pause($music);
vio_audio_resume($music);
vio_audio_stop($music);

// 3D Spatial Audio (optional)
vio_audio_set_listener($audio, $camera_position, $camera_forward);
vio_audio_set_source_position($sfx, [10.0, 0.0, -5.0]);
```

### Backend-Strategie Audio

| Plattform | Backend |
|-----------|---------|
| Linux | PulseAudio / PipeWire / ALSA |
| macOS | Core Audio |
| Windows | WASAPI / XAudio2 |

Audio-Backends werden analog zu den Grafik-Backends über die Vtable registriert und können unabhängig gewählt werden.

---

## Headless Rendering

php-vio unterstützt Offscreen-Rendering ohne Fenster für zwei primäre Use-Cases:

1. **VRT (Visual Regression Testing)** – Automatisiertes Rendering + Screenshot-Vergleich
2. **Video-Testing mit Interaktion** – Simulierte Input-Events gegen einen Headless-Context

```php
// Headless Context – kein Fenster, kein Window-System nötig
$ctx = vio_create('vulkan', [
    'width'    => 1920,
    'height'   => 1080,
    'headless' => true,
]);

// Rendering funktioniert identisch
vio_begin($ctx);
vio_draw($ctx, $scene);
vio_end($ctx);

// Framebuffer als Bild auslesen
$pixels = vio_read_pixels($ctx);                          // Raw RGBA Buffer
vio_save_screenshot($ctx, 'output/frame_001.png');         // Direkt als PNG
vio_save_screenshot($ctx, 'output/frame_001.exr');         // HDR-Format

// VRT: Screenshot-Vergleich
$diff = vio_compare_images('reference/scene.png', 'output/frame_001.png', [
    'threshold'  => 0.01,    // Max. Abweichung pro Pixel (0.0–1.0)
    'tolerance'  => 0.001,   // Anteil abweichender Pixel erlaubt
]);

if (!$diff['passed']) {
    echo "Visual regression detected: {$diff['deviation']}% deviation\n";
    vio_save_diff_image($diff, 'output/diff_scene.png');  // Differenzbild
}
```

### Simulierte Interaktion (Headless)

Für Testing von interaktiven Szenen ohne echtes Fenster:

```php
// Simulierte Input-Events injizieren
vio_inject_key($ctx, VIO_KEY_W, VIO_PRESS);
vio_inject_mouse_move($ctx, 400, 300);
vio_inject_mouse_button($ctx, VIO_MOUSE_LEFT, VIO_PRESS);

// Frame rendern mit simuliertem Input
vio_poll_events($ctx);  // Verarbeitet injizierte Events
vio_begin($ctx);
vio_draw($ctx, $scene);
vio_end($ctx);

// Ergebnis prüfen
vio_save_screenshot($ctx, 'output/after_click.png');
```

---

## Video Recording

php-vio unterstützt In-Game Video-Recording als nativen Output-Kanal.

```php
// Recorder erstellen
$recorder = vio_recorder_create($ctx, [
    'output'     => 'capture/gameplay.mp4',
    'codec'      => VIO_CODEC_H264,         // oder VIO_CODEC_H265, VIO_CODEC_VP9
    'fps'        => 60,
    'bitrate'    => 8_000_000,              // 8 Mbit/s
    'resolution' => [1920, 1080],           // Optional, Default = Context-Größe
    'audio'      => true,                   // Audio-Stream mit aufnehmen
]);

// Recording starten
vio_recorder_start($recorder);

// Render-Loop – Recording läuft automatisch mit
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw($ctx, $scene);
    vio_end($ctx);

    // Frame wird automatisch in den Encoder gepusht
    vio_recorder_capture_frame($recorder);

    vio_poll_events($ctx);
}

// Recording beenden
vio_recorder_stop($recorder);
vio_recorder_destroy($recorder);
```

### Recording-Architektur

```
Render Loop
    │
    ├── Swapchain Present (Bildschirm)
    │
    └── vio_recorder_capture_frame()
            │
            ├── Framebuffer Readback (async, eigener Command Buffer)
            │
            └── Encoder-Thread (läuft parallel zum Rendering)
                    │
                    ├── H.264 / H.265 (FFmpeg libavcodec)
                    ├── VP9 (libvpx)
                    └── Muxer → .mp4 / .mkv / .webm
```

Der Encoder läuft auf einem separaten Thread, sodass Recording die Framerate minimal beeinflusst. PHPolygons Multi-Threading-Fähigkeit macht dies möglich.

### Hardware-Encoding (optional)

```php
$recorder = vio_recorder_create($ctx, [
    'output'   => 'capture/gameplay.mp4',
    'encoder'  => VIO_ENCODER_HARDWARE,  // NVENC, VideoToolbox, VA-API
    // Fallback auf Software-Encoder wenn HW nicht verfügbar
]);
```

---

## Shader Reflection

php-vio leitet aus SPIR-V automatisch Vertex-Layouts, Descriptor-Set-Layouts und Push-Constant-Ranges ab. Manuelle Definition bleibt möglich, wird aber in den meisten Fällen unnötig.

```php
// Shader laden – Reflection passiert automatisch
$shader = vio_shader($ctx, [
    'vertex'   => file_get_contents('shaders/pbr.vert.spv'),
    'fragment' => file_get_contents('shaders/pbr.frag.spv'),
]);

// Reflection-Daten abfragen (für Debugging / Tooling)
$reflection = vio_shader_reflect($shader);
/*
[
    'vertex_inputs' => [
        ['location' => 0, 'name' => 'inPosition',  'format' => VIO_FLOAT3],
        ['location' => 1, 'name' => 'inNormal',    'format' => VIO_FLOAT3],
        ['location' => 2, 'name' => 'inTexCoord',  'format' => VIO_FLOAT2],
    ],
    'descriptor_sets' => [
        0 => [
            ['binding' => 0, 'type' => VIO_UNIFORM_BUFFER, 'name' => 'CameraUBO',  'size' => 128],
            ['binding' => 1, 'type' => VIO_SAMPLER,        'name' => 'albedoMap'],
            ['binding' => 2, 'type' => VIO_SAMPLER,        'name' => 'normalMap'],
        ],
    ],
    'push_constants' => [
        ['offset' => 0, 'size' => 64, 'name' => 'ModelMatrix'],
    ],
]
*/

// Pipeline OHNE manuelles Vertex-Layout – wird aus Shader abgeleitet
$pipeline = vio_pipeline($ctx, [
    'shader' => $shader,
    // vertex_layout wird automatisch aus Reflection erzeugt
    // descriptor_set_layout wird automatisch aus Reflection erzeugt
]);

// Mesh-Erstellung validiert automatisch gegen das reflektierte Layout
$mesh = vio_mesh($ctx, [
    'vertices' => $vertex_data,
    'indices'  => $index_data,
    'shader'   => $shader,  // Optional: Validierung gegen Shader-Layout
]);
```

### Reflection-Pipeline intern

```
SPIR-V Binary
    │
    └── SPIRV-Cross Reflection API
            │
            ├── Vertex Input Attributes → vio_vertex_layout
            ├── Descriptor Set Bindings → vio_descriptor_layout
            ├── Push Constant Ranges   → vio_push_constant_layout
            └── Output Attachments     → Render Pass Konfiguration
```

### Override-Prinzip

Automatische Reflection ist der Default, kann aber jederzeit überschrieben werden:

```php
$pipeline = vio_pipeline($ctx, [
    'shader'        => $shader,
    'vertex_layout' => [...],  // Überschreibt Reflection
    // descriptor_layout bleibt automatisch
]);
```

---

## Threading-Modell

PHPolygon ist multi-threaded fähig. php-vio nutzt das und parallelisiert intern, wo es sinnvoll ist.

### Verantwortlichkeiten

| Komponente | Threading |
|-----------|-----------|
| Render-Loop (Main) | PHPolygon Main-Thread |
| Command Buffer Recording | php-vio intern, parallel auf Worker-Threads |
| Video Encoder | Eigener Thread, async zum Rendering |
| Audio Mixer | Eigener Thread, Echtzeit-Priorität |
| Shader Compilation | Async, blockiert nicht den Main-Thread |
| Asset Loading | Async über PHPolygon Thread-Pool |

### Synchronisation

```
PHPolygon Thread-Pool
    │
    ├── Thread 1: Game Logic
    ├── Thread 2: Physics
    ├── Thread 3: Asset Loading
    │       └──→ vio_texture_load_async($ctx, 'big_texture.png', $callback)
    │
    └── Main Thread: Rendering
            │
            └── php-vio
                ├── Command Buffer 1 (Schatten)     ← Worker Thread A
                ├── Command Buffer 2 (Geometrie)    ← Worker Thread B
                ├── Command Buffer 3 (Post-FX)      ← Worker Thread C
                └── Submit + Present                 ← Main Thread
```

php-vio stellt sicher, dass alle Backend-Synchronisation (Fences, Semaphores, Barriers) intern korrekt gehandhabt wird. Der PHP-Code sieht davon nichts, es sei denn, er greift explizit über Ebene 3 (Raw) auf native Handles zu.

---

## Gesamtübersicht: VIO Scope

```
┌─────────────────────────────────────────────────────┐
│                      php-vio                         │
│              "Video Input Output"                    │
│                                                      │
│   INPUT                        OUTPUT                │
│   ─────                        ──────                │
│   Fenster/Surface              2D Rendering          │
│   Keyboard                     3D Rendering          │
│   Maus                         Audio Playback        │
│   Gamepad/Controller           Video Recording       │
│   Headless (simuliert)         Netzwerk-Streaming    │
│                                Screenshot Export      │
│                                Framebuffer Readback   │
│                                                      │
│   INTERN                                             │
│   ──────                                             │
│   Backend Dispatch (OpenGL / Vulkan / Metal)         │
│   Shader Compilation + Reflection                    │
│   Resource Management                                │
│   Threading / Synchronisation                        │
│   VRT (Visual Regression Testing)                    │
└─────────────────────────────────────────────────────┘
```

---

## Abgrenzung: php-vio vs. PHPolygon

```
┌──────────────────────────────────────────────────────────┐
│                     PHPolygon                             │
│                 (Game Engine)                             │
│                                                           │
│   Scene Graph          GUI / Widgets        Game Logic    │
│   Entity System        UI-Elemente          Scripting     │
│   Physics              Menüs, HUD           AI / KI      │
│   Asset Pipeline       Text-Input            Networking   │
│   Animation            Dialoge              Savegames     │
│                                                           │
└────────────────────────┬─────────────────────────────────┘
                         │  nutzt
┌────────────────────────┴─────────────────────────────────┐
│                      php-vio                              │
│              (Video Input Output)                         │
│                                                           │
│   INPUT                        OUTPUT                     │
│   Fenster / Surface            2D / 3D Rendering          │
│   Keyboard                     Audio Playback             │
│   Maus                         Video Recording            │
│   Gamepad / Controller         Netzwerk-Streaming         │
│   Headless (simuliert)         Screenshot Export          │
│                                Framebuffer Readback       │
│                                                           │
│   INTERN                                                  │
│   Backend Dispatch (OpenGL / Vulkan / Metal)              │
│   Shader Compilation + Reflection                         │
│   Resource Management                                     │
│   Threading / Synchronisation                             │
│   VRT (Visual Regression Testing)                         │
└──────────────────────────────────────────────────────────┘
```

**Faustregel:** php-vio kümmert sich um alles, was direkt mit Hardware-I/O zu tun hat. PHPolygon kümmert sich um alles, was mit Spiellogik und Engine-Architektur zu tun hat. vio weiß nicht, was eine "Szene" oder ein "Entity" ist – es kennt nur Meshes, Shader, Buffer und Pixel.

---

## Gamepad / Controller Input

Gamepad-Support ist Teil von php-vio als Input-Kanal.

```php
// Gamepads erkennen
$gamepads = vio_gamepads($ctx);
// → [0 => ['name' => 'Xbox Controller', 'id' => 0], ...]

// Polling
if (vio_gamepad_connected($ctx, 0)) {
    $axes    = vio_gamepad_axes($ctx, 0);
    // → ['left_x' => 0.0, 'left_y' => -0.8, 'right_x' => 0.1, ...]

    $buttons = vio_gamepad_buttons($ctx, 0);
    // → ['a' => true, 'b' => false, 'x' => false, ...]

    $triggers = vio_gamepad_triggers($ctx, 0);
    // → ['left' => 0.0, 'right' => 0.95]
}

// Callback-basiert
vio_on_gamepad_connect($ctx, function(int $id, string $name) {
    echo "Gamepad connected: $name\n";
});

vio_on_gamepad_button($ctx, function(int $id, int $button, int $action) {
    // ...
});

// Rumble / Haptic Feedback
vio_gamepad_rumble($ctx, 0, [
    'strong' => 0.8,   // Starker Motor
    'weak'   => 0.3,   // Schwacher Motor
    'duration' => 200,  // Millisekunden
]);

// Deadzone-Konfiguration
vio_gamepad_set_deadzone($ctx, 0, [
    'left_stick'  => 0.15,
    'right_stick' => 0.10,
    'triggers'    => 0.05,
]);
```

### Gamepad-Abstraktionsschicht

```
Physischer Controller
    │
    └── Plattform-API
            ├── Linux: evdev / SDL2
            ├── macOS: IOKit / GCController
            └── Windows: XInput / DirectInput
                    │
                    └── php-vio Gamepad-Abstraction
                            │
                            └── Einheitliches Mapping
                                (Xbox-Layout als Referenz)
```

php-vio mappt intern alle Controller auf ein einheitliches Layout (Xbox-Style: A/B/X/Y, Bumper, Triggers, Sticks). Unbekannte Controller werden über eine Mapping-Datenbank (SDL_GameControllerDB) unterstützt.

---

## Netzwerk-Streaming

Video-Output ist nicht auf Dateien beschränkt. php-vio unterstützt Live-Streaming als Output-Kanal.

```php
// RTMP-Stream (z.B. Twitch, YouTube Live)
$stream = vio_stream_create($ctx, [
    'url'      => 'rtmp://live.twitch.tv/app/STREAM_KEY',
    'codec'    => VIO_CODEC_H264,
    'fps'      => 30,
    'bitrate'  => 4_500_000,
    'audio'    => true,
    'preset'   => VIO_STREAM_FAST,  // Encoding-Speed vs. Qualität
]);

vio_stream_start($stream);

// Render-Loop
while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_draw($ctx, $scene);
    vio_end($ctx);

    vio_stream_push_frame($stream);
    vio_poll_events($ctx);
}

vio_stream_stop($stream);
vio_stream_destroy($stream);

// Gleichzeitig Recording + Streaming möglich
$recorder = vio_recorder_create($ctx, ['output' => 'local.mp4']);
$stream   = vio_stream_create($ctx, ['url' => 'rtmp://...']);

vio_recorder_start($recorder);
vio_stream_start($stream);

// Beide bekommen denselben Frame
vio_recorder_capture_frame($recorder);
vio_stream_push_frame($stream);
```

### Unterstützte Protokolle

| Protokoll | Use-Case |
|-----------|----------|
| RTMP | Twitch, YouTube Live, Kick |
| SRT | Low-Latency Streaming |
| WebRTC | Browser-zu-Browser, niedrige Latenz |
| HLS/DASH | Adaptive Streaming (Datei-basiert) |

---

## Plugin-System

Die Vtable-Architektur von php-vio wird nach außen geöffnet. Drittanbieter können eigene Backends, Output-Formate und Input-Devices als Plugins registrieren.

### Plugin-Typen

```c
// Drei Plugin-Kategorien mit je eigener Vtable

typedef enum {
    VIO_PLUGIN_BACKEND,   // Rendering-Backend (z.B. WebGPU, Software-Renderer)
    VIO_PLUGIN_OUTPUT,    // Output-Kanal (z.B. NDI, benutzerdefinierter Codec)
    VIO_PLUGIN_INPUT,     // Input-Device (z.B. MIDI-Controller, VR-Tracker)
} vio_plugin_type;

typedef struct {
    const char       *name;
    const char       *version;
    vio_plugin_type   type;

    int   (*init)(vio_plugin_config *cfg);
    void  (*shutdown)(void);

    union {
        vio_backend        backend;    // Rendering-Vtable
        vio_output_driver  output;     // Output-Vtable
        vio_input_driver   input;      // Input-Vtable
    };
} vio_plugin;

// Plugin registriert sich in seiner MINIT:
void vio_register_plugin(const vio_plugin *plugin);
```

### Beispiel: Custom Output-Plugin

```c
// vio_plugin_ndi.c – NDI-Streaming als Plugin

static int ndi_init(vio_plugin_config *cfg) {
    // NDI SDK initialisieren
    return NDIlib_initialize() ? VIO_OK : VIO_ERROR;
}

static int ndi_push_frame(const vio_frame *frame) {
    NDIlib_video_frame_v2_t ndi_frame = {
        .xres = frame->width,
        .yres = frame->height,
        .p_data = frame->pixels,
    };
    NDIlib_send_send_video_v2(ndi_sender, &ndi_frame);
    return VIO_OK;
}

static vio_plugin ndi_plugin = {
    .name    = "ndi",
    .version = "1.0.0",
    .type    = VIO_PLUGIN_OUTPUT,
    .output  = {
        .init        = ndi_init,
        .push_frame  = ndi_push_frame,
        .push_audio  = ndi_push_audio,
        .shutdown    = ndi_shutdown,
    },
};

// PHP Extension MINIT
PHP_MINIT_FUNCTION(vio_ndi) {
    vio_register_plugin(&ndi_plugin);
    return SUCCESS;
}
```

### Nutzung in PHP

```php
// Plugin-Backend nutzen
$ctx = vio_create('webgpu');  // Drittanbieter-Backend, wenn registriert

// Plugin-Output nutzen
$ndi = vio_output_create($ctx, 'ndi', [
    'name' => 'PHPolygon Game Output',
]);
vio_output_start($ndi);

// Im Render-Loop
vio_output_push_frame($ndi);

// Alle registrierten Plugins abfragen
$plugins = vio_plugins();
/*
[
    ['name' => 'opengl',  'type' => 'backend', 'version' => '1.0.0', 'builtin' => true],
    ['name' => 'vulkan',  'type' => 'backend', 'version' => '1.0.0', 'builtin' => true],
    ['name' => 'metal',   'type' => 'backend', 'version' => '1.0.0', 'builtin' => true],
    ['name' => 'ndi',     'type' => 'output',  'version' => '1.0.0', 'builtin' => false],
]
*/

// Plugin-Input-Device nutzen
vio_on_input($ctx, 'midi', function(array $event) {
    // ['channel' => 0, 'note' => 60, 'velocity' => 127, 'type' => 'note_on']
});
```

### Distribution

Plugins sind eigenständige PHP-Extensions und werden über PIE verteilt:

```bash
pie install php-vio              # Kern
pie install php-vio-vulkan       # Offizielles Backend
pie install php-vio-ndi          # Community-Plugin
pie install php-vio-midi-input   # Community-Plugin
```

### Stabilitätsgarantien

| API | Stabilität |
|-----|-----------|
| `vio_backend` Vtable | Stabil ab 1.0, versioniert |
| `vio_output_driver` Vtable | Stabil ab 1.0, versioniert |
| `vio_input_driver` Vtable | Stabil ab 1.0, versioniert |
| `vio_plugin_config` | Erweiterbar, neue Felder nur am Ende |

Plugin-Vtables werden versioniert. php-vio prüft beim Registrieren die Plugin-Version und lehnt inkompatible Plugins mit einer klaren Fehlermeldung ab, statt undefiniertes Verhalten zu riskieren.

---

*Keine offenen Designfragen. Dokument bereit zur Implementierung.*
