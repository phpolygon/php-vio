/*
 * php-vio - Win32 platform (WIN32-PLATFORM-PLAN Phase 1, OPEN-ITEMS A2)
 *
 * Windows without GLFW: a window class with its own WndProc (keyboard via a
 * scancode table, text with surrogate pairs, mouse, wheel, raw input while the
 * cursor is captured, resize, DPI changes), a native GL context through WGL
 * (dummy-window trick, the 4.6 -> 3.0 version ladder, the combined
 * wglGetProcAddress / opengl32.dll loader), monitors and exclusive fullscreen
 * through EnumDisplayMonitors / ChangeDisplaySettingsExW, gamepads through
 * XInput (loaded at run time) and the Vulkan surface through
 * vkCreateWin32SurfaceKHR. The slots keep GLFW's Win32 semantics - window
 * sizes and cursor positions in physical pixels under per-monitor DPI
 * awareness V2, content scale = DPI / 96, gamepad Y axes up = -1, triggers
 * -1..1 - so everything above the vtable behaves the same.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_VULKAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#endif

#include "../../../include/vio_platform.h"
#include "../../../include/vio_constants.h"
#include "../../vio_input.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

/* ── Window state ─────────────────────────────────────────────────── */

typedef struct _vio_win32_window {
    HWND             hwnd;
    HDC              hdc;
    HGLRC            glrc;
    int              should_close;
    int              headless;
    int              decorated;
    int              auto_iconify;
    int              cursor_mode;
    int              raw_motion;
    double           cursor_x, cursor_y;       /* client pixels (virtual while DISABLED) */
    int              last_raw_x, last_raw_y;
    int              fb_w, fb_h;               /* last size reported to the resize callback */
    int              monitor;                  /* fullscreen monitor index, -1 windowed */
    DEVMODEW         monitor_saved_mode;
    WCHAR            monitor_device[32];
    vio_input_state *input;
} vio_win32_window;

static HINSTANCE        w32_instance;
static ATOM             w32_class;
static int              w32_initialized;
static const WCHAR      w32_class_name[] = L"vio_window";

/* ── DPI ──────────────────────────────────────────────────────────── */

typedef BOOL (WINAPI *w32_SetProcessDpiAwarenessContext_t)(HANDLE);
typedef UINT (WINAPI *w32_GetDpiForWindow_t)(HWND);
typedef BOOL (WINAPI *w32_AdjustWindowRectExForDpi_t)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HRESULT (WINAPI *w32_GetDpiForMonitor_t)(HMONITOR, int, UINT *, UINT *);

static w32_GetDpiForWindow_t          w32_GetDpiForWindow;
static w32_AdjustWindowRectExForDpi_t w32_AdjustWindowRectExForDpi;
static w32_GetDpiForMonitor_t         w32_GetDpiForMonitor;

/* Per-monitor DPI awareness V2 before any window exists: sizes are physical
 * pixels and nothing is stretched by DWM (the same call the GLFW platform makes). */
static void w32_dpi_init(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        w32_SetProcessDpiAwarenessContext_t set = (w32_SetProcessDpiAwarenessContext_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (set) set((HANDLE)-4);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        w32_GetDpiForWindow = (w32_GetDpiForWindow_t)GetProcAddress(user32, "GetDpiForWindow");
        w32_AdjustWindowRectExForDpi = (w32_AdjustWindowRectExForDpi_t)GetProcAddress(user32, "AdjustWindowRectExForDpi");
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) w32_GetDpiForMonitor = (w32_GetDpiForMonitor_t)GetProcAddress(shcore, "GetDpiForMonitor");
}

static UINT w32_monitor_dpi(HMONITOR m)
{
    UINT x = 96, y = 96;
    if (m && w32_GetDpiForMonitor && SUCCEEDED(w32_GetDpiForMonitor(m, 0 /* MDT_EFFECTIVE_DPI */, &x, &y))) return x;
    HDC dc = GetDC(NULL);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(NULL, dc);
    return d ? d : 96;
}

static UINT w32_window_dpi(HWND h)
{
    if (h && w32_GetDpiForWindow) {
        UINT d = w32_GetDpiForWindow(h);
        if (d) return d;
    }
    return w32_monitor_dpi(h ? MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) : NULL);
}

static DWORD w32_style(const vio_win32_window *w)
{
    if (w->monitor >= 0 || w->headless || !w->decorated) return WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    return WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
}

static void w32_adjust(RECT *r, DWORD style, UINT dpi)
{
    if (w32_AdjustWindowRectExForDpi) w32_AdjustWindowRectExForDpi(r, style, FALSE, 0, dpi);
    else AdjustWindowRectEx(r, style, FALSE, 0);
}

/* ── Keyboard: scancode -> VIO_KEY_* (GLFW's Win32 table) ─────────── */

static short w32_keycodes[512];

