/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: placeholder - regenerate with build/gen_stub.php */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_create, 0, 0, VioContext, MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, backend, IS_STRING, 0, "\"auto\"")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_destroy, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_should_close, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_close, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_poll_events, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_begin, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_end, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_gpu_flush, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_clear, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, r, IS_DOUBLE, 0, "0.1")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, g, IS_DOUBLE, 0, "0.1")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, b, IS_DOUBLE, 0, "0.1")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, a, IS_DOUBLE, 0, "1.0")
ZEND_END_ARG_INFO()

/* ── Input functions ──────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_key_pressed, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_vio_key_just_pressed arginfo_vio_key_pressed
#define arginfo_vio_key_released arginfo_vio_key_pressed

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_mouse_position, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

#define arginfo_vio_mouse_delta arginfo_vio_mouse_position
#define arginfo_vio_mouse_scroll arginfo_vio_mouse_position

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_mouse_button, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, button, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_on_key, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_on_resize, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_on_char, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_chars_typed, 0, 1, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_toggle_fullscreen, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_set_title, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, title, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_set_borderless, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_set_windowed, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_set_fullscreen, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_window_size, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_framebuffer_size, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_content_scale, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_pixel_ratio, 0, 1, IS_DOUBLE, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

/* ── Mesh/Draw functions ──────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_mesh, 0, 2, VioMesh, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_draw, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, mesh, VioMesh, 0)
ZEND_END_ARG_INFO()

/* ── Shader/Pipeline functions ────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_shader, 0, 2, VioShader, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_pipeline, 0, 2, VioPipeline, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_bind_pipeline, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, pipeline, VioPipeline, 0)
ZEND_END_ARG_INFO()

/* ── Texture functions ───────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_texture, 0, 2, VioTexture, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_bind_texture, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, texture, VioTexture, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, slot, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

/* ── Buffer functions ────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_uniform_buffer, 0, 2, VioBuffer, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_update_buffer, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, buffer, VioBuffer, 0)
	ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, offset, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_bind_buffer, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, buffer, VioBuffer, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, binding, IS_LONG, 0, "-1")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_set_uniform, 0, 3, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

/* ── 2D API functions ────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_rect, 0, 5, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, width, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, height, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_circle, 0, 4, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, cx, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, cy, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, radius, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_line, 0, 5, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, x1, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y1, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, x2, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y2, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_sprite, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, texture, VioTexture, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_font, 0, 2, VioFont, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, size, IS_DOUBLE, 0, "24.0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_text, 0, 5, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, font, VioFont, 0)
	ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_rounded_rect, 0, 6, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, width, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, height, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, radius, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_vio_text_measure, 0, 2, MAY_BE_ARRAY|MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, font, VioFont, 0)
	ZEND_ARG_TYPE_INFO(0, text, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_push_transform, 0, 7, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, a, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, b, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, c, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, d, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, e, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, f, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_pop_transform, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_push_scissor, 0, 5, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, w, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, h, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_pop_scissor, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_draw_2d, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

/* ── Shader reflection ──────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_vio_shader_reflect, 0, 1, MAY_BE_ARRAY|MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, shader, VioShader, 0)
ZEND_END_ARG_INFO()

/* ── Audio functions ─────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_audio_load, 0, 1, VioSound, MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_audio_play, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, sound, VioSound, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_audio_stop, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, sound, VioSound, 0)
ZEND_END_ARG_INFO()

#define arginfo_vio_audio_pause arginfo_vio_audio_stop
#define arginfo_vio_audio_resume arginfo_vio_audio_stop

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_audio_volume, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, sound, VioSound, 0)
	ZEND_ARG_TYPE_INFO(0, volume, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_audio_playing, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, sound, VioSound, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_audio_position, 0, 4, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, sound, VioSound, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, z, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_audio_listener, 0, 6, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, z, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, forward_x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, forward_y, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, forward_z, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

/* ── Input injection functions ────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_inject_key, 0, 3, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, key, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, action, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_inject_mouse_move, 0, 3, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_DOUBLE, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_inject_mouse_button, 0, 3, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, button, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, action, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* ── Headless / Screenshot functions ─────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_vio_read_pixels, 0, 1, MAY_BE_STRING|MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_save_screenshot, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_vio_compare_images, 0, 2, MAY_BE_ARRAY|MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO(0, reference, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, current, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_save_diff_image, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, diff, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* ── Video recording functions ────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_recorder, 0, 2, VioRecorder, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_recorder_capture, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, recorder, VioRecorder, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_recorder_stop, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, recorder, VioRecorder, 0)
ZEND_END_ARG_INFO()

/* ── Streaming functions ──────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_stream, 0, 2, VioStream, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_stream_push, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, stream, VioStream, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_stream_stop, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, stream, VioStream, 0)
ZEND_END_ARG_INFO()

/* ── Gamepad functions ────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_gamepads, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_gamepad_connected, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_gamepad_name, 0, 1, IS_STRING, 1)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_gamepad_buttons, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_gamepad_axes, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_vio_gamepad_triggers arginfo_vio_gamepad_axes

/* ── Backend info functions ───────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_backend_name, 0, 1, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_backend_count, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_backends, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* ── Plugin functions ────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_plugins, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_vio_plugin_info, 0, 1, MAY_BE_ARRAY|MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* ── Async texture loading ───────────────────────────────────────── */

