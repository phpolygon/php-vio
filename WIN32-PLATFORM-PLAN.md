# Win32 Native Platform — Umsetzungs-Blueprint

> **Ziel:** Windowing, Input und Gamepad direkt in php-vio realisieren — ohne GLFW.
> **Entscheidungen (bestätigt):** nativer GL-Kontext **inklusive** (GLFW voll entfernbar) · **Windows zuerst** · nativer WGL-Pfad, damit PHPolygon auf Legacy-Rechnern OpenGL bekommt.
> **Vorlage:** `NATIVE-PLATFORM-PLAN.md` (Phasen) + `OPENGL-REFACTOR-PLAN.md` (Vtable + Audit-Gate). Dieses Dokument ist die **code-verankerte Ausführungsspezifikation** für Phase 0 + Phase 1 (Win32).
> **Status (2026-10-08):** Phase 0 + Phase 1 umgesetzt (OPEN-ITEMS A1/A2, Batch 7). Abweichungen: ein Quelltext
> `src/platform/win32/vio_platform_win32.c` statt vier; Tasten über GLFWs Scancode-Tabelle (layout-robust) statt VK;
> Audit-Gate ist `tests/core/212` (089 war vergeben), Win32-Test `tests/window/213`; kein `--with-native-platform`-Schalter –
> die Win32-Plattform wird auf Windows immer gebaut und gewinnt die Auto-Wahl, `VIO_PLATFORM=glfw` wählt GLFW;
> das GL-Backend hängt an `HAVE_OPENGL` (Windows immer), ein Build ohne GLFW hat alle Backends.

---

## 0. Leitplanken

1. **Abstraktion vor Ersatz.** Erst die `vio_platform`-Vtable einziehen und GLFW dahinter schieben (Phase 0, 0 Verhaltensänderung), dann den Win32-Layer daneben (Phase 1). Kein Schritt lässt die **88 Tests** rot werden.
2. **`VIO_KEY_*`/`VIO_MOUSE_*`/`VIO_GAMEPAD_*` sind die kanonische ABI** (`include/vio_constants.h`, GLFW-nummeriert). Der Win32-Layer liefert **Übersetzungstabellen** VK→VIO / XInput→VIO. Kein PHP-Skript bricht.
3. **Opakes Handle statt `GLFWwindow*`.** Neuer Typ `vio_window_handle`; die GPU-Backends fragen `native_handle(w, VIO_NATIVE_HWND)` statt `glfwGetWin32Window`.
4. **Additiv & gegated.** Neuer Layer hinter `--with-native-platform`; solange GLFW gebaut wird, bleibt es Default, bis der Win32-Layer die Smoke-Tests grün fährt.

---

## 1. Ist-Zustand: vollständige GLFW-Landkarte (verifiziert per grep)

GLFW steckt heute in **vier** Dateien. Gemessen (nicht geschätzt):

| Datei | GLFW-Tokens | Belang |
|-------|-------------|--------|
| `php_vio.c` | **104** (39× `#ifdef HAVE_GLFW`) | Fenster-Properties, Cursor, Monitore, Fullscreen, VideoModes, **Gamepad**, Native-Handle-Bridges |
| `src/vio_window.c` | ~40 | Fenster-Lifecycle + Erzeugung + GL-Kontext-Ladder + Win32-DPI + Monitor-Placement |
| `src/vio_input.c` | 22 | 6 Callbacks (key/char/cursor/button/scroll/fbsize) + `install_callbacks` |
| `src/backends/opengl/vio_opengl.c` | 5 | **GLAD-Loader:** `gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)` (Zeile 1340) |

### 1.1 `php_vio.c` — die 104 Tokens nach Funktion gruppiert (Ziel-Umbau in Phase 0)

