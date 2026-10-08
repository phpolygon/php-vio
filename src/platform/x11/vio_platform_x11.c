/*
 * php-vio - X11 platform (NATIVE-PLATFORM-PLAN Phase 3, OPEN-ITEMS A3)
 *
 * Linux without GLFW: Xlib windows, keys through the XKB key names (the
 * physical position, independent of the layout - GLFW's mapping), text
 * through XIM / Xutf8LookupString, detectable auto-repeat, mouse buttons and
 * wheels, a grabbed and warped pointer for VIO_CURSOR_DISABLED, monitors and
 * video modes through XRandR (one monitor of the screen size without it),
 * fullscreen through _NET_WM_STATE_FULLSCREEN plus an XRandR mode switch,
 * gamepads through evdev (/dev/input/event*), the Vulkan Xlib surface, and
 * OpenGL through GLX - libGL is opened at run time, so the build needs only
 * the x11 and xrandr headers. The slots keep GLFW's X11 semantics: sizes in
 * pixels, content scale from Xft.dpi, visible windows scaled to it.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_X11

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xrandr.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_VULKAN
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#endif

#include "../../../include/vio_platform.h"
#include "../../../include/vio_constants.h"
#include "../../vio_input.h"

/* ── Display, errors ──────────────────────────────────────────────── */

static Display *x11_dpy;
static int      x11_screen;
static Window   x11_root;
static XContext x11_ctx;
static XIM      x11_im;
static int      x11_error_code;
static Atom     x11_wm_delete, x11_wm_protocols, x11_net_wm_state, x11_net_fullscreen,
                x11_net_max_h, x11_net_max_v, x11_motif_hints, x11_net_workarea, x11_net_active;
static Cursor   x11_blank_cursor;
static int      x11_randr, x11_randr_event;

/* Xlib's default handler exits the process; a failed GLX rung or a stale
 * window must only be recorded. */
static int x11_error_handler(Display *d, XErrorEvent *e)
{
    (void)d;
    x11_error_code = e->error_code;
    return 0;
}

