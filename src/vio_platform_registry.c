/*
 * php-vio - Platform registry (NATIVE-PLATFORM-PLAN, OPEN-ITEMS A1)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"   /* the build's HAVE_* switches (config.w32.h / php_config.h) */
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#include "../include/vio_platform.h"
#include "vio_window.h"

#define VIO_MAX_PLATFORMS 8

static const vio_platform *vio_platforms[VIO_MAX_PLATFORMS];
static int                 vio_platform_count = 0;
static const vio_platform *vio_platform_current = NULL;

int vio_register_platform(const vio_platform *p)
{
    if (!p || !p->name || vio_platform_count >= VIO_MAX_PLATFORMS) return -1;
    for (int i = 0; i < vio_platform_count; i++) {
        if (strcmp(vio_platforms[i]->name, p->name) == 0) return 0;
    }
    vio_platforms[vio_platform_count++] = p;
    return 0;
}

const vio_platform *vio_find_platform(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < vio_platform_count; i++) {
        if (strcmp(vio_platforms[i]->name, name) == 0) return vio_platforms[i];
    }
    return NULL;
}

/* VIO_PLATFORM=<name> picks one explicitly; otherwise the native platform of
 * the OS when it is built in, then GLFW, then null. On Linux, Wayland comes
 * before X11 when it is registered (WAYLAND_DISPLAY set, as GLFW 3.4). */
const vio_platform *vio_platform_active(void)
{
    if (vio_platform_current) return vio_platform_current;
    const char *env = getenv("VIO_PLATFORM");
    const vio_platform *p = (env && *env) ? vio_find_platform(env) : NULL;
    static const char *order[] = { "win32", "cocoa", "wayland", "x11", "glfw", "null" };
    for (size_t i = 0; !p && i < sizeof(order) / sizeof(order[0]); i++) p = vio_find_platform(order[i]);
    if (!p) {
        vio_platform_null_register();
        p = vio_find_platform("null");
    }
    vio_platform_current = p;
    return p;
}

void vio_platform_set_active(const vio_platform *p)
{
    vio_platform_current = p;
}

/* ── Wake (vio_wake / wait_events) ────────────────────────────────────
 * One process-wide sticky flag plus a waitable object (a pipe on POSIX, a
 * manual-reset event on Windows) that is readable/signalled while the flag is
 * set. Created in MINIT, before any thread could call vio_wake. */

static volatile long vio_wake_flag;
#ifdef _WIN32
static HANDLE vio_wake_event;
#else
static int vio_wake_pipe[2] = { -1, -1 };
#endif

static void vio_platform_wake_create(void)
{
#ifdef _WIN32
    if (!vio_wake_event) vio_wake_event = CreateEventW(NULL, TRUE, FALSE, NULL);
#else
    if (vio_wake_pipe[0] >= 0) return;
    if (pipe(vio_wake_pipe) != 0) { vio_wake_pipe[0] = vio_wake_pipe[1] = -1; return; }
    for (int i = 0; i < 2; i++) {
        fcntl(vio_wake_pipe[i], F_SETFL, fcntl(vio_wake_pipe[i], F_GETFL) | O_NONBLOCK);
        fcntl(vio_wake_pipe[i], F_SETFD, FD_CLOEXEC);
    }
#endif
}

void vio_platform_wake_signal(void)
{
#ifdef _WIN32
    if (InterlockedExchange(&vio_wake_flag, 1) == 0 && vio_wake_event) SetEvent(vio_wake_event);
#else
    if (__sync_lock_test_and_set(&vio_wake_flag, 1) == 0 && vio_wake_pipe[1] >= 0) {
        char c = 1;
        while (write(vio_wake_pipe[1], &c, 1) < 0 && errno == EINTR) {}
    }
#endif
}

int vio_platform_wake_take(void)
{
#ifdef _WIN32
    if (InterlockedExchange(&vio_wake_flag, 0) == 0) return 0;
    if (vio_wake_event) ResetEvent(vio_wake_event);
#else
    if (__sync_lock_test_and_set(&vio_wake_flag, 0) == 0) return 0;
    char buf[64];
    if (vio_wake_pipe[0] >= 0) while (read(vio_wake_pipe[0], buf, sizeof(buf)) > 0) {}
#endif
    return 1;
}

int vio_platform_wake_pending(void)
{
    return vio_wake_flag != 0;
}

void *vio_platform_wake_handle(void)
{
#ifdef _WIN32
    return vio_wake_event;
#else
    return NULL;
#endif
}

int vio_platform_wake_fd(void)
{
#ifdef _WIN32
    return -1;
#else
    return vio_wake_pipe[0];
#endif
}