static void w32_keytable_init(void)
{
    for (int i = 0; i < 512; i++) w32_keycodes[i] = VIO_KEY_UNKNOWN;
    static const struct { int sc, key; } t[] = {
        {0x00B, VIO_KEY_0}, {0x002, VIO_KEY_1}, {0x003, VIO_KEY_2}, {0x004, VIO_KEY_3}, {0x005, VIO_KEY_4},
        {0x006, VIO_KEY_5}, {0x007, VIO_KEY_6}, {0x008, VIO_KEY_7}, {0x009, VIO_KEY_8}, {0x00A, VIO_KEY_9},
        {0x01E, VIO_KEY_A}, {0x030, VIO_KEY_B}, {0x02E, VIO_KEY_C}, {0x020, VIO_KEY_D}, {0x012, VIO_KEY_E},
        {0x021, VIO_KEY_F}, {0x022, VIO_KEY_G}, {0x023, VIO_KEY_H}, {0x017, VIO_KEY_I}, {0x024, VIO_KEY_J},
        {0x025, VIO_KEY_K}, {0x026, VIO_KEY_L}, {0x032, VIO_KEY_M}, {0x031, VIO_KEY_N}, {0x018, VIO_KEY_O},
        {0x019, VIO_KEY_P}, {0x010, VIO_KEY_Q}, {0x013, VIO_KEY_R}, {0x01F, VIO_KEY_S}, {0x014, VIO_KEY_T},
        {0x016, VIO_KEY_U}, {0x02F, VIO_KEY_V}, {0x011, VIO_KEY_W}, {0x02D, VIO_KEY_X}, {0x015, VIO_KEY_Y},
        {0x02C, VIO_KEY_Z},
        {0x028, VIO_KEY_APOSTROPHE}, {0x02B, VIO_KEY_BACKSLASH}, {0x033, VIO_KEY_COMMA}, {0x00D, VIO_KEY_EQUAL},
        {0x029, VIO_KEY_GRAVE_ACCENT}, {0x01A, VIO_KEY_LEFT_BRACKET}, {0x00C, VIO_KEY_MINUS}, {0x034, VIO_KEY_PERIOD},
        {0x01B, VIO_KEY_RIGHT_BRACKET}, {0x027, VIO_KEY_SEMICOLON}, {0x035, VIO_KEY_SLASH},
        {0x056, 162 /* WORLD_2 */},
        {0x00E, VIO_KEY_BACKSPACE}, {0x153, VIO_KEY_DELETE}, {0x14F, VIO_KEY_END}, {0x01C, VIO_KEY_ENTER},
        {0x001, VIO_KEY_ESCAPE}, {0x147, VIO_KEY_HOME}, {0x152, VIO_KEY_INSERT}, {0x15D, VIO_KEY_MENU},
        {0x151, VIO_KEY_PAGE_DOWN}, {0x149, VIO_KEY_PAGE_UP}, {0x045, VIO_KEY_PAUSE}, {0x039, VIO_KEY_SPACE},
        {0x00F, VIO_KEY_TAB}, {0x03A, VIO_KEY_CAPS_LOCK}, {0x145, VIO_KEY_NUM_LOCK}, {0x046, VIO_KEY_SCROLL_LOCK},
        {0x03B, VIO_KEY_F1}, {0x03C, VIO_KEY_F2}, {0x03D, VIO_KEY_F3}, {0x03E, VIO_KEY_F4}, {0x03F, VIO_KEY_F5},
        {0x040, VIO_KEY_F6}, {0x041, VIO_KEY_F7}, {0x042, VIO_KEY_F8}, {0x043, VIO_KEY_F9}, {0x044, VIO_KEY_F10},
        {0x057, VIO_KEY_F11}, {0x058, VIO_KEY_F12},
        {0x064, 302}, {0x065, 303}, {0x066, 304}, {0x067, 305}, {0x068, 306}, {0x069, 307}, {0x06A, 308},
        {0x06B, 309}, {0x06C, 310}, {0x06D, 311}, {0x06E, 312}, {0x076, 313},     /* F13 .. F24 */
        {0x038, VIO_KEY_LEFT_ALT}, {0x01D, VIO_KEY_LEFT_CONTROL}, {0x02A, VIO_KEY_LEFT_SHIFT}, {0x15B, VIO_KEY_LEFT_SUPER},
        {0x137, VIO_KEY_PRINT_SCREEN}, {0x138, VIO_KEY_RIGHT_ALT}, {0x11D, VIO_KEY_RIGHT_CONTROL},
        {0x036, VIO_KEY_RIGHT_SHIFT}, {0x15C, VIO_KEY_RIGHT_SUPER},
        {0x150, VIO_KEY_DOWN}, {0x14B, VIO_KEY_LEFT}, {0x14D, VIO_KEY_RIGHT}, {0x148, VIO_KEY_UP},
        {0x052, VIO_KEY_KP_0}, {0x04F, VIO_KEY_KP_1}, {0x050, VIO_KEY_KP_2}, {0x051, VIO_KEY_KP_3},
        {0x04B, VIO_KEY_KP_4}, {0x04C, VIO_KEY_KP_5}, {0x04D, VIO_KEY_KP_6}, {0x047, VIO_KEY_KP_7},
        {0x048, VIO_KEY_KP_8}, {0x049, VIO_KEY_KP_9}, {0x04E, VIO_KEY_KP_ADD}, {0x053, VIO_KEY_KP_DECIMAL},
        {0x135, VIO_KEY_KP_DIVIDE}, {0x11C, VIO_KEY_KP_ENTER}, {0x059, VIO_KEY_KP_EQUAL},
        {0x037, VIO_KEY_KP_MULTIPLY}, {0x04A, VIO_KEY_KP_SUBTRACT},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) w32_keycodes[t[i].sc] = (short)t[i].key;
}

static int w32_mods(void)
{
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000)   m |= VIO_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= VIO_MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)    m |= VIO_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= VIO_MOD_SUPER;
    if (GetKeyState(VK_CAPITAL) & 1) m |= VIO_MOD_CAPS_LOCK;
    if (GetKeyState(VK_NUMLOCK) & 1) m |= VIO_MOD_NUM_LOCK;
    return m;
}

/* ── Input delivery ───────────────────────────────────────────────── */

/* While a replay runs it owns the input: OS events are dropped. */
static vio_input_state *w32_input(vio_win32_window *w)
{
    return (w && w->input && !w->input->replaying) ? w->input : NULL;
}

static void w32_key_message(vio_win32_window *w, WPARAM wp, LPARAM lp, int down)
{
    int scancode = (HIWORD(lp) & (KF_EXTENDED | 0xff));
    if (!scancode) scancode = (int)MapVirtualKeyW((UINT)wp, MAPVK_VK_TO_VSC);
    /* Alt+PrintScreen / Ctrl+Pause / NumLock report other scancodes (GLFW's fixups) */
    if (scancode == 0x54) scancode = 0x137;
    if (scancode == 0x146) scancode = 0x45;
    if (scancode == 0x136) scancode = 0x36;
    int key = (scancode >= 0 && scancode < 512) ? w32_keycodes[scancode] : VIO_KEY_UNKNOWN;
    /* A right Alt (AltGr) first sends a fake left Control: skip it. */
    if (wp == VK_CONTROL && !(HIWORD(lp) & KF_EXTENDED)) {
        MSG next;
        DWORD t = GetMessageTime();
        if (PeekMessageW(&next, NULL, 0, 0, PM_NOREMOVE)
            && (next.message == WM_KEYDOWN || next.message == WM_SYSKEYDOWN || next.message == WM_KEYUP || next.message == WM_SYSKEYUP)
            && next.wParam == VK_MENU && (HIWORD(next.lParam) & KF_EXTENDED) && next.time == t) return;
    }
    int action = down ? ((lp & 0x40000000) ? VIO_REPEAT : VIO_PRESS) : VIO_RELEASE;
    vio_input_state *s = w32_input(w);
    if (!s) return;
    if (!down && wp == VK_SHIFT) {
        /* Releasing one shift while the other is held reports only one release: send both. */
        vio_input_key_event(s, VIO_KEY_LEFT_SHIFT, VIO_RELEASE, w32_mods());
        vio_input_key_event(s, VIO_KEY_RIGHT_SHIFT, VIO_RELEASE, w32_mods());
        return;
    }
    if (wp == VK_SNAPSHOT && !down) {
        /* PrintScreen sends no key-down: report the press with the release */
        vio_input_key_event(s, key, VIO_PRESS, w32_mods());
    }
    vio_input_key_event(s, key, action, w32_mods());
}