ZEND_BEGIN_ARG_INFO_EX(arginfo_vio_texture_load_async, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_vio_texture_load_poll, 0, 0, 1)
	ZEND_ARG_INFO(0, handle)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_texture_size, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_OBJ_INFO(0, texture, VioTexture, 0)
ZEND_END_ARG_INFO()

/* ── 3D: Render targets, cubemaps, instancing, viewport ──────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_viewport, 0, 5, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, width, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, height, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_draw_3d, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_draw_instanced, 0, 4, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, mesh, VioMesh, 0)
	ZEND_ARG_TYPE_INFO(0, matrices, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, instanceCount, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_render_target, 0, 2, VioRenderTarget, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_bind_render_target, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, target, VioRenderTarget, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_unbind_render_target, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_render_target_texture, 0, 1, VioTexture, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, target, VioRenderTarget, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_vio_cubemap, 0, 2, VioCubemap, MAY_BE_FALSE)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, config, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_bind_cubemap, 0, 2, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_OBJ_INFO(0, cubemap, VioCubemap, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, slot, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_vio_set_window_size, 0, 3, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, context, VioContext, 0)
	ZEND_ARG_TYPE_INFO(0, width, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, height, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* ── Function declarations ────────────────────────────────────────── */

ZEND_FUNCTION(vio_create);
ZEND_FUNCTION(vio_destroy);
ZEND_FUNCTION(vio_should_close);
ZEND_FUNCTION(vio_close);
ZEND_FUNCTION(vio_poll_events);
ZEND_FUNCTION(vio_begin);
ZEND_FUNCTION(vio_end);
ZEND_FUNCTION(vio_gpu_flush);
ZEND_FUNCTION(vio_clear);
ZEND_FUNCTION(vio_key_pressed);
ZEND_FUNCTION(vio_key_just_pressed);
ZEND_FUNCTION(vio_key_released);
ZEND_FUNCTION(vio_mouse_position);
ZEND_FUNCTION(vio_mouse_delta);
ZEND_FUNCTION(vio_mouse_button);
ZEND_FUNCTION(vio_mouse_scroll);
ZEND_FUNCTION(vio_on_key);
ZEND_FUNCTION(vio_on_resize);
ZEND_FUNCTION(vio_on_char);
ZEND_FUNCTION(vio_chars_typed);
ZEND_FUNCTION(vio_toggle_fullscreen);
ZEND_FUNCTION(vio_set_title);
ZEND_FUNCTION(vio_set_borderless);
ZEND_FUNCTION(vio_set_windowed);
ZEND_FUNCTION(vio_set_fullscreen);
ZEND_FUNCTION(vio_window_size);
ZEND_FUNCTION(vio_framebuffer_size);
ZEND_FUNCTION(vio_content_scale);
ZEND_FUNCTION(vio_pixel_ratio);
ZEND_FUNCTION(vio_mesh);
ZEND_FUNCTION(vio_draw);
ZEND_FUNCTION(vio_rect);
ZEND_FUNCTION(vio_circle);
ZEND_FUNCTION(vio_line);
ZEND_FUNCTION(vio_sprite);
ZEND_FUNCTION(vio_font);
ZEND_FUNCTION(vio_text);
ZEND_FUNCTION(vio_draw_2d);
ZEND_FUNCTION(vio_rounded_rect);
ZEND_FUNCTION(vio_text_measure);
ZEND_FUNCTION(vio_push_transform);
ZEND_FUNCTION(vio_pop_transform);
ZEND_FUNCTION(vio_push_scissor);
ZEND_FUNCTION(vio_pop_scissor);
ZEND_FUNCTION(vio_shader);
ZEND_FUNCTION(vio_pipeline);
ZEND_FUNCTION(vio_bind_pipeline);
ZEND_FUNCTION(vio_texture);
ZEND_FUNCTION(vio_bind_texture);
ZEND_FUNCTION(vio_uniform_buffer);
ZEND_FUNCTION(vio_update_buffer);
ZEND_FUNCTION(vio_bind_buffer);
ZEND_FUNCTION(vio_set_uniform);
ZEND_FUNCTION(vio_shader_reflect);
ZEND_FUNCTION(vio_audio_load);
ZEND_FUNCTION(vio_audio_play);
ZEND_FUNCTION(vio_audio_stop);
ZEND_FUNCTION(vio_audio_pause);
ZEND_FUNCTION(vio_audio_resume);
ZEND_FUNCTION(vio_audio_volume);
ZEND_FUNCTION(vio_audio_playing);
ZEND_FUNCTION(vio_audio_position);
ZEND_FUNCTION(vio_audio_listener);
ZEND_FUNCTION(vio_recorder);
ZEND_FUNCTION(vio_recorder_capture);
ZEND_FUNCTION(vio_recorder_stop);
ZEND_FUNCTION(vio_stream);
ZEND_FUNCTION(vio_stream_push);
ZEND_FUNCTION(vio_stream_stop);
ZEND_FUNCTION(vio_inject_key);
ZEND_FUNCTION(vio_inject_mouse_move);
ZEND_FUNCTION(vio_inject_mouse_button);
ZEND_FUNCTION(vio_read_pixels);
ZEND_FUNCTION(vio_save_screenshot);
ZEND_FUNCTION(vio_compare_images);
ZEND_FUNCTION(vio_save_diff_image);
ZEND_FUNCTION(vio_gamepads);
ZEND_FUNCTION(vio_gamepad_connected);
ZEND_FUNCTION(vio_gamepad_name);
ZEND_FUNCTION(vio_gamepad_buttons);
ZEND_FUNCTION(vio_gamepad_axes);
ZEND_FUNCTION(vio_gamepad_triggers);
ZEND_FUNCTION(vio_backend_name);
ZEND_FUNCTION(vio_backend_count);
ZEND_FUNCTION(vio_backends);
ZEND_FUNCTION(vio_plugins);
ZEND_FUNCTION(vio_plugin_info);
ZEND_FUNCTION(vio_texture_load_async);
ZEND_FUNCTION(vio_texture_load_poll);
ZEND_FUNCTION(vio_texture_size);
ZEND_FUNCTION(vio_viewport);
ZEND_FUNCTION(vio_draw_3d);
ZEND_FUNCTION(vio_draw_instanced);
ZEND_FUNCTION(vio_render_target);
ZEND_FUNCTION(vio_bind_render_target);
ZEND_FUNCTION(vio_unbind_render_target);
ZEND_FUNCTION(vio_render_target_texture);
ZEND_FUNCTION(vio_cubemap);
ZEND_FUNCTION(vio_bind_cubemap);
ZEND_FUNCTION(vio_set_window_size);