| Zeilenbereich (heute) | GLFW-Calls | Wird zu |
|-----------------------|-----------|---------|
| 418 | `glfwPollEvents` | `g_platform->poll_events()` |
| 472–547 | `glfwGetFramebufferSize`, `glfwGetWindowContentScale`, `glfwGetWindowSize` | `get_framebuffer_size` / `get_content_scale` / `get_window_size` |
| 810–835 | `glfwGetCursorPos`, `glfwGetWindowContentScale` | `get_cursor_pos` |
| 1042–1046 | `glfwSetInputMode`, `glfwRawMouseMotionSupported` | `set_cursor_mode` (+ raw-motion intern) |
| 1204–1327 | `glfwGet/SetWindowMonitor`, `glfwGetPrimaryMonitor`, `glfwGetVideoMode`, `glfwSetWindowTitle`, `glfwGetWindowAttrib`, `glfwGetWindowPos/Size`, `glfwSetWindowAttrib`, `glfwMaximize/RestoreWindow`, `glfwSetWindowSize/Pos` | `set_title` / `set_fullscreen` / `set_windowed` / `set_borderless` / `get_window_pos` / `get_window_size` / `maximize` / `restore` |
| 1336–1417 | `glfwGetMonitors`, `glfwGetVideoMode`, `glfwSetWindowMonitor`, Framebuffer-/Scale-Queries | `monitor_count` / `monitor_info` / `set_fullscreen` |
| **1487–1491** | `glfwGetCocoaWindow`, `glfwGetWin32Window`, `glfwGetX11Window` | **`native_handle(w, kind)`** ← die Bridge zu D3D/Metal/Vulkan |
| 1514–1719 | Monitor-Enum, VideoModes, Framebuffer/Window-Size | `monitor_info` / `get_*_size` |
| **6070–6189** | `glfwJoystickPresent`, `glfwJoystickIsGamepad`, `glfwGetGamepadName`, `glfwGetJoystickName/Buttons/Axes`, `glfwGetGamepadState` | **`gamepad_present` / `gamepad_state`** |
| 7906–8607 | `glfwGetFramebufferSize`, `glfwSetWindowSize` | `get_framebuffer_size` / `set_window_size` |

Diese Tabelle ist die **Arbeitsliste für Phase 0.3** — jede Zeile wird ein Vtable-Call.

---

## 2. Die `vio_platform`-Vtable (`include/vio_platform.h`)

Verfeinert gegenüber `NATIVE-PLATFORM-PLAN.md §4` — ergänzt um die Slots, die der **echte** Code oben tatsächlich braucht (Content-Scale, Cursor-Pos-Query, Input-Mode, Window-Attrib-Get, VideoModes, Maximize/Restore). Grobkörnig (pro-Frame/pro-Fenster), also kein messbarer Dispatch-Overhead.

```c
#ifndef VIO_PLATFORM_H
#define VIO_PLATFORM_H
#include "vio_types.h"

typedef void *vio_window_handle;   /* opak; die aktive Impl kennt den echten Typ */

typedef enum {
    VIO_NATIVE_HWND = 0,           /* Windows: HWND        */
    VIO_NATIVE_HINSTANCE,          /* Windows: HINSTANCE   */
    VIO_NATIVE_NSWINDOW,           /* macOS: NSWindow*     */
    VIO_NATIVE_XLIB_WINDOW,        /* Linux: Window        */
    VIO_NATIVE_XLIB_DISPLAY,       /* Linux: Display*      */
    VIO_NATIVE_WAYLAND_SURFACE,
    VIO_NATIVE_WAYLAND_DISPLAY,
} vio_native_kind;

typedef enum { VIO_CURSOR_NORMAL=0, VIO_CURSOR_HIDDEN=1, VIO_CURSOR_DISABLED=2 } vio_cursor_mode;

typedef struct {
    char  name[128];
    int   x, y, width, height;         /* work area (px) */
    float scale;                       /* content scale (1.0 = 96 dpi) */
    int   refresh_hz;
} vio_monitor_info;

typedef struct {
    int   present;
    char  name[128];
    float axes[6];                     /* VIO_GAMEPAD_AXIS_* order */
    unsigned char buttons[15];         /* VIO_GAMEPAD_* order */
} vio_gamepad_state;

/* Forward-decl aus vio_input.h */
struct _vio_input_state;

typedef struct vio_platform {
    const char *name;                  /* "glfw" | "win32" | "null" | ... */

    /* ── Global lifecycle (MINIT/MSHUTDOWN) ── */
    int   (*init)(void);
    void  (*shutdown)(void);

    /* ── Window lifecycle ── */
    vio_window_handle (*create_window)(vio_config *cfg, const char *backend_name);
    void  (*destroy_window)(vio_window_handle w);
    int   (*should_close)(vio_window_handle w);
    void  (*set_should_close)(vio_window_handle w, int v);
    void  (*show)(vio_window_handle w);

    /* ── Frame ── */
    void  (*poll_events)(void);
    void  (*swap_buffers)(vio_window_handle w);              /* GL-Pfad */
    void  (*get_framebuffer_size)(vio_window_handle w, int *fw, int *fh);
    void  (*get_window_size)(vio_window_handle w, int *cw, int *ch);
    float (*get_content_scale)(vio_window_handle w);

    /* ── GL-Kontext (nur wenn diese Plattform nativen GL kann) ── */
    int   (*gl_make_current)(vio_window_handle w);
    void  (*gl_set_swap_interval)(int interval);
    void *(*gl_get_proc_address)(const char *name);          /* GLAD-Loader-Quelle */
    int   (*gl_context_version)(vio_window_handle w, int *major, int *minor);

    /* ── Fenster-Properties ── */
    void  (*set_title)(vio_window_handle w, const char *utf8);
    void  (*set_window_size)(vio_window_handle w, int cw, int ch);
    void  (*set_window_pos)(vio_window_handle w, int x, int y);
    void  (*get_window_pos)(vio_window_handle w, int *x, int *y);
    void  (*set_fullscreen)(vio_window_handle w, int monitor_idx, int refresh);
    void  (*set_windowed)(vio_window_handle w, int x, int y, int cw, int ch);
    void  (*set_borderless)(vio_window_handle w, int monitor_idx);
    void  (*maximize)(vio_window_handle w);
    void  (*restore)(vio_window_handle w);
    int   (*is_maximized)(vio_window_handle w);
    void  (*set_cursor_mode)(vio_window_handle w, vio_cursor_mode mode);
    void  (*get_cursor_pos)(vio_window_handle w, double *x, double *y);

    /* ── Monitore ── */
    int   (*monitor_count)(void);
    int   (*monitor_info)(int idx, vio_monitor_info *out);
    int   (*monitor_video_modes)(int idx, vio_monitor_info *out, int max);  /* returns count */

    /* ── Gamepad ── */
    int   (*gamepad_present)(int id);
    int   (*gamepad_state)(int id, vio_gamepad_state *out);

    /* ── Native-Handle-Bridge (für GPU-Backends) ── */
    void *(*native_handle)(vio_window_handle w, vio_native_kind kind);

    /* ── Vulkan-Surface-Hooks (nur wo Vulkan relevant) ── */
    const char **(*vk_instance_extensions)(uint32_t *count);
    int   (*vk_create_surface)(vio_window_handle w, void *instance, void *out_surface);

    /* ── Input-Wiring ── */
    void  (*install_input)(vio_window_handle w, struct _vio_input_state *state);
} vio_platform;

/* Registry (analog vio_backend) */
int             vio_register_platform(const vio_platform *p);
const vio_platform *vio_find_platform(const char *name);
const vio_platform *vio_get_active_platform(void);   /* Auto: win32|cocoa|x11 > glfw > null */
void            vio_set_active_platform(const vio_platform *p);

#endif
```