static WCHAR w32_high_surrogate;

static void w32_char_message(vio_win32_window *w, WPARAM wp)
{
    unsigned int c = (unsigned int)wp;
    if (c >= 0xD800 && c <= 0xDBFF) { w32_high_surrogate = (WCHAR)c; return; }
    if (c >= 0xDC00 && c <= 0xDFFF) {
        if (!w32_high_surrogate) return;
        c = 0x10000 + (((unsigned int)w32_high_surrogate - 0xD800) << 10) + (c - 0xDC00);
        w32_high_surrogate = 0;
    }
    if (c < 32 || (c > 126 && c < 160)) return;   /* control characters come as keys */
    vio_input_emit_char(w32_input(w), c);
}

static void w32_button(vio_win32_window *w, int button, int action)
{
    if (action == VIO_PRESS) SetCapture(w->hwnd);
    vio_input_button_event(w32_input(w), button, action);
    if (action == VIO_RELEASE && GetCapture() == w->hwnd) {
        int any = 0;
        if (w->input) for (int i = 0; i <= VIO_MOUSE_LAST; i++) any |= w->input->mouse_buttons[i];
        if (!any) ReleaseCapture();
    }
}

static void w32_update_clip(vio_win32_window *w)
{
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED && GetActiveWindow() == w->hwnd) {
        RECT r;
        GetClientRect(w->hwnd, &r);
        ClientToScreen(w->hwnd, (POINT *)&r.left);
        ClientToScreen(w->hwnd, (POINT *)&r.right);
        ClipCursor(&r);
    } else {
        ClipCursor(NULL);
    }
}

static void w32_report_size(vio_win32_window *w)
{
    RECT r;
    GetClientRect(w->hwnd, &r);
    int cw = r.right - r.left, ch = r.bottom - r.top;
    if (cw == w->fb_w && ch == w->fb_h) return;
    w->fb_w = cw;
    w->fb_h = ch;
    vio_input_resize_event(w->input, cw, ch);
}