static const zend_function_entry ext_functions[] = {
	ZEND_FE(vio_create, arginfo_vio_create)
	ZEND_FE(vio_destroy, arginfo_vio_destroy)
	ZEND_FE(vio_should_close, arginfo_vio_should_close)
	ZEND_FE(vio_close, arginfo_vio_close)
	ZEND_FE(vio_poll_events, arginfo_vio_poll_events)
	ZEND_FE(vio_begin, arginfo_vio_begin)
	ZEND_FE(vio_end, arginfo_vio_end)
	ZEND_FE(vio_gpu_flush, arginfo_vio_gpu_flush)
	ZEND_FE(vio_clear, arginfo_vio_clear)
	ZEND_FE(vio_key_pressed, arginfo_vio_key_pressed)
	ZEND_FE(vio_key_just_pressed, arginfo_vio_key_just_pressed)
	ZEND_FE(vio_key_released, arginfo_vio_key_released)
	ZEND_FE(vio_mouse_position, arginfo_vio_mouse_position)
	ZEND_FE(vio_mouse_delta, arginfo_vio_mouse_delta)
	ZEND_FE(vio_mouse_button, arginfo_vio_mouse_button)
	ZEND_FE(vio_mouse_scroll, arginfo_vio_mouse_scroll)
	ZEND_FE(vio_on_key, arginfo_vio_on_key)
	ZEND_FE(vio_on_resize, arginfo_vio_on_resize)
	ZEND_FE(vio_on_char, arginfo_vio_on_char)
	ZEND_FE(vio_chars_typed, arginfo_vio_chars_typed)
	ZEND_FE(vio_toggle_fullscreen, arginfo_vio_toggle_fullscreen)
	ZEND_FE(vio_set_title, arginfo_vio_set_title)
	ZEND_FE(vio_set_borderless, arginfo_vio_set_borderless)
	ZEND_FE(vio_set_windowed, arginfo_vio_set_windowed)
	ZEND_FE(vio_set_fullscreen, arginfo_vio_set_fullscreen)
	ZEND_FE(vio_window_size, arginfo_vio_window_size)
	ZEND_FE(vio_framebuffer_size, arginfo_vio_framebuffer_size)
	ZEND_FE(vio_content_scale, arginfo_vio_content_scale)
	ZEND_FE(vio_pixel_ratio, arginfo_vio_pixel_ratio)
	ZEND_FE(vio_mesh, arginfo_vio_mesh)
	ZEND_FE(vio_draw, arginfo_vio_draw)
	ZEND_FE(vio_rect, arginfo_vio_rect)
	ZEND_FE(vio_circle, arginfo_vio_circle)
	ZEND_FE(vio_line, arginfo_vio_line)
	ZEND_FE(vio_sprite, arginfo_vio_sprite)
	ZEND_FE(vio_font, arginfo_vio_font)
	ZEND_FE(vio_text, arginfo_vio_text)
	ZEND_FE(vio_draw_2d, arginfo_vio_draw_2d)
	ZEND_FE(vio_rounded_rect, arginfo_vio_rounded_rect)
	ZEND_FE(vio_text_measure, arginfo_vio_text_measure)
	ZEND_FE(vio_push_transform, arginfo_vio_push_transform)
	ZEND_FE(vio_pop_transform, arginfo_vio_pop_transform)
	ZEND_FE(vio_push_scissor, arginfo_vio_push_scissor)
	ZEND_FE(vio_pop_scissor, arginfo_vio_pop_scissor)
	ZEND_FE(vio_shader, arginfo_vio_shader)
	ZEND_FE(vio_pipeline, arginfo_vio_pipeline)
	ZEND_FE(vio_bind_pipeline, arginfo_vio_bind_pipeline)
	ZEND_FE(vio_texture, arginfo_vio_texture)
	ZEND_FE(vio_bind_texture, arginfo_vio_bind_texture)
	ZEND_FE(vio_uniform_buffer, arginfo_vio_uniform_buffer)
	ZEND_FE(vio_update_buffer, arginfo_vio_update_buffer)
	ZEND_FE(vio_bind_buffer, arginfo_vio_bind_buffer)
	ZEND_FE(vio_set_uniform, arginfo_vio_set_uniform)
	ZEND_FE(vio_shader_reflect, arginfo_vio_shader_reflect)
	ZEND_FE(vio_audio_load, arginfo_vio_audio_load)
	ZEND_FE(vio_audio_play, arginfo_vio_audio_play)
	ZEND_FE(vio_audio_stop, arginfo_vio_audio_stop)
	ZEND_FE(vio_audio_pause, arginfo_vio_audio_pause)
	ZEND_FE(vio_audio_resume, arginfo_vio_audio_resume)
	ZEND_FE(vio_audio_volume, arginfo_vio_audio_volume)
	ZEND_FE(vio_audio_playing, arginfo_vio_audio_playing)
	ZEND_FE(vio_audio_position, arginfo_vio_audio_position)
	ZEND_FE(vio_audio_listener, arginfo_vio_audio_listener)
	ZEND_FE(vio_recorder, arginfo_vio_recorder)
	ZEND_FE(vio_recorder_capture, arginfo_vio_recorder_capture)
	ZEND_FE(vio_recorder_stop, arginfo_vio_recorder_stop)
	ZEND_FE(vio_stream, arginfo_vio_stream)
	ZEND_FE(vio_stream_push, arginfo_vio_stream_push)
	ZEND_FE(vio_stream_stop, arginfo_vio_stream_stop)
	ZEND_FE(vio_inject_key, arginfo_vio_inject_key)
	ZEND_FE(vio_inject_mouse_move, arginfo_vio_inject_mouse_move)
	ZEND_FE(vio_inject_mouse_button, arginfo_vio_inject_mouse_button)
	ZEND_FE(vio_read_pixels, arginfo_vio_read_pixels)
	ZEND_FE(vio_save_screenshot, arginfo_vio_save_screenshot)
	ZEND_FE(vio_compare_images, arginfo_vio_compare_images)
	ZEND_FE(vio_save_diff_image, arginfo_vio_save_diff_image)
	ZEND_FE(vio_gamepads, arginfo_vio_gamepads)
	ZEND_FE(vio_gamepad_connected, arginfo_vio_gamepad_connected)
	ZEND_FE(vio_gamepad_name, arginfo_vio_gamepad_name)
	ZEND_FE(vio_gamepad_buttons, arginfo_vio_gamepad_buttons)
	ZEND_FE(vio_gamepad_axes, arginfo_vio_gamepad_axes)
	ZEND_FE(vio_gamepad_triggers, arginfo_vio_gamepad_triggers)
	ZEND_FE(vio_backend_name, arginfo_vio_backend_name)
	ZEND_FE(vio_backend_count, arginfo_vio_backend_count)
	ZEND_FE(vio_backends, arginfo_vio_backends)
	ZEND_FE(vio_plugins, arginfo_vio_plugins)
	ZEND_FE(vio_plugin_info, arginfo_vio_plugin_info)
	ZEND_FE(vio_texture_load_async, arginfo_vio_texture_load_async)
	ZEND_FE(vio_texture_load_poll, arginfo_vio_texture_load_poll)
	ZEND_FE(vio_texture_size, arginfo_vio_texture_size)
	ZEND_FE(vio_viewport, arginfo_vio_viewport)
	ZEND_FE(vio_draw_3d, arginfo_vio_draw_3d)
	ZEND_FE(vio_draw_instanced, arginfo_vio_draw_instanced)
	ZEND_FE(vio_render_target, arginfo_vio_render_target)
	ZEND_FE(vio_bind_render_target, arginfo_vio_bind_render_target)
	ZEND_FE(vio_unbind_render_target, arginfo_vio_unbind_render_target)
	ZEND_FE(vio_render_target_texture, arginfo_vio_render_target_texture)
	ZEND_FE(vio_cubemap, arginfo_vio_cubemap)
	ZEND_FE(vio_bind_cubemap, arginfo_vio_bind_cubemap)
	ZEND_FE(vio_set_window_size, arginfo_vio_set_window_size)
	ZEND_FE_END
};
