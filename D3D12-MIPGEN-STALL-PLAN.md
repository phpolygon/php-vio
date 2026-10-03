# D3D12: `vio_generate_mipmaps` mitten im Frame blockiert die CPU

Stand 2026-10-03, php-vio v2.29.0, PHPolygon v0.48.3, gemessen in Code Rescue (CodeCity) auf
Windows 11 / D3D12. Status: **✅ umgesetzt** (Phase 1 + 2; Phase 3 = Gegenprüfung im Spiel).

Ergebnis lokal (RTX 2080 / WARP, Repro-Skript unten): D3D12 „6 Seiten rendern + `vio_generate_mipmaps`“
10,8 → **0,09 ms**, gleichauf mit Vulkan. Test 145 prüft Rendern → Mips → Sampeln im selben Frame auf
allen Backends und die Zeit auf D3D12-Hardware (v2.29.0 fügte dort ~3 ms hinzu, jetzt < 0,1 ms).
Nebenbefunde: `GEOMETRY` = 0 kam vom Vulkan SDK 1.3.296 der Windows-CI (Release-DLLs), CI jetzt auf
1.4.341; `vio_set_uniform("u_inv[$i]", …)` für `mat4`-Arrays ist behoben (Test 146).

## Befund aus dem Spiel

`PHPOLYGON_PROFILE=1`, CodeCity (`php game.php --world=codecity`), 2 Läufe à 100–120 s:

| Sektion | ms/Frame (Mittel) | Anmerkung |
|---|---|---|
| `render3d.vio_submit` | 4,1–6,2 | gesamter 3D-Submit |
| `render3d.submit.opaque` | 1,8–3,0 | |
| `render3d.submit.envcube` | **0,4–2,1** | Environment-Cubemap (Reflection-Probe) |
| `render3d.submit.shadow` | 1,1–1,5 | davon `shadow.draw` nur 0,3–0,45, Rest ist PHP-Sammeln |

Die Engine rendert die Env-Cube (`VioRenderer3D::updateEnvironmentCubemap`, 128²) höchstens
**4× pro Sekunde** (`VioEnvironmentCubemap::MIN_UPDATE_INTERVAL = 0.25`): 6× `vio_bind_render_target($rt, $face)`
+ einige Fullscreen-Sky-Draws je Seite, danach `vio_generate_mipmaps($ctx, $rt)`. Bei ~65 fps
heißt 0,4–2,1 ms/Frame im Mittel: **jedes einzelne Update kostet ~8–30 ms**, also ein Ruckler alle
250 ms. In den Fenstern mit hohem envcube-Wert steigt p95 der Frametime auf ~38 ms.

## Isolierte Messung

Skript unten, headless, Cube-RT 128² mit Mips, 6 Seiten × 6 Alpha-geblendete Fullscreen-Draws,
60 Frames, Median der CPU-Zeit des Blocks (ohne `vio_end`):

| | D3D12 | D3D11 | Vulkan |
|---|---|---|---|
| 6 Seiten rendern, ohne Mips | 0,08 ms | 0,44 ms | 0,07 ms |
| **6 Seiten rendern + `vio_generate_mipmaps`** | **10,8 ms** | 0,43 ms | 0,09 ms |
| nur `vio_generate_mipmaps` (nichts vorher im Frame) | 0,5 ms | 0,003 ms | 0,02 ms |
| Layered: `VIO_RT_ALL_LAYERS`, 6 Instanzen je Schicht, ohne Mips | 0,03 ms | 0,12 ms | 0,03 ms |
| Layered + `vio_generate_mipmaps` | 10,3 ms | 0,12 ms | 0,05 ms |

Nur D3D12 zeigt den Effekt, und nur, wenn im selben Frame schon Arbeit aufgezeichnet ist. Im Spiel
liegt vor dem Envcube-Update der ganze Schattenpass, deshalb ist der Stall dort noch größer.

