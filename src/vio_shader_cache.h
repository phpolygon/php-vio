/*
 * php-vio - On-disk shader / pipeline cache (GAP-PHASE5 Block 4)
 *
 * vio_create(['shader_cache' => '<dir>']) points every backend at one directory:
 * D3D11 / D3D12 keep the DXBC of each compiled HLSL stage, OpenGL (>= 4.1) the
 * linked program binary, Vulkan its VkPipelineCache blob. Keys are 64-bit FNV-1a
 * hashes of the source (plus profile / flags / driver strings where the artifact
 * is driver-specific); a miss simply compiles as before and stores the result.
 * Process-wide: one directory per process, set by the last vio_create.
 */

#ifndef VIO_SHADER_CACHE_H
#define VIO_SHADER_CACHE_H

#include <stddef.h>
#include <stdint.h>

/* Set (copy) the cache directory; NULL or "" disables the cache. Creates the
 * directory when missing. */
void        vio_shader_cache_set_dir(const char *dir);
/* Current directory or NULL when disabled. */
const char *vio_shader_cache_dir(void);

/* FNV-1a 64: hash `tag` (e.g. the compile profile) followed by `len` bytes. */
uint64_t    vio_shader_cache_hash(const char *tag, const void *data, size_t len);
uint64_t    vio_shader_cache_hash_more(uint64_t h, const void *data, size_t len);

/* Load <dir>/<key>.<ext>; malloc'd bytes (free() them) or NULL on a miss. */
void       *vio_shader_cache_load(uint64_t key, const char *ext, size_t *out_len);
/* Store bytes as <dir>/<key>.<ext> (written to a temp name, then renamed). */
int         vio_shader_cache_store(uint64_t key, const char *ext, const void *data, size_t len);

/* Cumulative counters for tests / diagnostics (vio_shader_cache_stats()). */
void        vio_shader_cache_stats(long *hits, long *misses, long *stores);

#endif /* VIO_SHADER_CACHE_H */
