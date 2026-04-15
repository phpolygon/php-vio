<?php

/**
 * @generate-class-entries
 * @generate-function-entries
 */

/**
 * Create a new VIO rendering context.
 *
 * @param string $backend Backend name ("auto", "opengl", "vulkan", "metal", "null")
 * @param array $options Configuration options (width, height, title, vsync, samples, debug)
 * @return VioContext|false Context object or false on failure
 */
function vio_create(string $backend = "auto", array $options = []): VioContext|false {}

/**
 * Destroy a VIO context and free all associated resources.
 */
function vio_destroy(VioContext $context): void {}

/**
 * Check if the context window should close.
 */
function vio_should_close(VioContext $context): bool {}

/**
 * Request the context window to close.
 */
function vio_close(VioContext $context): void {}

/**
 * Poll for pending window/input events.
 */
function vio_poll_events(VioContext $context): void {}

/**
 * Begin a new frame. Must be paired with vio_end().
 */
function vio_begin(VioContext $context): void {}

/**
 * End the current frame, present to screen.
 */
function vio_end(VioContext $context): void {}

/**
 * Clear the framebuffer with a color.
 */
function vio_clear(VioContext $context, float $r = 0.1, float $g = 0.1, float $b = 0.1, float $a = 1.0): void {}

/**
 * Check if a key is currently held down.
 */
function vio_key_pressed(VioContext $context, int $key): bool {}

/**
 * Check if a key was pressed this frame (not held from previous).
 */
function vio_key_just_pressed(VioContext $context, int $key): bool {}

/**
 * Check if a key was released this frame.
 */
function vio_key_released(VioContext $context, int $key): bool {}

/**
 * Get the current mouse position.
 *
 * @return array{0: float, 1: float}
 */
function vio_mouse_position(VioContext $context): array {}

/**
 * Get the mouse movement delta since last frame.
 *
 * @return array{0: float, 1: float}
 */
function vio_mouse_delta(VioContext $context): array {}

/**
 * Check if a mouse button is currently pressed.
 */
function vio_mouse_button(VioContext $context, int $button): bool {}

/**
 * Get the scroll wheel delta for this frame.
 *
 * @return array{0: float, 1: float}
 */
function vio_mouse_scroll(VioContext $context): array {}

/**
 * Set cursor mode: VIO_CURSOR_NORMAL (0), VIO_CURSOR_DISABLED (1), VIO_CURSOR_HIDDEN (2).
 * Disabled mode hides the cursor, confines it, and enables raw mouse motion for FPS controls.
 */
function vio_set_cursor_mode(VioContext $context, int $mode): void {}

/**
 * Register a callback for key events.
 *
 * @param callable(int $key, int $action, int $mods): void $callback
 */
function vio_on_key(VioContext $context, callable $callback): void {}

/**
 * Register a callback for window resize events.
 *
 * @param callable(int $width, int $height): void $callback
 */
function vio_on_resize(VioContext $context, callable $callback): void {}

/**
 * Register a callback for character input events (Unicode codepoints).
 *
 * @param callable(int $codepoint): void $callback
 */
function vio_on_char(VioContext $context, callable $callback): void {}

/**
 * Get all characters typed this frame as a UTF-8 string.
 */
function vio_chars_typed(VioContext $context): string {}

/**
 * Toggle fullscreen mode.
 */
function vio_toggle_fullscreen(VioContext $context): void {}

/**
 * Set the window title.
 */
function vio_set_title(VioContext $context, string $title): void {}

/**
 * Switch to borderless windowed mode (maximized, no decorations).
 */
function vio_set_borderless(VioContext $context): void {}

/**
 * Switch to windowed mode (restore decorations and size).
 */
function vio_set_windowed(VioContext $context): void {}

/**
 * Switch to exclusive fullscreen mode.
 */
function vio_set_fullscreen(VioContext $context): void {}

/**
 * Get the window size in screen coordinates.
 *
 * @return array{0: int, 1: int}
 */
function vio_window_size(VioContext $context): array {}

/**
 * Get the framebuffer size in pixels.
 *
 * @return array{0: int, 1: int}
 */
function vio_framebuffer_size(VioContext $context): array {}