Layered Rendering (2.28) ist damit **keine** Lösung. Es spart CPU-seitig nur ~0,05 ms, ist aber
korrekt: Farb-Cube mit Mips über `VIO_RT_ALL_LAYERS` + `gl_Layer = gl_InstanceIndex` im
Vertex-Stage liefert auf D3D12/D3D11/Vulkan pro Seite dieselben Pixel wie 6 Einzel-Binds.

## Ursache

`src/backends/d3d12/vio_d3d12.c`, `d3d12_generate_mips_gpu()` (~Z. 3768–3889):

1. Mitten im Frame (`vio_d3d12.in_frame && cmd_list`) setzt es `compute_async_pending = 1` und ruft
   `d3d12_compute_wait()` auf. Das schließt die Frame-Command-List, führt sie aus, wartet auf die GPU
   und öffnet die Liste neu (**Drain 1**).
2. Danach legt es **pro Aufruf** einen neuen `ID3D12CommandAllocator` + eine neue `ID3D12GraphicsCommandList` an,
   zeichnet die Mip-Dispatches auf, führt sie aus und ruft `vio_d3d12_wait_for_gpu()` auf (**Drain 2**).
   Anschließend werden beide Objekte wieder freigegeben.

Die CPU steht also, bis die GPU alles fertig hat, was der Frame bis dahin aufgezeichnet hat. Dazu kommt
das Anlegen und Freigeben von Allocator und Liste bei jedem Update.

## Fix-Plan

### Phase 1: Mip-Dispatches in die offene Frame-Liste aufzeichnen (D3D12)

- `in_frame`: kein `d3d12_compute_wait()`, keine eigene Liste. Barriers + `Dispatch` direkt in
  `vio_d3d12.cmd_list` aufzeichnen. Vorbild ist der Async-Compute-Pfad seit 2.24.3 (`['async' => true]`).
- Deskriptoren: `mipgen_heap` ist heute ein einziger Satz (2 × `VIO_D3D12_MIPGEN_MAX_LEVELS`).
  Weil Frames in flight sind, braucht jeder Frame eigene Slots: Ring über `frame_count`, oder die Slots aus
  dem shader-sichtbaren Frame-Heap nehmen. Dann ist kein zweites `SetDescriptorHeaps` nötig.
- Falls doch ein eigener Heap gesetzt wird: danach den Frame-Heap, die Graphics-Root-Signature, die PSO
  und die Root-Tabellen neu binden. Den gecachten Bind-State invalidieren, sonst überspringt der nächste
  Draw das Rebind.
- Resource-States: der RT steht nach dem Unbind in `RENDER_TARGET` bzw. `PIXEL_SHADER_RESOURCE`
  (`d3d12_color_is_srv`). Der Steady State am Ende muss zu dem passen, was der nächste Bind/Sample-Pfad
  erwartet (heute `st`). Das ändert sich nicht.
- Außerhalb eines Frames darf der bisherige synchrone Pfad bleiben (Laden, Tests).
- Allocator + Liste nicht mehr pro Aufruf anlegen.

### Phase 2: Test

- Neuer PHPT unter `tests/render3d/`: Cube-RT mit Mips im Frame rendern, `vio_generate_mipmaps`, im
  **selben Frame** `textureLod(cube, dir, maxLevel)` sampeln und das Ergebnis per Readback prüfen.
  Pflicht auf allen Backends, damit die Reihenfolge Rendern → Mips → Sampeln ohne Drain stimmt.
- Zeitprüfung: im Frame rendern + Mips darf auf D3D12 nicht mehr als ein Vielfaches von „nur Mips" kosten.
  Als weiche Grenze, wie die übrigen Perf-Tests, wegen WARP in der CI.

### Phase 3: Gegenprüfung im Spiel

- `PHPOLYGON_PROFILE=1` in CodeCity: `render3d.submit.envcube` sollte bei < 0,1 ms/Frame landen,
  der p95-Ausreißer auf ~38 ms verschwinden.
