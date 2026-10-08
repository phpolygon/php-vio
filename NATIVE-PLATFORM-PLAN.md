# Native Platform Layer — GLFW-Ablöse-Plan

> **Stand (2026-10-08):** Phase 0 und Phase 1 (Win32) umgesetzt, siehe `WIN32-PLATFORM-PLAN.md` und OPEN-ITEMS A1/A2.

> **Status:** 📋 **Entwurf / geplant** — noch kein Branch, keine Commits.
> **Ziel-Issue:** _(anzulegen)_ „Eigene Windowing/Input-Schicht, GLFW optional bzw. entfernbar".
> **Vorlage:** dieselbe Mechanik wie `OPENGL-REFACTOR-PLAN.md` (Abstraktion + erzwungener Audit-Gate), angewandt auf **GLFW statt OpenGL**.
> **Präzedenzfall im Repo:** Der iOS-Backend (`src/backends/ios/`) ist bereits „die Window+Input-Hälfte von GLFW" und speist Metal über den GLFW-agnostischen Pfad `vio_metal_setup_context_native`. Die Ablöse verallgemeinert genau dieses Muster auf Desktop.

---

## 1. Ziel

GLFW wird von einer **fest verdrahteten Pflicht-Dependency** zu einer **austauschbaren `vio_platform`-Implementierung hinter einer Vtable**. Nach Abschluss gilt:

- `grep -rE 'glfw[A-Za-z]|GLFW_' src/ include/ php_vio.c` liefert **0 Treffer außerhalb `src/platform/glfw/`** — bewacht durch einen Audit-Gate-Test analog zu `tests/070_audit_gate_no_gl_outside_backend.php`.
- Es existieren **native Backends** (`src/platform/win32/`, `src/platform/cocoa/`, `src/platform/x11/` …), die dieselbe `vio_platform`-Vtable erfüllen. Ein Build kann mit `--with-native-platform` **ganz ohne GLFW** ein Fenster öffnen, Input verarbeiten und präsentieren.
- Die **PHP-API bleibt byte-identisch**: `VIO_KEY_*`/`VIO_MOUSE_*` behalten ihre (GLFW-nummerierten) Werte; native Layer übersetzen Plattform-Keycodes auf diese kanonischen IDs.
- GLFW bleibt zunächst als Default-Impl erhalten (Fallback + Referenz), wird aber **nicht mehr direkt** aus `php_vio.c` oder den GPU-Backends aufgerufen.

Nicht-Ziel: GLFW sofort löschen. Der Wert entsteht bereits in **Phase 0** (Kapselung); die nativen Layer folgen plattformweise und risikoarm, ohne je einen Big-Bang-Cutover.

---

## 2. Ist-Zustand — was GLFW heute liefert

GLFW deckt in php-vio **fünf** Belange ab, verteilt über den Baum:

| Belang | Heutige Fundstellen | GLFW-Calls |
|--------|---------------------|-----------|
| **Fenster-Lifecycle & -Properties** | `php_vio.c` (Fullscreen, Monitor-Placement, Titel, Position, Content-Scale, Cursor-Mode, Maximize/Restore) | ~81 Call-Sites, 41 `#ifdef HAVE_GLFW`-Blöcke |
| **Fenster-/Kontext-Erzeugung** | `src/vio_window.c` (GL-Context-Ladder 4.6→3.3, `GLFW_NO_API` für Vulkan/Metal/D3D, MSAA-Hints, `SCALE_TO_MONITOR`, DPI-Awareness) | 47 |
| **Input + Callbacks** | `src/vio_input.c` (key/char/cursor/button/scroll/framebuffer-size, `SetWindowUserPointer`) | 20 |
| **GPU-Anbindung (native Handles / Surface)** | `vio_vulkan.c` (`glfwCreateWindowSurface`, `glfwGetRequiredInstanceExtensions`), `vio_d3d11/12.c` (`glfwGetWin32Window`→HWND), `vio_metal.m` (`glfwGetCocoaWindow`→NSWindow), `vio_opengl.c` (`glfwGetProcAddress` für GLAD) | ~14 |
| **Monitore & Gamepad** | `php_vio.c` (Monitor-Enum/VideoModes, Joystick/Gamepad-State via GLFWs SDL-Mapping-DB) | Teil der 81 |

