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