static int x11_open(void)
{
    if (x11_dpy) return 1;
    XInitThreads();
    x11_dpy = XOpenDisplay(NULL);
    if (!x11_dpy) return 0;
    XSetErrorHandler(x11_error_handler);
    x11_screen = DefaultScreen(x11_dpy);
    x11_root = RootWindow(x11_dpy, x11_screen);
    x11_ctx = XUniqueContext();
    x11_wm_delete      = XInternAtom(x11_dpy, "WM_DELETE_WINDOW", False);
    x11_wm_protocols   = XInternAtom(x11_dpy, "WM_PROTOCOLS", False);
    x11_net_wm_state   = XInternAtom(x11_dpy, "_NET_WM_STATE", False);
    x11_net_fullscreen = XInternAtom(x11_dpy, "_NET_WM_STATE_FULLSCREEN", False);
    x11_net_max_h      = XInternAtom(x11_dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    x11_net_max_v      = XInternAtom(x11_dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    x11_motif_hints    = XInternAtom(x11_dpy, "_MOTIF_WM_HINTS", False);
    x11_net_workarea   = XInternAtom(x11_dpy, "_NET_WORKAREA", False);
    x11_net_active     = XInternAtom(x11_dpy, "_NET_ACTIVE_WINDOW", False);
    Bool supported = False;
    XkbSetDetectableAutoRepeat(x11_dpy, True, &supported);   /* repeats: press, press, ..., release */
    int err_base = 0;
    x11_randr = XRRQueryExtension(x11_dpy, &x11_randr_event, &err_base);
    if (x11_randr) {
        int major = 0, minor = 0;
        XRRQueryVersion(x11_dpy, &major, &minor);
        x11_randr = major > 1 || (major == 1 && minor >= 3);   /* GetScreenResourcesCurrent, primary output */
    }
    XSetLocaleModifiers("");
    x11_im = XOpenIM(x11_dpy, NULL, NULL, NULL);
    char none[1] = { 0 };
    Pixmap pm = XCreateBitmapFromData(x11_dpy, x11_root, none, 1, 1);
    XColor black;
    memset(&black, 0, sizeof(black));
    x11_blank_cursor = XCreatePixmapCursor(x11_dpy, pm, pm, &black, &black, 0, 0);
    XFreePixmap(x11_dpy, pm);
    return 1;
}

/* ── Keyboard: XKB key names -> VIO_KEY_* (GLFW's table) ─────────── */

static short x11_keycodes[256];

static int x11_key_by_name(const char *n)
{
    static const struct { const char *name; int key; } t[] = {
        {"TLDE", VIO_KEY_GRAVE_ACCENT}, {"AE01", VIO_KEY_1}, {"AE02", VIO_KEY_2}, {"AE03", VIO_KEY_3}, {"AE04", VIO_KEY_4},
        {"AE05", VIO_KEY_5}, {"AE06", VIO_KEY_6}, {"AE07", VIO_KEY_7}, {"AE08", VIO_KEY_8}, {"AE09", VIO_KEY_9},
        {"AE10", VIO_KEY_0}, {"AE11", VIO_KEY_MINUS}, {"AE12", VIO_KEY_EQUAL},
        {"AD01", VIO_KEY_Q}, {"AD02", VIO_KEY_W}, {"AD03", VIO_KEY_E}, {"AD04", VIO_KEY_R}, {"AD05", VIO_KEY_T},
        {"AD06", VIO_KEY_Y}, {"AD07", VIO_KEY_U}, {"AD08", VIO_KEY_I}, {"AD09", VIO_KEY_O}, {"AD10", VIO_KEY_P},
        {"AD11", VIO_KEY_LEFT_BRACKET}, {"AD12", VIO_KEY_RIGHT_BRACKET},
        {"AC01", VIO_KEY_A}, {"AC02", VIO_KEY_S}, {"AC03", VIO_KEY_D}, {"AC04", VIO_KEY_F}, {"AC05", VIO_KEY_G},
        {"AC06", VIO_KEY_H}, {"AC07", VIO_KEY_J}, {"AC08", VIO_KEY_K}, {"AC09", VIO_KEY_L}, {"AC10", VIO_KEY_SEMICOLON},
        {"AC11", VIO_KEY_APOSTROPHE},
        {"AB01", VIO_KEY_Z}, {"AB02", VIO_KEY_X}, {"AB03", VIO_KEY_C}, {"AB04", VIO_KEY_V}, {"AB05", VIO_KEY_B},
        {"AB06", VIO_KEY_N}, {"AB07", VIO_KEY_M}, {"AB08", VIO_KEY_COMMA}, {"AB09", VIO_KEY_PERIOD}, {"AB10", VIO_KEY_SLASH},
        {"BKSL", VIO_KEY_BACKSLASH}, {"LSGT", 162 /* WORLD_2 */},
        {"SPCE", VIO_KEY_SPACE}, {"ESC", VIO_KEY_ESCAPE}, {"RTRN", VIO_KEY_ENTER}, {"TAB", VIO_KEY_TAB},
        {"BKSP", VIO_KEY_BACKSPACE}, {"INS", VIO_KEY_INSERT}, {"DELE", VIO_KEY_DELETE},
        {"RGHT", VIO_KEY_RIGHT}, {"LEFT", VIO_KEY_LEFT}, {"DOWN", VIO_KEY_DOWN}, {"UP", VIO_KEY_UP},
        {"PGUP", VIO_KEY_PAGE_UP}, {"PGDN", VIO_KEY_PAGE_DOWN}, {"HOME", VIO_KEY_HOME}, {"END", VIO_KEY_END},
        {"CAPS", VIO_KEY_CAPS_LOCK}, {"SCLK", VIO_KEY_SCROLL_LOCK}, {"NMLK", VIO_KEY_NUM_LOCK},
        {"PRSC", VIO_KEY_PRINT_SCREEN}, {"PAUS", VIO_KEY_PAUSE},
        {"FK01", VIO_KEY_F1}, {"FK02", VIO_KEY_F2}, {"FK03", VIO_KEY_F3}, {"FK04", VIO_KEY_F4}, {"FK05", VIO_KEY_F5},
        {"FK06", VIO_KEY_F6}, {"FK07", VIO_KEY_F7}, {"FK08", VIO_KEY_F8}, {"FK09", VIO_KEY_F9}, {"FK10", VIO_KEY_F10},
        {"FK11", VIO_KEY_F11}, {"FK12", VIO_KEY_F12}, {"FK13", 302}, {"FK14", 303}, {"FK15", 304}, {"FK16", 305},
        {"FK17", 306}, {"FK18", 307}, {"FK19", 308}, {"FK20", 309}, {"FK21", 310}, {"FK22", 311}, {"FK23", 312},
        {"FK24", 313}, {"FK25", 314},
        {"KP0", VIO_KEY_KP_0}, {"KP1", VIO_KEY_KP_1}, {"KP2", VIO_KEY_KP_2}, {"KP3", VIO_KEY_KP_3}, {"KP4", VIO_KEY_KP_4},
        {"KP5", VIO_KEY_KP_5}, {"KP6", VIO_KEY_KP_6}, {"KP7", VIO_KEY_KP_7}, {"KP8", VIO_KEY_KP_8}, {"KP9", VIO_KEY_KP_9},
        {"KPDL", VIO_KEY_KP_DECIMAL}, {"KPDV", VIO_KEY_KP_DIVIDE}, {"KPMU", VIO_KEY_KP_MULTIPLY},
        {"KPSU", VIO_KEY_KP_SUBTRACT}, {"KPAD", VIO_KEY_KP_ADD}, {"KPEN", VIO_KEY_KP_ENTER}, {"KPEQ", VIO_KEY_KP_EQUAL},
        {"LFSH", VIO_KEY_LEFT_SHIFT}, {"LCTL", VIO_KEY_LEFT_CONTROL}, {"LALT", VIO_KEY_LEFT_ALT}, {"LWIN", VIO_KEY_LEFT_SUPER},
        {"RTSH", VIO_KEY_RIGHT_SHIFT}, {"RCTL", VIO_KEY_RIGHT_CONTROL}, {"RALT", VIO_KEY_RIGHT_ALT}, {"LVL3", VIO_KEY_RIGHT_ALT},
        {"MDSW", VIO_KEY_RIGHT_ALT}, {"RWIN", VIO_KEY_RIGHT_SUPER}, {"MENU", VIO_KEY_MENU}, {"COMP", VIO_KEY_MENU},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (strncmp(n, t[i].name, XkbKeyNameLength) == 0) return t[i].key;
    return VIO_KEY_UNKNOWN;
}

/* Keycodes XKB has no known name for: the unshifted keysym decides. */
static int x11_key_by_keysym(KeySym ks)
{
    if (ks >= XK_a && ks <= XK_z) return VIO_KEY_A + (int)(ks - XK_a);
    if (ks >= XK_0 && ks <= XK_9) return VIO_KEY_0 + (int)(ks - XK_0);
    switch (ks) {
        case XK_Escape: return VIO_KEY_ESCAPE;       case XK_Return: return VIO_KEY_ENTER;
        case XK_Tab: return VIO_KEY_TAB;             case XK_BackSpace: return VIO_KEY_BACKSPACE;
        case XK_space: return VIO_KEY_SPACE;         case XK_Shift_L: return VIO_KEY_LEFT_SHIFT;
        case XK_Shift_R: return VIO_KEY_RIGHT_SHIFT; case XK_Control_L: return VIO_KEY_LEFT_CONTROL;
        case XK_Control_R: return VIO_KEY_RIGHT_CONTROL; case XK_Alt_L: return VIO_KEY_LEFT_ALT;
        case XK_Alt_R: case XK_ISO_Level3_Shift: return VIO_KEY_RIGHT_ALT;
        case XK_Super_L: return VIO_KEY_LEFT_SUPER;  case XK_Super_R: return VIO_KEY_RIGHT_SUPER;
        case XK_Left: return VIO_KEY_LEFT;           case XK_Right: return VIO_KEY_RIGHT;
        case XK_Up: return VIO_KEY_UP;               case XK_Down: return VIO_KEY_DOWN;
    }
    return VIO_KEY_UNKNOWN;
}

static void x11_keytable_init(void)
{
    for (int i = 0; i < 256; i++) x11_keycodes[i] = VIO_KEY_UNKNOWN;
    XkbDescPtr desc = XkbGetMap(x11_dpy, 0, XkbUseCoreKbd);
    if (desc && XkbGetNames(x11_dpy, XkbKeyNamesMask | XkbKeyAliasesMask, desc) == Success && desc->names) {
        for (int kc = desc->min_key_code; kc <= desc->max_key_code && kc < 256; kc++) {
            char name[XkbKeyNameLength + 1];
            memcpy(name, desc->names->keys[kc].name, XkbKeyNameLength);
            name[XkbKeyNameLength] = 0;
            int key = x11_key_by_name(name);
            /* an alias (e.g. "LatQ" for AD01) names the real key */
            for (int a = 0; key == VIO_KEY_UNKNOWN && a < desc->names->num_key_aliases; a++) {
                if (strncmp(desc->names->key_aliases[a].real, name, XkbKeyNameLength) == 0) {
                    char alias[XkbKeyNameLength + 1];
                    memcpy(alias, desc->names->key_aliases[a].alias, XkbKeyNameLength);
                    alias[XkbKeyNameLength] = 0;
                    key = x11_key_by_name(alias);
                }
            }
            x11_keycodes[kc] = (short)key;
        }
        XkbFreeNames(desc, XkbKeyNamesMask | XkbKeyAliasesMask, True);
    }
    if (desc) XkbFreeKeyboard(desc, 0, True);
    for (int kc = 8; kc < 256; kc++) {
        if (x11_keycodes[kc] != VIO_KEY_UNKNOWN) continue;
        x11_keycodes[kc] = (short)x11_key_by_keysym(XkbKeycodeToKeysym(x11_dpy, (KeyCode)kc, 0, 0));
    }
}

static int x11_mods(unsigned int state)
{
    int m = 0;
    if (state & ShiftMask)   m |= VIO_MOD_SHIFT;
    if (state & ControlMask) m |= VIO_MOD_CONTROL;
    if (state & Mod1Mask)    m |= VIO_MOD_ALT;
    if (state & Mod4Mask)    m |= VIO_MOD_SUPER;
    if (state & LockMask)    m |= VIO_MOD_CAPS_LOCK;
    if (state & Mod2Mask)    m |= VIO_MOD_NUM_LOCK;
    return m;
}

/* ── GLX (libGL at run time) ──────────────────────────────────────── */

typedef struct __GLXFBConfigRec *x11_GLXFBConfig;
typedef struct __GLXcontextRec  *x11_GLXContext;
typedef x11_GLXFBConfig *(*x11_glXChooseFBConfig_t)(Display *, int, const int *, int *);
typedef XVisualInfo *(*x11_glXGetVisualFromFBConfig_t)(Display *, x11_GLXFBConfig);
typedef x11_GLXContext (*x11_glXCreateContextAttribsARB_t)(Display *, x11_GLXFBConfig, x11_GLXContext, Bool, const int *);
typedef x11_GLXContext (*x11_glXCreateNewContext_t)(Display *, x11_GLXFBConfig, int, x11_GLXContext, Bool);
typedef Bool (*x11_glXMakeCurrent_t)(Display *, Drawable, x11_GLXContext);
typedef void (*x11_glXDestroyContext_t)(Display *, x11_GLXContext);
typedef void (*x11_glXSwapBuffers_t)(Display *, Drawable);
typedef void *(*x11_glXGetProcAddress_t)(const unsigned char *);
typedef void (*x11_glXSwapIntervalEXT_t)(Display *, Drawable, int);
typedef int  (*x11_glXSwapIntervalMESA_t)(unsigned int);
typedef int  (*x11_glXSwapIntervalSGI_t)(int);
typedef x11_GLXContext (*x11_glXGetCurrentContext_t)(void);
typedef Drawable (*x11_glXGetCurrentDrawable_t)(void);

#define X11_GLX_RGBA_TYPE           0x8014
#define X11_GLX_X_RENDERABLE        0x8012
#define X11_GLX_DRAWABLE_TYPE       0x8010
#define X11_GLX_WINDOW_BIT          0x0001
#define X11_GLX_RENDER_TYPE         0x8011
#define X11_GLX_RGBA_BIT            0x0001
#define X11_GLX_X_VISUAL_TYPE       0x22
#define X11_GLX_TRUE_COLOR          0x8002
#define X11_GLX_DOUBLEBUFFER        5
#define X11_GLX_RED_SIZE            8
#define X11_GLX_GREEN_SIZE          9
#define X11_GLX_BLUE_SIZE           10
#define X11_GLX_ALPHA_SIZE          11
#define X11_GLX_DEPTH_SIZE          12
#define X11_GLX_STENCIL_SIZE        13
#define X11_GLX_SAMPLE_BUFFERS      100000
#define X11_GLX_SAMPLES             100001
#define X11_GLX_CONTEXT_MAJOR       0x2091
#define X11_GLX_CONTEXT_MINOR       0x2092
#define X11_GLX_CONTEXT_PROFILE     0x9126
#define X11_GLX_CONTEXT_CORE_BIT    0x0001

static void *x11_libgl;
static struct {
    x11_glXChooseFBConfig_t          ChooseFBConfig;
    x11_glXGetVisualFromFBConfig_t   GetVisualFromFBConfig;
    x11_glXCreateContextAttribsARB_t CreateContextAttribsARB;
    x11_glXCreateNewContext_t        CreateNewContext;
    x11_glXMakeCurrent_t             MakeCurrent;
    x11_glXDestroyContext_t          DestroyContext;
    x11_glXSwapBuffers_t             SwapBuffers;
    x11_glXGetProcAddress_t          GetProcAddress;
    x11_glXSwapIntervalEXT_t         SwapIntervalEXT;
    x11_glXSwapIntervalMESA_t        SwapIntervalMESA;
    x11_glXSwapIntervalSGI_t         SwapIntervalSGI;
    x11_glXGetCurrentContext_t       GetCurrentContext;
    x11_glXGetCurrentDrawable_t      GetCurrentDrawable;
} glx;

static int x11_glx_load(void)
{
    if (x11_libgl) return 1;
    x11_libgl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!x11_libgl) x11_libgl = dlopen("libGL.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!x11_libgl) return 0;
    glx.ChooseFBConfig        = (x11_glXChooseFBConfig_t)dlsym(x11_libgl, "glXChooseFBConfig");
    glx.GetVisualFromFBConfig = (x11_glXGetVisualFromFBConfig_t)dlsym(x11_libgl, "glXGetVisualFromFBConfig");
    glx.CreateNewContext      = (x11_glXCreateNewContext_t)dlsym(x11_libgl, "glXCreateNewContext");
    glx.MakeCurrent           = (x11_glXMakeCurrent_t)dlsym(x11_libgl, "glXMakeCurrent");
    glx.DestroyContext        = (x11_glXDestroyContext_t)dlsym(x11_libgl, "glXDestroyContext");
    glx.SwapBuffers           = (x11_glXSwapBuffers_t)dlsym(x11_libgl, "glXSwapBuffers");
    glx.GetCurrentContext     = (x11_glXGetCurrentContext_t)dlsym(x11_libgl, "glXGetCurrentContext");
    glx.GetCurrentDrawable    = (x11_glXGetCurrentDrawable_t)dlsym(x11_libgl, "glXGetCurrentDrawable");
    glx.GetProcAddress        = (x11_glXGetProcAddress_t)dlsym(x11_libgl, "glXGetProcAddressARB");
    if (!glx.GetProcAddress) glx.GetProcAddress = (x11_glXGetProcAddress_t)dlsym(x11_libgl, "glXGetProcAddress");
    if (!glx.ChooseFBConfig || !glx.GetVisualFromFBConfig || !glx.MakeCurrent || !glx.SwapBuffers || !glx.GetProcAddress) {
        dlclose(x11_libgl);
        x11_libgl = NULL;
        return 0;
    }
    glx.CreateContextAttribsARB = (x11_glXCreateContextAttribsARB_t)glx.GetProcAddress((const unsigned char *)"glXCreateContextAttribsARB");
    glx.SwapIntervalEXT  = (x11_glXSwapIntervalEXT_t)glx.GetProcAddress((const unsigned char *)"glXSwapIntervalEXT");
    glx.SwapIntervalMESA = (x11_glXSwapIntervalMESA_t)glx.GetProcAddress((const unsigned char *)"glXSwapIntervalMESA");
    glx.SwapIntervalSGI  = (x11_glXSwapIntervalSGI_t)glx.GetProcAddress((const unsigned char *)"glXSwapIntervalSGI");
    return 1;
}

static void *x11_gl_get_proc_address(const char *name)
{
    if (!x11_glx_load()) return NULL;
    void *p = glx.GetProcAddress((const unsigned char *)name);
    return p ? p : dlsym(x11_libgl, name);
}

static x11_GLXFBConfig x11_choose_fbconfig(int samples)
{
    if (!x11_glx_load()) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        int ms = pass == 0 && samples > 1;
        int attrs[] = {
            X11_GLX_X_RENDERABLE, True, X11_GLX_DRAWABLE_TYPE, X11_GLX_WINDOW_BIT, X11_GLX_RENDER_TYPE, X11_GLX_RGBA_BIT,
            X11_GLX_X_VISUAL_TYPE, X11_GLX_TRUE_COLOR, X11_GLX_DOUBLEBUFFER, True,
            X11_GLX_RED_SIZE, 8, X11_GLX_GREEN_SIZE, 8, X11_GLX_BLUE_SIZE, 8, X11_GLX_ALPHA_SIZE, 8,
            X11_GLX_DEPTH_SIZE, 24, X11_GLX_STENCIL_SIZE, 8,
            X11_GLX_SAMPLE_BUFFERS, ms ? 1 : 0, X11_GLX_SAMPLES, ms ? samples : 0,
            None
        };
        int n = 0;
        x11_GLXFBConfig *list = glx.ChooseFBConfig(x11_dpy, x11_screen, attrs, &n);
        if (list && n > 0) {
            x11_GLXFBConfig c = list[0];
            XFree(list);
            return c;
        }
        if (list) XFree(list);
        if (!ms) break;
    }
    return NULL;
}

/* ── Window state ─────────────────────────────────────────────────── */

typedef struct _vio_x11_window {
    Window           win;
    Colormap         cmap;
    XIC              ic;
    x11_GLXContext   glrc;
    int              should_close;
    int              headless;
    int              decorated;
    int              auto_iconify;
    int              cursor_mode;
    double           cursor_x, cursor_y;     /* virtual position while DISABLED */
    int              warp_x, warp_y;
    int              fb_w, fb_h;
    int              monitor;                /* fullscreen monitor index, -1 windowed */
    RRCrtc           saved_crtc;
    RRMode           saved_mode;
    unsigned char    keys_down[256];
    vio_input_state *input;
} vio_x11_window;

static vio_input_state *x11_input(vio_x11_window *w)
{
    return (w && w->input && !w->input->replaying) ? w->input : NULL;
}

/* ── Monitors (XRandR) ────────────────────────────────────────────── */

#define X11_MAX_MONITORS 16

typedef struct {
    RROutput output;
    RRCrtc   crtc;
    int      x, y, w, h;
    char     name[128];
    int      primary;
} x11_monitor;

static x11_monitor x11_monitors[X11_MAX_MONITORS];
static int         x11_monitor_n;

static void x11_monitors_refresh(void)
{
    x11_monitor_n = 0;
    if (!x11_open()) return;
    if (x11_randr) {
        XRRScreenResources *sr = XRRGetScreenResourcesCurrent(x11_dpy, x11_root);
        RROutput primary = XRRGetOutputPrimary(x11_dpy, x11_root);
        for (int i = 0; sr && i < sr->noutput && x11_monitor_n < X11_MAX_MONITORS; i++) {
            XRROutputInfo *oi = XRRGetOutputInfo(x11_dpy, sr, sr->outputs[i]);
            if (oi && oi->connection == RR_Connected && oi->crtc) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(x11_dpy, sr, oi->crtc);
                if (ci && ci->width > 0 && ci->height > 0) {
                    x11_monitor *m = &x11_monitors[x11_monitor_n++];
                    memset(m, 0, sizeof(*m));
                    m->output = sr->outputs[i];
                    m->crtc = oi->crtc;
                    m->x = ci->x; m->y = ci->y;
                    m->w = (int)ci->width; m->h = (int)ci->height;
                    snprintf(m->name, sizeof(m->name), "%s", oi->name ? oi->name : "");
                    m->primary = sr->outputs[i] == primary;
                }
                if (ci) XRRFreeCrtcInfo(ci);
            }
            if (oi) XRRFreeOutputInfo(oi);
        }
        if (sr) XRRFreeScreenResources(sr);
    }
    if (x11_monitor_n == 0) {   /* no RandR outputs (Xvfb, old servers): the screen is the monitor */
        x11_monitor *m = &x11_monitors[x11_monitor_n++];
        memset(m, 0, sizeof(*m));
        m->w = DisplayWidth(x11_dpy, x11_screen);
        m->h = DisplayHeight(x11_dpy, x11_screen);
        snprintf(m->name, sizeof(m->name), "Display");
        m->primary = 1;
    }
    /* primary first, as GLFW orders them */
    for (int i = 1; i < x11_monitor_n; i++) {
        if (x11_monitors[i].primary) {
            x11_monitor t = x11_monitors[0];
            x11_monitors[0] = x11_monitors[i];
            x11_monitors[i] = t;
            break;
        }
    }
    if (!x11_monitors[0].primary) x11_monitors[0].primary = 1;
}

static int x11_monitor_count(void)
{
    x11_monitors_refresh();
    return x11_monitor_n;
}

static int x11_primary_monitor(void)
{
    x11_monitors_refresh();
    return x11_monitor_n > 0 ? 0 : -1;
}

static float x11_content_scale_value(void)
{
    float dpi = 96.0f;
    if (!x11_open()) return 1.0f;
    char *rms = XResourceManagerString(x11_dpy);
    if (rms) {
        XrmInitialize();
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db) {
            XrmValue value;
            char *type = NULL;
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && type && strcmp(type, "String") == 0) {
                dpi = (float)atof(value.addr);
            }
            XrmDestroyDatabase(db);
        }
    }
    return dpi > 0.0f ? dpi / 96.0f : 1.0f;
}