**Wichtig:** `ctx->window` ist schon `void*` (`vio_context.h:25`). Nach dem Umbau ist es ein `vio_window_handle` — kein ABI-Bruch im Context-Objekt, nur die Semantik des Handles wandert von „GLFWwindow*" zu „was die aktive Platform-Impl erzeugt hat".

---

## 3. Phase 0 — Kapselung (plattformunabhängig, hier auf Linux baubar + testbar)

**Definition of Done:** 88/88 Tests grün · Audit-Gate `073` grün · `grep -E 'glfw[A-Z]|GLFW_' php_vio.c src/vio_*.c src/backends/` = **0 außerhalb `src/platform/glfw/`** · GLFW bleibt lauffähige Default-Impl.

### 0.1 Neue Header
- `include/vio_platform.h` (Vtable oben) + Ergänzungen in `vio_types.h` falls nötig (aktuell reichen `vio_config`/`vio_feature`).

### 0.2 GLFW-Impl isolieren → `src/platform/glfw/vio_platform_glfw.c`
- Kompletten Inhalt von `src/vio_window.c` hierher verschieben und in Vtable-Slots gießen (`create_window` = heutiges `vio_window_create`, `poll_events` = `glfwPollEvents`, …).
- Die 6 Callbacks aus `src/vio_input.c` (`glfw_*_callback` + `vio_input_install_callbacks`) hierher; `install_input` füllt sie via `glfwSetWindowUserPointer(state)`.
- `#include <GLFW/glfw3.h>` verschwindet aus `vio_window.h` / `vio_input.h` — nur noch diese eine `.c` zieht GLFW.
- Die `#ifdef _WIN32`-DPI-Awareness + Cursor-Monitor-Placement (`vio_window.c:33–92`) bleiben vorerst hier; Phase 1 übernimmt sie **1:1** in den Win32-Layer (der Code ist bereits nativer Win32).