static LRESULT CALLBACK w32_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    vio_win32_window *w = (vio_win32_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_CLOSE:
            w->should_close = 1;
            return 0;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            w32_key_message(w, wp, lp, 1);
            if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;   /* no menu beep on Alt+key; Alt+F4 still closes */
            break;
        case WM_KEYUP: case WM_SYSKEYUP:
            w32_key_message(w, wp, lp, 0);
            if (msg == WM_SYSKEYUP) return 0;
            break;
        case WM_CHAR: case WM_SYSCHAR:
            w32_char_message(w, wp);
            return 0;
        case WM_UNICHAR:
            if (wp == UNICODE_NOCHAR) return TRUE;
            if (wp >= 32) vio_input_emit_char(w32_input(w), (unsigned int)wp);
            return 0;
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) {
                if (!w->raw_motion) {   /* no raw input: deltas against the last position */
                    w->cursor_x += x - w->last_raw_x;
                    w->cursor_y += y - w->last_raw_y;
                    w->last_raw_x = x;
                    w->last_raw_y = y;
                    vio_input_cursor_event(w32_input(w), w->cursor_x, w->cursor_y);
                }
                return 0;
            }
            w->cursor_x = x;
            w->cursor_y = y;
            vio_input_cursor_event(w32_input(w), (double)x, (double)y);
            return 0;
        }
        case WM_INPUT: {
            if (w->cursor_mode != VIO_PLATFORM_CURSOR_DISABLED || !w->raw_motion) break;
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) break;
            if (raw.header.dwType != RIM_TYPEMOUSE) break;
            int dx = raw.data.mouse.lLastX, dy = raw.data.mouse.lLastY;
            if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                dx -= w->last_raw_x; dy -= w->last_raw_y;
                w->last_raw_x = raw.data.mouse.lLastX; w->last_raw_y = raw.data.mouse.lLastY;
            }
            w->cursor_x += dx;
            w->cursor_y += dy;
            vio_input_cursor_event(w32_input(w), w->cursor_x, w->cursor_y);
            break;
        }
        case WM_LBUTTONDOWN: w32_button(w, 0, VIO_PRESS);   return 0;
        case WM_LBUTTONUP:   w32_button(w, 0, VIO_RELEASE); return 0;
        case WM_RBUTTONDOWN: w32_button(w, 1, VIO_PRESS);   return 0;
        case WM_RBUTTONUP:   w32_button(w, 1, VIO_RELEASE); return 0;
        case WM_MBUTTONDOWN: w32_button(w, 2, VIO_PRESS);   return 0;
        case WM_MBUTTONUP:   w32_button(w, 2, VIO_RELEASE); return 0;
        case WM_XBUTTONDOWN: w32_button(w, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4, VIO_PRESS);   return TRUE;
        case WM_XBUTTONUP:   w32_button(w, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 3 : 4, VIO_RELEASE); return TRUE;
        case WM_MOUSEWHEEL:
            vio_input_scroll_event(w32_input(w), 0.0, (double)(SHORT)HIWORD(wp) / (double)WHEEL_DELTA);
            return 0;
        case WM_MOUSEHWHEEL:   /* GLFW reports right as negative */
            vio_input_scroll_event(w32_input(w), -(double)(SHORT)HIWORD(wp) / (double)WHEEL_DELTA, 0.0);
            return 0;
        case WM_SIZE:
            w32_report_size(w);
            w32_update_clip(w);
            break;
        case WM_ACTIVATE:
            w32_update_clip(w);
            if (LOWORD(wp) == WA_INACTIVE && w->monitor >= 0 && w->auto_iconify) ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case WM_DPICHANGED:
            if (!w->headless && w->monitor < 0) {
                const RECT *s = (const RECT *)lp;
                SetWindowPos(hwnd, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && w->cursor_mode != VIO_PLATFORM_CURSOR_NORMAL) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ── WGL ──────────────────────────────────────────────────────────── */

#define W32_WGL_DRAW_TO_WINDOW_ARB          0x2001
#define W32_WGL_SUPPORT_OPENGL_ARB          0x2010
#define W32_WGL_DOUBLE_BUFFER_ARB           0x2011
#define W32_WGL_PIXEL_TYPE_ARB              0x2013
#define W32_WGL_TYPE_RGBA_ARB               0x202B
#define W32_WGL_ACCELERATION_ARB            0x2003
#define W32_WGL_FULL_ACCELERATION_ARB       0x2027
#define W32_WGL_COLOR_BITS_ARB              0x2014
#define W32_WGL_ALPHA_BITS_ARB              0x201B
#define W32_WGL_DEPTH_BITS_ARB              0x2022
#define W32_WGL_STENCIL_BITS_ARB            0x2023
#define W32_WGL_SAMPLE_BUFFERS_ARB          0x2041
#define W32_WGL_SAMPLES_ARB                 0x2042
#define W32_WGL_CONTEXT_MAJOR_VERSION_ARB   0x2091
#define W32_WGL_CONTEXT_MINOR_VERSION_ARB   0x2092
#define W32_WGL_CONTEXT_FLAGS_ARB           0x2094
#define W32_WGL_CONTEXT_PROFILE_MASK_ARB    0x9126
#define W32_WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001

typedef BOOL  (WINAPI *w32_wglChoosePixelFormatARB_t)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
typedef HGLRC (WINAPI *w32_wglCreateContextAttribsARB_t)(HDC, HGLRC, const int *);
typedef BOOL  (WINAPI *w32_wglSwapIntervalEXT_t)(int);

static w32_wglChoosePixelFormatARB_t    w32_wglChoosePixelFormatARB;
static w32_wglCreateContextAttribsARB_t w32_wglCreateContextAttribsARB;
static w32_wglSwapIntervalEXT_t         w32_wglSwapIntervalEXT;
static HMODULE                          w32_opengl32;
static int                              w32_wgl_probed;

static void *w32_gl_get_proc_address(const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (p == NULL || p == (void *)0x1 || p == (void *)0x2 || p == (void *)0x3 || p == (void *)-1) {
        /* GL 1.0 / 1.1 entry points live in opengl32.dll, wglGetProcAddress does not hand them out */
        if (!w32_opengl32) w32_opengl32 = LoadLibraryW(L"opengl32.dll");
        p = w32_opengl32 ? (void *)GetProcAddress(w32_opengl32, name) : NULL;
    }
    return p;
}

/* The ARB entry points need a current context: a hidden dummy window with a
 * legacy pixel format and context, once per process. */
static void w32_wgl_probe(void)
{
    if (w32_wgl_probed) return;
    w32_wgl_probed = 1;
    HWND dummy = CreateWindowExW(0, w32_class_name, L"vio dummy", WS_POPUP, 0, 0, 1, 1, NULL, NULL, w32_instance, NULL);
    if (!dummy) return;
    HDC dc = GetDC(dummy);
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (pf && SetPixelFormat(dc, pf, &pfd)) {
        HGLRC rc = wglCreateContext(dc);
        if (rc && wglMakeCurrent(dc, rc)) {
            w32_wglChoosePixelFormatARB = (w32_wglChoosePixelFormatARB_t)wglGetProcAddress("wglChoosePixelFormatARB");
            w32_wglCreateContextAttribsARB = (w32_wglCreateContextAttribsARB_t)wglGetProcAddress("wglCreateContextAttribsARB");
            w32_wglSwapIntervalEXT = (w32_wglSwapIntervalEXT_t)wglGetProcAddress("wglSwapIntervalEXT");
            wglMakeCurrent(NULL, NULL);
        }
        if (rc) wglDeleteContext(rc);
    }
    ReleaseDC(dummy, dc);
    DestroyWindow(dummy);
}

/* Pixel format + context on the real window: the newest of the version
 * ladder the driver gives (core profile from 3.2), else a legacy context. */
static int w32_gl_create(vio_win32_window *w, int samples)
{
    w32_wgl_probe();
    int pf = 0;
    UINT n = 0;
    PIXELFORMATDESCRIPTOR pfd = {0};
    if (w32_wglChoosePixelFormatARB) {
        int attrs[] = {
            W32_WGL_DRAW_TO_WINDOW_ARB, 1, W32_WGL_SUPPORT_OPENGL_ARB, 1, W32_WGL_DOUBLE_BUFFER_ARB, 1,
            W32_WGL_PIXEL_TYPE_ARB, W32_WGL_TYPE_RGBA_ARB, W32_WGL_ACCELERATION_ARB, W32_WGL_FULL_ACCELERATION_ARB,
            W32_WGL_COLOR_BITS_ARB, 32, W32_WGL_ALPHA_BITS_ARB, 8, W32_WGL_DEPTH_BITS_ARB, 24, W32_WGL_STENCIL_BITS_ARB, 8,
            W32_WGL_SAMPLE_BUFFERS_ARB, samples > 1 ? 1 : 0, W32_WGL_SAMPLES_ARB, samples > 1 ? samples : 0,
            0
        };
        if (!w32_wglChoosePixelFormatARB(w->hdc, attrs, NULL, 1, &pf, &n) || n == 0) {
            attrs[sizeof(attrs) / sizeof(attrs[0]) - 4] = 0;   /* without MSAA */
            if (!w32_wglChoosePixelFormatARB(w->hdc, attrs, NULL, 1, &pf, &n) || n == 0) pf = 0;
        }
    }
    if (!pf) {
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pf = ChoosePixelFormat(w->hdc, &pfd);
    }
    if (!pf) return -1;
    DescribePixelFormat(w->hdc, pf, sizeof(pfd), &pfd);
    if (!SetPixelFormat(w->hdc, pf, &pfd)) return -1;

    if (w32_wglCreateContextAttribsARB) {
        static const int ladder[][2] = { {4, 6}, {4, 5}, {4, 3}, {4, 1}, {3, 3}, {3, 2}, {3, 1}, {3, 0} };
        for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]) && !w->glrc; i++) {
            int attrs[] = {
                W32_WGL_CONTEXT_MAJOR_VERSION_ARB, ladder[i][0], W32_WGL_CONTEXT_MINOR_VERSION_ARB, ladder[i][1],
                W32_WGL_CONTEXT_PROFILE_MASK_ARB, ladder[i][0] * 10 + ladder[i][1] >= 32 ? W32_WGL_CONTEXT_CORE_PROFILE_BIT_ARB : 0,
                0
            };
            if (ladder[i][0] * 10 + ladder[i][1] < 32) attrs[4] = 0;   /* no profile before 3.2 */
            w->glrc = w32_wglCreateContextAttribsARB(w->hdc, NULL, attrs);
        }
    }
    if (!w->glrc) w->glrc = wglCreateContext(w->hdc);   /* legacy drivers: whatever the driver has */
    if (!w->glrc) return -1;
    return wglMakeCurrent(w->hdc, w->glrc) ? 0 : -1;
}

