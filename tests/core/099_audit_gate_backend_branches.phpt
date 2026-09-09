--TEST--
Audit gate: backend-specific branches in php_vio.c are frozen (may shrink, never grow)
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
if (!is_file(__DIR__ . '/../../php_vio.c')) die('skip source tree not available (installed extension)');
?>
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md Phase 0.6 / API-ROADMAP.md ground rule: new backend
 * capabilities go through vio_backend vtable slots, not through
 * `strcmp(ctx->backend->name, "…")` or `#ifdef HAVE_D3D11 / HAVE_D3D12 /
 * HAVE_VULKAN` blocks inside php_vio.c. This gate counts both kinds of branch
 * and pins them at the number measured when the gate was introduced. Moving
 * code into a backend lowers the count — then lower the ceiling here too so the
 * gain is locked in. A higher count fails the build.
 *
 * Comments are stripped first so a mention in prose does not count. */
$src = file_get_contents(__DIR__ . '/../../php_vio.c');
$code = preg_replace('#/\*.*?\*/#s', '', $src);
$code = preg_replace('#//[^\n]*#', '', $code);

$strcmp_count = preg_match_all('/\bstrcmp\(\s*[A-Za-z_][A-Za-z0-9_>.\-]*backend->name\b/', $code);
$ifdef_count  = preg_match_all('/^[ \t]*#[ \t]*(?:ifdef|if)\b[^\n]*HAVE_(?:D3D11|D3D12|VULKAN)\b/m', $code);

/* Ceilings: measured 2026-09-09 after GAP-PLAN Phase 2 (render-target creation,
 * binding, readback and cubemap upload moved into the D3D11 / D3D12 backends:
 * 77 -> 66 strcmp branches, 61 -> 47 #if blocks, php_vio.c 9931 -> ~9050
 * lines). Lower them whenever a refactor brings the count down. */
$STRCMP_CEILING = 66;
$IFDEF_CEILING  = 47;

echo "strcmp(backend->name) branches: ", ($strcmp_count <= $STRCMP_CEILING ? "OK" : "FAIL ($strcmp_count > $STRCMP_CEILING)"), "\n";
echo "#if HAVE_D3D11/D3D12/VULKAN blocks: ", ($ifdef_count <= $IFDEF_CEILING ? "OK" : "FAIL ($ifdef_count > $IFDEF_CEILING)"), "\n";
?>
--EXPECT--
strcmp(backend->name) branches: OK
#if HAVE_D3D11/D3D12/VULKAN blocks: OK