/* A wait with no event source but vio_wake (null platform, no display). */
void vio_platform_wait_wake_only(double timeout)
{
    if (vio_platform_wake_take()) return;
    int ms = vio_platform_timeout_ms(timeout);
#ifdef _WIN32
    if (vio_wake_event) WaitForSingleObject(vio_wake_event, ms < 0 ? INFINITE : (DWORD)ms);
    else if (ms > 0) Sleep((DWORD)ms);
#else
    struct pollfd p = { .fd = vio_wake_pipe[0], .events = POLLIN };
    if (ms != 0) while (poll(&p, p.fd >= 0 ? 1 : 0, ms) < 0 && errno == EINTR) {}
#endif
    vio_platform_wake_take();
}

int vio_platform_test_dialog(int *ms, int *action)
{
#ifdef _WIN32
    /* PHP's putenv() reaches the process environment, not the CRT's copy that getenv() reads */
    static char buf[64];
    DWORD n = GetEnvironmentVariableA("VIO_TEST_DIALOG", buf, sizeof(buf));
    const char *v = n > 0 && n < sizeof(buf) ? buf : NULL;
#else
    const char *v = getenv("VIO_TEST_DIALOG");
#endif
    if (!v || !*v) return 0;
    char *end = NULL;
    long t = strtol(v, &end, 10);
    if (!end || *end != ':' || t < 0) return 0;
    const char *a = end + 1;
    *ms = (int)t;
    if (strcmp(a, "cancel") == 0) *action = -1;
    else if (strcmp(a, "accept") == 0) *action = -2;
    else *action = atoi(a);
    return 1;
}

int vio_platform_timeout_ms(double timeout)
{
    if (timeout < 0.0) return -1;
    double ms = timeout * 1000.0 + 0.999;
    return ms > 2147483647.0 ? 2147483647 : (int)ms;
}

/* ── Opener (vio_open_url / vio_reveal_file on macOS and Linux) ───── */

#ifndef _WIN32
#define VIO_OPENER_MAX 32
static pid_t vio_opener_pids[VIO_OPENER_MAX];

static void vio_opener_reap(void)
{
    for (int i = 0; i < VIO_OPENER_MAX; i++) {
        if (vio_opener_pids[i] > 0 && waitpid(vio_opener_pids[i], NULL, WNOHANG) != 0) vio_opener_pids[i] = 0;
    }
}
#endif

int vio_platform_spawn_opener(const char *default_cmd, const char *const *args, int nargs)
{
#ifdef _WIN32
    (void)default_cmd; (void)args; (void)nargs;
    return 0;
#else
    const char *cmd = getenv("VIO_OPEN_COMMAND");
    if (!cmd || !*cmd) cmd = default_cmd;
    if (nargs < 0 || nargs > 4) return 0;
    char *argv[6];
    argv[0] = (char *)cmd;
    for (int i = 0; i < nargs; i++) argv[i + 1] = (char *)args[i];
    argv[nargs + 1] = NULL;
    vio_opener_reap();
    pid_t pid;
    /* argv straight to the program, no shell: a URL cannot inject a command */
    if (posix_spawnp(&pid, cmd, NULL, NULL, argv, environ) != 0) return 0;
    for (int i = 0; i < VIO_OPENER_MAX; i++) if (vio_opener_pids[i] <= 0) { vio_opener_pids[i] = pid; return 1; }
    waitpid(pid, NULL, 0);   /* table full: wait for this one */
    return 1;
#endif
}

/* MINIT / MSHUTDOWN (declared in src/vio_window.h): the built-in platforms
 * register, the active one starts. Without any window system the null
 * platform is active and init succeeds. */
int vio_window_init(void)
{
    vio_platform_wake_create();
    vio_platform_null_register();
#ifdef HAVE_GLFW
    vio_platform_glfw_register();
#endif
#ifdef _WIN32
    vio_platform_win32_register();
#endif
#ifdef HAVE_X11
    vio_platform_x11_register();
#endif
#ifdef HAVE_WAYLAND
    /* only inside a Wayland session (or when asked for): otherwise X11 */
    {
        const char *wd = getenv("WAYLAND_DISPLAY"), *want = getenv("VIO_PLATFORM");
        if ((wd && *wd) || (want && strcmp(want, "wayland") == 0)) vio_platform_wayland_register();
    }
#endif
#if defined(HAVE_COCOA) && !defined(HAVE_IOS)
    vio_platform_cocoa_register();
#endif
    vio_platform_current = NULL;   /* choose among everything registered now */
    return vio_platform_active()->init();
}

void vio_window_shutdown(void)
{
    vio_platform_active()->shutdown();
}
