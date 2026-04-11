--TEST--
OpenGL backend registered and selectable
--EXTENSIONS--
vio
--FILE--
<?php
var_dump(vio_backend_count() >= 2); // null + opengl

// Create with null backend (no window needed for test)
$ctx = vio_create('null');
var_dump(vio_backend_name($ctx) === 'null');
vio_destroy($ctx);

// vio_mesh with null backend (no GL calls)
$ctx = vio_create('null');
$mesh = vio_mesh($ctx, [
    'vertices' => [-0.5, -0.5, 0.0, 0.5, -0.5, 0.0, 0.0, 0.5, 0.0],
    'layout' => [VIO_FLOAT3],
]);
var_dump($mesh instanceof VioMesh);

// vio_clear + draw in null backend (no-ops)
vio_clear($ctx, 0.2, 0.3, 0.4, 1.0);
vio_begin($ctx);
vio_draw($ctx, $mesh);
vio_end($ctx);
echo "draw OK\n";

vio_destroy($ctx);
echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
draw OK
done