**Bereits gut:** `ctx->window` ist in `vio_context.h` als `void*` typisiert (Kommentar erklärt warum), die Backends nehmen `void *glfw_window` in ihren `*_setup_context()`-Signaturen, Metal hat schon einen `_native`-Zweig, und alles ist `#ifdef HAVE_GLFW`-gegated (kompiliert ohne GLFW, z. B. iOS).

**Die Lecks (siehe frühere Bewertung):** `GLFWwindow*` steht in den öffentlichen Headern `vio_window.h`/`vio_input.h`; die 81 direkten Calls in `php_vio.c` umgehen die dünne `vio_window`-Schicht; es gibt keine Windowing-Vtable und keinen Audit-Gate für GLFW. Genau das räumt Phase 0 auf.

---

## 3. Kernentscheidungen

1. **Abstraktion zuerst, Ersatz danach.** Wie beim OpenGL-Refactor: erst *eine* Vtable (`vio_platform`) einziehen und GLFW dahinter schieben (0-Verhaltensänderung), dann native Impls plattformweise ergänzen. Kein Schritt lässt die 38 Tests rot werden.

2. **Opaques Handle statt `GLFWwindow*`.** Neuer Typ `vio_window_handle` (opaker `void*`), der von der aktiven Plattform-Impl erzeugt wird und die nativen Handles trägt. Backends fragen **nicht** mehr `glfwGetWin32Window()`, sondern `vio_platform_native(handle, VIO_NATIVE_HWND)` (bzw. `_NSWINDOW`, `_XLIB_WINDOW`, `_WAYLAND_SURFACE`). Kein öffentlicher Header zieht danach `<GLFW/glfw3.h>`.

3. **`VIO_KEY_*`/`VIO_MOUSE_*` sind die kanonische ABI.** Werte bleiben GLFW-nummeriert (`vio_constants.h` unverändert), damit keine PHP-Skripte brechen. Jede native Impl liefert eine **Übersetzungstabelle** Plattform-Scancode/VKey → `VIO_KEY_*`. Das ist der einzige „Datenfleiß"-Teil je Plattform, aber rein tabellarisch.

4. **Gamepad braucht eine Mapping-DB.** GLFW bringt intern `SDL_GameControllerDB` mit. Optionen: (a) `gamecontrollerdb.txt` vendorn und selbst parsen (Zlib-/CC0-Lizenz, passt zum stb/miniaudio-Muster), oder (b) OS-APIs nutzen (XInput auf Windows liefert ein festes 360-Layout ohne DB; IOKit/GameController auf macOS; evdev/`js` auf Linux). Empfehlung: **XInput-first auf Windows** (kein DB nötig für die 90 %-Controller), DB-Parser als Phase-4-Politur für den Long-Tail.