static int x11_mode_refresh(const XRRModeInfo *mi)
{
    if (!mi->hTotal || !mi->vTotal) return 0;
    return (int)lround((double)mi->dotClock / ((double)mi->hTotal * (double)mi->vTotal));
}

static void x11_fill_bits(vio_video_mode *m)
{
    int depth = DefaultDepth(x11_dpy, x11_screen);
    int c = depth >= 24 ? 8 : depth == 16 ? 5 : depth / 3;
    m->red_bits = c;
    m->green_bits = depth == 16 ? 6 : c;
    m->blue_bits = c;
}

static int x11_monitor_desc(int index, vio_monitor_desc *out)
{
    memset(out, 0, sizeof(*out));
    x11_monitors_refresh();
    if (index < 0 || index >= x11_monitor_n) return -1;
    x11_monitor *m = &x11_monitors[index];
    snprintf(out->name, sizeof(out->name), "%s", m->name);
    out->x = m->x; out->y = m->y;
    out->work_x = m->x; out->work_y = m->y; out->work_width = m->w; out->work_height = m->h;
    /* the desktop's work area, intersected with the monitor */
    Atom type;
    int format;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(x11_dpy, x11_root, x11_net_workarea, 0, 4, False, XA_CARDINAL,
                           &type, &format, &n, &after, &data) == Success && data && n >= 4 && format == 32) {
        long *wa = (long *)data;
        int x0 = (int)wa[0] > m->x ? (int)wa[0] : m->x;
        int y0 = (int)wa[1] > m->y ? (int)wa[1] : m->y;
        int x1 = (int)(wa[0] + wa[2]) < m->x + m->w ? (int)(wa[0] + wa[2]) : m->x + m->w;
        int y1 = (int)(wa[1] + wa[3]) < m->y + m->h ? (int)(wa[1] + wa[3]) : m->y + m->h;
        if (x1 > x0 && y1 > y0) { out->work_x = x0; out->work_y = y0; out->work_width = x1 - x0; out->work_height = y1 - y0; }
    }
    if (data) XFree(data);
    out->scale_x = out->scale_y = x11_content_scale_value();
    out->mode.width = m->w;
    out->mode.height = m->h;
    out->mode.refresh_hz = 60;
    x11_fill_bits(&out->mode);
    if (x11_randr && m->crtc) {
        XRRScreenResources *sr = XRRGetScreenResourcesCurrent(x11_dpy, x11_root);
        XRRCrtcInfo *ci = sr ? XRRGetCrtcInfo(x11_dpy, sr, m->crtc) : NULL;
        for (int i = 0; ci && i < sr->nmode; i++) {
            if (sr->modes[i].id == ci->mode) out->mode.refresh_hz = x11_mode_refresh(&sr->modes[i]);
        }
        if (ci) XRRFreeCrtcInfo(ci);
        if (sr) XRRFreeScreenResources(sr);
    }
    out->primary = m->primary;
    return 0;
}

