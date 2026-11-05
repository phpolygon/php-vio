/*
 * php-vio - Open a URL / show a file with the system's default handler
 * (DESKTOP-APP-PLAN Phase 1), shared by the Win32, X11, Wayland and GLFW
 * platforms. Cocoa uses NSWorkspace instead.
 *
 * Windows: ShellExecuteW (open verb) and SHOpenFolderAndSelectItems.
 * macOS (GLFW): `open URL` / `open -R path`. Linux: `xdg-open URL` /
 * `xdg-open <folder>`. On macOS and Linux VIO_OPEN_COMMAND replaces the
 * program (vio_platform_spawn_opener).
 *
 * The callers have checked the input: the URL has a scheme and is not file:,
 * the path exists, neither holds control characters.
 */

#ifndef VIO_SHELL_H
#define VIO_SHELL_H

#include <stdlib.h>
#include <string.h>
#include "../../include/vio_platform.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

static inline WCHAR *vio_shell_wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (w && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0) { free(w); w = NULL; }
    return w;
}

static inline int vio_shell_open_url(const char *url)
{
    WCHAR *w = vio_shell_wide(url);
    if (!w) return 0;
    HINSTANCE r = ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    return (INT_PTR)r > 32;
}

static inline int vio_shell_reveal_file(const char *path)
{
    WCHAR *w = vio_shell_wide(path);
    if (!w) return 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    PIDLIST_ABSOLUTE pidl = NULL;
    int ok = SUCCEEDED(SHParseDisplayName(w, NULL, &pidl, 0, NULL)) && pidl
          && SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, NULL, 0));
    if (pidl) CoTaskMemFree(pidl);
    if (SUCCEEDED(co)) CoUninitialize();
    free(w);
    return ok;
}

#else

static inline int vio_shell_open_url(const char *url)
{
    const char *a[1] = { url };
#ifdef __APPLE__
    return vio_platform_spawn_opener("open", a, 1);
#else
    return vio_platform_spawn_opener("xdg-open", a, 1);
#endif
}

static inline int vio_shell_reveal_file(const char *path)
{
#ifdef __APPLE__
    const char *a[2] = { "-R", path };
    return vio_platform_spawn_opener("open", a, 2);
#else
    /* no portable "select this file": open its folder */
    size_t n = strlen(path);
    char *dir = (char *)malloc(n + 2);
    if (!dir) return 0;
    memcpy(dir, path, n + 1);
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else { dir[0] = '.'; dir[1] = '\0'; }
    const char *a[1] = { dir };
    int ok = vio_platform_spawn_opener("xdg-open", a, 1);
    free(dir);
    return ok;
#endif
}

#endif

#endif /* VIO_SHELL_H */