/* ── Monitors ─────────────────────────────────────────────────────── */

#define W32_MAX_MONITORS 16

typedef struct {
    HMONITOR handle;
    WCHAR    device[32];
    char     name[128];
    int      primary;
} w32_monitor;

static w32_monitor w32_monitors[W32_MAX_MONITORS];
static int         w32_monitor_n;

static BOOL CALLBACK w32_monitor_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)r; (void)lp;
    if (w32_monitor_n >= W32_MAX_MONITORS) return FALSE;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(m, (MONITORINFO *)&mi)) return TRUE;
    w32_monitor *e = &w32_monitors[w32_monitor_n];
    memset(e, 0, sizeof(*e));
    e->handle = m;
    wcsncpy(e->device, mi.szDevice, 31);
    e->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DISPLAY_DEVICEW dd;
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
        WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, e->name, (int)sizeof(e->name) - 1, NULL, NULL);
    } else {
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, e->name, (int)sizeof(e->name) - 1, NULL, NULL);
    }
    w32_monitor_n++;
    return TRUE;
}

/* The monitor list, primary first (GLFW's order). */
static void w32_monitors_refresh(void)
{
    w32_monitor_n = 0;
    EnumDisplayMonitors(NULL, NULL, w32_monitor_enum, 0);
    for (int i = 1; i < w32_monitor_n; i++) {
        if (w32_monitors[i].primary) {
            w32_monitor t = w32_monitors[0];
            w32_monitors[0] = w32_monitors[i];
            w32_monitors[i] = t;
            break;
        }
    }
}

static int w32_monitor_count(void)
{
    w32_monitors_refresh();
    return w32_monitor_n;
}

static int w32_primary_monitor(void)
{
    w32_monitors_refresh();
    return w32_monitor_n > 0 ? 0 : -1;
}

static void w32_mode_from_devmode(const DEVMODEW *dm, vio_video_mode *m)
{
    m->width = (int)dm->dmPelsWidth;
    m->height = (int)dm->dmPelsHeight;
    m->refresh_hz = (int)dm->dmDisplayFrequency;
    int bpp = (int)dm->dmBitsPerPel;
    int c = bpp >= 24 ? 8 : bpp == 16 ? 5 : bpp / 3;
    m->red_bits = c;
    m->green_bits = bpp == 16 ? 6 : c;
    m->blue_bits = c;
}

static int w32_monitor_desc(int index, vio_monitor_desc *out)
{
    memset(out, 0, sizeof(*out));
    if (index < 0 || index >= w32_monitor_n) w32_monitors_refresh();
    if (index < 0 || index >= w32_monitor_n) return -1;
    w32_monitor *e = &w32_monitors[index];
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(e->handle, &mi)) return -1;
    snprintf(out->name, sizeof(out->name), "%s", e->name);
    out->x = mi.rcMonitor.left;
    out->y = mi.rcMonitor.top;
    out->work_x = mi.rcWork.left;
    out->work_y = mi.rcWork.top;
    out->work_width = mi.rcWork.right - mi.rcWork.left;
    out->work_height = mi.rcWork.bottom - mi.rcWork.top;
    out->scale_x = out->scale_y = (float)w32_monitor_dpi(e->handle) / 96.0f;
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(e->device, ENUM_CURRENT_SETTINGS, &dm)) w32_mode_from_devmode(&dm, &out->mode);
    out->primary = e->primary;
    return 0;
}

static int w32_mode_cmp(const void *a, const void *b)
{
    const vio_video_mode *x = (const vio_video_mode *)a, *y = (const vio_video_mode *)b;
    int bx = x->red_bits + x->green_bits + x->blue_bits, by = y->red_bits + y->green_bits + y->blue_bits;
    if (bx != by) return bx - by;
    long ax = (long)x->width * x->height, ay = (long)y->width * y->height;
    if (ax != ay) return ax < ay ? -1 : 1;
    if (x->width != y->width) return x->width - y->width;
    return x->refresh_hz - y->refresh_hz;
}

static int w32_video_modes(int index, vio_video_mode *out, int max)
{
    if (index < 0 || index >= w32_monitor_n) w32_monitors_refresh();
    if (index < 0 || index >= w32_monitor_n || max <= 0) return 0;
    int n = 0;
    DEVMODEW dm;
    for (DWORD i = 0; n < max; i++) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(w32_monitors[index].device, i, &dm)) break;
        if (dm.dmBitsPerPel < 15) continue;
        vio_video_mode m;
        w32_mode_from_devmode(&dm, &m);
        int dup = 0;
        for (int k = 0; k < n && !dup; k++) dup = memcmp(&out[k], &m, sizeof(m)) == 0;
        if (!dup) out[n++] = m;
    }
    qsort(out, (size_t)n, sizeof(out[0]), w32_mode_cmp);
    return n;
}

/* ── Window lifecycle ─────────────────────────────────────────────── */

static int w32_init(void)
{
    if (w32_initialized) return 1;
    w32_instance = GetModuleHandleW(NULL);
    w32_dpi_init();
    w32_keytable_init();
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = w32_wndproc;
    wc.hInstance = w32_instance;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    wc.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512) /* IDI_APPLICATION */);
    wc.lpszClassName = w32_class_name;
    w32_class = RegisterClassExW(&wc);
    if (!w32_class && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        php_error_docref(NULL, E_WARNING, "Win32: RegisterClassExW failed (%lu)", GetLastError());
        return 0;
    }
    w32_initialized = 1;
    return 1;
}

static void w32_shutdown(void)
{
    if (!w32_initialized) return;
    UnregisterClassW(w32_class_name, w32_instance);
    w32_initialized = 0;
    w32_wgl_probed = 0;
}

