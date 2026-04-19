# [1.9.0](https://github.com/phpolygon/php-vio/compare/v1.8.15...v1.9.0) (2026-04-19)


### Bug Fixes

* **d3d:** read_pixels survives Present + D3D11 non-instanced draws render ([989f979](https://github.com/phpolygon/php-vio/commit/989f979f94ae2a0899fa8c1634b9143bf38fa302))


### Features

* **d3d:** complete D3D11/D3D12 backend integration ([16ae78a](https://github.com/phpolygon/php-vio/commit/16ae78a94e45aec7e79469ea0f74d1b8f86cd854)), closes [#include](https://github.com/phpolygon/php-vio/issues/include)

## [1.8.15](https://github.com/phpolygon/php-vio/compare/v1.8.14...v1.8.15) (2026-04-18)


### Bug Fixes

* **ci:** use v-prefixed ref when triggering build workflow ([4d9343e](https://github.com/phpolygon/php-vio/commit/4d9343ed8bccc34f605eb8c8f7cac786e3f34588))

## [1.8.14](https://github.com/phpolygon/php-vio/compare/v1.8.13...v1.8.14) (2026-04-18)


### Bug Fixes

* **vulkan:** gate portability enumeration behind __APPLE__ ([219d5df](https://github.com/phpolygon/php-vio/commit/219d5df3572bac0ae58a28fe31cf5df9255e999a))

## [1.8.13](https://github.com/phpolygon/php-vio/compare/v1.8.12...1.8.13) (2026-04-18)


### Bug Fixes

* **windows:** D3D/Vulkan window visibility + GLFW detection ([4d7c53e](https://github.com/phpolygon/php-vio/commit/4d7c53e0059da6f3d3023eecc793917545502e49))

## [1.8.10](https://github.com/phpolygon/php-vio/compare/1.8.9...1.8.10) (2026-04-16)


### Bug Fixes

* include config.h unconditionally in VMA wrapper ([064a108](https://github.com/phpolygon/php-vio/commit/064a108a73da7dcf886a49c5c23a661bd26030f6))

## [1.8.9](https://github.com/phpolygon/php-vio/compare/1.8.8...1.8.9) (2026-04-16)


### Bug Fixes

* include php.h before HAVE_METAL check in Metal backend ([da3b794](https://github.com/phpolygon/php-vio/commit/da3b7946b7cba50dc20778062bbfa9abd364de8a))

## [1.8.8](https://github.com/phpolygon/php-vio/compare/1.8.7...1.8.8) (2026-04-16)


### Bug Fixes

* revert PHP_GLOBAL_OBJS (doesn't work for .lo targets), keep MSL lib fix ([5c2efde](https://github.com/phpolygon/php-vio/commit/5c2efdebb608d07507440b70557c18685ca31279))

## [1.8.7](https://github.com/phpolygon/php-vio/compare/1.8.6...1.8.7) (2026-04-16)


### Bug Fixes

* add missing spirv-cross-msl.lib to Windows config.w32 ([d25a03f](https://github.com/phpolygon/php-vio/commit/d25a03f45afdd51259a9b293d6af197bd2394a9f))

## [1.8.6](https://github.com/phpolygon/php-vio/compare/1.8.5...1.8.6) (2026-04-16)


### Bug Fixes

* include Metal object in static builds via PHP_GLOBAL_OBJS ([67c5407](https://github.com/phpolygon/php-vio/commit/67c5407397deee464b8e6763825bf1a52d7fa2c9))

## [1.8.5](https://github.com/phpolygon/php-vio/compare/1.8.4...1.8.5) (2026-04-16)


### Bug Fixes

* use correct PHP CFLAGS in Metal Makefile.frag rule ([3739e47](https://github.com/phpolygon/php-vio/commit/3739e47131b8ba1ffbcd97cad3b49ac2d788d5a5))

## [1.8.4](https://github.com/phpolygon/php-vio/compare/1.8.3...1.8.4) (2026-04-16)


### Bug Fixes

* add vio_spirv_get_uniform_offsets stub when SPIRV-Cross unavailable ([ef8254f](https://github.com/phpolygon/php-vio/commit/ef8254f3981ba3b16e0f1384aa91d4fca401069b)), closes [#else](https://github.com/phpolygon/php-vio/issues/else)

## [1.8.3](https://github.com/phpolygon/php-vio/compare/1.8.2...1.8.3) (2026-04-16)


### Bug Fixes

* add missing VIO_2D_MAX_VERTICES constant for D3D vertex buffers ([76c7e05](https://github.com/phpolygon/php-vio/commit/76c7e05356f66ae1bb68d2b23adffadbc30439aa))

## [1.8.2](https://github.com/phpolygon/php-vio/compare/1.8.1...1.8.2) (2026-04-16)


### Bug Fixes

* regenerate GLAD with core GL 4.1 only (no vendor extensions) ([584a36e](https://github.com/phpolygon/php-vio/commit/584a36eb4031b8c6a2dcaefc1547840333417bfe))

## [1.8.1](https://github.com/phpolygon/php-vio/compare/1.8.0...1.8.1) (2026-04-16)


### Bug Fixes

* remove Windows build artifacts from repository ([8c04af2](https://github.com/phpolygon/php-vio/commit/8c04af21112190b2ff73d73b0c1d7c2d20596f3a))

# [1.7.0](https://github.com/phpolygon/php-vio/compare/1.6.0...1.7.0) (2026-04-16)


### Features

* D3D11 correctness — depth convention fixup, shadow sampling, pipeline enhancements ([de5b26c](https://github.com/phpolygon/php-vio/commit/de5b26c7b0e1d314656eaf2c15e13ebe41e9e1d3))
* D3D11 cubemap support, read_pixels, and SRV unbind fix ([7eafda4](https://github.com/phpolygon/php-vio/commit/7eafda400c38b5d1ca271b55f1ed9e8680e383fd))
* D3D11 fragment cbuffers, struct array uniforms, instancing, shadow maps ([96b3659](https://github.com/phpolygon/php-vio/commit/96b36597f242ffeb27cafab63650ad0dc9e323a1))
* D3D11/D3D12 2D rendering + font atlas support ([e872cd0](https://github.com/phpolygon/php-vio/commit/e872cd0a02ab684cd0de929dacf3bb99de2ffc95))
* D3D11/D3D12 rendering pipeline — vtable wiring, shader transpilation, render targets ([e243e6f](https://github.com/phpolygon/php-vio/commit/e243e6f7dee2d47a8f11a8cf88d9fe708fe2ba82))
* D3D12 backend — full feature parity with D3D11 ([686dcb8](https://github.com/phpolygon/php-vio/commit/686dcb81409bf687dbd0d7c871a59608f0595a75))
* D3D12 dynamic cbuffer heap with auto-growth ([2b6d8a0](https://github.com/phpolygon/php-vio/commit/2b6d8a03041b21a76a2f132dc28d6c18b92aae45))
* HDR render target support + color texture SRV infrastructure ([6a16613](https://github.com/phpolygon/php-vio/commit/6a166137177232b8fa3710386f8fb4dab0c9b5e8))
* uniform cbuffer pipeline for D3D — SPIRV reflection + constant buffer upload ([1f43f98](https://github.com/phpolygon/php-vio/commit/1f43f987e2bc7a5ee97390e77c283f6f4c478038))
* vio_set_cursor_mode + mat3 cbuffer padding fix ([267d586](https://github.com/phpolygon/php-vio/commit/267d586a8acd5a4006a1a30702aa873b6da9b6c9))


### Performance Improvements

* vio_draw_instanced accepts packed binary string for zero-copy instancing ([f106f8c](https://github.com/phpolygon/php-vio/commit/f106f8c3e2ab74194ab1d2a9c38ac2a4b646badd))

# [1.6.0](https://github.com/phpolygon/php-vio/compare/1.5.3...1.6.0) (2026-04-15)


### Features

* add vio_gpu_flush() and fix Metal drawable management ([d7e1e0c](https://github.com/phpolygon/php-vio/commit/d7e1e0c00d9f8d90ad6f1c89df017ee36b38ddb8))

## [1.5.3](https://github.com/phpolygon/php-vio/compare/1.5.2...1.5.3) (2026-04-14)


### Bug Fixes

* Metal flickering caused by AppKit layer redraw policy ([a5631e4](https://github.com/phpolygon/php-vio/commit/a5631e4c9b21665b2c2b23611ff04bec7b802002))

## [1.5.2](https://github.com/phpolygon/php-vio/compare/1.5.1...1.5.2) (2026-04-14)


### Bug Fixes

* Metal vsync-off uses offscreen rendering to avoid nextDrawable blocking ([a2be14b](https://github.com/phpolygon/php-vio/commit/a2be14b8b6690b51f41a595c1e56e2778dae55b7))

## [1.5.1](https://github.com/phpolygon/php-vio/compare/1.5.0...1.5.1) (2026-04-14)


### Bug Fixes

* use explicit depth-disabled state in Metal 2D flush ([a57d9da](https://github.com/phpolygon/php-vio/commit/a57d9da9c30aad2c0c9bcc93fec74a729846add5))

# [1.5.0](https://github.com/phpolygon/php-vio/compare/1.4.0...1.5.0) (2026-04-14)


### Bug Fixes

* guard GL texture cleanup and Metal vsync/readback ([1b4261b](https://github.com/phpolygon/php-vio/commit/1b4261bfc915e956353f1397dd9d39b7e1043a8d))


### Features

* implement Metal 2D rendering pipeline ([f265e3f](https://github.com/phpolygon/php-vio/commit/f265e3fa6c0a5168e476aeec69d9e863d8545048))

# [1.4.0](https://github.com/phpolygon/php-vio/compare/1.3.0...1.4.0) (2026-04-14)


### Bug Fixes

* platform-specific backend auto-selection (Metal on macOS, Vulkan on Linux) ([c98400b](https://github.com/phpolygon/php-vio/commit/c98400ba4575faf3f9848cf211c497c12f042d3e))


### Features

* dynamic 2D batch buffer and space glyph fix ([abfd09b](https://github.com/phpolygon/php-vio/commit/abfd09b54f2a4e5982a36703b20b76ce72375e22))

# [1.3.0](https://github.com/phpolygon/php-vio/compare/1.2.5...1.3.0) (2026-04-13)


### Features

* multi-range Unicode font atlas with hashmap glyph lookup ([d85b4c6](https://github.com/phpolygon/php-vio/commit/d85b4c673e93ceacb096c32b949b6aadcd098883))

## [1.2.5](https://github.com/phpolygon/php-vio/compare/1.2.4...1.2.5) (2026-04-13)


### Bug Fixes

* name binary vio.so inside release zips for PIE compatibility ([d725c7d](https://github.com/phpolygon/php-vio/commit/d725c7d922d25aa5c1d231ff5afd89bc200278e1))

## [1.2.1](https://github.com/phpolygon/php-vio/compare/1.2.0...1.2.1) (2026-04-13)


### Bug Fixes

* extend font atlas to Latin-1 and add UTF-8 text decoding ([665885e](https://github.com/phpolygon/php-vio/commit/665885ee9e80ec053f64e036e058e2df87ad8e96))

# [1.2.0](https://github.com/phpolygon/php-vio/compare/1.1.4...1.2.0) (2026-04-12)


### Bug Fixes

* add COBJMACROS/INITGUID for Windows C compilation and link dxguid.lib ([478008c](https://github.com/phpolygon/php-vio/commit/478008cdba8572c9f0fdc682e74b70685043e16e))
* add forward declaration for d3d12_shutdown (fixes C2371 on MSVC) ([abdb4e8](https://github.com/phpolygon/php-vio/commit/abdb4e857b253c5c42e670b37fde4cab347700b4))
* address code review findings for D3D11/D3D12 backends ([ee15dae](https://github.com/phpolygon/php-vio/commit/ee15dae3737b92d2cc691acb1254708e4ad4b1b7))


### Features

* add DirectX 11 and DirectX 12 render backends ([e9341f6](https://github.com/phpolygon/php-vio/commit/e9341f6118c1e1b43dc34934e6c6e03c73e41988))
* DirectX 11 and DirectX 12 render backends ([#1](https://github.com/phpolygon/php-vio/issues/1)) ([90927ee](https://github.com/phpolygon/php-vio/commit/90927ee03cd9f8007cf2c4b7e18238f98d68c7b6))

## [1.1.4](https://github.com/phpolygon/php-vio/compare/1.1.3...1.1.4) (2026-04-12)


### Bug Fixes

* add bare DLL release assets for Windows and simplify Windows install docs ([9775de3](https://github.com/phpolygon/php-vio/commit/9775de314a6d7e71bc72708909be59e9bfa47a47))

## [1.1.3](https://github.com/phpolygon/php-vio/compare/1.1.2...1.1.3) (2026-04-11)


### Bug Fixes

* rename binaries inside ZIP to match asset name for PIE ([7fb6f7e](https://github.com/phpolygon/php-vio/commit/7fb6f7e16150e1c236fc1b77d3272f67926c2fd7))

## [1.1.2](https://github.com/phpolygon/php-vio/compare/1.1.1...1.1.2) (2026-04-11)


### Bug Fixes

* add TSRMLS cache for thread-safe (ZTS) PHP builds on Windows ([93f3831](https://github.com/phpolygon/php-vio/commit/93f3831da3538f7e9f10b047cd4cd012b11b2ae5))

## [1.1.1](https://github.com/phpolygon/php-vio/compare/1.1.0...1.1.1) (2026-04-11)


### Bug Fixes

* use PIE-compatible asset naming for pre-built binaries ([522f9ed](https://github.com/phpolygon/php-vio/commit/522f9ed91787097e020e10b415a93f298902fe91))

# [1.1.0](https://github.com/phpolygon/php-vio/compare/1.0.6...1.1.0) (2026-04-11)


### Features

* build binaries for PHP 8.4 and 8.5 ([06d8a5f](https://github.com/phpolygon/php-vio/commit/06d8a5ffceeb2909d7b7522a14e245f83147e879))

## [1.0.6](https://github.com/phpolygon/php-vio/compare/1.0.5...1.0.6) (2026-04-11)


### Bug Fixes

* use MSVC-compatible visibility macros in stb implementation files ([0492ed4](https://github.com/phpolygon/php-vio/commit/0492ed47d80bf1ae82d7245d58a9427d6276f80d))

## [1.0.5](https://github.com/phpolygon/php-vio/compare/1.0.4...1.0.5) (2026-04-11)


### Bug Fixes

* correct include path for vio_plugin.h on Windows ([b86a787](https://github.com/phpolygon/php-vio/commit/b86a787c80cdd497cfb25793181f45754ee735eb))

## [1.0.4](https://github.com/phpolygon/php-vio/compare/1.0.3...1.0.4) (2026-04-11)


### Bug Fixes

* replace pthread with _beginthreadex on Windows for async texture loading ([7f22100](https://github.com/phpolygon/php-vio/commit/7f22100d6dc8b7f7cc6b42ca359eed9c76e386f2))

## [1.0.3](https://github.com/phpolygon/php-vio/compare/1.0.2...1.0.3) (2026-04-11)


### Bug Fixes

* guard GL calls in vio_begin with vio_gl.initialized check ([49c12c0](https://github.com/phpolygon/php-vio/commit/49c12c09d5baca10cbcbca834c1072baaa2d4883))

## [1.0.2](https://github.com/phpolygon/php-vio/compare/1.0.1...1.0.2) (2026-04-11)


### Bug Fixes

* avoid stb_truetype.h macro conflict with PHP headers on Windows ([98b620a](https://github.com/phpolygon/php-vio/commit/98b620a39e3de35d5da1da78a667dcfdf18f821a))

## [1.0.1](https://github.com/phpolygon/php-vio/compare/1.0.0...1.0.1) (2026-04-11)


### Bug Fixes

* conditional Metal/VMA compilation, Vulkan includes, Windows deps dir ([5c7c847](https://github.com/phpolygon/php-vio/commit/5c7c8477a8afea25c2fd4aea915f62402e9951b4))

# 1.0.0 (2026-04-11)


### Bug Fixes

* trigger build workflow after semantic release via gh workflow run ([4686e86](https://github.com/phpolygon/php-vio/commit/4686e8691c9b0d5fbcc5a21258384110598a02ae))
* use tags without v-prefix for PIE/Composer compatibility ([708df70](https://github.com/phpolygon/php-vio/commit/708df70a23ab57063737c214f6e35ed7d252a2d7))


### Features

* add semantic release workflow for automated versioning ([70d2664](https://github.com/phpolygon/php-vio/commit/70d2664fec9efec01dec1bf6521130d00a9c1de4))

## [0.2.1](https://github.com/phpolygon/php-vio/compare/v0.2.0...v0.2.1) (2026-04-11)


### Bug Fixes

* trigger build workflow after semantic release via gh workflow run ([4686e86](https://github.com/phpolygon/php-vio/commit/4686e8691c9b0d5fbcc5a21258384110598a02ae))

# [0.2.0](https://github.com/phpolygon/php-vio/compare/v0.1.0...v0.2.0) (2026-04-11)


### Features

* add semantic release workflow for automated versioning ([70d2664](https://github.com/phpolygon/php-vio/commit/70d2664fec9efec01dec1bf6521130d00a9c1de4))
