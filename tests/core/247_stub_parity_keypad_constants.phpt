--TEST--
Core: vio.stub.php declares exactly the registered functions (vio_gpu_flush included), keypad key constants are registered, VIO_UINT* / VIO_SHADER_HLSL stay internal
--EXTENSIONS--
vio
--FILE--
<?php
// 1. Stub <-> registered functions: every function the extension exports has a
//    stub declaration and vice versa (vio_gpu_flush was missing from the stub).
$stub = file_get_contents(__DIR__ . '/../../vio.stub.php');
preg_match_all('/^function\s+(\w+)\s*\(/m', $stub, $m);
$stubFuncs = $m[1];
$registered = get_extension_funcs('vio');
sort($stubFuncs);
sort($registered);
$missingInStub = array_values(array_diff($registered, $stubFuncs));
$missingInExt  = array_values(array_diff($stubFuncs, $registered));
echo 'stub parity ', ($missingInStub === [] && $missingInExt === [])
    ? 'ok' : json_encode(['not in stub' => $missingInStub, 'not registered' => $missingInExt]), "\n";
echo 'stub duplicates ', count($stubFuncs) === count(array_unique($stubFuncs)) ? 'none' : 'FOUND', "\n";

// 2. vio_gpu_flush: signature as declared, and a call on the null backend is a no-op.
$rf = new ReflectionFunction('vio_gpu_flush');
echo 'vio_gpu_flush(', $rf->getParameters()[0]->getType(), ' $', $rf->getParameters()[0]->getName(), '): ', $rf->getReturnType(), "\n";
$ctx = vio_create('null');
var_dump(vio_gpu_flush($ctx));

// 3. Keypad constants: GLFW values 320..336, accepted by vio_key_name.
$kp = ['KP_0' => 320, 'KP_1' => 321, 'KP_2' => 322, 'KP_3' => 323, 'KP_4' => 324,
       'KP_5' => 325, 'KP_6' => 326, 'KP_7' => 327, 'KP_8' => 328, 'KP_9' => 329,
       'KP_DECIMAL' => 330, 'KP_DIVIDE' => 331, 'KP_MULTIPLY' => 332, 'KP_SUBTRACT' => 333,
       'KP_ADD' => 334, 'KP_ENTER' => 335, 'KP_EQUAL' => 336];
$bad = [];
foreach ($kp as $name => $value) {
    $c = 'VIO_KEY_' . $name;
    if (!defined($c) || constant($c) !== $value) $bad[] = $c;
    $n = vio_key_name($value);
    if ($n !== null && !is_string($n)) $bad[] = "$c name";
}
echo 'keypad ', $bad === [] ? 'ok' : 'BAD ' . implode(',', $bad), "\n";

// 4. Internal enum values stay unregistered (vio_mesh uploads floats only;
//    vio_shader always passes SPIR-V to the backends).
foreach (['VIO_UINT1', 'VIO_UINT2', 'VIO_UINT3', 'VIO_UINT4', 'VIO_SHADER_HLSL', 'VIO_KEY_LAST'] as $c) {
    echo $c, ' ', defined($c) ? 'defined' : 'internal', "\n";
}
?>
--EXPECT--
stub parity ok
stub duplicates none
vio_gpu_flush(VioContext $context): void
NULL
keypad ok
VIO_UINT1 internal
VIO_UINT2 internal
VIO_UINT3 internal
VIO_UINT4 internal
VIO_SHADER_HLSL internal
VIO_KEY_LAST internal