static vio_window_handle w32_create_window(vio_config *cfg, const char *backend_name)
{
    if (!w32_initialized && !w32_init()) return NULL;
    vio_win32_window *w = (vio_win32_window *)calloc(1, sizeof(vio_win32_window));
    if (!w) return NULL;
    w->headless = cfg->headless;
    w->decorated = !cfg->headless;
    w->auto_iconify = 0;   /* never minimise a fullscreen window on focus loss (see the GLFW platform) */
    w->monitor = -1;
    w->fb_w = w->fb_h = -1;

    int width  = cfg->width  > 0 ? cfg->width  : 800;
    int height = cfg->height > 0 ? cfg->height : 600;
    const char *title = cfg->title ? cfg->title : "php-vio";
    WCHAR wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);

    /* Centre on the monitor under the cursor; a visible window's client area
     * scales with that monitor's DPI (GLFW_SCALE_TO_MONITOR), a headless one is
     * exactly the requested pixels. */
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = w32_monitor_dpi(mon);
    int cw = width, ch = height;
    if (!cfg->headless) {
        cw = MulDiv(width, (int)dpi, 96);
        ch = MulDiv(height, (int)dpi, 96);
    }
    DWORD style = w32_style(w);
    RECT r = { 0, 0, cw, ch };
    w32_adjust(&r, style, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    int x = 0, y = 0;
    if (GetMonitorInfoW(mon, &mi)) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    }
    w->hwnd = CreateWindowExW(cfg->headless ? WS_EX_TOOLWINDOW : WS_EX_APPWINDOW, w32_class_name, wtitle, style,
                              x, y, ww, wh, NULL, NULL, w32_instance, NULL);
    if (!w->hwnd) {
        php_error_docref(NULL, E_WARNING, "Win32: CreateWindowExW failed (%lu)", GetLastError());
        free(w);
        return NULL;
    }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
    w->hdc = GetDC(w->hwnd);
    if (cfg->headless) {
        /* A WS_POPUP has no minimum size: the client area is exactly the request. */
        SetWindowPos(w->hwnd, NULL, 0, 0, cw, ch, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (backend_name && strcmp(backend_name, "opengl") == 0) {
        if (w32_gl_create(w, cfg->samples) != 0) {
            php_error_docref(NULL, E_WARNING, "Win32: no OpenGL context >= 3.0 available");
            ReleaseDC(w->hwnd, w->hdc);
            DestroyWindow(w->hwnd);
            free(w);
            return NULL;
        }
        if (w32_wglSwapIntervalEXT) w32_wglSwapIntervalEXT(cfg->vsync ? 1 : 0);
    }

    if (!cfg->headless) {
        ShowWindow(w->hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(w->hwnd);
        UpdateWindow(w->hwnd);
        /* Pump once so the window is visible to DWM before a swapchain exists
         * (a FLIP_DISCARD swapchain on an occluded window presents nothing). */
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    w->fb_w = cr.right - cr.left;
    w->fb_h = cr.bottom - cr.top;
    return w;
}

static void w32_restore_display(vio_win32_window *w)
{
    if (w->monitor >= 0 && w->monitor_device[0]) {
        ChangeDisplaySettingsExW(w->monitor_device, NULL, NULL, CDS_FULLSCREEN, NULL);
        w->monitor_device[0] = 0;
    }
}

static void w32_destroy_window(vio_window_handle h)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return;
    w32_restore_display(w);
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) ClipCursor(NULL);
    if (w->glrc) {
        if (wglGetCurrentContext() == w->glrc) wglMakeCurrent(NULL, NULL);
        wglDeleteContext(w->glrc);
    }
    if (w->hdc) ReleaseDC(w->hwnd, w->hdc);
    if (w->hwnd) {
        SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(w->hwnd);
    }
    free(w);
}

static int  w32_should_close(vio_window_handle h) { return h ? ((vio_win32_window *)h)->should_close : 1; }
static void w32_set_should_close(vio_window_handle h, int v) { if (h) ((vio_win32_window *)h)->should_close = v; }

static void w32_poll_events(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void w32_wait_events(void)
{
    WaitMessage();
    w32_poll_events();
}

static void w32_swap_buffers(vio_window_handle h)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (w && w->hdc) SwapBuffers(w->hdc);
}

static void w32_get_framebuffer_size(vio_window_handle h, int *cw, int *ch)
{
    vio_win32_window *w = (vio_win32_window *)h;
    RECT r = {0};
    if (w && w->hwnd) GetClientRect(w->hwnd, &r);
    if (cw) *cw = r.right - r.left;
    if (ch) *ch = r.bottom - r.top;
}

static void w32_get_content_scale(vio_window_handle h, float *sx, float *sy)
{
    vio_win32_window *w = (vio_win32_window *)h;
    float s = (float)w32_window_dpi(w ? w->hwnd : NULL) / 96.0f;
    if (sx) *sx = s;
    if (sy) *sy = s;
}

static void w32_get_cursor_pos(vio_window_handle h, double *x, double *y)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return;
    if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) {
        if (x) *x = w->cursor_x;
        if (y) *y = w->cursor_y;
        return;
    }
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(w->hwnd, &p);
    if (x) *x = p.x;
    if (y) *y = p.y;
}

static int w32_gl_make_current(vio_window_handle h)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return wglMakeCurrent(NULL, NULL) ? 0 : -1;
    return wglMakeCurrent(w->hdc, w->glrc) ? 0 : -1;
}

static void w32_gl_swap_interval(int interval)
{
    if (w32_wglSwapIntervalEXT) w32_wglSwapIntervalEXT(interval);
}

/* ── Window properties ────────────────────────────────────────────── */

static void w32_set_title(vio_window_handle h, const char *utf8)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w || !utf8) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    WCHAR *t = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(n > 0 ? n : 1));
    if (!t) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, t, n);
    SetWindowTextW(w->hwnd, t);
    free(t);
}

/* Client-area size in pixels (GLFW's Win32 window size). */
static void w32_set_window_size(vio_window_handle h, int cw, int ch)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w || w->monitor >= 0) return;
    RECT r = { 0, 0, cw, ch };
    w32_adjust(&r, (DWORD)GetWindowLongW(w->hwnd, GWL_STYLE), w32_window_dpi(w->hwnd));
    SetWindowPos(w->hwnd, HWND_TOP, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOMOVE | SWP_NOZORDER);
}

/* Position of the client area's top-left corner. */
static void w32_get_window_pos(vio_window_handle h, int *x, int *y)
{
    vio_win32_window *w = (vio_win32_window *)h;
    POINT p = { 0, 0 };
    if (w) ClientToScreen(w->hwnd, &p);
    if (x) *x = p.x;
    if (y) *y = p.y;
}