/**
 * Get the window content scale (HiDPI).
 *
 * @return array{0: float, 1: float}
 */
function vio_content_scale(VioContext $context): array {}

/**
 * Get the pixel ratio (framebuffer width / window width).
 */
function vio_pixel_ratio(VioContext $context): float {}

/**
 * Create a mesh from vertex data.
 *
 * The 'layout' key defines the vertex attribute layout. Two formats are supported:
 *
 * **Flat format (legacy)** - array of VIO_FLOAT* constants, attributes assigned to
 * sequential locations starting at 0:
 * ```php
 * 'layout' => [VIO_FLOAT3, VIO_FLOAT4]  // location 0: vec3, location 1: vec4
 * ```
 *
 * **Dict format** - array of associative arrays with explicit location and component count.
 * Stride and offsets are computed automatically from the sequential attribute sizes:
 * ```php
 * 'layout' => [
 *     ['location' => 0, 'components' => 3, 'type' => VIO_FLOAT3],  // position
 *     ['location' => 1, 'components' => 3, 'type' => VIO_FLOAT3],  // normal
 *     ['location' => 2, 'components' => 2, 'type' => VIO_FLOAT2],  // uv
 * ]
 * ```
 *
 * Each dict entry accepts:
 * - 'location' (int, 0-15) - vertex attribute location, defaults to sequential index
 * - 'components' (int, 1-4) - number of float components per vertex
 * - 'type' (int, optional) - VIO_FLOAT1..VIO_FLOAT4, used as fallback if 'components' is omitted
 *
 * If no layout is provided, defaults to position-only (vec3, location 0).
 *
 * @param array $config Mesh configuration:
 *   - 'vertices' (float[]) - flat interleaved vertex data (required)
 *   - 'layout' (array) - vertex attribute layout (optional, see above)
 *   - 'indices' (int[]) - index buffer (optional)
 * @return VioMesh|false Mesh object or false on failure
 */
function vio_mesh(VioContext $context, array $config): VioMesh|false {}

/**
 * Draw a mesh in the current frame.
 */
function vio_draw(VioContext $context, VioMesh $mesh): void {}

/**
 * Draw a filled or outlined rectangle.
 *
 * @param array|null $options ['fill' => int, 'color' => int, 'outline' => bool, 'z' => float]
 */
function vio_rect(VioContext $context, float $x, float $y, float $width, float $height, ?array $options = null): void {}

/**
 * Draw a filled or outlined circle.
 *
 * @param array|null $options ['fill' => int, 'color' => int, 'outline' => bool, 'segments' => int, 'z' => float]
 */
function vio_circle(VioContext $context, float $cx, float $cy, float $radius, ?array $options = null): void {}

/**
 * Draw a line between two points.
 *
 * @param array|null $options ['color' => int, 'z' => float]
 */
function vio_line(VioContext $context, float $x1, float $y1, float $x2, float $y2, ?array $options = null): void {}

/**
 * Draw a textured sprite.
 *
 * @param array|null $options ['x' => float, 'y' => float, 'width' => float, 'height' => float, 'scale_x' => float, 'scale_y' => float, 'color' => int, 'z' => float]
 */
function vio_sprite(VioContext $context, VioTexture $texture, ?array $options = null): void {}

/**
 * Load a TTF font for text rendering.
 *
 * @return VioFont|false Font object or false on failure
 */
function vio_font(VioContext $context, string $path, float $size = 24.0): VioFont|false {}

/**
 * Draw text using a loaded font.
 *
 * @param array|null $options ['color' => int, 'z' => float]
 */
function vio_text(VioContext $context, VioFont $font, string $text, float $x, float $y, ?array $options = null): void {}

/**
 * Draw a filled or outlined rounded rectangle.
 *
 * @param array|null $options ['fill' => int, 'color' => int, 'outline' => bool, 'z' => float]
 */
function vio_rounded_rect(VioContext $context, float $x, float $y, float $width, float $height, float $radius, ?array $options = null): void {}

/**
 * Measure the dimensions of a text string without rendering it.
 *
 * @return array{width: float, height: float}|false
 */
