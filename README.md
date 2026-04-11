# php-vio

A PHP extension that brings GPU rendering, audio, video recording, streaming, and input handling to PHP. Foundation layer for the [PHPolygon](https://github.com/phpolygon) game engine.

## Features

- **Multi-Backend GPU Rendering** — OpenGL 4.1, Vulkan (MoltenVK), Metal
- **2D Batch Renderer** — Rects, circles, lines, sprites, text (z-sorted, up to 4096 items)
- **3D Pipeline** — Meshes, shaders, pipelines, textures, uniform/storage buffers
- **Shader Toolchain** — GLSL → SPIR-V compilation, reflection, cross-compilation
- **Audio** — Playback via miniaudio (MP3, WAV, FLAC)
- **Video Recording** — H.264 encoding via FFmpeg (with VideoToolbox HW acceleration)
- **Network Streaming** — RTMP and SRT via FFmpeg
- **Input** — Keyboard, mouse, and gamepad support via GLFW
- **Headless Mode** — Offscreen rendering for tests and visual regression testing
- **Plugin System** — Extensible output/input/filter plugins

## Requirements

- PHP >= 8.4
- macOS (Linux support planned)
- [GLFW](https://www.glfw.org/) 3.4
- [glslang](https://github.com/KhronosGroup/glslang)
- [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)
- [FFmpeg](https://ffmpeg.org/) (optional, for recording/streaming)
- Vulkan SDK (optional, for Vulkan backend)

## Installation

### Via PIE (PHP Installer for Extensions)

```bash
pie install phpolygon/php-vio
```

### From Source

```bash
phpize
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross --with-vulkan --with-ffmpeg --with-metal
make -j$(nproc)
make install
```

Dependencies on macOS via Homebrew:

```bash
brew install glfw glslang spirv-cross ffmpeg
```

## Quick Start

```php
<?php

// Create a window with OpenGL backend
$ctx = vio_create("opengl", [
    "width"  => 800,
    "height" => 600,
    "title"  => "Hello VIO",
]);

while (!vio_should_close($ctx)) {
    vio_begin($ctx);
    vio_clear($ctx, 0.1, 0.1, 0.1);

    // 2D drawing
    vio_rect($ctx, 50, 50, 200, 100, ["color" => 0xFF0000FF]);
    vio_circle($ctx, 400, 300, 60, ["color" => 0x00FF00FF]);
    vio_draw_2d($ctx);

    vio_end($ctx);
    vio_poll_events($ctx);
}

vio_close($ctx);
vio_destroy($ctx);
```

## API Overview

| Category | Functions |
|----------|-----------|
| Context | `vio_create`, `vio_begin`, `vio_end`, `vio_clear`, `vio_close`, `vio_destroy` |
| Input | `vio_key_pressed`, `vio_mouse_position`, `vio_mouse_button`, `vio_on_key` |
| 2D | `vio_rect`, `vio_circle`, `vio_line`, `vio_sprite`, `vio_text`, `vio_draw_2d` |
| 3D | `vio_mesh`, `vio_shader`, `vio_pipeline`, `vio_bind_pipeline`, `vio_draw` |
| Textures | `vio_texture`, `vio_bind_texture`, `vio_texture_load_async` |
| Audio | `vio_audio_load`, `vio_audio_play`, `vio_audio_pause`, `vio_audio_stop` |
| Recording | `vio_recorder`, `vio_recorder_capture`, `vio_recorder_stop` |
| Streaming | `vio_stream`, `vio_stream_push`, `vio_stream_stop` |
| Headless | `vio_read_pixels`, `vio_save_screenshot`, `vio_compare_images` |

Full API documentation: see [vio.stub.php](vio.stub.php)

## Backends

| Backend | Status | Notes |
|---------|--------|-------|
| OpenGL 4.1 | ✅ Stable | Default on macOS, full feature support |
| Metal | ✅ Stable | Native macOS GPU, auto-selected when available |
| Vulkan | ⚠️ Experimental | Via MoltenVK on macOS, limited by SIP restrictions |
| Null | ✅ Stable | No-op backend for unit testing |

Backend auto-selection priority: Metal → OpenGL → Null

## Testing

```bash
NO_INTERACTION=1 TEST_PHP_EXECUTABLE=$(which php) \
  php run-tests.php -d extension=$PWD/modules/vio.so tests/
```

38 tests included, covering headless rendering, 2D/3D pipelines, shaders, audio, input, and visual regression testing.

## License

[MIT](LICENSE)
