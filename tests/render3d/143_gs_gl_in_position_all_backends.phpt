--TEST--
Geometry stage reading gl_in[].gl_Position (and gl_InvocationID without an HLSL override) on every backend with a geometry stage
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_GEOMETRY) && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a geometry stage");
?>
--FILE--
<?php
/* SPIRV-Cross's HLSL backend rejects the gl_in[].gl_Position and
 * gl_InvocationID builtins of a geometry shader ("Unsupported builtin in
 * HLSL"). vio rewrites the module before transpiling (an SV_Position input,
 * SV_GSInstanceID + [instance(N)]), so the plain GLSL forms work on D3D too.
 *   A: one point at NDC (0.25, 0.25) expands to a quad of half size 0.25 around
 *      gl_in[0].gl_Position -> the quad covers x 16..24, y 8..16 of 32x32.
 *   B: invocations = 2, each invocation offsets the quad by -0.5 * id in x and
 *      paints its own colour -> two quads side by side. */
$W = 32;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$GS_A = "#version 450\nlayout(points) in;\nlayout(triangle_strip, max_vertices = 4) out;\nuniform float u_half;\n"
      . "void main(){ vec4 c = gl_in[0].gl_Position;\n"
      . "  gl_Position = c + vec4(-u_half, -u_half, 0, 0); EmitVertex();\n"
      . "  gl_Position = c + vec4( u_half, -u_half, 0, 0); EmitVertex();\n"
      . "  gl_Position = c + vec4(-u_half,  u_half, 0, 0); EmitVertex();\n"
      . "  gl_Position = c + vec4( u_half,  u_half, 0, 0); EmitVertex(); EndPrimitive(); }";
$GS_B = "#version 450\nlayout(points, invocations = 2) in;\nlayout(triangle_strip, max_vertices = 4) out;\nlayout(location=0) out vec3 gColor;\n"
      . "void main(){ vec4 c = gl_in[0].gl_Position - vec4(0.5 * float(gl_InvocationID), 0, 0, 0);\n"
      . "  vec3 col = gl_InvocationID == 0 ? vec3(0, 1, 0) : vec3(0, 0, 1);\n"
      . "  for (int i = 0; i < 4; i++) { gColor = col;\n"
      . "    gl_Position = c + vec4((i % 2 == 0) ? -0.25 : 0.25, (i < 2) ? -0.25 : 0.25, 0, 0); EmitVertex(); }\n"
      . "  EndPrimitive(); }";
$FS_A = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0, 1, 0, 1); }";
$FS_B = "#version 450\nlayout(location=0) in vec3 gColor;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(gColor, 1); }";

function run_backend(string $name): string {
    global $W, $VS, $GS_A, $GS_B, $FS_A, $FS_B;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no geometry stage)";
    }
    $fail = [];
    $base = ['topology' => VIO_POINTS, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $point = vio_mesh($ctx, ['vertices' => [0.25, 0.25, 0], 'layout' => [VIO_FLOAT3]]);
    $frame = function ($pipe, ?callable $set = null) use ($ctx, $point): string {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        if ($set) $set();
        vio_draw($ctx, $point);
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    $shA = vio_shader($ctx, ['vertex' => $VS, 'geometry' => $GS_A, 'fragment' => $FS_A]);
    if (!$shA) $fail[] = "A: gl_in shader not created";
    else {
        $p = $frame(vio_pipeline($ctx, ['shader' => $shA] + $base), fn() => vio_set_uniform($ctx, 'u_half', 0.25));
        if (!near(px($p, 20, 12, $W), [0, 255, 0])) $fail[] = "A: inside " . json_encode(px($p, 20, 12, $W));
        if (!near(px($p, 12, 12, $W), [0, 0, 0])) $fail[] = "A: left of quad " . json_encode(px($p, 12, 12, $W));
        if (!near(px($p, 20, 20, $W), [0, 0, 0])) $fail[] = "A: below quad " . json_encode(px($p, 20, 20, $W));
    }

    if (vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY_INSTANCING)) {
        $shB = vio_shader($ctx, ['vertex' => $VS, 'geometry' => $GS_B, 'fragment' => $FS_B]);
        if (!$shB) $fail[] = "B: instanced shader not created";
        else {
            $p = $frame(vio_pipeline($ctx, ['shader' => $shB] + $base));
            if (!near(px($p, 20, 12, $W), [0, 255, 0])) $fail[] = "B: invocation 0 " . json_encode(px($p, 20, 12, $W));
            if (!near(px($p, 12, 12, $W), [0, 0, 255])) $fail[] = "B: invocation 1 " . json_encode(px($p, 12, 12, $W));
            if (!near(px($p, 4, 12, $W), [0, 0, 0])) $fail[] = "B: outside " . json_encode(px($p, 4, 12, $W));
        }
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
vulkan: %s
metal: %s
DONE