function vio_text_measure(VioFont $font, string $text): array|false {}

/**
 * Push a 2D affine transform matrix onto the stack.
 * Matrix layout: | a b e |  (a,b,c,d = 2x2 rotation/scale, e,f = translation)
 *                | c d f |
 *                | 0 0 1 |
 */
function vio_push_transform(VioContext $context, float $a, float $b, float $c, float $d, float $e, float $f): void {}

/**
 * Pop the top transform matrix from the stack.
 */
function vio_pop_transform(VioContext $context): void {}

/**
 * Push a scissor rectangle onto the stack. Drawing is clipped to this region.
 * Nested scissor rects are intersected with the parent.
 */
function vio_push_scissor(VioContext $context, float $x, float $y, float $w, float $h): void {}

/**
 * Pop the top scissor rectangle from the stack.
 */
function vio_pop_scissor(VioContext $context): void {}

/**
 * Flush all batched 2D draw calls, sorted by z-order.
 */
function vio_draw_2d(VioContext $context): void {}

/**
 * Compile a shader program from vertex and fragment source.
 * Accepts GLSL (compiled to SPIR-V via glslang) or raw SPIR-V binary.
 * Format is auto-detected from SPIR-V magic number if not specified.
 *
 * @param array $config ['vertex' => string, 'fragment' => string, 'format' => int (VIO_SHADER_AUTO|VIO_SHADER_GLSL|VIO_SHADER_SPIRV)]
 * @return VioShader|false Shader object or false on failure
 */
function vio_shader(VioContext $context, array $config): VioShader|false {}

/**
 * Reflect shader resources from SPIR-V binary stored in a VioShader.
 * Returns vertex and fragment stage inputs, UBOs, textures, and uniforms.
 *
 * @return array|false Array with 'vertex' and 'fragment' keys, each containing 'inputs', 'ubos', 'textures', 'uniforms'
 */
function vio_shader_reflect(VioShader $shader): array|false {}

/**
 * Create a rendering pipeline with shader and state configuration.
 *
 * @param array $config ['shader' => VioShader, 'topology' => int, 'cull_mode' => int, 'depth_test' => bool, 'blend' => int]
 * @return VioPipeline|false Pipeline object or false on failure
 */
function vio_pipeline(VioContext $context, array $config): VioPipeline|false {}

/**
 * Bind a pipeline for subsequent draw calls.
 */
function vio_bind_pipeline(VioContext $context, VioPipeline $pipeline): void {}

/**
 * Load a texture from file or raw pixel data.
 *
 * @param array $config ['file' => string] or ['data' => string, 'width' => int, 'height' => int], plus optional 'filter', 'wrap', 'mipmaps'
 * @return VioTexture|false Texture object or false on failure
 */
function vio_texture(VioContext $context, array $config): VioTexture|false {}

/**
 * Bind a texture to a texture slot.
 */
function vio_bind_texture(VioContext $context, VioTexture $texture, int $slot = 0): void {}

/**
 * Create a uniform buffer.
 *
 * @param array $config ['size' => int, 'binding' => int, 'data' => string]
 * @return VioBuffer|false Buffer object or false on failure
 */
function vio_uniform_buffer(VioContext $context, array $config): VioBuffer|false {}

/**
 * Update buffer data.
 */
function vio_update_buffer(VioBuffer $buffer, string $data, int $offset = 0): void {}

/**
 * Bind a buffer to a binding point.
 */
function vio_bind_buffer(VioContext $context, VioBuffer $buffer, int $binding = -1): void {}

/**
 * Set a uniform value on the currently bound pipeline shader.
 * Supports int, float, vec2/3/4 (flat array), mat3 (9 floats), mat4 (16 floats).
 * Silently ignores uniforms not found in the shader.
 */
function vio_set_uniform(VioContext $context, string $name, int|float|array $value): void {}

/**
 * Get the name of the backend in use.
 */
function vio_backend_name(VioContext $context): string {}

/**
 * Get the number of registered backends.
 */
function vio_backend_count(): int {}

/**
 * Get the names of all registered backends.
 *
 * @return string[]
 */
function vio_backends(): array {}