### 0.3 `php_vio.c` entkernen (die Tabelle aus §1.1)
- Jede der 104 Call-Sites → `vio_get_active_platform()->slot(...)`. Praktisch: einen `static const vio_platform *g_plat;` in MINIT setzen, dann mechanisch ersetzen.
- Die 39 `#ifdef HAVE_GLFW`-Blöcke fallen weg — Fallback „keine Plattform aktiv" lebt zentral in der **Null-Platform** (0.5), nicht mehr als verstreute `#else`-No-ops.

### 0.4 Backends auf die Bridge umstellen
- `vio_opengl.c:1340` — der GLAD-Loader wird `gladLoadGLLoader((GLADloadproc)g_plat->gl_get_proc_address)`. **Das ist der einzige GL-seitige GLFW-Kontaktpunkt** und der Angelpunkt für den nativen WGL-Loader in Phase 1.
- `php_vio.c:1487–1491` — `glfwGetWin32Window/CocoaWindow/X11Window` → `g_plat->native_handle(w, kind)`. D3D11/D3D12/Vulkan/Metal-`setup_context` nehmen das Ergebnis unverändert (sie nehmen heute schon `void*`).

### 0.5 Null-Platform → `src/platform/null/vio_platform_null.c`
- Alle Slots als sichere No-ops / „headless"-Defaults. Ersetzt die `#else`-Zweige. Damit baut php-vio **ganz ohne** Windowing-Lib (wie heute iOS).

### 0.6 Audit-Gate → `tests/089_audit_gate_no_glfw_outside_platform.phpt`
- Kopie von `tests/070_audit_gate_no_gl_outside_backend.phpt`. Pattern `/\bglfw[A-Z]/` bzw. `GLFW_`, Exempt-Verzeichnis `src/platform/glfw/`. Ab hier hält die Grenze dauerhaft.
- (Nummer 089, weil 073 im Repo bereits doppelt belegt ist: `073_metal_render_target` **und** `073_render_target_stack_pressure`.)

> Phase 0 ist auf Linux vollständig baubar und die 88 Tests laufen headless (Xvfb + Mesa). **Diesen Teil kann ich in dieser Session real verifizieren.**

---

## 4. Phase 1 — Win32-Layer (`src/platform/win32/`)

Dateien: `vio_platform_win32.c` (Fenster/Frame/Props/Monitore), `vio_win32_input.c` (WndProc→Input), `vio_win32_gl.c` (WGL), `vio_win32_gamepad.c` (XInput), `vio_win32_keymap.h` (VK→VIO). Registriert sich in MINIT als Platform `"win32"`.

### 4.1 Fenster & Frame
- **Klasse:** `RegisterClassExW` (einmalig, in `init`), `CreateWindowExW` mit `WS_OVERLAPPEDWINDOW`. UTF-16-Titel via `MultiByteToWideChar(CP_UTF8,…)`.
- **Event-Pump (`poll_events`):** `while (PeekMessageW(&msg, NULL, 0,0, PM_REMOVE)) { TranslateMessage; DispatchMessageW; }` — dasselbe pull-basierte Modell wie `glfwPollEvents`, kein Callback-Thread.
- **`should_close`:** Flag im Fenster-State, gesetzt bei `WM_CLOSE`.
- **DPI:** `vio_enable_dpi_awareness_windows()` aus `vio_window.c:33–48` **1:1 übernehmen** (ist schon nativ). Content-Scale via `GetDpiForWindow(hwnd)/96.0f`. Physische Framebuffer-Größe = Client-Rect in physischen Pixeln (Per-Monitor-V2 liefert das direkt).
- **Monitor-Placement:** `vio_place_window_on_cursor_monitor()` aus `vio_window.c:58–92` **1:1 übernehmen** (bereits `MonitorFromPoint`/`GetMonitorInfoW`).
- **Fullscreen/Borderless/Maximize:** `SetWindowLongPtrW(GWL_STYLE)` + `SetWindowPos`; „echtes" Fullscreen = Style ohne Deko + Monitor-Rect. Die gespeicherte Windowed-Geometrie (`vio_context.h:47` `saved_win_*`) bleibt die Restore-Quelle.
- **Monitore:** `EnumDisplayMonitors` + `GetMonitorInfoW` → `vio_monitor_info`; VideoModes via `EnumDisplaySettingsW`.

### 4.2 Der native GL-Kontext (WGL) — der „intelligente Backend"-Kern

Das ist der von dir betonte Teil: **PHPolygon soll auf Legacy-Rechnern OpenGL bekommen, und das Backend soll die volle GL-Funktionsmenge abdecken.** Umsetzung:

