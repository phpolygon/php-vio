/*
 * php-vio - text/uri-list (RFC 2483) to local paths, for file drops on X11
 * (XDND) and Wayland (wl_data_offer). Only file: URIs on this host count;
 * comments, other schemes and other hosts are skipped. Percent escapes are
 * decoded, so the paths are the bytes the file system uses (UTF-8 in practice).
 */

#ifndef VIO_URI_LIST_H
#define VIO_URI_LIST_H

#include <stdlib.h>
#include <string.h>

static inline int vio_uri_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Fills *out with malloc'd paths (free each and the array); returns the count. */
static inline int vio_uri_list_paths(const char *data, size_t len, char ***out)
{
    *out = NULL;
    int n = 0, cap = 0;
    char **paths = NULL;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') i++;
        size_t end = i;
        while (i < len && (data[i] == '\n' || data[i] == '\r')) i++;
        const char *line = data + start;
        size_t l = end - start;
        if (l < 7 || line[0] == '#' || strncmp(line, "file://", 7) != 0) continue;
        line += 7; l -= 7;
        if (l >= 9 && strncmp(line, "localhost", 9) == 0) { line += 9; l -= 9; }
        if (l == 0 || line[0] != '/') continue;   /* another host */
        char *p = (char *)malloc(l + 1);
        if (!p) break;
        size_t k = 0;
        for (size_t j = 0; j < l; j++) {
            int hi, lo;
            if (line[j] == '%' && j + 2 < l && (hi = vio_uri_hex(line[j + 1])) >= 0 && (lo = vio_uri_hex(line[j + 2])) >= 0) {
                p[k++] = (char)(hi * 16 + lo);
                j += 2;
            } else {
                p[k++] = line[j];
            }
        }
        p[k] = '\0';
        if (k == 0 || memchr(p, 0, k)) { free(p); continue; }   /* %00 */
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            char **grown = (char **)realloc(paths, (size_t)cap * sizeof(char *));
            if (!grown) { free(p); break; }
            paths = grown;
        }
        paths[n++] = p;
    }
    *out = paths;
    return n;
}

static inline void vio_uri_list_free(char **paths, int n)
{
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

#endif /* VIO_URI_LIST_H */