/**
 * Create a video recorder for capturing frames to a video file.
 *
 * @param array $config ['path' => string, 'fps' => int (default 30), 'codec' => string (optional)]
 * @return VioRecorder|false Recorder object or false on failure
 */
function vio_recorder(VioContext $context, array $config): VioRecorder|false {}

/**
 * Capture the current frame from the context into the recorder.
 */
function vio_recorder_capture(VioRecorder $recorder, VioContext $context): bool {}

/**
 * Stop recording and finalize the video file.
 */
function vio_recorder_stop(VioRecorder $recorder): void {}

/**
 * Create a live stream to an RTMP or SRT endpoint.
 *
 * @param array $config ['url' => string, 'fps' => int, 'bitrate' => int, 'codec' => string, 'format' => string]
 * @return VioStream|false Stream object or false on failure
 */
function vio_stream(VioContext $context, array $config): VioStream|false {}

/**
 * Push the current frame to the stream.
 */
function vio_stream_push(VioStream $stream, VioContext $context): bool {}

/**
 * Stop streaming and close the connection.
 */
function vio_stream_stop(VioStream $stream): void {}

/**
 * Inject a simulated key event into the context input state.
 */
function vio_inject_key(VioContext $context, int $key, int $action): void {}

/**
 * Inject a simulated mouse move event.
 */
function vio_inject_mouse_move(VioContext $context, float $x, float $y): void {}

/**
 * Inject a simulated mouse button event.
 */
function vio_inject_mouse_button(VioContext $context, int $button, int $action): void {}

/**
 * Read the framebuffer as raw RGBA pixel data.
 *
 * @return string|false RGBA pixel data (width * height * 4 bytes) or false on failure
 */
function vio_read_pixels(VioContext $context): string|false {}

/**
 * Save the current framebuffer as a PNG screenshot.
 */
function vio_save_screenshot(VioContext $context, string $path): bool {}

/**
 * Compare two images pixel-by-pixel for visual regression testing.
 *
 * @param array|null $options ['threshold' => float] (0.0 = exact match, 1.0 = all different)
 * @return array|false Array with 'passed', 'diff_ratio', 'diff_pixels', 'width', 'height', 'diff_data'
 */
function vio_compare_images(string $reference, string $current, ?array $options = null): array|false {}

/**
 * Save a diff image from vio_compare_images result as PNG.
 */
function vio_save_diff_image(array $diff, string $path): bool {}

/**
 * Get IDs of all connected gamepads/joysticks.
 *
 * @return int[]
 */
function vio_gamepads(): array {}

/**
 * Check if a gamepad is connected.
 */
function vio_gamepad_connected(int $id): bool {}

/**
 * Get the name of a connected gamepad.
 */
function vio_gamepad_name(int $id): ?string {}

/**
 * Get button states of a gamepad (indexed by VIO_GAMEPAD_* constants).
 *
 * @return bool[]
 */
function vio_gamepad_buttons(int $id): array {}

/**
 * Get axis values of a gamepad (indexed by VIO_GAMEPAD_AXIS_* constants).
 * Values range from -1.0 to 1.0 (triggers: -1.0 = released, 1.0 = fully pressed).
 *
 * @return float[]
 */
function vio_gamepad_axes(int $id): array {}

/**
 * Get trigger values of a gamepad.
 *
 * @return array{left: float, right: float}
 */
function vio_gamepad_triggers(int $id): array {}

/**
 * Load an audio file (WAV, MP3, FLAC, OGG).
 * Lazily initializes the audio engine on first call.
 *
 * @return VioSound|false Sound object or false on failure
 */
function vio_audio_load(string $path): VioSound|false {}

/**
 * Play a loaded sound.
 *
 * @param array|null $options ['volume' => float, 'loop' => bool, 'pan' => float, 'pitch' => float]
 */
function vio_audio_play(VioSound $sound, ?array $options = null): void {}

/**
 * Stop a sound and rewind to the beginning.
 */
function vio_audio_stop(VioSound $sound): void {}

/**
 * Pause a sound without rewinding.
 */
function vio_audio_pause(VioSound $sound): void {}

/**
 * Resume a paused sound.
 */
function vio_audio_resume(VioSound $sound): void {}