static int x11_mode_cmp(const void *a, const void *b)
{
    const vio_video_mode *x = (const vio_video_mode *)a, *y = (const vio_video_mode *)b;
    long ax = (long)x->width * x->height, ay = (long)y->width * y->height;
    if (ax != ay) return ax < ay ? -1 : 1;
    if (x->width != y->width) return x->width - y->width;
    return x->refresh_hz - y->refresh_hz;
}

static int x11_video_modes(int index, vio_video_mode *out, int max)
{
    x11_monitors_refresh();
    if (index < 0 || index >= x11_monitor_n || max <= 0) return 0;
    x11_monitor *m = &x11_monitors[index];
    int n = 0;
    if (x11_randr && m->output) {
        XRRScreenResources *sr = XRRGetScreenResourcesCurrent(x11_dpy, x11_root);
        XRROutputInfo *oi = sr ? XRRGetOutputInfo(x11_dpy, sr, m->output) : NULL;
        for (int i = 0; oi && i < oi->nmode && n < max; i++) {
            for (int k = 0; k < sr->nmode; k++) {
                const XRRModeInfo *mi = &sr->modes[k];
                if (mi->id != oi->modes[i] || (mi->modeFlags & RR_Interlace)) continue;
                vio_video_mode v;
                v.width = (int)mi->width; v.height = (int)mi->height; v.refresh_hz = x11_mode_refresh(mi);
                x11_fill_bits(&v);
                int dup = 0;
                for (int d = 0; d < n && !dup; d++) dup = memcmp(&out[d], &v, sizeof(v)) == 0;
                if (!dup) out[n++] = v;
            }
        }
        if (oi) XRRFreeOutputInfo(oi);
        if (sr) XRRFreeScreenResources(sr);
    }
    if (n == 0) {
        vio_monitor_desc md;
        if (x11_monitor_desc(index, &md) == 0) out[n++] = md.mode;
    }
    qsort(out, (size_t)n, sizeof(out[0]), x11_mode_cmp);
    return n;
}

/* ── Window lifecycle ─────────────────────────────────────────────── */

static int x11_keytable_ready;

static int x11_init(void)
{
    if (!x11_open()) return 0;   /* no display yet (MINIT without DISPLAY): create_window tries again */
    if (!x11_keytable_ready) {
        x11_keytable_init();
        x11_keytable_ready = 1;
    }
    return 1;
}

static void x11_shutdown(void)
{
    if (!x11_dpy) return;
    if (x11_im) { XCloseIM(x11_im); x11_im = NULL; }
    if (x11_blank_cursor) { XFreeCursor(x11_dpy, x11_blank_cursor); x11_blank_cursor = 0; }
    XCloseDisplay(x11_dpy);
    x11_dpy = NULL;
    x11_keytable_ready = 0;
}

static void x11_set_decorations(vio_x11_window *w, int on)
{
    struct { unsigned long flags, functions, decorations; long input_mode; unsigned long status; } hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = 2;   /* MWM_HINTS_DECORATIONS */
    hints.decorations = on ? 1 : 0;
    XChangeProperty(x11_dpy, w->win, x11_motif_hints, x11_motif_hints, 32, PropModeReplace,
                    (unsigned char *)&hints, sizeof(hints) / sizeof(long));
}

