--TEST--
Work graphs (VIO_FEATURE_WORK_GRAPHS): vio_work_graph() builds a two-node lib_6_8 graph, vio_dispatch_graph() feeds CPU records to its entry node, the entry node broadcasts one record per item to a leaf node that adds atomically into a bound storage buffer; vio_swapchain_info() reports agility_sdk
--EXTENSIONS--
vio
--FILE--
<?php
/* D3D12 only (SM 6.8, OPTIONS21.WorkGraphsTier >= 1_0); every other backend
 * reports 0 and vio_work_graph() returns false with a warning. Work graphs are
 * HLSL only (GLSL has no node shaders).
 *
 * The entry node gets record {count, base} and emits count leaf records with
 * values base .. base+count-1; the leaf node adds its value to word 0 and 1 to
 * word 1. Records {10, 1} and {5, 100}: sum 55 + 510 = 565, 15 leaf launches.
 *
 * Most Windows builds need the Agility SDK (1.613+) for work graphs:
 * VIO_AGILITY_SDK=<dir with D3D12Core.dll> passes 'agility_sdk' to the D3D12
 * context. VIO_REQUIRE_WORK_GRAPHS=d3d12 makes the backend mandatory. */
$HLSL = <<<'HLSL'
RWByteAddressBuffer g_out : register(u0);

struct EntryRecord { uint count; uint base; };
struct LeafRecord  { uint value; };

[Shader("node")]
[NodeIsProgramEntry]
[NodeLaunch("broadcasting")]
[NodeDispatchGrid(1, 1, 1)]
[NumThreads(64, 1, 1)]
void producer(DispatchNodeInputRecord<EntryRecord> input,
              [MaxRecords(64)] NodeOutput<LeafRecord> leaf,
              uint tid : SV_GroupThreadID)
{
    EntryRecord r = input.Get();
    bool emit = tid < r.count;
    ThreadNodeOutputRecords<LeafRecord> o = leaf.GetThreadNodeOutputRecords(emit ? 1 : 0);
    if (emit) o.Get().value = r.base + tid;
    o.OutputComplete();
}

[Shader("node")]
[NodeLaunch("thread")]
void leaf(ThreadNodeInputRecord<LeafRecord> input)
{
    uint prev;
    g_out.InterlockedAdd(0, input.Get().value, prev);
    g_out.InterlockedAdd(4, 1, prev);
}
HLSL;

$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$agility = getenv('VIO_AGILITY_SDK') ?: '';
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_WORK_GRAPHS') ?: ''));

/* Contract on the null backend: the flag is 0, the key exists, no graph. */
$null = vio_create('null', ['headless' => true]);
$info = vio_swapchain_info($null);
echo "null agility_sdk: ", var_export($info['agility_sdk'] ?? 'missing', true), "\n";
echo "null work graphs: ", (int)vio_supports_feature($null, VIO_FEATURE_WORK_GRAPHS), "\n";
echo "null vio_work_graph: ", var_export(@vio_work_graph($null, ['hlsl' => $HLSL, 'entry' => 'producer', 'record_size' => 8]), true), "\n";
vio_destroy($null);

function run_backend(string $name): string {
    global $HLSL, $opts, $agility, $require;
    $req = in_array($name, $require, true);
    $o = $opts;
    if ($name === 'd3d12' && $agility !== '') $o['agility_sdk'] = $agility;
    $ctx = @vio_create($name, $o);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $info = vio_swapchain_info($ctx);
    $fail = [];
    if (!is_int($info['agility_sdk'] ?? null)) $fail[] = "vio_swapchain_info()['agility_sdk'] missing";
    if ($name === 'd3d12' && $agility !== '' && ($info['agility_sdk'] ?? 0) <= 0) $fail[] = "agility_sdk given but not active";
    if ($name !== 'd3d12' && ($info['agility_sdk'] ?? 0) !== 0) $fail[] = "agility_sdk on a non-D3D12 backend";
    if (!vio_supports_feature($ctx, VIO_FEATURE_WORK_GRAPHS)) {
        vio_destroy($ctx);
        if ($fail) return "FAIL\n  " . implode("\n  ", $fail);
        return $req ? "FAIL\n  required but VIO_FEATURE_WORK_GRAPHS is 0" : "skip (no work graphs)";
    }

    /* Argument contract. */
    try { vio_work_graph($ctx, ['entry' => 'producer', 'record_size' => 8]); $fail[] = "no 'hlsl' accepted"; } catch (ValueError $e) {}
    try { vio_work_graph($ctx, ['hlsl' => $HLSL, 'entry' => 'producer', 'record_size' => 0]); $fail[] = "record_size 0 accepted"; } catch (ValueError $e) {}
    if (@vio_work_graph($ctx, ['hlsl' => $HLSL, 'entry' => 'nope', 'record_size' => 8]) !== false) $fail[] = "unknown entry node accepted";

    $g = vio_work_graph($ctx, ['hlsl' => $HLSL, 'entry' => 'producer', 'record_size' => 8]);
    if (!$g instanceof VioWorkGraph) { vio_destroy($ctx); return "FAIL\n  graph not created"; }
    $out = vio_storage_buffer($ctx, ['size' => 16, 'data' => str_repeat("\0", 16)]);
    vio_work_graph_bind_buffer($ctx, $g, $out, 0);
    try { vio_dispatch_graph($ctx, $g, pack('V3', 10, 1, 5), 2); $fail[] = "short records accepted"; } catch (ValueError $e) {}

    vio_dispatch_graph($ctx, $g, pack('V4', 10, 1, 5, 100), 2);
    $r = unpack('V2', vio_storage_buffer_read($ctx, $out));
    if ($r[1] !== 565 || $r[2] !== 15) $fail[] = "first dispatch: sum {$r[1]} launches {$r[2]} (want 565 / 15)";

    /* A second dispatch reuses the backing memory and adds on top. */
    vio_dispatch_graph($ctx, $g, pack('V2', 3, 1000), 1);
    $r = unpack('V2', vio_storage_buffer_read($ctx, $out));
    if ($r[1] !== 565 + 3003 || $r[2] !== 18) $fail[] = "second dispatch: sum {$r[1]} launches {$r[2]} (want 3568 / 18)";

    /* Inside a frame the graph runs in order with the frame's work. */
    vio_begin($ctx);
    vio_dispatch_graph($ctx, $g, pack('V2', 1, 7), 1);
    vio_end($ctx);
    $r = unpack('V2', vio_storage_buffer_read($ctx, $out));
    if ($r[1] !== 3575 || $r[2] !== 19) $fail[] = "in-frame dispatch: sum {$r[1]} launches {$r[2]} (want 3575 / 19)";

    unset($g, $out);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
null agility_sdk: 0
null work graphs: 0
null vio_work_graph: false
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE
