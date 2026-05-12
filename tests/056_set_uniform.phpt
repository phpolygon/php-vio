--TEST--
vio_set_uniform: accepts int / float / vec2-4 / mat3 / mat4 / nested mat
--EXTENSIONS--
vio
--FILE--
<?php
// Smoke-tests the type-dispatch in vio_set_uniform. The function is called
// in every per-draw uniform push (u_model, u_view, u_projection, lighting,
// material, custom shader params), so any silent type mishandling breaks
// rendering for every game built on the engine.
//
// Uses the null backend so the call goes through ZEND_FUNCTION argument
// parsing and the type-dispatch switch without needing a real GPU. The
// null backend has no bound_shader_program, so the OpenGL fast path is
// skipped — that's fine, we're verifying that the call doesn't crash or
// error for any of the supported value shapes.
$ctx = vio_create("null");
var_dump($ctx instanceof VioContext);

vio_begin($ctx);

// Scalars
vio_set_uniform($ctx, "u_int", 42);
vio_set_uniform($ctx, "u_float", 3.14);
echo "scalars OK\n";

// vec2, vec3, vec4 (flat float arrays)
vio_set_uniform($ctx, "u_vec2", [1.0, 2.0]);
vio_set_uniform($ctx, "u_vec3", [1.0, 2.0, 3.0]);
vio_set_uniform($ctx, "u_vec4", [1.0, 2.0, 3.0, 4.0]);
echo "vectors OK\n";

// Flat mat4 (16 floats)
vio_set_uniform($ctx, "u_model", [
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
]);
echo "flat mat4 OK\n";

// Nested mat4 (4 rows of 4 floats) — engine code uses Mat4::toRows() etc.
vio_set_uniform($ctx, "u_view", [
    [1.0, 0.0, 0.0, 0.0],
    [0.0, 1.0, 0.0, 0.0],
    [0.0, 0.0, 1.0, 0.0],
    [0.0, 0.0, 0.0, 1.0],
]);
echo "nested mat4 OK\n";

// Flat mat3 (9 floats) — normal matrix.
vio_set_uniform($ctx, "u_normal_matrix", [
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0,
]);
echo "flat mat3 OK\n";

// Integer in array — typical for sampler slot indices passed via array.
vio_set_uniform($ctx, "u_int_array", [0, 1, 2, 3]);
echo "int array OK\n";

// Empty array — should be silently absorbed, not crash.
vio_set_uniform($ctx, "u_empty", []);
echo "empty array OK\n";

// Mixed int/float in the same array — zval_get_double should coerce.
vio_set_uniform($ctx, "u_mixed", [1, 2.5, 3, 4.0]);
echo "mixed array OK\n";

vio_end($ctx);

// Calling outside a frame must emit a warning, not crash.
@vio_set_uniform($ctx, "u_after", 1.0);
echo "outside-frame guard OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
scalars OK
vectors OK
flat mat4 OK
nested mat4 OK
flat mat3 OK
int array OK
empty array OK
mixed array OK
outside-frame guard OK
OK