static void x11_wait_mapped(vio_x11_window *w)
{
    XEvent ev;
    for (int i = 0; i < 200; i++) {   /* at most ~1 s: a window manager may be slow to map */
        if (XCheckTypedWindowEvent(x11_dpy, w->win, MapNotify, &ev)) return;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

static vio_window_handle x11_create_window(vio_config *cfg, const char *backend_name)
{
    {
        if (!x11_init()) {
            php_error_docref(NULL, E_WARNING, "X11: cannot open display \"%s\"", getenv("DISPLAY") ? getenv("DISPLAY") : "");
            return NULL;
        }
    }
    vio_x11_window *w = (vio_x11_window *)calloc(1, sizeof(vio_x11_window));
    if (!w) return NULL;
    w->headless = cfg->headless;
    w->decorated = !cfg->headless;
    w->monitor = -1;
    w->fb_w = w->fb_h = -1;

    int width  = cfg->width  > 0 ? cfg->width  : 800;
    int height = cfg->height > 0 ? cfg->height : 600;
    if (!cfg->headless) {   /* GLFW_SCALE_TO_MONITOR */
        float s = x11_content_scale_value();
        width = (int)lroundf((float)width * s);
        height = (int)lroundf((float)height * s);
    }

    int is_gl = backend_name && strcmp(backend_name, "opengl") == 0;
    x11_GLXFBConfig fbc = NULL;
    Visual *visual = DefaultVisual(x11_dpy, x11_screen);
    int depth = DefaultDepth(x11_dpy, x11_screen);
    XVisualInfo *vi = NULL;
    if (is_gl) {
        fbc = x11_choose_fbconfig(cfg->samples);
        vi = fbc ? glx.GetVisualFromFBConfig(x11_dpy, fbc) : NULL;
        if (!vi) {
            php_error_docref(NULL, E_WARNING, "X11: no GLX framebuffer configuration (RGBA8, depth 24, stencil 8)");
            free(w);
            return NULL;
        }
        visual = vi->visual;
        depth = vi->depth;
    }
    w->cmap = XCreateColormap(x11_dpy, x11_root, visual, AllocNone);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = w->cmap;
    swa.border_pixel = 0;
    swa.event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask | PointerMotionMask | ButtonPressMask |
                     ButtonReleaseMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask | ExposureMask;
    x11_error_code = 0;
    w->win = XCreateWindow(x11_dpy, x11_root, 0, 0, (unsigned)width, (unsigned)height, 0, depth, InputOutput, visual,
                           CWBorderPixel | CWColormap | CWEventMask, &swa);
    if (vi) XFree(vi);
    XSync(x11_dpy, False);
    if (!w->win || x11_error_code) {
        php_error_docref(NULL, E_WARNING, "X11: XCreateWindow failed (error %d)", x11_error_code);
        if (w->cmap) XFreeColormap(x11_dpy, w->cmap);
        free(w);
        return NULL;
    }
    XSaveContext(x11_dpy, w->win, x11_ctx, (XPointer)w);
    XSetWMProtocols(x11_dpy, w->win, &x11_wm_delete, 1);
    const char *title = cfg->title ? cfg->title : "php-vio";
    Xutf8SetWMProperties(x11_dpy, w->win, title, title, NULL, 0, NULL, NULL, NULL);
    if (!w->decorated) x11_set_decorations(w, 0);
    if (x11_im) {
        w->ic = XCreateIC(x11_im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                          XNClientWindow, w->win, XNFocusWindow, w->win, NULL);
        if (w->ic) {
            unsigned long filter = 0;
            if (XGetICValues(w->ic, XNFilterEvents, &filter, NULL) == NULL) {
                XSelectInput(x11_dpy, w->win, swa.event_mask | (long)filter);
            }
        }
    }

    if (is_gl) {
        if (glx.CreateContextAttribsARB) {
            static const int ladder[][2] = { {4, 6}, {4, 5}, {4, 3}, {4, 1}, {3, 3}, {3, 2}, {3, 1}, {3, 0} };
            for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]) && !w->glrc; i++) {
                int core = ladder[i][0] * 10 + ladder[i][1] >= 32;
                int attrs[] = {
                    X11_GLX_CONTEXT_MAJOR, ladder[i][0], X11_GLX_CONTEXT_MINOR, ladder[i][1],
                    core ? X11_GLX_CONTEXT_PROFILE : None, X11_GLX_CONTEXT_CORE_BIT, None
                };
                x11_error_code = 0;
                w->glrc = glx.CreateContextAttribsARB(x11_dpy, fbc, NULL, True, attrs);
                XSync(x11_dpy, False);
                if (x11_error_code && w->glrc) { glx.DestroyContext(x11_dpy, w->glrc); w->glrc = NULL; }
            }
        }
        if (!w->glrc && glx.CreateNewContext) w->glrc = glx.CreateNewContext(x11_dpy, fbc, X11_GLX_RGBA_TYPE, NULL, True);
        int ok = w->glrc && glx.MakeCurrent(x11_dpy, w->win, w->glrc);
        if (ok) {
            /* the floor is 3.0, like GLFW's ladder */
            typedef const unsigned char *(*x11_glGetString_t)(unsigned int);
            x11_glGetString_t gs = (x11_glGetString_t)x11_gl_get_proc_address("glGetString");
            const char *ver = gs ? (const char *)gs(0x1F02) : NULL;
            ok = ver && atoi(ver) >= 3;
        }
        if (!ok) {
            php_error_docref(NULL, E_WARNING, "X11: no OpenGL context >= 3.0 available");
            if (w->glrc) { glx.MakeCurrent(x11_dpy, None, NULL); glx.DestroyContext(x11_dpy, w->glrc); }
            if (w->ic) XDestroyIC(w->ic);
            XDeleteContext(x11_dpy, w->win, x11_ctx);
            XDestroyWindow(x11_dpy, w->win);
            XFreeColormap(x11_dpy, w->cmap);
            free(w);
            return NULL;
        }
        if (glx.SwapIntervalEXT) glx.SwapIntervalEXT(x11_dpy, w->win, cfg->vsync ? 1 : 0);
        else if (glx.SwapIntervalMESA) glx.SwapIntervalMESA(cfg->vsync ? 1 : 0);
        else if (glx.SwapIntervalSGI && cfg->vsync) glx.SwapIntervalSGI(1);
    }

    if (!cfg->headless) {
        /* centre on the primary monitor */
        vio_monitor_desc md;
        if (x11_monitor_desc(0, &md) == 0) {
            XMoveWindow(x11_dpy, w->win, md.work_x + (md.work_width - width) / 2, md.work_y + (md.work_height - height) / 2);
        }
        XMapRaised(x11_dpy, w->win);
        x11_wait_mapped(w);
    }
    XFlush(x11_dpy);
    w->fb_w = width;
    w->fb_h = height;
    return w;
}

static void x11_restore_mode(vio_x11_window *w)
{
    if (!w->saved_crtc || !x11_randr) return;
    XRRScreenResources *sr = XRRGetScreenResourcesCurrent(x11_dpy, x11_root);
    XRRCrtcInfo *ci = sr ? XRRGetCrtcInfo(x11_dpy, sr, w->saved_crtc) : NULL;
    if (ci) {
        XRRSetCrtcConfig(x11_dpy, sr, w->saved_crtc, CurrentTime, ci->x, ci->y, w->saved_mode, ci->rotation, ci->outputs, ci->noutput);
        XRRFreeCrtcInfo(ci);
    }
    if (sr) XRRFreeScreenResources(sr);
    w->saved_crtc = 0;
}

static void x11_destroy_window(vio_window_handle h)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w || !x11_dpy) { free(w); return; }
    x11_restore_mode(w);
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) XUngrabPointer(x11_dpy, CurrentTime);
    if (w->glrc) {
        if (glx.GetCurrentContext && glx.GetCurrentContext() == w->glrc) glx.MakeCurrent(x11_dpy, None, NULL);
        glx.DestroyContext(x11_dpy, w->glrc);
    }
    if (w->ic) XDestroyIC(w->ic);
    XDeleteContext(x11_dpy, w->win, x11_ctx);
    XDestroyWindow(x11_dpy, w->win);
    XFreeColormap(x11_dpy, w->cmap);
    XFlush(x11_dpy);
    free(w);
}

static int  x11_should_close(vio_window_handle h) { return h ? ((vio_x11_window *)h)->should_close : 1; }
static void x11_set_should_close(vio_window_handle h, int v) { if (h) ((vio_x11_window *)h)->should_close = v; }

/* ── Events ───────────────────────────────────────────────────────── */

static void x11_utf8_chars(vio_x11_window *w, const char *s, int n)
{
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        unsigned int cp;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) break;
        cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        if (cp >= 32 && cp != 127 && !(cp >= 0x80 && cp < 0xA0)) vio_input_emit_char(x11_input(w), cp);
    }
}

