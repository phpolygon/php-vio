# php-vio

A PHP extension that brings GPU rendering, audio, video recording, streaming, and input handling to PHP. Foundation layer for the [PHPolygon](https://github.com/phpolygon) game engine.

## Features

- **Multi-Backend GPU Rendering** — OpenGL 4.1, Vulkan (MoltenVK), Metal
- **2D Batch Renderer** — Rects, circles, lines, sprites, text (z-sorted, up to 4096 items)
- **3D Pipeline** — Meshes, shaders, pipelines, textures, uniform/storage buffers
- **Shader Toolchain** — GLSL to SPIR-V compilation, reflection, cross-compilation
- **Audio** — Playback via miniaudio (MP3, WAV, FLAC)
- **Video Recording** — H.264 encoding via FFmpeg (with VideoToolbox HW acceleration)
- **Network Streaming** — RTMP and SRT via FFmpeg
- **Input** — Keyboard, mouse, and gamepad support via GLFW
- **Headless Mode** — Offscreen rendering for tests and visual regression testing
- **Plugin System** — Extensible output/input/filter plugins
- **Cross-Platform** — macOS, Linux, Windows

## Requirements

- PHP >= 8.4
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

### Pre-built Binaries

Download platform-specific binaries from the [Releases](https://github.com/phpolygon/php-vio/releases) page. Available for:

- Linux x86_64 / ARM64
- macOS x86_64 (Intel) / ARM64 (Apple Silicon)
- Windows x64

### From Source (macOS)

```bash
brew install glfw glslang spirv-cross ffmpeg

phpize
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross \
  --with-vulkan --with-ffmpeg --with-metal
make -j$(sysctl -n hw.ncpu)
sudo make install
```

### From Source (Linux)

```bash
# Ubuntu/Debian
sudo apt install php-dev libglfw3-dev glslang-dev libvulkan-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  spirv-cross libspirv-cross-c-shared-dev

phpize
./configure --enable-vio --with-glfw --with-glslang --with-spirv-cross \
  --with-vulkan --with-ffmpeg
make -j$(nproc)
sudo make install
```

### From Source (Windows)

Requires [PHP SDK](https://wiki.php.net/internals/windows/stepbystepbuild_sdk_2) and Visual Studio Build Tools. Dependencies (GLFW, Vulkan SDK, FFmpeg, glslang, SPIRV-Cross) as pre-compiled libs.

```cmd
configure --enable-vio --with-glfw=C:\deps\glfw ^
  --with-vulkan=C:\VulkanSDK\1.3.xxx ^
  --with-glslang=C:\deps\glslang ^
  --with-spirv-cross=C:\deps\spirv-cross ^
  --with-ffmpeg=C:\deps\ffmpeg
nmake
```

All backends use conditional compilation (`#ifdef HAVE_*`), so builds without certain dependencies compile fine — those features are simply unavailable at runtime.

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

71 functions across these categories:

| Category | Functions |
|----------|-----------|
| Context | `vio_create`, `vio_begin`, `vio_end`, `vio_clear`, `vio_close`, `vio_destroy` |
| Input | `vio_key_pressed`, `vio_mouse_position`, `vio_mouse_button`, `vio_on_key` |
| 2D | `vio_rect`, `vio_circle`, `vio_line`, `vio_sprite`, `vio_text`, `vio_draw_2d` |
| 3D | `vio_mesh`, `vio_shader`, `vio_pipeline`, `vio_bind_pipeline`, `vio_draw` |
| Textures | `vio_texture`, `vio_bind_texture`, `vio_texture_load_async` |
| Buffers | `vio_uniform_buffer`, `vio_update_buffer` |
| Audio | `vio_audio_load`, `vio_audio_play`, `vio_audio_pause`, `vio_audio_stop` |
| Recording | `vio_recorder`, `vio_recorder_capture`, `vio_recorder_stop` |
| Streaming | `vio_stream`, `vio_stream_push`, `vio_stream_stop` |
| Headless | `vio_read_pixels`, `vio_save_screenshot`, `vio_compare_images` |
| Shaders | `vio_shader_reflect`, shader compilation via glslang |
| Plugins | `vio_plugins`, `vio_plugin_info` |

Full API documentation: see [vio.stub.php](vio.stub.php)

## Backends

| Backend | Status | Platforms |
|---------|--------|-----------|
| OpenGL 4.1 | Stable | macOS, Linux, Windows |
| Metal | Stable | macOS only |
| Vulkan | Experimental | macOS (MoltenVK), Linux, Windows |
| Null | Stable | All (for unit testing) |

Backend auto-selection priority: Vulkan > Metal > OpenGL > Null

## Testing

```bash
NO_INTERACTION=1 TEST_PHP_EXECUTABLE=$(which php) \
  php run-tests.php -d extension=$PWD/modules/vio.so tests/
```

38 tests included, covering headless rendering, 2D/3D pipelines, shaders, audio, input, and visual regression testing.

## License

[MIT](LICENSE)