- `VIO_TRACE_MIPGEN=1` zeigt weiter `compute` (nicht den CPU-Box-Filter).

## Repro-Skript

```php
<?php
// php envcube_bench.php [d3d12|d3d11|vulkan]
$backend = $argv[1] ?? 'd3d12';
$ctx = vio_create($backend, ['width' => 256, 'height' => 256, 'headless' => true, 'vsync' => false]);
$SIZE = 128; $LAYERS = 6; $N = 60;
$tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
$fs = "#version 450\nlayout(location=0) in vec3 vDir;\nlayout(location=0) out vec4 o;\nuniform vec4 u_tint;\n"
    . "void main(){ vec3 d = normalize(vDir); float s = 0.0; for (int i = 0; i < 24; i++) s += sin(d.x*float(i)+d.y*3.0+d.z);"
    . " o = vec4(u_tint.rgb*(0.5+0.02*s), u_tint.a); }";
$vs = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec3 vDir;\n"
    . "void main(){ vDir = vec3(aPos.xy, 1.0); gl_Position = vec4(aPos.xy, 0.0, 1.0); }";
$p = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]),
    'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_ALPHA]);
$rt = vio_render_target($ctx, ['cube' => true, 'size' => $SIZE, 'mipmaps' => true]);

$draw = function () use ($ctx, $rt, $p, $tri, $SIZE, $LAYERS) {
    for ($f = 0; $f < 6; $f++) {
        vio_bind_render_target($ctx, $rt, $f);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_viewport($ctx, 0, 0, $SIZE, $SIZE);
        for ($l = 0; $l < $LAYERS; $l++) {
            vio_bind_pipeline($ctx, $p);
            vio_set_uniform($ctx, 'u_tint', [0.2 * $l, 0.5, 1.0, 0.5]);
            vio_draw($ctx, $tri);
        }
    }
    vio_unbind_render_target($ctx);
};
$time = function (string $label, callable $fn) use ($ctx, $N) {
    $s = [];
    for ($i = 0; $i < $N + 5; $i++) {
        vio_begin($ctx); $t = hrtime(true); $fn(); $dt = (hrtime(true) - $t) / 1e6; vio_end($ctx);
        if ($i >= 5) $s[] = $dt;
    }
    sort($s);
    printf("%-28s median %6.3f ms\n", $label, $s[intdiv(count($s), 2)]);
};
$time('render', $draw);
$time('render + generate_mipmaps', function () use ($draw, $ctx, $rt) { $draw(); vio_generate_mipmaps($ctx, $rt); });
$time('generate_mipmaps only', function () use ($ctx, $rt) { vio_generate_mipmaps($ctx, $rt); });
vio_destroy($ctx);
```

## Nebenbefunde aus derselben Session

- php-vio 2.29.0 läuft im Spiel ohne Fehler (CodeCity, D3D12, ~62–70 echte fps).
- Feature-Flags auf dieser Maschine (headless): D3D12/D3D11 melden `LAYERED_RENDER`, `VERTEX_LAYER`,
  `MULTI_VIEWPORT`, `TESSELLATION`, `HLSL_STAGE_OVERRIDE` = 1, aber **`GEOMETRY` = 0**, obwohl 2.28
  GS-Stages für D3D nennt. Prüfen, ob das Flag absichtlich 0 ist oder nachgezogen werden muss.
- `vio_set_uniform($ctx, "u_inv[$i]", $mat4)` für ein `uniform mat4 u_inv[6]` im Vertex-Stage hat im Bench
  auf D3D12/D3D11/Vulkan nichts gesetzt: Matrix 0, Bild schwarz. Nicht weiter untersucht. Prüfen, ob
  Element-Uniforms für `mat4`-Arrays unterstützt werden sollen; falls nicht, dokumentieren.
- Layered-Schatten (CSM in einem Pass) bringt im Spiel wenig: die Draws des Schattenpasses kosten nur
  0,3–0,45 ms/Frame.