5. **Nativer GL-Kontext ist der teure Long-Tail — Reihenfolge drum herum planen.** `glfwCreateWindow`+`glfwMakeContextCurrent`+`glfwGetProcAddress` durch WGL (Win32) / CGL (Cocoa) / GLX+EGL (Linux) zu ersetzen ist der aufwändigste Einzelposten. Deshalb: Plattformen zuerst, deren **moderne Default-API kein GL braucht** — Windows→D3D11/12, macOS→Metal. Native GL wird pro Plattform optional nachgezogen (oder bewusst weggelassen: „ohne GLFW ⇒ nur moderne Backends", GL bleibt der GLFW-Pfad).

6. **Additiv & gegated.** Neue Layer hinter `--with-native-platform` bzw. Auto-Wahl. Solange GLFW gebaut wird, ist es Default; native Impl greift nur, wenn angefordert oder GLFW fehlt. Jeder Merge lässt bestehende Builds grün.

7. **Event-/Thread-Modell wie heute.** php-vio pollt synchron pro Frame (`vio_poll_events` → `glfwPollEvents`). Native Impls implementieren dasselbe pull-basierte `poll()` (Win32: `PeekMessage`-Pump; Cocoa: `nextEventMatchingMask` im Poll; X11: `XPending`/`xcb_poll_for_event`). Keine Callback-Thread-Umstellung.

---

## 4. Die `vio_platform`-Vtable (`include/vio_platform.h`)

Analog zu `vio_backend` — ein Struct aus Funktionszeigern, das jede Impl in ihrer Registrierung füllt. Grobkörnig (pro-Frame/pro-Fenster, **nie** pro-Item), also ohne messbaren Dispatch-Overhead.

```c
typedef struct vio_platform {
    const char *name;                 /* "glfw" | "win32" | "cocoa" | "x11" | "wayland" */

    /* ── Global lifecycle (MINIT/MSHUTDOWN) ─────────────────────── */
    int   (*init)(void);
    void  (*shutdown)(void);

    /* ── Window lifecycle ───────────────────────────────────────── */
    vio_window_handle (*create_window)(vio_config *cfg, const char *backend_name);
    void  (*destroy_window)(vio_window_handle w);
    int   (*should_close)(vio_window_handle w);
    void  (*set_should_close)(vio_window_handle w, int v);
    void  (*show)(vio_window_handle w);

    /* ── Frame ──────────────────────────────────────────────────── */
    void  (*poll_events)(void);
    void  (*swap_buffers)(vio_window_handle w);       /* GL-Pfad */
    void  (*get_framebuffer_size)(vio_window_handle w, int *w_, int *h_);
    float (*get_content_scale)(vio_window_handle w);

    /* ── GL context (nur wenn diese Plattform nativen GL kann) ───── */
    int   (*gl_make_current)(vio_window_handle w);
    void  (*gl_set_swap_interval)(int interval);
    void *(*gl_get_proc_address)(const char *name);   /* GLAD-Loader */

    /* ── Window properties ──────────────────────────────────────── */
    void  (*set_title)(vio_window_handle w, const char *utf8);
    void  (*set_fullscreen)(vio_window_handle w, int monitor_idx, int refresh);
    void  (*set_windowed)(vio_window_handle w, int x, int y, int cw, int ch);
    void  (*set_borderless)(vio_window_handle w, int on);
    void  (*get_window_pos)(vio_window_handle w, int *x, int *y);
    void  (*get_window_size)(vio_window_handle w, int *cw, int *ch);
    void  (*set_cursor_mode)(vio_window_handle w, int mode);   /* NORMAL/HIDDEN/DISABLED */

    /* ── Monitors ───────────────────────────────────────────────── */
    int   (*monitor_count)(void);
    int   (*monitor_info)(int idx, vio_monitor_info *out);     /* name, workarea, scale, modes */

    /* ── Gamepad ────────────────────────────────────────────────── */
    int   (*gamepad_present)(int id);
    int   (*gamepad_state)(int id, vio_gamepad_state *out);    /* buttons[], axes[], name */

    /* ── Native handle bridge (für die GPU-Backends) ────────────── */
    void *(*native_handle)(vio_window_handle w, vio_native_kind kind);

    /* ── Vulkan surface hooks (nur wo Vulkan relevant) ──────────── */
    const char **(*vk_instance_extensions)(uint32_t *count);
    int   (*vk_create_surface)(vio_window_handle w, void *instance, void *out_surface);

    /* ── Input wiring ───────────────────────────────────────────── */
    void  (*install_input)(vio_window_handle w, vio_input_state *state);
} vio_platform;
```

`vio_native_kind` = `{ VIO_NATIVE_HWND, VIO_NATIVE_NSWINDOW, VIO_NATIVE_XLIB_WINDOW, VIO_NATIVE_XLIB_DISPLAY, VIO_NATIVE_WAYLAND_SURFACE, VIO_NATIVE_WAYLAND_DISPLAY }`. Ein Backend fragt nur die Sorte, die es versteht; alles andere liefert `NULL`.

---

## 5. Phasen

| Phase | Inhalt | GLFW-Abhängigkeit danach |
|-------|--------|--------------------------|
| **0** | Abstraktions-Refactor: `vio_platform`-Vtable, GLFW als einzige Impl in `src/platform/glfw/`, alle `glfw*` aus `php_vio.c`/Backends raus, Audit-Gate `073_*` | GLFW noch Pflicht, aber **gekapselt** |
| **1** | `src/platform/win32/` — natives Fenster + Raw-Input + XInput-Gamepad (+ optional WGL) | Windows baubar **ohne** GLFW (mit D3D) |
| **2** | `src/platform/cocoa/` — NSWindow + Input + Metal-Layer (+ optional CGL) | macOS baubar **ohne** GLFW (mit Metal) |
| **3** | `src/platform/x11/` (Xlib/xcb + GLX/EGL), danach `wayland/` (libwayland + EGL) | Linux baubar **ohne** GLFW |
| **4** | Gamepad-DB, direkte Vulkan-Surfaces, GLFW → optional/entfernbar, Config/Docs/Audit-Gate-Flip | GLFW nur noch **optionaler** Alt-Pfad |

### Phase 0 — Kapselung (voraussetzungslos wertvoll, GLFW bleibt)

Der eigentliche Hebel und exakt die OpenGL-Refactor-Mechanik. Etappen:

- **0.1** `include/vio_platform.h` + `include/vio_types.h`-Ergänzungen (`vio_window_handle`, `vio_native_kind`, `vio_monitor_info`, `vio_gamepad_state`).
- **0.2** `src/platform/glfw/vio_platform_glfw.c` — verschiebt den kompletten Inhalt von `vio_window.c` + die GLFW-Teile von `vio_input.c` hierher und füllt die Vtable. `vio_window.c/.h` und `vio_input.h` verlieren `#include <GLFW/glfw3.h>`.
- **0.3** `php_vio.c` entkernen: jede der 81 Call-Sites → `g_platform->set_title(...)` etc. Ziel-Trend wie beim GL-Gate dokumentieren: `glfw*` in `php_vio.c` **81 → 0**. Die 41 `#ifdef HAVE_GLFW`-Blöcke verschwinden (Fallback-Verhalten „keine Plattform aktiv" lebt zentral in einer Null-Platform).
- **0.4** Backends auf `native_handle()` / `vk_*` umstellen: `vio_d3d11/12` nehmen `native_handle(w, VIO_NATIVE_HWND)` statt `glfwGetWin32Window`; `vio_metal` nutzt schon `_native` — jetzt gefüttert aus `native_handle(w, VIO_NATIVE_NSWINDOW)`; `vio_vulkan` ruft `vk_instance_extensions`/`vk_create_surface`; `vio_opengl` nimmt `gl_get_proc_address`.
- **0.5** `null`-Platform (`src/platform/null/`) für headless/kein-GLFW-Builds — ersetzt die verstreuten `#else`-No-ops.
- **0.6** Audit-Gate `tests/073_audit_gate_no_glfw_outside_platform.php` (Kopie von 070, Pattern `/\bglfw[A-Z]/` bzw. `GLFW_`, Exempt = `src/platform/glfw/`). Ab jetzt hält die Grenze dauerhaft.

**Definition of Done Phase 0:** 38/38 Tests grün, `073`-Gate grün, `grep glfw php_vio.c` = 0 funktional, kein Header außer denen in `src/platform/glfw/` inkludiert GLFW.

### Phase 1 — Win32 nativ (`src/platform/win32/`)

Warum zuerst: Windows ist euer Haupt-Build-Target (D3D11/D3D12 default, `_build_*.bat`), und der D3D-Pfad braucht **keinen** GL-Kontext — der teuerste GLFW-Teil entfällt hier komplett.

- Fenster: `RegisterClassExW` + `CreateWindowExW`, WndProc, `PeekMessage`-Pump als `poll_events`. DPI-Awareness (V2) ist in `vio_window.c` schon nativ vorhanden — direkt übernehmen.
- Input: `WM_KEY*`/`WM_CHAR`/`WM_MOUSE*`/`WM_INPUT` (Raw Input für `CURSOR_DISABLED`), Scancode→`VIO_KEY_*`-Tabelle. Content-Scale via `GetDpiForWindow`.
- Monitore: `EnumDisplayMonitors` + `GetMonitorInfoW` (Placement-Logik existiert bereits in `vio_window.c`).
- Gamepad: **XInput** (kein DB nötig).
- `native_handle(VIO_NATIVE_HWND)` = das HWND. D3D11/12 laufen damit unverändert.
- Optional (später): WGL + `wglChoosePixelFormatARB`/`wglCreateContextAttribsARB` + `wglGetProcAddress` für nativen GL — nur falls GL-ohne-GLFW auf Windows gebraucht wird.
- Vulkan: `vkCreateWin32SurfaceKHR`, Extensions `{VK_KHR_surface, VK_KHR_win32_surface}` hartkodiert.

### Phase 2 — Cocoa nativ (`src/platform/cocoa/`)

ObjC-Infrastruktur existiert bereits (Metal, iOS). NSWindow + NSView, `nextEventMatchingMask`-Poll, `NSEvent`-Keymap → `VIO_KEY_*`, `backingScaleFactor` = Content-Scale. Metal-Layer wie im iOS-Pfad; `native_handle(VIO_NATIVE_NSWINDOW)` füttert `vio_metal_setup_context_native`. Gamepad via GameController.framework. Nativer GL (CGL) optional — auf macOS ohnehin nur Legacy-4.1, kann bewusst dem GLFW-Pfad überlassen bleiben.

### Phase 3 — Linux nativ (`src/platform/x11/`, dann `wayland/`)

Der schwierigste Belang wegen zweier Display-Server. X11 zuerst (Xlib+xcb, `XPending`-Poll, `GLX` bzw. `EGL` für GL, `vkCreateXcbSurfaceKHR`), Wayland als Folge-Etappe (libwayland + `xdg-shell` + EGL, `vkCreateWaylandSurfaceKHR`). Keymap über `xkbcommon`. Realistisch der größte Einzelaufwand; solange nicht fertig, bleibt Linux beim GLFW-Pfad.

### Phase 4 — Abschluss

Gamepad-DB (`gamecontrollerdb.txt` vendorn + Parser) für Nicht-XInput-Controller; GLFW in `config.m4`/`config.w32` von Pflicht auf optional; `--with-native-platform` als Default dokumentieren; README/CLAUDE.md-Update; Audit-Gate-Flip (native Layer werden selbst gate-pflichtig: keine Roh-Win32/X11-Calls außerhalb ihres `src/platform/<name>/`).

---

## 6. Risiken & offene Fragen

- **Nativer GL-Kontext** (WGL/CGL/GLX/EGL) ist fehleranfällig (Pixel-Format-Aushandlung, Kontext-Attribute, Loader). Minderung: GL bewusst dem GLFW-Pfad überlassen, wo ein moderneres Backend Default ist. Frage an dich: **Muss GL überhaupt ohne GLFW laufen**, oder ist „ohne GLFW ⇒ D3D/Metal/Vulkan" akzeptabel?
- **Gamepad-Mapping-Parität.** GLFWs DB deckt hunderte Pads ab. XInput/GameController decken die Mehrheit ohne DB; der Long-Tail braucht Phase 4. Lizenz von `gamecontrollerdb.txt` (Zlib) prüfen.
- **Wayland** ist ein eigenes Projekt (Server-seitige Deko, Fractional Scaling, kein globales Fensterpositionieren). Kandidat, um es dauerhaft bei GLFW zu belassen.
- **Keymap/Scancode-Tabellen** je Plattform sind der Fleiß-Teil; Testabdeckung über Injection (`vio_inject_key`) bleibt plattformunabhängig.
- **Clipboard/IME** nutzt heute teils GLFW; Desktop-IME ist derzeit ohnehin dünn (der ausgebaute IME-Pfad ist iOS). Als Nicht-Ziel markieren oder Phase 4.
- **Binärgröße/Wartung:** eigene Layer = ~mehrere-tausend Zeilen Plattformcode, die GLFW heute kostenlos wartet. Der Gewinn (Dependency-Freiheit, ein Handle-Modell, iOS/Desktop-Einheitlichkeit) muss das rechtfertigen.

---

## 7. Aufwand & Empfehlung

- **Phase 0** ist klein, risikoarm und **unabhängig vom Rest wertvoll** (behebt die Kapselungs-Lecks, führt den `vio_platform`-Handle ein, macht iOS-/Desktop-Symmetrie explizit). Sollte in jedem Fall gebaut werden — auch wenn ihr GLFW nie ablöst.
- **Phase 1 (Win32)** ist der beste erste echte Ablöse-Schritt: höchster Nutzen (Haupt-Target, D3D braucht kein GL), überschaubares Risiko.
- **Phasen 2–3** nach Bedarf; Wayland und nativer GL sind legitime „bleibt-bei-GLFW"-Kandidaten.

**Konkrete Empfehlung:** Phase 0 als eigenen Branch `feat/platform-abstraction` starten, mit demselben Audit-Gate-getriebenen Etappen-Tracking wie beim OpenGL-Refactor (Trend `glfw*`-in-`php_vio.c` 81 → 0 als Fortschrittsmetrik). Erst danach über native Layer entscheiden.

---

## 8. Test-Strategie

- Die 38 bestehenden Tests laufen headless und bleiben der Regressions-Kern — sie dürfen in **keiner** Etappe rot werden.
- Neuer Audit-Gate `073` erzwingt die GLFW-Grenze (wie `070` für GL).
- Input bleibt über `vio_inject_key`/`vio_inject_*` plattformunabhängig testbar.
- Pro nativem Layer eine manuelle Sicht-Checkliste (Fenster öffnet auf richtigem Monitor, Resize feuert Callback, Fullscreen↔Windowed restauriert Geometrie, Gamepad-Achsen, DPI-Schärfe) — analog zu den heutigen Windows-only-Skips.

---

## 9. Verifikation: lokale Multi-Plattform-Matrix + CI-Parität

Leitidee: alle drei OSes **lokal parallel** bauen und testen und dieselbe Matrix in CI spiegeln — was lokal grün ist, ist es dann auch im CI. Das funktioniert, aber mit einer wichtigen Präzisierung, welches Tool welche Plattform trägt.

**Docker ist die Linux-Säule — und nur die.** Ein Linux-Container ist 1:1 das, was die Linux-CI ausführt (echte Parität, reproduzierbar, pinbar). **macOS lässt sich nicht dockern** (Apples EULA bindet macOS-Virtualisierung an Apple-Hardware; Docker kennt keine macOS-Container — Docker Desktop selbst ist nur eine Linux-VM). **Windows-Container** brauchen einen Windows-Host und liefern **kein** echtes Desktop-Fenster/GPU — sie taugen höchstens zum reinen *Kompilieren*. Also: Docker für Linux, je eine **native Säule** für Win/mac, lokal und in CI vom selben Runner-Typ.

| Plattform | Lokal | CI | Was automatisch testbar |
|-----------|-------|----|--------------------------|
| **Linux** | Docker-Image (`docker/`) mit gepinnten Deps | GitHub Actions `ubuntu-*` (dasselbe Image) | Voller Build **+ 38 headless Tests + Gates 070/073**. Mit **Mesa llvmpipe** (GL 4.5 SW) und **lavapipe** (Vulkan SW) + **Xvfb** sogar der echte X11-Native-Layer als Smoke; Wayland via `weston --backend=headless` |
| **Windows** | Windows-VM/-Host (`nmake`), optional MinGW-Cross oder Windows-Container **nur zum Compilen** | GitHub Actions `windows-*` (interaktive Session → echtes Fenster) | Build; D3D11/12 via **WARP** (Software-Rasterizer, kein GPU nötig) als Smoke; headless Tests |
| **macOS** | Dein Mac (der Herd-Build steht schon in `CLAUDE.md`) | GitHub Actions `macos-*` | Build; Metal offscreen als Smoke; headless Tests |

**Was in der vollen Matrix billig überall läuft** (der Kern des Nutzens): **Phase 0** ist reines Compile + headless — die Kapselung, beide Audit-Gates (`070` GL, `073` GLFW) und die 38 Tests laufen auf allen drei OSes ohne echten Bildschirm. Genau hier zahlt die Matrix sofort, noch bevor ein einziger nativer Layer existiert.

**Was Software-GPU zusätzlich hergibt** (für Phasen 1–3): Weil llvmpipe/lavapipe/WARP/Metal-offscreen **ohne echte GPU** rendern, kann jeder native Layer wenigstens „Fenster auf, ein Frame gerendert, `read_pixels` plausibel, Resize-Callback gefeuert" headless smoke-getestet werden. **Nicht** automatisierbar bleibt, was Hardware braucht: echte Gamepads, physische Multi-Monitor-DPI, Vsync-Timing — die bleiben in der manuellen Checkliste (§8).

**Build-Permutationen als eigener Matrix-Job.** `config.m4`/`config.w32` behaupten „kompiliert ohne jede Dependency, Feature dann nur zur Laufzeit weg". Diese Zusage sollte die Matrix *beweisen*: eine Achse `{mit, ohne} GLFW × {mit, ohne} je Backend/Vulkan/FFmpeg/HarfBuzz` (zumindest die sinnvollen Ecken), damit ein fehlendes `#ifdef`-Guard sofort auffällt statt erst beim Release-Zip. Das ist unabhängig von der GLFW-Ablöse wertvoll und wird durch sie noch wichtiger (die `--with-native-platform`- vs. `--with-glfw`-Kombinationen kommen als neue Achse dazu).

**Konkreter erster Schritt (unabhängig von Phase 0 machbar):** ein `docker/Dockerfile.linux` mit gepinnten `glfw3 / mesa (llvmpipe+lavapipe) / vulkan-loader / glslang / spirv-cross / ffmpeg / harfbuzz`, das lokal `make -j && NO_INTERACTION=1 php run-tests.php -d extension=modules/vio.so tests/` reproduzierbar grün macht — und exakt dieselben Schritte als `.github/workflows/ci.yml`-Matrix `{ubuntu, windows, macos} × {backend-perms}`. Damit ist die CI-Parität per Konstruktion gegeben: der Linux-Job *ist* der Container, die Win/mac-Jobs spiegeln die lokalen nativen Builds.