static void x11_key_event(vio_x11_window *w, XKeyEvent *ke, int press)
{
    int kc = (int)ke->keycode;
    int key = (kc >= 0 && kc < 256) ? x11_keycodes[kc] : VIO_KEY_UNKNOWN;
    int mods = x11_mods(ke->state);
    if (press) {
        int action = (kc < 256 && w->keys_down[kc]) ? VIO_REPEAT : VIO_PRESS;
        if (kc < 256) w->keys_down[kc] = 1;
        vio_input_key_event(x11_input(w), key, action, mods);
        char buf[64];
        KeySym ks = 0;
        int n;
        if (w->ic) {
            Status st = 0;
            n = Xutf8LookupString(w->ic, ke, buf, sizeof(buf) - 1, &ks, &st);
            if (st != XLookupChars && st != XLookupBoth) n = 0;
        } else {
            n = XLookupString(ke, buf, sizeof(buf) - 1, &ks, NULL);   /* Latin-1 */
            for (int i = 0; i < n; i++) {
                unsigned char c = (unsigned char)buf[i];
                if (c >= 32 && c != 127 && !(c >= 0x80 && c < 0xA0)) vio_input_emit_char(x11_input(w), c);
            }
            n = 0;
        }
        if (n > 0) x11_utf8_chars(w, buf, n);
    } else {
        if (kc < 256) w->keys_down[kc] = 0;
        vio_input_key_event(x11_input(w), key, VIO_RELEASE, mods);
    }
}

static void x11_button_event(vio_x11_window *w, XButtonEvent *be, int press)
{
    unsigned int b = be->button;
    if (b >= Button4 && b <= Button4 + 3) {   /* 4/5 vertical, 6/7 horizontal wheel; press only */
        if (!press) return;
        double dx = b == 6 ? 1.0 : b == 7 ? -1.0 : 0.0, dy = b == Button4 ? 1.0 : b == Button5 ? -1.0 : 0.0;
        vio_input_scroll_event(x11_input(w), dx, dy);
        return;
    }
    int idx = b == Button1 ? 0 : b == Button2 ? 2 : b == Button3 ? 1 : (int)b - 8 + 3;   /* 8, 9 -> buttons 4, 5 */
    if (idx < 0 || idx > VIO_MOUSE_LAST) return;
    vio_input_button_event(x11_input(w), idx, press ? VIO_PRESS : VIO_RELEASE);
}

static void x11_warp_center(vio_x11_window *w)
{
    XWindowAttributes wa;
    XGetWindowAttributes(x11_dpy, w->win, &wa);
    w->warp_x = wa.width / 2;
    w->warp_y = wa.height / 2;
    XWarpPointer(x11_dpy, None, w->win, 0, 0, 0, 0, w->warp_x, w->warp_y);
    XFlush(x11_dpy);
}

static void x11_motion_event(vio_x11_window *w, XMotionEvent *me)
{
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) {
        if (me->x == w->warp_x && me->y == w->warp_y) return;   /* our own warp */
        w->cursor_x += me->x - w->warp_x;
        w->cursor_y += me->y - w->warp_y;
        vio_input_cursor_event(x11_input(w), w->cursor_x, w->cursor_y);
        x11_warp_center(w);
        return;
    }
    w->cursor_x = me->x;
    w->cursor_y = me->y;
    vio_input_cursor_event(x11_input(w), (double)me->x, (double)me->y);
}

static void x11_dispatch(XEvent *ev)
{
    if (XFilterEvent(ev, None)) return;   /* input method */
    vio_x11_window *w = NULL;
    if (XFindContext(x11_dpy, ev->xany.window, x11_ctx, (XPointer *)&w) != 0 || !w) return;
    switch (ev->type) {
        case KeyPress:      x11_key_event(w, &ev->xkey, 1); break;
        case KeyRelease:    x11_key_event(w, &ev->xkey, 0); break;
        case ButtonPress:   x11_button_event(w, &ev->xbutton, 1); break;
        case ButtonRelease: x11_button_event(w, &ev->xbutton, 0); break;
        case MotionNotify:  x11_motion_event(w, &ev->xmotion); break;
        case ConfigureNotify:
            if (ev->xconfigure.width != w->fb_w || ev->xconfigure.height != w->fb_h) {
                w->fb_w = ev->xconfigure.width;
                w->fb_h = ev->xconfigure.height;
                vio_input_resize_event(w->input, w->fb_w, w->fb_h);
            }
            break;
        case ClientMessage:
            if (ev->xclient.message_type == x11_wm_protocols && (Atom)ev->xclient.data.l[0] == x11_wm_delete) w->should_close = 1;
            break;
        case FocusIn:
            if (w->ic) XSetICFocus(w->ic);
            if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) x11_warp_center(w);
            break;
        case FocusOut:
            if (w->ic) XUnsetICFocus(w->ic);
            if (w->monitor >= 0 && w->auto_iconify) XIconifyWindow(x11_dpy, w->win, x11_screen);
            break;
        default:
            break;
    }
}

static void x11_poll_events(void)
{
    if (!x11_dpy) return;
    /* A round trip first: every event the server generated before it is in
     * the queue afterwards. XPending alone left events another client had
     * sent (XSendEvent) for the next poll (test 214). */
    XSync(x11_dpy, False);
    while (XPending(x11_dpy)) {
        XEvent ev;
        XNextEvent(x11_dpy, &ev);
        x11_dispatch(&ev);
    }
}

static void x11_wait_events(void)
{
    if (!x11_dpy) return;
    XEvent ev;
    XNextEvent(x11_dpy, &ev);
    x11_dispatch(&ev);
    x11_poll_events();
}

/* ── Sizes, GL ────────────────────────────────────────────────────── */

static void x11_swap_buffers(vio_window_handle h)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (w && w->glrc) glx.SwapBuffers(x11_dpy, w->win);
}

static void x11_get_size(vio_window_handle h, int *cw, int *ch)
{
    vio_x11_window *w = (vio_x11_window *)h;
    XWindowAttributes wa;
    memset(&wa, 0, sizeof(wa));
    if (w && x11_dpy) XGetWindowAttributes(x11_dpy, w->win, &wa);
    if (cw) *cw = wa.width;
    if (ch) *ch = wa.height;
}

static void x11_get_content_scale(vio_window_handle h, float *sx, float *sy)
{
    (void)h;
    float s = x11_content_scale_value();
    if (sx) *sx = s;
    if (sy) *sy = s;
}

static void x11_get_cursor_pos(vio_window_handle h, double *x, double *y)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w || !x11_dpy) return;
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) {
        if (x) *x = w->cursor_x;
        if (y) *y = w->cursor_y;
        return;
    }
    Window root, child;
    int rx, ry, wx = 0, wy = 0;
    unsigned int mask;
    XQueryPointer(x11_dpy, w->win, &root, &child, &rx, &ry, &wx, &wy, &mask);
    if (x) *x = wx;
    if (y) *y = wy;
}

static int x11_gl_make_current(vio_window_handle h)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!x11_libgl) return -1;
    if (!w) return glx.MakeCurrent(x11_dpy, None, NULL) ? 0 : -1;
    return glx.MakeCurrent(x11_dpy, w->win, w->glrc) ? 0 : -1;
}

static void x11_gl_swap_interval(int interval)
{
    if (!x11_libgl) return;
    if (glx.SwapIntervalEXT && glx.GetCurrentDrawable) {
        Drawable d = glx.GetCurrentDrawable();
        if (d) { glx.SwapIntervalEXT(x11_dpy, d, interval); return; }
    }
    if (glx.SwapIntervalMESA) glx.SwapIntervalMESA((unsigned)interval);
    else if (glx.SwapIntervalSGI && interval > 0) glx.SwapIntervalSGI(interval);
}

/* ── Window properties ────────────────────────────────────────────── */

static void x11_set_title(vio_window_handle h, const char *utf8)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w || !utf8) return;
    Xutf8SetWMProperties(x11_dpy, w->win, utf8, utf8, NULL, 0, NULL, NULL, NULL);
    XFlush(x11_dpy);
}

static void x11_set_window_size(vio_window_handle h, int cw, int ch)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w || w->monitor >= 0 || cw <= 0 || ch <= 0) return;
    XResizeWindow(x11_dpy, w->win, (unsigned)cw, (unsigned)ch);
    XSync(x11_dpy, False);
}

static void x11_get_window_pos(vio_window_handle h, int *x, int *y)
{
    vio_x11_window *w = (vio_x11_window *)h;
    int rx = 0, ry = 0;
    Window child;
    if (w) XTranslateCoordinates(x11_dpy, w->win, x11_root, 0, 0, &rx, &ry, &child);
    if (x) *x = rx;
    if (y) *y = ry;
}

static void x11_set_window_pos(vio_window_handle h, int x, int y)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w || w->monitor >= 0) return;
    XMoveWindow(x11_dpy, w->win, x, y);
    XFlush(x11_dpy);
}

