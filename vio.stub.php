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
 * Number of active touches this frame. Always 0 on platforms without a
 * touch surface (macOS, Linux, Windows desktop). Driven by platform
 * backends on iOS / iPadOS, or by vio_touch_inject() for tests.
 */
function vio_touch_count(VioContext $context): int {}

/**
 * Return the active touch at compacted index $idx (0..vio_touch_count-1),
 * or null when out of range. Active touches are returned in slot order
 * with inactive slots skipped, so the index is stable within a single
 * frame but NOT across frames - track touches by 'id' for that.
 *
 * Phase values: VIO_TOUCH_BEGAN (1), VIO_TOUCH_MOVED (2),
 * VIO_TOUCH_STATIONARY (3), VIO_TOUCH_ENDED (4), VIO_TOUCH_CANCELLED (5).
 *
 * @return array{id: int, x: float, y: float, phase: int, delta_x: float, delta_y: float}|null
 */
function vio_touch_get(VioContext $context, int $index): ?array {}

/**
 * Inject a synthetic touch event. Used by tests and headless replays;
 * production touch events come from the platform backend.
 *
 * Coordinates are in framebuffer pixels (same space as the cursor
 * callback). Phase must be VIO_TOUCH_BEGAN / MOVED / ENDED / CANCELLED.
 * Returns true on success; false when the touch array is full (BEGAN)
 * or the phase is unknown.
 */
function vio_touch_inject(VioContext $context, int $id, int $phase, float $x = 0.0, float $y = 0.0): bool {}

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
 * Number of on-screen-keyboard backspaces since the last call (read-and-clear).
 * Non-zero only on iOS; 0 on desktop (physical Backspace uses the key API).
 */
function vio_ime_backspaces(VioContext $context): int {}

/**
 * Show the on-screen keyboard (iOS). No-op on desktop. Call on text-field focus.
 */
function vio_keyboard_show(VioContext $context): void {}

/**
 * Hide the on-screen keyboard (iOS). No-op on desktop. Call on text-field blur.
 */
function vio_keyboard_hide(VioContext $context): void {}

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
 *
 * @param int $monitor Monitor index (from vio_monitors); -1 = primary monitor.
 * @param int $width   Desired fullscreen width in pixels; 0 = the monitor's
 *                     native mode. Should be a mode listed by vio_video_modes;
 *                     GLFW falls back to the closest supported mode otherwise.
 * @param int $height  Desired fullscreen height in pixels; 0 = native mode.
 * @param int $refresh Desired refresh rate in Hz; 0 = let the mode decide.
 */
function vio_set_fullscreen(VioContext $context, int $monitor = -1, int $width = 0, int $height = 0, int $refresh = 0): void {}

/**
 * Get the window size in screen coordinates.
 *
 * @return array{0: int, 1: int}
 */
function vio_window_size(VioContext $context): array {}

/**
 * Returns the platform-native window handle as an integer:
 *   - macOS:   NSWindow* — pass to Metal\CAMetalLayer::createFromWindow().
 *   - Windows: HWND.
 *   - Linux:   X11 Window XID.
 *
 * Returns 0 if no window exists or the platform is unsupported.
 */
function vio_native_window_handle(VioContext $context): int {}

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
 * Get information about the primary monitor: native video mode (physical
 * pixels), work area (excludes the taskbar), content scale and name.
 *
 * @return array{width:int, height:int, refresh_rate:int, work_x:int, work_y:int, work_width:int, work_height:int, scale_x:float, scale_y:float, name:string}
 */
function vio_monitor_info(VioContext $context): array {}

/**
 * Enumerate all connected monitors. Each entry has its index (usable with
 * vio_set_fullscreen), name, primary flag, virtual-desktop position, native
 * video mode (physical pixels), work area, refresh rate and content scale.
 *
 * @return list<array{index:int, name:string, primary:bool, x:int, y:int, width:int, height:int, refresh_rate:int, work_x:int, work_y:int, work_width:int, work_height:int, scale_x:float, scale_y:float}>
 */
function vio_monitors(VioContext $context): array {}

/**
 * Enumerate the video modes (resolutions + refresh rates) a monitor supports,
 * for offering a fullscreen resolution picker. Duplicate (width, height,
 * refresh) combinations are collapsed; modes are returned ascending. Pass a
 * mode's width/height/refresh back into vio_set_fullscreen.
 *
 * @param int $monitor Monitor index (from vio_monitors); -1 = primary monitor.
 * @return list<array{width:int, height:int, refresh_rate:int}>
 */
function vio_video_modes(VioContext $context, int $monitor = -1): array {}

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
 * The glyph atlas is rasterized at $size * $scale physical pixels, while every
 * glyph metric (vio_text positions, vio_text_measure results) is reported in
 * logical $size units. Pass $scale = devicePixelRatio (e.g. framebuffer/logical
 * magnification) so text stays crisp when a transform magnifies it. $scale < 1
 * is clamped to 1.0.
 *
 * @return VioFont|false Font object or false on failure
 */
