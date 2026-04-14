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