static int x11_has_state(vio_x11_window *w, Atom a)
{
    Atom type;
    int format;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    int found = 0;
    if (XGetWindowProperty(x11_dpy, w->win, x11_net_wm_state, 0, 64, False, XA_ATOM,
                           &type, &format, &n, &after, &data) == Success && data) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) found |= atoms[i] == a;
    }
    if (data) XFree(data);
    return found;
}

/* _NET_WM_STATE request to the window manager (add = 1 / remove = 0). */
static void x11_send_state(vio_x11_window *w, int add, Atom a, Atom b)
{
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = w->win;
    ev.xclient.message_type = x11_net_wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = add ? 1 : 0;
    ev.xclient.data.l[1] = (long)a;
    ev.xclient.data.l[2] = (long)b;
    ev.xclient.data.l[3] = 1;
    XSendEvent(x11_dpy, x11_root, False, SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(x11_dpy);
}

static int x11_get_attrib(vio_window_handle h, vio_window_attrib a)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w) return 0;
    switch (a) {
        case VIO_WINDOW_DECORATED:    return w->decorated;
        case VIO_WINDOW_MAXIMIZED:    return x11_has_state(w, x11_net_max_h) && x11_has_state(w, x11_net_max_v);
        case VIO_WINDOW_AUTO_ICONIFY: return w->auto_iconify;
        case VIO_WINDOW_FOCUSED: {
            Window focus;
            int revert;
            XGetInputFocus(x11_dpy, &focus, &revert);
            return focus == w->win;
        }
        case VIO_WINDOW_VISIBLE: {
            XWindowAttributes wa;
            XGetWindowAttributes(x11_dpy, w->win, &wa);
            return wa.map_state == IsViewable;
        }
    }
    return 0;
}

static void x11_set_attrib(vio_window_handle h, vio_window_attrib a, int value)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w) return;
    switch (a) {
        case VIO_WINDOW_DECORATED:
            w->decorated = value ? 1 : 0;
            if (w->monitor < 0) x11_set_decorations(w, w->decorated);
            XFlush(x11_dpy);
            break;
        case VIO_WINDOW_AUTO_ICONIFY:
            w->auto_iconify = value ? 1 : 0;
            break;
        case VIO_WINDOW_VISIBLE:
            if (value) XMapWindow(x11_dpy, w->win); else XUnmapWindow(x11_dpy, w->win);
            XFlush(x11_dpy);
            break;
        default:
            break;
    }
}

static void x11_maximize(vio_window_handle h) { if (h) x11_send_state((vio_x11_window *)h, 1, x11_net_max_h, x11_net_max_v); }
static void x11_restore(vio_window_handle h)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w) return;
    x11_send_state(w, 0, x11_net_max_h, x11_net_max_v);
    XMapWindow(x11_dpy, w->win);   /* also de-iconifies */
    XFlush(x11_dpy);
}

static void x11_set_cursor_mode(vio_window_handle h, int mode)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w || w->cursor_mode == mode) return;
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) XUngrabPointer(x11_dpy, CurrentTime);
    if (mode == VIO_PLATFORM_CURSOR_NORMAL) XUndefineCursor(x11_dpy, w->win);
    else XDefineCursor(x11_dpy, w->win, x11_blank_cursor);
    if (mode == VIO_PLATFORM_CURSOR_DISABLED) {
        double x = 0, y = 0;
        w->cursor_mode = VIO_PLATFORM_CURSOR_NORMAL;
        x11_get_cursor_pos(w, &x, &y);
        w->cursor_x = x;
        w->cursor_y = y;
        XGrabPointer(x11_dpy, w->win, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, w->win, x11_blank_cursor, CurrentTime);
        x11_warp_center(w);
    }
    w->cursor_mode = mode;
    XFlush(x11_dpy);
}

static int x11_window_monitor(vio_window_handle h)
{
    return h ? ((vio_x11_window *)h)->monitor : -1;
}

static void x11_set_window_monitor(vio_window_handle h, int monitor, int x, int y, int width, int height, int refresh_hz)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w) return;
    if (monitor >= 0) {
        x11_monitors_refresh();
        if (monitor >= x11_monitor_n) monitor = 0;
        x11_monitor *m = &x11_monitors[monitor];
        /* the closest XRandR mode, when the requested one differs from the current */
        if (x11_randr && m->crtc && (width != m->w || height != m->h)) {
            XRRScreenResources *sr = XRRGetScreenResourcesCurrent(x11_dpy, x11_root);
            XRROutputInfo *oi = sr ? XRRGetOutputInfo(x11_dpy, sr, m->output) : NULL;
            XRRCrtcInfo *ci = sr ? XRRGetCrtcInfo(x11_dpy, sr, m->crtc) : NULL;
            RRMode best = 0;
            long best_score = -1;
            for (int i = 0; oi && i < oi->nmode; i++) {
                for (int k = 0; k < sr->nmode; k++) {
                    const XRRModeInfo *mi = &sr->modes[k];
                    if (mi->id != oi->modes[i] || (mi->modeFlags & RR_Interlace)) continue;
                    long score = labs((long)mi->width - width) * 1000 + labs((long)mi->height - height) * 1000
                               + (refresh_hz > 0 ? labs((long)x11_mode_refresh(mi) - refresh_hz) : 0);
                    if (best_score < 0 || score < best_score) { best_score = score; best = mi->id; }
                }
            }
            if (ci && best && best != ci->mode) {
                if (!w->saved_crtc) { w->saved_crtc = m->crtc; w->saved_mode = ci->mode; }
                XRRSetCrtcConfig(x11_dpy, sr, m->crtc, CurrentTime, ci->x, ci->y, best, ci->rotation, ci->outputs, ci->noutput);
            }
            if (ci) XRRFreeCrtcInfo(ci);
            if (oi) XRRFreeOutputInfo(oi);
            if (sr) XRRFreeScreenResources(sr);
            x11_monitors_refresh();
            m = &x11_monitors[monitor];
        }
        w->monitor = monitor;
        x11_set_decorations(w, 0);
        XMoveResizeWindow(x11_dpy, w->win, m->x, m->y, (unsigned)m->w, (unsigned)m->h);
        XMapRaised(x11_dpy, w->win);
        x11_send_state(w, 1, x11_net_fullscreen, None);
        XSync(x11_dpy, False);
        return;
    }
    x11_send_state(w, 0, x11_net_fullscreen, None);
    x11_restore_mode(w);
    w->monitor = -1;
    x11_set_decorations(w, w->decorated);
    XMoveResizeWindow(x11_dpy, w->win, x, y, (unsigned)(width > 0 ? width : 1), (unsigned)(height > 0 ? height : 1));
    XSync(x11_dpy, False);
}

/* ── Gamepads: evdev ──────────────────────────────────────────────── */

#define X11_PADS 16
#define X11_BITS(n) (((n) + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long)))

typedef struct {
    int           fd;
    char          path[64];
    char          name[128];
    unsigned char buttons[15];
    float         axes[6];
    int           hat_x, hat_y;
    struct input_absinfo abs[ABS_CNT];
    int           has_abs[ABS_CNT];
} x11_pad;

static x11_pad x11_pads[X11_PADS];
static double  x11_pad_scan_time = -10.0;

static double x11_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int x11_test_bit(const unsigned long *bits, int n)
{
    return (bits[n / (8 * sizeof(unsigned long))] >> (n % (8 * sizeof(unsigned long)))) & 1;
}

static void x11_pad_close(x11_pad *p)
{
    if (p->fd > 0) close(p->fd);
    memset(p, 0, sizeof(*p));
}