**(a) Pixelformat + moderner Kontext brauchen den „Dummy-Window-Trick".**
`wglChoosePixelFormatARB` / `wglCreateContextAttribsARB` sind selbst WGL-**Extensions** — man kommt an sie nur über einen bereits aktuellen Kontext. Also:
1. Verstecktes Dummy-Fenster + `PIXELFORMATDESCRIPTOR` + `wglCreateContext` (Legacy) → `wglMakeCurrent`.
2. `wglGetProcAddress("wglChoosePixelFormatARB" / "wglCreateContextAttribsARB" / "wglGetExtensionsStringARB")`.
3. Dummy zerstören, echtes Fenster mit dem via `wglChoosePixelFormatARB` gewählten Format (MSAA aus `cfg->samples` → `WGL_SAMPLES_ARB`) neu aufsetzen.

**(b) Versions-Aushandlung = die vorhandene Ladder, nur nativ.**
Die Leiter aus `vio_window.c:181–183` (`{4,6}…{3,0}`) wird hier zur Attribut-Liste für `wglCreateContextAttribsARB`:
```c
WGL_CONTEXT_MAJOR_VERSION_ARB, major,
WGL_CONTEXT_MINOR_VERSION_ARB, minor,
WGL_CONTEXT_PROFILE_MASK_ARB,  (major*10+minor>=32)
                               ? WGL_CONTEXT_CORE_PROFILE_BIT_ARB
                               : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
```
Von `{4,6}` abwärts probieren, bis einer erzeugt wird — identische Semantik zur GLFW-Ladder, gleiche `gl_context_version()`-Rückgabe.

**(c) Legacy-Fallback (dein Kernanliegen).**
Fehlt `wglCreateContextAttribsARB` (uralte Treiber / reiner Software-GDI-GL), bleibt der **Legacy-`wglCreateContext`-Kontext aus Schritt (a) gültig** — das liefert Compatibility-Profil GL 1.1–2.1. Die vorhandene Ladder floort heute bei 3.0; für echte Legacy-Rechner kann sie hier bewusst bis `{2,1}` verlängert werden, mit dem Compatibility-Profil-Zweig. So bekommt PHPolygon auf Alt-Hardware *irgendein* funktionierendes GL statt eines harten Fehlers.

**(d) „Alle GL-Funktionen abdecken" = Loader-Quelle korrekt wählen.**
GLAD deckt den vollen GL-Funktionssatz bereits ab (`vendor/glad/`) — es fehlt nur die Bezugsquelle. **Gotcha:** `wglGetProcAddress` gibt auf Windows für GL-**1.0/1.1**-Funktionen `NULL` zurück; die kommen aus `opengl32.dll`. Der native Loader muss deshalb kombinieren:
```c
void *vio_win32_gl_get_proc(const char *name) {
    void *p = (void*)wglGetProcAddress(name);
    if (p == NULL || p == (void*)0x1 || p == (void*)0x2 ||
        p == (void*)0x3 || p == (void*)-1) {          /* 1.1-Funktion */
        static HMODULE gl = NULL;
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(gl, name);
    }
    return p;
}
```
Genau dieser Loader wird die `gl_get_proc_address`-Slot-Impl; `vio_opengl.c` ruft ihn über die in 0.4 geänderte Zeile. **Ohne diesen kombinierten Loader fehlen `glClear`, `glViewport` & Co.** — das ist die häufigste WGL-Falle und der Grund, warum „alle Funktionen abdecken" eine explizite Anforderung ist.

**(e) VSync:** `wglGetProcAddress("wglSwapIntervalEXT")` (WGL_EXT_swap_control); `swap_buffers` = `SwapBuffers(hdc)`.

### 4.3 Input (`vio_win32_input.c`)
Ein `WndProc` schreibt direkt in den `vio_input_state` (Pointer via `SetWindowLongPtrW(GWLP_USERDATA)`). Nichts an `vio_input.c`s Kern-Logik ändert sich — nur die Quelle der Events:

| Windows-Message | → `vio_input_state` |
|-----------------|---------------------|
| `WM_KEYDOWN`/`WM_SYSKEYDOWN` (+`WM_KEYUP`) | Scancode/VKey → `VIO_KEY_*` (Tabelle §5), `keys[k]=1/0`, `on_key`-Callback (Action, Mods aus `GetKeyState`) |
| `WM_CHAR` | UTF-16 → Codepoint (Surrogat-Paare zusammensetzen) → **`vio_input_emit_char(state, cp)`** (existiert schon, `vio_input.c:143`) |
| `WM_MOUSEMOVE` | `mouse_x/y` = `LOWORD/HIWORD(lParam)` |
| `WM_LBUTTONDOWN/UP` etc. | `mouse_buttons[0..2]` |
| `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` | `scroll_y/x += delta/120` |
| `WM_SIZE` | `on_resize`-Callback (physische Größe), Backend-`resize` |
| `WM_INPUT` (Raw Input) | Relative Maus-Deltas für `VIO_CURSOR_DISABLED` |