static void w32_set_window_pos(vio_window_handle h, int x, int y)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w || w->monitor >= 0) return;
    RECT r = { x, y, x, y };
    w32_adjust(&r, (DWORD)GetWindowLongW(w->hwnd, GWL_STYLE), w32_window_dpi(w->hwnd));
    SetWindowPos(w->hwnd, NULL, r.left, r.top, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
}

static void w32_apply_style(vio_win32_window *w)
{
    RECT r;
    GetClientRect(w->hwnd, &r);
    POINT p = { 0, 0 };
    ClientToScreen(w->hwnd, &p);
    DWORD style = w32_style(w) | ((DWORD)GetWindowLongW(w->hwnd, GWL_STYLE) & (WS_VISIBLE | WS_MAXIMIZE | WS_MINIMIZE));
    SetWindowLongW(w->hwnd, GWL_STYLE, (LONG)style);
    RECT a = { p.x, p.y, p.x + (r.right - r.left), p.y + (r.bottom - r.top) };
    w32_adjust(&a, style, w32_window_dpi(w->hwnd));
    SetWindowPos(w->hwnd, HWND_TOP, a.left, a.top, a.right - a.left, a.bottom - a.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER);
}

static int w32_get_attrib(vio_window_handle h, vio_window_attrib a)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return 0;
    switch (a) {
        case VIO_WINDOW_DECORATED:    return w->decorated;
        case VIO_WINDOW_MAXIMIZED:    return IsZoomed(w->hwnd) ? 1 : 0;
        case VIO_WINDOW_AUTO_ICONIFY: return w->auto_iconify;
        case VIO_WINDOW_FOCUSED:      return GetActiveWindow() == w->hwnd;
        case VIO_WINDOW_VISIBLE:      return IsWindowVisible(w->hwnd) ? 1 : 0;
    }
    return 0;
}

static void w32_set_attrib(vio_window_handle h, vio_window_attrib a, int value)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return;
    switch (a) {
        case VIO_WINDOW_DECORATED:
            w->decorated = value ? 1 : 0;
            if (w->monitor < 0 && !w->headless) w32_apply_style(w);
            break;
        case VIO_WINDOW_AUTO_ICONIFY:
            w->auto_iconify = value ? 1 : 0;
            break;
        case VIO_WINDOW_VISIBLE:
            ShowWindow(w->hwnd, value ? SW_SHOWNA : SW_HIDE);
            break;
        default:
            break;
    }
}

static void w32_maximize(vio_window_handle h) { if (h) ShowWindow(((vio_win32_window *)h)->hwnd, SW_MAXIMIZE); }
static void w32_restore(vio_window_handle h)  { if (h) ShowWindow(((vio_win32_window *)h)->hwnd, SW_RESTORE); }

static void w32_set_cursor_mode(vio_window_handle h, int mode)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w || w->cursor_mode == mode) return;
    if (mode == VIO_PLATFORM_CURSOR_DISABLED) {
        RAWINPUTDEVICE rid = { 0x01, 0x02, 0, w->hwnd };   /* generic desktop / mouse */
        w->raw_motion = RegisterRawInputDevices(&rid, 1, sizeof(rid)) ? 1 : 0;
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(w->hwnd, &p);
        w->cursor_x = p.x;
        w->cursor_y = p.y;
        w->last_raw_x = p.x;
        w->last_raw_y = p.y;
    } else if (w->cursor_mode == VIO_PLATFORM_CURSOR_DISABLED) {
        RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_REMOVE, NULL };
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        w->raw_motion = 0;
    }
    w->cursor_mode = mode;
    w32_update_clip(w);
    /* re-evaluate the cursor shape now, not on the next mouse move */
    POINT p;
    GetCursorPos(&p);
    if (WindowFromPoint(p) == w->hwnd) SetCursor(mode == VIO_PLATFORM_CURSOR_NORMAL ? LoadCursorW(NULL, MAKEINTRESOURCEW(32512) /* IDC_ARROW */) : NULL);
}

static int w32_window_monitor(vio_window_handle h)
{
    return h ? ((vio_win32_window *)h)->monitor : -1;
}