function vio_font(VioContext $context, string $path, float $size = 24.0, float $scale = 1.0): VioFont|false {}

/**
 * Draw text using a loaded font.
 *
 * With HarfBuzz shaping enabled (VIO_HAS_SHAPING === 1), '\n' always starts a
 * new line and 'max_width' turns on word wrapping.
 *
 * @param array|null $options ['color' => int, 'z' => float, 'max_width' => float, 'line_height' => float]
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
 * Pass the same wrapping options as vio_text() ('max_width', 'line_height') to
 * measure wrapped/multi-line text. 'lines' is only meaningful with shaping.
 *
 * @param array|null $options ['max_width' => float, 'line_height' => float]
 * @return array{width: float, height: float, lines: int}|false
 */
function vio_text_measure(VioFont $font, string $text, ?array $options = null): array|false {}

/**
 * True if $font carries a real glyph for the given Unicode $codepoint.
 *
 * Reliable coverage detection for font-fallback routing. Unlike advance width
 * (a font's .notdef box can measure non-zero on some FreeType builds), this
 * reports actual glyph presence via the HarfBuzz nominal-glyph lookup, so a
 * fallback chain never lets the primary font claim an uncovered codepoint.
 */
function vio_font_has_glyph(VioFont $font, int $codepoint): bool {}

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
 * Create a 3D / volume texture from raw RGBA8 voxel data.
 *
 * Used by Fieldtracing to upload a baked Signed Distance Field volume. The data
 * is width*height*depth*4 bytes, Z-slices in ascending order. All backends
 * (OpenGL, D3D11, D3D12, Metal, Vulkan) report VIO_FEATURE_TEXTURE_3D and create
 * one; a backend that ever lacks the upload path returns false. Bind it with
 * vio_bind_texture() and sample it with a sampler3D.
 *
 * @param array $config ['data' => string, 'width' => int, 'height' => int, 'depth' => int], plus optional 'filter', 'wrap' (default CLAMP)
 * @return VioTexture|false Texture object or false on failure / unsupported backend
 */
function vio_texture_3d(VioContext $context, array $config): VioTexture|false {}

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

/* ── GPU compute primitive (M1: D3D12) ─────────────────────────────────
 *
 * Every vio_compute_* function returns false / no-ops (with an E_NOTICE) when
 * the active backend does not report VIO_FEATURE_COMPUTE, so callers fall back
 * to a CPU path silently.
 */

/**
 * Create a GPU compute pipeline from a GLSL compute shader.
 *
 * @param array $config ['source' => string]  // GLSL `#version 450` compute source
 * @return VioComputePipeline|false
 */
function vio_compute_pipeline(VioContext $context, array $config): VioComputePipeline|false {}

/**
 * Create a storage (UAV/SRV) buffer for compute.
 *
 * @param array $config ['size' => int]            // zeroed UAV output
 *                       | ['data' => string]       // SRV input (binary)
 *                       , optional ['stride' => int] // element stride (default 4)
 * @return VioBuffer|false
 */
function vio_storage_buffer(VioContext $context, array $config): VioBuffer|false {}

/**
 * Bind a storage buffer to a compute pipeline slot.
 *
 * @param int $access VIO_COMPUTE_READ (SRV t#) or VIO_COMPUTE_WRITE (UAV u#)
 */
function vio_compute_bind_buffer(VioContext $context, VioComputePipeline $pipeline, VioBuffer $buffer, int $slot, int $access): void {}

/**
 * Stage the small params constant block (b0) for the next dispatch.
 */
function vio_compute_set_uniforms(VioContext $context, VioComputePipeline $pipeline, string $data): void {}

/**
 * Dispatch the compute pipeline (group counts). Blocks until the GPU finishes.
 */
function vio_compute_dispatch(VioContext $context, VioComputePipeline $pipeline, int $gx, int $gy, int $gz): void {}

/**
 * Read back a storage buffer's bytes (GPU -> CPU). Blocks on a fence.
 *
 * @return string|false Raw bytes, or false on failure / unsupported.
 */
function vio_storage_buffer_read(VioContext $context, VioBuffer $buffer): string|false {}

/**
 * Set a uniform value on the currently bound pipeline shader.
 * Supports int, float, vec2/3/4 (flat array), mat3 (9 floats), mat4 (16 floats).
 * Silently ignores uniforms not found in the shader.
 */
function vio_set_uniform(VioContext $context, string $name, int|float|array $value): void {}

/**
 * Batch form of vio_set_uniform: apply a map of ['u_name' => value, ...] in a
 * single native call. Each value follows the same rules as vio_set_uniform
 * (int, float, or float array for vec/mat). Avoids the per-uniform PHP->C call
 * overhead on the hot draw path; the resulting cbuffer state is identical.
 */
function vio_set_uniforms(VioContext $context, array $uniforms): void {}