/* Opens new gamepads at most once a second (vio_gamepads() asks every frame). */
static void x11_pads_scan(void)
{
    double now = x11_now();
    if (now - x11_pad_scan_time < 1.0) return;
    x11_pad_scan_time = now;
    DIR *dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int known = 0;
        for (int i = 0; i < X11_PADS && !known; i++) known = x11_pads[i].fd > 0 && strcmp(x11_pads[i].path, path) == 0;
        if (known) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[X11_BITS(KEY_CNT)], abs[X11_BITS(ABS_CNT)];
        memset(keys, 0, sizeof(keys));
        memset(abs, 0, sizeof(abs));
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
        if (!x11_test_bit(keys, BTN_GAMEPAD) || !x11_test_bit(abs, ABS_X)) { close(fd); continue; }
        int slot = -1;
        for (int i = 0; i < X11_PADS && slot < 0; i++) if (x11_pads[i].fd <= 0) slot = i;
        if (slot < 0) { close(fd); break; }
        x11_pad *p = &x11_pads[slot];
        memset(p, 0, sizeof(*p));
        p->fd = fd;
        snprintf(p->path, sizeof(p->path), "%s", path);
        if (ioctl(fd, EVIOCGNAME(sizeof(p->name) - 1), p->name) < 0) snprintf(p->name, sizeof(p->name), "Gamepad");
        for (int a = 0; a < ABS_CNT; a++) {
            if (x11_test_bit(abs, a) && ioctl(fd, EVIOCGABS(a), &p->abs[a]) == 0) p->has_abs[a] = 1;
        }
        p->axes[4] = p->axes[5] = -1.0f;   /* released triggers */
    }
    closedir(dir);
}

static float x11_pad_norm(x11_pad *p, int code, int value)
{
    const struct input_absinfo *ai = &p->abs[code];
    if (ai->maximum == ai->minimum) return 0.0f;
    float v = 2.0f * (float)(value - ai->minimum) / (float)(ai->maximum - ai->minimum) - 1.0f;
    return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

/* Kernel gamepad layout (xpad convention: BTN_X is X, BTN_Y is Y) -> VIO_GAMEPAD_* order. */
static void x11_pad_key(x11_pad *p, int code, int value)
{
    int b = -1;
    switch (code) {
        case BTN_A: b = 0; break;       case BTN_B: b = 1; break;
        case BTN_X: b = 2; break;       case BTN_Y: b = 3; break;
        case BTN_TL: b = 4; break;      case BTN_TR: b = 5; break;
        case BTN_SELECT: b = 6; break;  case BTN_START: b = 7; break;
        case BTN_MODE: b = 8; break;    case BTN_THUMBL: b = 9; break;
        case BTN_THUMBR: b = 10; break;
        case BTN_DPAD_UP: b = 11; break; case BTN_DPAD_RIGHT: b = 12; break;
        case BTN_DPAD_DOWN: b = 13; break; case BTN_DPAD_LEFT: b = 14; break;
    }
    if (b >= 0) p->buttons[b] = value ? 1 : 0;
}

static void x11_pad_abs(x11_pad *p, int code, int value)
{
    switch (code) {
        case ABS_X:  p->axes[0] = x11_pad_norm(p, code, value); break;
        case ABS_Y:  p->axes[1] = x11_pad_norm(p, code, value); break;   /* down positive: up = -1 */
        case ABS_RX: p->axes[2] = x11_pad_norm(p, code, value); break;
        case ABS_RY: p->axes[3] = x11_pad_norm(p, code, value); break;
        case ABS_Z: case ABS_BRAKE: p->axes[4] = x11_pad_norm(p, code, value); break;
        case ABS_RZ: case ABS_GAS:  p->axes[5] = x11_pad_norm(p, code, value); break;
        case ABS_HAT0X: p->hat_x = value; p->buttons[14] = value < 0; p->buttons[12] = value > 0; break;
        case ABS_HAT0Y: p->hat_y = value; p->buttons[11] = value < 0; p->buttons[13] = value > 0; break;
    }
}

/* Drain the pad's pending events; 0 when it is gone. */
static int x11_pad_update(int id)
{
    if (id < 0 || id >= X11_PADS) return 0;
    x11_pads_scan();
    x11_pad *p = &x11_pads[id];
    if (p->fd <= 0) return 0;
    struct input_event ev[32];
    for (;;) {
        ssize_t n = read(p->fd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) break;
            x11_pad_close(p);   /* unplugged */
            return 0;
        }
        if (n == 0) break;
        for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
            if (ev[i].type == EV_KEY) x11_pad_key(p, ev[i].code, ev[i].value);
            else if (ev[i].type == EV_ABS) x11_pad_abs(p, ev[i].code, ev[i].value);
        }
    }
    return 1;
}

static int x11_joystick_present(int id) { return x11_pad_update(id); }

static const char *x11_pad_name(int id)
{
    return x11_pad_update(id) ? x11_pads[id].name : NULL;
}

static int x11_gamepad_state(int id, unsigned char buttons[15], float axes[6])
{
    if (!x11_pad_update(id)) return 0;
    memcpy(buttons, x11_pads[id].buttons, 15);
    memcpy(axes, x11_pads[id].axes, sizeof(float) * 6);
    return 1;
}

static const unsigned char *x11_joystick_buttons(int id, int *count)
{
    *count = 0;
    if (!x11_pad_update(id)) return NULL;
    *count = 15;
    return x11_pads[id].buttons;
}

static const float *x11_joystick_axes(int id, int *count)
{
    *count = 0;
    if (!x11_pad_update(id)) return NULL;
    *count = 6;
    return x11_pads[id].axes;
}

/* ── Native handles, Vulkan, input ────────────────────────────────── */

static void *x11_native_handle(vio_window_handle h, vio_native_kind kind)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w) return NULL;
    if (kind == VIO_NATIVE_XLIB_WINDOW) return (void *)(uintptr_t)w->win;
    if (kind == VIO_NATIVE_XLIB_DISPLAY) return (void *)x11_dpy;
    return NULL;
}

#ifdef HAVE_VULKAN
static const char **x11_vk_instance_extensions(uint32_t *count)
{
    static const char *ext[] = { "VK_KHR_surface", "VK_KHR_xlib_surface" };
    *count = 2;
    return ext;
}

static int x11_vk_create_surface(vio_window_handle h, void *instance, void *out_surface)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (!w) return (int)VK_ERROR_INITIALIZATION_FAILED;
    VkXlibSurfaceCreateInfoKHR ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    ci.dpy = x11_dpy;
    ci.window = w->win;
    return (int)vkCreateXlibSurfaceKHR((VkInstance)instance, &ci, NULL, (VkSurfaceKHR *)out_surface);
}
#endif

static void x11_install_input(vio_window_handle h, vio_input_state *state)
{
    vio_x11_window *w = (vio_x11_window *)h;
    if (w) w->input = state;
}

static const vio_platform vio_platform_x11 = {
    .name                 = "x11",
    .init                 = x11_init,
    .shutdown             = x11_shutdown,
    .create_window        = x11_create_window,
    .destroy_window       = x11_destroy_window,
    .should_close         = x11_should_close,
    .set_should_close     = x11_set_should_close,
    .poll_events          = x11_poll_events,
    .wait_events          = x11_wait_events,
    .swap_buffers         = x11_swap_buffers,
    .get_framebuffer_size = x11_get_size,
    .get_window_size      = x11_get_size,   /* pixels on X11, as with GLFW */
    .get_content_scale    = x11_get_content_scale,
    .get_cursor_pos       = x11_get_cursor_pos,
    .gl_make_current      = x11_gl_make_current,
    .gl_swap_interval     = x11_gl_swap_interval,
    .gl_get_proc_address  = x11_gl_get_proc_address,
    .set_title            = x11_set_title,
    .set_window_size      = x11_set_window_size,
    .get_window_pos       = x11_get_window_pos,
    .set_window_pos       = x11_set_window_pos,
    .get_attrib           = x11_get_attrib,
    .set_attrib           = x11_set_attrib,
    .maximize             = x11_maximize,
    .restore              = x11_restore,
    .set_cursor_mode      = x11_set_cursor_mode,
    .window_monitor       = x11_window_monitor,
    .set_window_monitor   = x11_set_window_monitor,
    .monitor_count        = x11_monitor_count,
    .primary_monitor      = x11_primary_monitor,
    .monitor_desc         = x11_monitor_desc,
    .video_modes          = x11_video_modes,
    .joystick_present     = x11_joystick_present,
    .joystick_is_gamepad  = x11_joystick_present,
    .joystick_name        = x11_pad_name,
    .gamepad_name         = x11_pad_name,
    .gamepad_state        = x11_gamepad_state,
    .joystick_buttons     = x11_joystick_buttons,
    .joystick_axes        = x11_joystick_axes,
    .native_handle        = x11_native_handle,
#ifdef HAVE_VULKAN
    .vk_instance_extensions = x11_vk_instance_extensions,
    .vk_create_surface      = x11_vk_create_surface,
#endif
    .install_input        = x11_install_input,
};

void vio_platform_x11_register(void)
{
    vio_register_platform(&vio_platform_x11);
}

#endif /* HAVE_X11 */