- **Cursor-Modi:** `NORMAL`=`ShowCursor(TRUE)`; `HIDDEN`=`ShowCursor(FALSE)`; `DISABLED`=Cursor verstecken + `RAWINPUTDEVICE` registrieren + `ClipCursor` ans Client-Rect (ersetzt `glfwSetInputMode(GLFW_CURSOR_DISABLED)` + `GLFW_RAW_MOUSE_MOTION`).
- **`vio_inject_key`** (Test-API) bleibt plattformunabhängig — schreibt weiter direkt in `vio_input_state`, unabhängig vom WndProc. **Damit bleiben die Input-Tests ohne echtes Fenster grün.**

### 4.4 Gamepad (`vio_win32_gamepad.c`) — XInput
Ersetzt `glfwJoystick*`/`glfwGetGamepadState` (`php_vio.c:6070–6189`). XInput liefert ein festes 360-Layout — **keine Mapping-DB nötig** für die 90 %-Controller.

| XInput | → VIO |
|--------|-------|
| `XINPUT_GAMEPAD_A/B/X/Y` | `VIO_GAMEPAD_A/B/X/Y` (0–3) |
| `LEFT/RIGHT_SHOULDER` | `LEFT/RIGHT_BUMPER` (4/5) |
| `BACK`/`START` | `BACK`/`START` (6/7) |
| `LEFT/RIGHT_THUMB` | `LEFT/RIGHT_THUMB` (9/10) |
| `DPAD_UP/RIGHT/DOWN/LEFT` | `DPAD_*` (11–14) |
| `sThumbLX/LY/RX/RY` (/32767) | `AXIS_LEFT_X/Y`, `AXIS_RIGHT_X/Y` (0–3) |
| `bLeftTrigger/bRightTrigger` (/255, →[-1,1] oder [0,1]) | `AXIS_LEFT/RIGHT_TRIGGER` (4/5) |
| — (kein Guide in Standard-XInput) | `VIO_GAMEPAD_GUIDE` (8) = 0 |

- `gamepad_present(id)` = `XInputGetState(id,…) == ERROR_SUCCESS`; `id` 0–3.
- **Achsen-Konvention** exakt an GLFWs Mapping angleichen (Y-Invertierung, Trigger-Range), damit `tests/017_gamepad.phpt` / `060_audio_3d_and_gamepad.phpt` unverändert bestehen. **Vor dem Umbau die erwarteten Wertebereiche aus diesen Tests ablesen** und die XInput-Normalisierung danach ausrichten.
- Long-Tail (DualShock etc. ohne XInput) = spätere `gamecontrollerdb.txt`-Politur (Plan §Phase 4), **kein** Blocker für v1.

### 4.5 Native-Handle-Bridge
`native_handle(w, VIO_NATIVE_HWND)` → das HWND. D3D11/D3D12 (`glfwGetWin32Window`→HWND heute) und Vulkan (`vkCreateWin32SurfaceKHR`, Extensions `{VK_KHR_surface, VK_KHR_win32_surface}`) laufen damit unverändert. `vk_instance_extensions`/`vk_create_surface` ersetzen `glfwGetRequiredInstanceExtensions`/`glfwCreateWindowSurface`.

---

## 5. Konkrete Mapping-Tabellen (der „Fleiß-Teil")

### 5.1 VK → `VIO_KEY_*` (Auszug; vollständige Tabelle in `vio_win32_keymap.h`)
Windows liefert Scancodes/VKeys; `VIO_KEY_*` ist GLFW-nummeriert (US-Layout-Positionen). Robusteste Quelle ist der **Scancode** (`(lParam>>16)&0xFF`, +extended-Bit) → VIO, weil layout-unabhängig — genau wie GLFW es macht.

