/*
 * php-vio - the one way to include miniaudio
 *
 * php-vio compiles its own miniaudio (src/vio_miniaudio_impl.c) and renames all
 * of its public functions to vio_ma_* (src/vio_miniaudio_ns.h, generated). The
 * static runtimes link php-glfw into the same binary, and php-glfw compiles a
 * different miniaudio version under the plain ma_* names; without the rename
 * php-vio's calls bound to that copy, whose structs (ma_engine, ma_sound,
 * ma_engine_config, ...) have another size and layout than the header php-vio
 * was compiled with. With the rename both copies coexist, and a build that
 * drops php-vio's implementation fails to link instead of rebinding silently.
 *
 * Every php-vio source that needs miniaudio includes this header - never
 * vendor/miniaudio/miniaudio.h directly (tests/core/246 enforces it).
 */

#ifndef VIO_MINIAUDIO_H
#define VIO_MINIAUDIO_H

#include "vio_miniaudio_ns.h"
#include "../vendor/miniaudio/miniaudio.h"

/* The rename list must come from exactly this miniaudio.h: a vendor update
 * without re-running scripts/gen-miniaudio-namespace.php could leave new
 * functions un-renamed (they would link against a foreign copy again). */
#if MA_VERSION_MAJOR != VIO_MINIAUDIO_NS_VERSION_MAJOR || \
    MA_VERSION_MINOR != VIO_MINIAUDIO_NS_VERSION_MINOR || \
    MA_VERSION_REVISION != VIO_MINIAUDIO_NS_VERSION_REVISION
#error "src/vio_miniaudio_ns.h was generated from another miniaudio version - run php scripts/gen-miniaudio-namespace.php"
#endif

/* Returns 1 when the miniaudio implementation linked into this binary is the
 * version this translation unit was compiled against, 0 otherwise. A second
 * line of defence behind the rename: checked once before the audio engine
 * starts, so a mismatch refuses audio instead of corrupting memory. */
static inline int vio_miniaudio_version_matches(void)
{
    ma_uint32 major = 0, minor = 0, revision = 0;
    ma_version(&major, &minor, &revision);
    return major == MA_VERSION_MAJOR && minor == MA_VERSION_MINOR && revision == MA_VERSION_REVISION;
}

#endif /* VIO_MINIAUDIO_H */
