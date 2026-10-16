/*
 * php-vio - miniaudio implementation translation unit
 *
 * Compiles miniaudio exactly once, with every public function renamed to
 * vio_ma_* (see src/vio_miniaudio.h for why). Lives in src/ rather than
 * vendor/miniaudio/: static-php-cli strips vio's vendor/* sources that php-glfw
 * also ships when both extensions are built together - this file is not a
 * duplicate of php-glfw's copy any more and must always be compiled.
 */
#define MINIAUDIO_IMPLEMENTATION
#include "vio_miniaudio.h"