| Windows | VIO_KEY_* |
|---------|-----------|
| `'A'`–`'Z'` (0x41–0x5A) | `VIO_KEY_A`–`Z` (65–90, **identisch**) |
| `'0'`–`'9'` (0x30–0x39) | `VIO_KEY_0`–`9` (48–57, **identisch**) |
| `VK_SPACE` | 32 · `VK_ESCAPE` → 256 · `VK_RETURN` → 257 · `VK_TAB` → 258 · `VK_BACK` → 259 |
| `VK_LEFT/RIGHT/UP/DOWN` | 263/262/265/264 |
| `VK_F1`–`VK_F12` | 290–301 |
| `VK_LSHIFT/LCONTROL/LMENU` | 340/341/342 · rechte Varianten 344/345/346 |
| `VK_NUMPAD0`–`9` | 320–329 · Operatoren 330–336 |

> Die ASCII-Bereiche A–Z/0–9 sind bei Windows-VKey **und** GLFW identisch — nur die Nicht-ASCII-Tasten (Pfeile, F-Tasten, Modifier, Numpad) brauchen die Tabelle. ~100 Einträge, rein mechanisch.

### 5.2 Mods
`VIO_MOD_SHIFT/CONTROL/ALT/SUPER/CAPS_LOCK/NUM_LOCK` (Bitmaske, `vio_constants.h:121`) aus `GetKeyState(VK_SHIFT/CONTROL/MENU/LWIN|RWIN)` + `GetKeyState(VK_CAPITAL/NUMLOCK) & 1`.

---

## 6. Build-Integration (`config.w32`)

- Neuer Schalter `--with-native-platform` (Default in Phase 1: **aus**, GLFW bleibt Default, bis Smoke grün).
- Bei aktiv: `src/platform/win32/*.c` + `src/platform/null/*.c` in die Objektliste; Libs `opengl32.lib gdi32.lib user32.lib xinput.lib` (bzw. `xinput9_1_0.lib` für breitere Kompatibilität).
- `--with-glfw` und `--with-native-platform` schließen sich **nicht** aus — beide Impls können koexistieren, Auto-Wahl bevorzugt `win32`. So bleibt jeder Merge grün.
- `config.m4` (Linux/macOS) unberührt in Phase 1; die `win32`-Sources sind `#ifdef _WIN32`-gegated.

---

## 7. Test & Verifikation

| Ebene | Wie | Wo lauffähig |
|-------|-----|--------------|
| **Audit-Gate 089** | grep-Test: kein `glfw*` außerhalb `src/platform/glfw/` | überall (reiner PHP-Test) |
| **88 Bestandstests** | dürfen in **keiner** Etappe rot werden | Linux (hier) / Win/mac |
| **Input-Injection** (`018`, `026`, `058`) | `vio_inject_key`/`_char` — plattformunabhängig | überall |
| **Gamepad** (`017`, `060`) | Wertebereiche als Kontrakt für die XInput-Normalisierung | Win nativ |
| **GL-Smoke Win32** | Fenster auf, 1 Frame gerendert, `vio_read_pixels` plausibel, Resize-Callback feuert | Win-Runner (ggf. WARP/Mesa-GL) |
| **Perm-Matrix** | `{mit,ohne} GLFW × {mit,ohne} native-platform` kompiliert & lädt | CI |

**Aufteilung der Verifikation für diese Zusammenarbeit:**
- **Was ich in dieser Cloud-Session (Linux) real bauen + testen kann:** Phase 0 komplett (Vtable, GLFW-Kapselung, Null-Platform, Gate 089) inkl. der 88 Tests headless. Das ist der risikoreichste Refactor-Teil und dort ist ein grünes Netz sofort verfügbar.
- **Was du lokal bauen musst (Win32 kann ich hier nicht kompilieren):** den gesamten `src/platform/win32/`-Layer — ich liefere den Code + eine manuelle Sicht-Checkliste (Fenster auf richtigem Monitor, WGL-Version stimmt via `vio_gl_info`, Resize-Callback, Fullscreen↔Windowed-Restore, XInput-Achsen, DPI-Schärfe), du fährst `nmake` + `run-tests.php`.

---

## 8. Risiken & Gegenmaßnahmen

| Risiko | Gegenmaßnahme |
|--------|---------------|
| **WGL-Loader-Gotcha** (1.1-Funktionen fehlen) | Kombinierter Loader §4.2(d) — von Anfang an, nicht nachträglich |
| **Pixelformat-Aushandlung** schlägt fehl | Dummy-Window-Trick §4.2(a); auf `ChoosePixelFormat`-Legacy zurückfallen |
| **Legacy-Treiber** ohne `wglCreateContextAttribsARB` | Legacy-`wglCreateContext` bleibt gültig (Compatibility 1.1–2.1) §4.2(c) |
| **XInput-Achsen** weichen von GLFW ab → Gamepad-Tests rot | Wertebereiche vorab aus `017`/`060` ablesen, Normalisierung danach ausrichten |
| **Keymap-Lücken** | Scancode-basiert (layout-robust); Tabelle gegen `002_constants` prüfen |
| **DirectInput-Controller** (Long-Tail) | Bewusst Phase-4-Politur (`gamecontrollerdb.txt`), nicht v1 |
| **Wartungslast** (mehrere tausend Zeilen) | Gerechtfertigt durch: GLFW-Freiheit, self-contained `vio.dll` (Deployment-Vorteil ggü. php-glfw), einheitliches Handle-Modell mit iOS |

