--TEST--
Check if VIO constants are defined
--EXTENSIONS--
vio
--FILE--
<?php
// Key constants
var_dump(defined('VIO_KEY_ESCAPE'));
var_dump(VIO_KEY_ESCAPE);

// Format constants
var_dump(defined('VIO_FLOAT3'));
var_dump(VIO_FLOAT3);

// Topology
var_dump(defined('VIO_TRIANGLES'));
var_dump(VIO_TRIANGLES);

// Blend
var_dump(defined('VIO_BLEND_ALPHA'));
var_dump(VIO_BLEND_ALPHA);

// Shader format
var_dump(defined('VIO_SHADER_SPIRV'));
var_dump(VIO_SHADER_SPIRV);

// Actions
var_dump(defined('VIO_PRESS'));
var_dump(VIO_PRESS);

// Features
var_dump(defined('VIO_FEATURE_COMPUTE'));
var_dump(VIO_FEATURE_COMPUTE);

// Mouse
var_dump(defined('VIO_MOUSE_LEFT'));
var_dump(VIO_MOUSE_LEFT);

// Backend count (at least null + opengl)
var_dump(vio_backend_count() >= 1);
?>
--EXPECT--
bool(true)
int(256)
bool(true)
int(3)
bool(true)
int(0)
bool(true)
int(1)
bool(true)
int(1)
bool(true)
int(1)
bool(true)
int(0)
bool(true)
int(0)
bool(true)
