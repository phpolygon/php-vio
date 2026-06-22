<?php
/**
 * Shared --SKIPIF-- guard for tests that need a real OpenGL >= 3.3 context.
 *
 * CI runners are headless: Linux has no GPU (we run these under Xvfb +
 * Mesa/llvmpipe software GL, where a context CAN be created), but the macOS
 * runners expose no OpenGL >= 3.3 Core context at all. Probe for one here and
 * skip cleanly when it is unavailable, instead of letting vio_create() fail
 * mid-test and break the --EXPECT-- match (which counts as FAIL, not SKIP).
 */
if (!extension_loaded('vio')) {
    die('skip vio not loaded');
}

$__probe = @vio_create('opengl', ['width' => 8, 'height' => 8, 'headless' => true]);
if (!$__probe) {
    die('skip no headless OpenGL >= 3.3 context available');
}
vio_destroy($__probe);
