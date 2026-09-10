/*
 * php-vio - On-disk shader / pipeline cache (GAP-PHASE5 Block 4)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_shader_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define vio_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define vio_mkdir(p) mkdir((p), 0755)
#endif

static char vio_cache_dir[1024];
static int  vio_cache_enabled = 0;
static long vio_cache_hits = 0, vio_cache_misses = 0, vio_cache_stores = 0;

void vio_shader_cache_set_dir(const char *dir)
{
    if (!dir || !*dir) {
        vio_cache_enabled = 0;
        vio_cache_dir[0] = '\0';
        return;
    }
    size_t n = strlen(dir);
    if (n >= sizeof(vio_cache_dir) - 1) {
        vio_cache_enabled = 0;
        return;
    }
    memcpy(vio_cache_dir, dir, n + 1);
    /* Strip a trailing separator so paths join cleanly. */
    while (n > 1 && (vio_cache_dir[n - 1] == '/' || vio_cache_dir[n - 1] == '\\')) vio_cache_dir[--n] = '\0';
    vio_mkdir(vio_cache_dir);   /* EEXIST is fine; a failure shows up as store failures */
    vio_cache_enabled = 1;
}

const char *vio_shader_cache_dir(void)
{
    return vio_cache_enabled ? vio_cache_dir : NULL;
}

uint64_t vio_shader_cache_hash_more(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t vio_shader_cache_hash(const char *tag, const void *data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    if (tag) h = vio_shader_cache_hash_more(h, tag, strlen(tag));
    h = vio_shader_cache_hash_more(h, "|", 1);
    return vio_shader_cache_hash_more(h, data, len);
}

static int vio_cache_path(char *out, size_t cap, uint64_t key, const char *ext, const char *suffix)
{
    int n = snprintf(out, cap, "%s/%016llx.%s%s", vio_cache_dir, (unsigned long long)key, ext, suffix ? suffix : "");
    return n > 0 && (size_t)n < cap;
}

void *vio_shader_cache_load(uint64_t key, const char *ext, size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!vio_cache_enabled) return NULL;
    char path[1200];
    if (!vio_cache_path(path, sizeof(path), key, ext, NULL)) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { vio_cache_misses++; return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); vio_cache_misses++; return NULL; }
    long size = ftell(f);
    if (size <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); vio_cache_misses++; return NULL; }
    void *buf = malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); vio_cache_misses++; return NULL; }
    vio_cache_hits++;
    if (out_len) *out_len = (size_t)size;
    return buf;
}

int vio_shader_cache_store(uint64_t key, const char *ext, const void *data, size_t len)
{
    if (!vio_cache_enabled || !data || len == 0) return -1;
    char path[1200], tmp[1200];
    if (!vio_cache_path(path, sizeof(path), key, ext, NULL) || !vio_cache_path(tmp, sizeof(tmp), key, ext, ".tmp")) return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t put = fwrite(data, 1, len, f);
    fclose(f);
    if (put != len) { remove(tmp); return -1; }
    remove(path);              /* rename() does not overwrite on Windows */
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    vio_cache_stores++;
    return 0;
}

void vio_shader_cache_stats(long *hits, long *misses, long *stores)
{
    if (hits) *hits = vio_cache_hits;
    if (misses) *misses = vio_cache_misses;
    if (stores) *stores = vio_cache_stores;
}