static void w32_set_window_monitor(vio_window_handle h, int monitor, int x, int y, int width, int height, int refresh_hz)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return;
    if (monitor >= 0) {
        vio_monitor_desc md;
        if (monitor >= w32_monitor_count() || w32_monitor_desc(monitor, &md) != 0) return;
        /* switch the display to the closest mode, then cover it with a popup */
        DEVMODEW dm;
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = (DWORD)width;
        dm.dmPelsHeight = (DWORD)height;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (refresh_hz > 0) { dm.dmDisplayFrequency = (DWORD)refresh_hz; dm.dmFields |= DM_DISPLAYFREQUENCY; }
        if (width != md.mode.width || height != md.mode.height || (refresh_hz > 0 && refresh_hz != md.mode.refresh_hz)) {
            if (ChangeDisplaySettingsExW(w32_monitors[monitor].device, &dm, NULL, CDS_FULLSCREEN, NULL) == DISP_CHANGE_SUCCESSFUL) {
                wcsncpy(w->monitor_device, w32_monitors[monitor].device, 31);
                w32_monitor_desc(monitor, &md);
            }
        }
        w->monitor = monitor;
        SetWindowLongW(w->hwnd, GWL_STYLE, (LONG)(w32_style(w) | WS_VISIBLE));
        SetWindowPos(w->hwnd, HWND_TOPMOST, md.x, md.y, md.mode.width, md.mode.height,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
        return;
    }
    /* back to a window at x, y with a width x height client area */
    w32_restore_display(w);
    w->monitor = -1;
    DWORD style = w32_style(w) | WS_VISIBLE;
    SetWindowLongW(w->hwnd, GWL_STYLE, (LONG)style);
    RECT r = { x, y, x + width, y + height };
    w32_adjust(&r, style, w32_window_dpi(w->hwnd));
    SetWindowPos(w->hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

/* ── Gamepads: XInput ─────────────────────────────────────────────── */

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } w32_xgamepad;
typedef struct { DWORD dwPacketNumber; w32_xgamepad Gamepad; } w32_xstate;
typedef DWORD (WINAPI *w32_XInputGetState_t)(DWORD, w32_xstate *);

static w32_XInputGetState_t w32_XInputGetState;
static int                  w32_xinput_loaded;
static unsigned char        w32_raw_buttons[4][15];
static float                w32_raw_axes[4][6];

static int w32_xinput(int id, w32_xstate *st)
{
    if (!w32_xinput_loaded) {
        w32_xinput_loaded = 1;
        static const WCHAR *dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll" };
        for (size_t i = 0; i < sizeof(dlls) / sizeof(dlls[0]) && !w32_XInputGetState; i++) {
            HMODULE m = LoadLibraryW(dlls[i]);
            if (m) w32_XInputGetState = (w32_XInputGetState_t)GetProcAddress(m, "XInputGetState");
        }
    }
    if (!w32_XInputGetState || id < 0 || id > 3) return 0;
    memset(st, 0, sizeof(*st));
    return w32_XInputGetState((DWORD)id, st) == ERROR_SUCCESS;
}

static int w32_joystick_present(int id)
{
    w32_xstate st;
    return w32_xinput(id, &st);
}

static const char *w32_pad_name(int id)
{
    w32_xstate st;
    return w32_xinput(id, &st) ? "XInput Gamepad (GLFW)" : NULL;
}

/* GLFW's XInput gamepad mapping: axes -1..1 with Y up = -1, triggers -1 (released) .. 1. */
static float w32_stick(SHORT v) { return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f; }

static int w32_gamepad_state(int id, unsigned char buttons[15], float axes[6])
{
    w32_xstate st;
    if (!w32_xinput(id, &st)) return 0;
    WORD b = st.Gamepad.wButtons;
    static const WORD bits[15] = {
        0x1000 /* A */, 0x2000 /* B */, 0x4000 /* X */, 0x8000 /* Y */, 0x0100 /* LB */, 0x0200 /* RB */,
        0x0020 /* BACK */, 0x0010 /* START */, 0x0400 /* GUIDE (XInputGetStateEx only) */, 0x0040 /* LTHUMB */,
        0x0080 /* RTHUMB */, 0x0001 /* UP */, 0x0008 /* RIGHT */, 0x0002 /* DOWN */, 0x0004 /* LEFT */
    };
    for (int i = 0; i < 15; i++) buttons[i] = (b & bits[i]) ? 1 : 0;
    axes[0] = w32_stick(st.Gamepad.sThumbLX);
    axes[1] = -w32_stick(st.Gamepad.sThumbLY);
    axes[2] = w32_stick(st.Gamepad.sThumbRX);
    axes[3] = -w32_stick(st.Gamepad.sThumbRY);
    axes[4] = (float)st.Gamepad.bLeftTrigger / 127.5f - 1.0f;
    axes[5] = (float)st.Gamepad.bRightTrigger / 127.5f - 1.0f;
    return 1;
}

static const unsigned char *w32_joystick_buttons(int id, int *count)
{
    *count = 0;
    if (id < 0 || id > 3 || !w32_gamepad_state(id, w32_raw_buttons[id], w32_raw_axes[id])) return NULL;
    *count = 15;
    return w32_raw_buttons[id];
}

static const float *w32_joystick_axes(int id, int *count)
{
    *count = 0;
    if (id < 0 || id > 3 || !w32_gamepad_state(id, w32_raw_buttons[id], w32_raw_axes[id])) return NULL;
    *count = 6;
    return w32_raw_axes[id];
}

/* ── Native handles, Vulkan, input ────────────────────────────────── */

static void *w32_native_handle(vio_window_handle h, vio_native_kind kind)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return NULL;
    if (kind == VIO_NATIVE_HWND) return (void *)w->hwnd;
    if (kind == VIO_NATIVE_HINSTANCE) return (void *)w32_instance;
    return NULL;
}

#ifdef HAVE_VULKAN
static const char **w32_vk_instance_extensions(uint32_t *count)
{
    static const char *ext[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    *count = 2;
    return ext;
}

static int w32_vk_create_surface(vio_window_handle h, void *instance, void *out_surface)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (!w) return (int)VK_ERROR_INITIALIZATION_FAILED;
    VkWin32SurfaceCreateInfoKHR ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = w32_instance;
    ci.hwnd = w->hwnd;
    return (int)vkCreateWin32SurfaceKHR((VkInstance)instance, &ci, NULL, (VkSurfaceKHR *)out_surface);
}
#endif

static void w32_install_input(vio_window_handle h, vio_input_state *state)
{
    vio_win32_window *w = (vio_win32_window *)h;
    if (w) w->input = state;
}

static const vio_platform vio_platform_win32 = {
    .name                 = "win32",
    .init                 = w32_init,
    .shutdown             = w32_shutdown,
    .create_window        = w32_create_window,
    .destroy_window       = w32_destroy_window,
    .should_close         = w32_should_close,
    .set_should_close     = w32_set_should_close,
    .poll_events          = w32_poll_events,
    .wait_events          = w32_wait_events,
    .swap_buffers         = w32_swap_buffers,
    .get_framebuffer_size = w32_get_framebuffer_size,
    .get_window_size      = w32_get_framebuffer_size,   /* client pixels, as GLFW's Win32 window size */
    .get_content_scale    = w32_get_content_scale,
    .get_cursor_pos       = w32_get_cursor_pos,
    .gl_make_current      = w32_gl_make_current,
    .gl_swap_interval     = w32_gl_swap_interval,
    .gl_get_proc_address  = w32_gl_get_proc_address,
    .set_title            = w32_set_title,
    .set_window_size      = w32_set_window_size,
    .get_window_pos       = w32_get_window_pos,
    .set_window_pos       = w32_set_window_pos,
    .get_attrib           = w32_get_attrib,
    .set_attrib           = w32_set_attrib,
    .maximize             = w32_maximize,
    .restore              = w32_restore,
    .set_cursor_mode      = w32_set_cursor_mode,
    .window_monitor       = w32_window_monitor,
    .set_window_monitor   = w32_set_window_monitor,
    .monitor_count        = w32_monitor_count,
    .primary_monitor      = w32_primary_monitor,
    .monitor_desc         = w32_monitor_desc,
    .video_modes          = w32_video_modes,
    .joystick_present     = w32_joystick_present,
    .joystick_is_gamepad  = w32_joystick_present,
    .joystick_name        = w32_pad_name,
    .gamepad_name         = w32_pad_name,
    .gamepad_state        = w32_gamepad_state,
    .joystick_buttons     = w32_joystick_buttons,
    .joystick_axes        = w32_joystick_axes,
    .native_handle        = w32_native_handle,
#ifdef HAVE_VULKAN
    .vk_instance_extensions = w32_vk_instance_extensions,
    .vk_create_surface      = w32_vk_create_surface,
#endif
    .install_input        = w32_install_input,
};

void vio_platform_win32_register(void)
{
    vio_register_platform(&vio_platform_win32);
}

#endif /* _WIN32 */