/**
 * Set the volume of a sound (0.0 = silent, 1.0 = full).
 */
function vio_audio_volume(VioSound $sound, float $volume): void {}

/**
 * Check if a sound is currently playing.
 */
function vio_audio_playing(VioSound $sound): bool {}

/**
 * Set the 3D position of a sound source.
 */
function vio_audio_position(VioSound $sound, float $x, float $y, float $z): void {}

/**
 * Set the audio listener position and facing direction.
 */
function vio_audio_listener(float $x, float $y, float $z, float $fx, float $fy, float $fz): void {}

/**
 * List all registered plugin names.
 *
 * @return string[] Array of registered plugin names
 */
function vio_plugins(): array {}

/**
 * Get detailed information about a registered plugin.
 *
 * @param string $name Plugin name
 * @return array|false Plugin info array or false if not found
 */
function vio_plugin_info(string $name): array|false {}

/**
 * Start loading a texture file asynchronously in a background thread.
 *
 * @param string $path Path to the image file
 * @return resource|false Async load handle or false on failure
 */
function vio_texture_load_async(string $path): mixed {}

/**
 * Poll an async texture load for completion.
 *
 * Returns null if still loading, false on failure, or an array with
 * 'width', 'height', and 'data' keys on success.
 *
 * @param resource $handle Handle from vio_texture_load_async()
 * @return array|null|false Load result, null if pending, false on failure
 */
function vio_texture_load_poll(mixed $handle): array|null|false {}

/**
 * Get the dimensions of a texture.
 * @return array{0: int, 1: int}
 */
function vio_texture_size(VioTexture $texture): array {}

/* ── 3D: Render targets, cubemaps, instancing, viewport ──────────── */

/**
 * Set the GL viewport rectangle.
 */
function vio_viewport(VioContext $context, int $x, int $y, int $width, int $height): void {}

/**
 * Flush/finalize 3D draw calls (parallel to vio_draw_2d for 2D).
 */
function vio_draw_3d(VioContext $context): void {}

/**
 * Draw a mesh multiple times using GPU instancing.
 *
 * @param array|string $matrices Flat array of 4x4 model matrices (16 floats per instance),
 *                               or packed binary string (pack('f*', ...)) for zero-copy fast path
 * @param int $instanceCount Number of instances to draw
 */
function vio_draw_instanced(VioContext $context, VioMesh $mesh, array|string $matrices, int $instanceCount): void {}

/**
 * Create an offscreen render target (FBO).
 *
 * @param array $config ['width' => int, 'height' => int, 'depth_only' => bool]
 * @return VioRenderTarget|false Render target or false on failure
 */
function vio_render_target(VioContext $context, array $config): VioRenderTarget|false {}

/**
 * Bind a render target for subsequent draw calls (redirects rendering to FBO).
 */
function vio_bind_render_target(VioContext $context, VioRenderTarget $target): void {}

/**
 * Unbind the current render target, restoring the default framebuffer.
 */
function vio_unbind_render_target(VioContext $context): void {}

/**
 * Get the depth or color texture from a render target for sampling.
 *
 * Returns the depth texture for depth-only targets, color texture otherwise.
 *
 * @return VioTexture|false Texture object or false on failure
 */
function vio_render_target_texture(VioRenderTarget $target): VioTexture|false {}

/**
 * Create a cubemap texture from 6 face images or raw pixel data.
 *
 * File-based: $config = ['faces' => string[6]] (paths in +X,-X,+Y,-Y,+Z,-Z order)
 * Procedural: $config = ['pixels' => array[6], 'width' => int, 'height' => int] (RGBA bytes per face)
 *
 * @param array $config Cubemap configuration
 * @return VioCubemap|false Cubemap object or false on failure
 */
function vio_cubemap(VioContext $context, array $config): VioCubemap|false {}

/**
 * Bind a cubemap to a texture slot for 3D rendering.
 */
function vio_bind_cubemap(VioContext $context, VioCubemap $cubemap, int $slot = 0): void {}

/**
 * Set the window size in screen coordinates.
 */
function vio_set_window_size(VioContext $context, int $width, int $height): void {}