/**
 * Batched draw submission: record an ordered list of draws in a SINGLE PHP->C
 * crossing. Each element of $draws is an associative array:
 *
 *   [
 *     'mesh'     => VioMesh,                 // required
 *     'pipeline' => VioPipeline,             // optional; bound only on change
 *     'textures' => [ slot => VioTexture ],  // optional; GL slot => texture
 *     'uniforms' => [ 'u_name' => value ],   // optional; per-draw uniform deltas
 *   ]
 *
 * Records are applied strictly in array order, so the bound shader's sticky
 * cbuffer ends up byte-identical to issuing vio_bind_pipeline / vio_bind_texture
 * / vio_set_uniforms / vio_draw per draw. Must be called between vio_begin and
 * vio_end. Collapses the per-draw FFI crossings (and per-call argument parsing)
 * of a heavy opaque submit into one, which is the main CPU/submit bottleneck.
 */
function vio_submit_batch(VioContext $context, array $draws): void {}

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
 * Read the host's thermal pressure level.
 *
 * On macOS / iOS this maps NSProcessInfo.thermalState to the string tokens
 * "nominal", "fair", "serious", or "critical". On every other platform the
 * function returns "unknown" so callers can fall back to their own metric.
 */
function vio_thermal_state(): string {}

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
 * Query GPU and system memory information.
 *
 * Returns an associative array:
 *   'name'       => string : GPU description (e.g. "NVIDIA GeForce RTX 3060"),
 *                            '' if unknown (non-D3D12 backend, or not yet init).
 *   'vram_bytes' => int    : dedicated video memory in bytes, 0 if unknown.
 *   'ram_bytes'  => int    : total physical system RAM in bytes, 0 if unknown.
 *
 * GPU name/VRAM are only populated on the D3D12 backend after the device has
 * been created (i.e. after a window/renderer exists). On every backend the
 * 'ram_bytes' field is filled, so this call is useful everywhere.
 *
 * @return array|false false only on hard failure.
 */
function vio_gpu_info(): array|false {}

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
 * Start loading a TTF/OTF font asynchronously in a background thread.
 *
 * The CPU-heavy work — reading the file and rasterizing the multi-range glyph
 * atlas with stb_truetype — runs on a worker thread, so loading large fallback
 * fonts (e.g. CJK NotoSansSC/KR, ~13 MB / ~32k glyphs) no longer stalls the
 * render thread. The GPU upload is deferred to vio_font_load_poll(), which must
 * be called on the render thread.
 *
 * @param string $path  Path to the TTF/OTF file
 * @param float  $size  Logical pixel size the atlas represents
 * @param float  $scale devicePixelRatio - atlas rasterized at $size * $scale,
 *                      metrics reported in logical $size units (clamped to >= 1)
 * @return resource|false Async load handle or false on failure
 */
function vio_font_load_async(VioContext $context, string $path, float $size = 24.0, float $scale = 1.0): mixed {}

/**
 * Poll an async font load for completion.
 *
 * Returns null while still loading, false on failure, or a ready-to-use VioFont
 * once the worker has finished. The font's atlas is uploaded to the GPU inside
 * this call, so it must run on the render thread (a current GL/Metal/D3D/Vulkan
 * context). The returned VioFont is interchangeable with one from vio_font().
 *
 * @param resource $handle Handle from vio_font_load_async()
 * @return VioFont|null|false The font, null if pending, false on failure
 */
function vio_font_load_poll(mixed $handle): VioFont|null|false {}

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

/**
 * Diagnostics for the OpenGL backend (issue #3 part 3).
 *
 * Returns an array describing the runtime OpenGL context — version,
 * GLSL level, renderer/vendor strings, the extension list and a
 * features map matching the VIO_FEATURE_* enum. Returns false on
 * non-OpenGL backends or before vio_opengl_setup_context() has run.
 *
 * @return array{
 *   version: string,
 *   glsl: int,
 *   renderer: string,
 *   vendor: string,
 *   profile: string,
 *   extensions: string[],
 *   features: array<string, bool>,
 * }|false
 */
function vio_gl_info(VioContext $context): array|false {}

/**
 * Query whether the context's backend supports a VIO_FEATURE_* capability.
 * Returns false on backends that don't implement the feature, including
 * unrecognized constants.
 */
function vio_supports_feature(VioContext $context, int $feature): bool {}

/**
 * Convenience alias for vio_render_target() with explicit option keys
 * (issue #4). Returns an object interchangeable with the existing API.
 */
function vio_create_render_target(VioContext $context, int $width, int $height, array $options = []): VioRenderTarget|false {}

/**
 * Switch the active draw target. Pass null to restore the window default.
 */
function vio_set_render_target(VioContext $context, ?VioRenderTarget $target): void {}

/**
 * Explicit destroy — releases the GPU resources held by the render target
 * immediately, rather than waiting for GC.
 */
function vio_destroy_render_target(VioRenderTarget $target): void {}

/**
 * Push the current render target onto a stack and switch to $target.
 * Stack max depth is 8.
 */
function vio_push_render_target(VioContext $context, VioRenderTarget $target): void {}

/**
 * Pop the topmost render target off the stack and re-bind it.
 */
function vio_pop_render_target(VioContext $context): void {}