---

## 9. Etappen-Checkliste (Fortschrittsmetrik: `glfw*`-Tokens 104 → 0)

**Phase 0 (hier baubar):**
- [ ] `include/vio_platform.h` + Registry (`vio_register_platform`/`vio_get_active_platform`)
- [ ] `src/platform/glfw/vio_platform_glfw.c` — `vio_window.c` + Input-Callbacks hierher, Vtable füllen
- [ ] `vio_window.h`/`vio_input.h` verlieren `#include <GLFW/glfw3.h>`
- [ ] `php_vio.c` entkernen (Tabelle §1.1) → `g_plat->…`; 39 `#ifdef HAVE_GLFW` raus
- [ ] `vio_opengl.c:1340` GLAD-Loader → `g_plat->gl_get_proc_address`
- [ ] Native-Handle-Bridge (`php_vio.c:1487–1491`) → `native_handle()`
- [ ] `src/platform/null/vio_platform_null.c`
- [ ] `tests/089_audit_gate_no_glfw_outside_platform.phpt`
- [ ] **Gate:** 88/88 grün, 089 grün, `grep glfw` = 0 außerhalb `src/platform/glfw/`

**Phase 1 (du baust lokal):**
- [ ] `src/platform/win32/vio_platform_win32.c` — Fenster/Frame/Props/Monitore (+ DPI/Placement aus `vio_window.c` übernommen)
- [ ] `vio_win32_gl.c` — WGL-Kontext (Dummy-Trick, Ladder, Legacy-Fallback, kombinierter Loader, Swap-Interval)
- [ ] `vio_win32_input.c` — WndProc → `vio_input_state`
- [ ] `vio_win32_keymap.h` — VK/Scancode → `VIO_KEY_*`
- [ ] `vio_win32_gamepad.c` — XInput → `VIO_GAMEPAD_*`
- [ ] `config.w32` — `--with-native-platform`, Libs, Auto-Wahl `win32 > glfw`
- [ ] Windows-Build ohne `glfw3.dll` öffnet Fenster, rendert (D3D **und** WGL-GL), Input + Gamepad
- [ ] Manuelle Sicht-Checkliste grün

---

## 10. Ausführungshinweise (für Claude Code)

Dieser Plan wird von Claude Code im Repo umgesetzt. Reihenfolge und Kontrakt für die Umsetzung:

1. **Phase 0 zuerst, in einem eigenen Branch** (`feat/platform-abstraction`). Sie ist plattformunabhängig, lässt sich auf Linux (Xvfb + Mesa) bauen und gegen die **88 Bestandstests** headless grün fahren — das ist das Sicherheitsnetz für den riskantesten Refactor-Teil. Erst wenn Gate 089 grün ist und `grep -E 'glfw[A-Z]|GLFW_'` außerhalb `src/platform/glfw/` **0** liefert, ist Phase 0 fertig.
2. **Phase 1 (Win32) als zweiter Branch**, nach Phase 0. Der Win32-Layer ist nur auf einem Windows-Host mit PHP-SDK + MSVC baubar (`nmake`); dort die manuelle Sicht-Checkliste aus §7 abarbeiten. Auf Nicht-Windows-CI nur kompilieren/laden (Perm-Matrix), nicht laufen lassen.
3. **Kein Big-Bang.** Jeder Merge lässt bestehende Builds grün: GLFW bleibt Default-Impl, bis der Win32-Layer die Smoke-Tests besteht (`--with-native-platform` erst dann auf Default).
4. **Fortschrittsmetrik** in jedem PR mitführen: `glfw*`-Tokens in `php_vio.c` **104 → 0** (analog zum GL-Gate-Trend beim OpenGL-Refactor).
5. **Die Kontrakte aus diesem Dokument sind bindend:** die Vtable-Signatur (§2), die Fundstellen-Tabelle (§1.1), der kombinierte WGL-Loader (§4.2 d), die Keymap/XInput-Angleichung an die Bestandstests (§5, §4.4). Abweichungen nur mit dokumentierter Begründung.
