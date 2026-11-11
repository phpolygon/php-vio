# System-Webview – Plan

> **Status:** 🚧 macOS und Linux umgesetzt (2026-11-06, Tests 292–299), Issue #120. Offen: Win32 (WebView2).
> **Baut auf:** `DESKTOP-APP-PLAN.md` Phasen 0–5 (Event-Loop, mehrere Fenster, Menüs, Dialoge, Tray, IME, AccessKit).
> **Ziel:** NativePHP-Apps (Laravel + Blade/Livewire/Inertia) laufen auf php-vio ohne Electron. php-vio liefert eine
> System-Webview als Kind eines `VioWindow` und eine Brücke, die Anfragen der Webview im selben Prozess an PHP gibt –
> ohne HTTP-Server. Der NativePHP-Treiber (Facades → vio) ist ein eigenes PHP-Paket.

## 1. API

```php
$wv = vio_webview($window, [                        // VioContext|VioWindow; false ohne Fähigkeit
    'url'         => 'app://localhost/',
    'bounds'      => [0, 0, 1280, 800],             // logisch; ohne: ganzer Client-Bereich, folgt dem Resize
    'devtools'    => true,
    'transparent' => false,                         // GPU-Inhalt unter der Webview sichtbar
    'schemes'     => ['app' => fn(array $req) => [  // $req: id, method, url, headers, body
        'status' => 200, 'headers' => ['Content-Type' => 'text/html'], 'body' => '…',   // body: string | iterable<string>
    ]],
]);
vio_webview_navigate($wv, 'app://localhost/settings');
vio_webview_eval($wv, 'window.dispatchEvent(new CustomEvent("native", {detail: 1}))');
vio_webview_on_message($wv, fn(string $json) => …);          // JS: window.vio.postMessage(wert)
vio_webview_on_event($wv, fn(string $type, array $data) => …); // navigated, title, new_window, closed
vio_webview_bounds($wv, [$x, $y, $w, $h]);                   // null: ganzer Client-Bereich
vio_webview_destroy($wv);
vio_platform_info()['webview'];                              // bool, mit Grund wie die D-Bus-Fähigkeiten
```

- Neue Zend-Klasse `VioWebview` (hält das Fenster), neue Plattform-Slots `webview_create/_destroy/_navigate/_eval/
  _set_bounds` + Fähigkeit `VIO_PLATFORM_FEATURE_WEBVIEW` über `available`. GLFW und `null`: `NULL`-Slots.
- **Abweichung vom Issue:** Schemes hängen an der Webview (`'schemes'`), nicht an einem globalen
  `vio_webview_scheme()`. Alle drei Engines verlangen das Scheme beim Anlegen (WebView2: Environment-Optionen,
  WKWebView: `WKWebViewConfiguration`, WPE: Web-Context); ein späteres globales Registrieren hätte für
  bestehende Webviews keine Wirkung gehabt.
- Callbacks (Scheme, Nachricht, Ereignis) laufen nur in `vio_poll_events`/`vio_wait_events` (Grundsatz 4). Die
  Engine-Rückrufe legen die Anfrage in die Queue des Fensters und halten die Antwort offen (Deferral bzw.
  `WKURLSchemeTask`), `vio_wake` weckt die Schleife.
- Ein langsamer PHP-Handler blockiert die vio-Schleife, nicht die Webview (alle drei Engines rendern in eigenen
  Prozessen). Dokumentieren, kein Thread-Pool.
- Hat die Webview den Fokus, gehen Tasten an sie, `vio_on_key` feuert nicht. `vio_on_focus` meldet den Wechsel.

## 2. Engines

| Plattform | Engine | Laden | Einbettung |
|---|---|---|---|
| Win32 | WebView2 (Evergreen-Runtime) | eigener Loader: Runtime-Pfad aus der Registry (EdgeUpdate `ClientState`), `EmbeddedBrowserWebView.dll` per `LoadLibraryW`; Fallback `WebView2Loader.dll` neben `php.exe` | Controller als Kind des HWND, über der Swapchain |
| Cocoa | WKWebView | `WebKit.framework` per `dlopen` + `NSClassFromString` (wie `UNUserNotificationCenter`) | Subview der Content-View, über der Metal-Layer-View |
| X11 / Wayland | WPE WebKit, sonst WebKitGTK 4.1 | `libWPEWebKit-2.0` + `libWPEBackend-fdo-1.0` bzw. `libwebkit2gtk-4.1` per `dlopen` | WPE: SHM-Frames als X11-Kindfenster / Wayland-Subsurface, Eingaben von vio weitergereicht. GTK: X11 umgehängtes GTK-Fenster, Wayland eigenes Fenster, headless `GtkOffscreenWindow` |
| GLFW, null | – | – | `NULL` |

- **WebView2 ohne SDK zur Build-Zeit** (Grundsatz 5): die benötigten COM-Interfaces (Environment, Controller,
  CoreWebView2, Settings, WebResourceRequested/-Response, WebMessageReceived, Custom-Scheme-Registration) als
  C-Vtables mit lokalen GUIDs in `src/platform/win32/vio_webview2.h`, wie bei den Datei-Dialogen. Das STA kommt aus der
  vorhandenen `CoInitializeEx`-Stelle der Win32-Plattform. Fehlt die Runtime: `webview` = false mit Grund.
- **Brücken-Skript** beim Dokumentstart: WebView2 `AddScriptToExecuteOnDocumentCreated` + `WebMessageReceived`,
  WKWebView `WKUserScript` + `WKScriptMessageHandler`, WPE `WebKitUserContentManager`. Überall dasselbe
  `window.vio.postMessage(v)` → `JSON.stringify` → PHP-String.
- **Devtools:** WebView2 `AreDevToolsEnabled`, WKWebView `isInspectable` (macOS 13.3+), WPE `enable-developer-extras`.

## 3. Entscheidungen und Befunde

1. **Linux-Engine: WPE WebKit, sonst WebKitGTK** (entschieden 2026-11-06; zuerst nur WPE, dann fiel auf, dass Ubuntu 24.04 und 25.10 kein WPE paketieren – Debian trixie hat beides). WebKitGTK braucht GTK und dessen Fenster; die X11-/Wayland-
   Plattformen von vio sind kein GTK, und ein GTK-Widget als Kind eines fremden Fensters geht nur unter X11 (GTK3
   `GtkPlug`/XEmbed), unter Wayland gar nicht. WPE (`libWPEWebKit` + WPE-Backend, per `dlopen`) rendert ohne Toolkit in
   EGL-/SHM-Puffer; vio zeigt sie als Textur (bzw. Subsurface) und reicht Maus, Tasten, Scrollen und IME durch. Gleich
   auf X11 und Wayland, Webview über GPU-Inhalt ist eingebaut. Kosten: Eingabe-Weiterleitung, Cursor, Zwischenablage und
   Barrierefreiheit verdrahtet vio selbst; Paketlage je Distribution prüft W0. GMainContext wird in `wait_events`
   eingebunden (`g_main_context_query` → FDs in das vorhandene `poll()`, wie `vio_linux_dbus_fd`).
2. **Cookies hält vio** (gemessen 2026-11-06). WKWebView speichert für eigene Schemes gar keine Cookies, auch keine aus
   `document.cookie`; `localStorage` geht. vio führt deshalb auf jeder Engine einen Cookie-Speicher je Webview (nur Sitzung):
   Set-Cookie einer PHP-Antwort geht dorthin statt an die Engine, jede Anfrage bekommt ihren Cookie-Header von dort, und ein
   Startskript (`webview_set_script`) stellt `document.cookie` für die nicht-HttpOnly-Cookies bereit (axios liest dort
   `XSRF-TOKEN`); Zuweisungen aus JS kommen über die Brücke zurück. Überall gleich, auch wo eine Engine es selbst könnte.
3. **Weiterleitungen folgt vio** (gemessen 2026-11-06). WKWebView gibt `fetch` am Custom Scheme die rohe 302 und zeigt nach
   einem weitergeleiteten Form-POST eine leere Seite – Laravels Standardantwort auf Formulare. Die private Methode
   `_willPerformRedirection:newRequest:completionHandler:` bestätigt bei `fetch` nie und bleibt ungenutzt. vio übernimmt die
   Cookies der Weiterleitung; eine Navigation (die Plattform meldet sie) bekommt eine Seite mit `location.replace(ziel)` –
   Adresse stimmt danach, Ziel per GET wie nach 303 –, ein `fetch` die Antwort des Ziels über den Handler (GET nach 303 und
   weitergeleitetem POST, sonst wie gestellt; `Response.url` bleibt die erste URL), höchstens 20.
4. **Weitere Befunde W0 (WKWebView):** POST-Bodies kommen bei `fetch` als `HTTPBodyStream`, nicht als `HTTPBody`;
   Streaming und SSE funktionieren am Custom Scheme; 60 MB Antwort und 20 MB Upload gehen; die Webview läuft in einem
   versteckten (headless) Fenster.

## 4. Phasen

### W0 – Spike (je Engine ein Wegwerf-Programm) – ✅ WKWebView (Befunde in Abschnitt 3)
Beantworten und hier festhalten: POST-Body am Scheme (WKWebView lieferte für `fetch` mit Body zeitweise `nil` →
`HTTPBodyStream`), Cookies/`localStorage`/`document.cookie` am Custom Scheme, Streaming (mehrere Chunks, SSE),
Bodies > 50 MB, Redirects (302 mit `Location`), Webview in einem versteckten/headless Fenster (für die CI), WebView2-
Runtime auf den Windows-Runnern, WPE-Pakete und -Backend auf Ubuntu 24.04 (Xvfb und weston).

### W1 – Win32 und Cocoa – ✅ Cocoa
`vio_webview`, `_navigate`, `_eval`, `_bounds`, `_destroy`, `'schemes'` (Antwort als String, Binärdaten), Nachrichten
in beide Richtungen, Devtools, Resize folgt dem Fenster, Freigabe mit dem Fenster.
**Tests:** 292 Vertrag (`vio_platform_info()['webview']`, `false` auf GLFW/null ohne Warnung, Argumentfehler →
`ValueError`, Freigabe vor/nach dem Fenster), 293 Scheme (HTML + JS + CSS + PNG aus PHP, `fetch` POST mit JSON- und
Binär-Body, Header, Status 404/302), 294 Brücke (Seite meldet sich per `postMessage`, PHP antwortet per `eval`, die
Seite bestätigt). Skip mit Grund ohne Runtime.

### W2 – Linux – ✅ WPE + WebKitGTK, X11 + Wayland
`src/platform/linux/vio_webview_linux.c`, gemeinsam für X11 und Wayland. Beide Ports haben dieselbe `webkit_*`-API: Scheme,
Brücke, Ereignisse und Streaming sind einmal geschrieben (eine `WebKitWebContext` je Webview, Schemes als sicher und
CORS-fähig registriert). GLib hängt in den `poll()`-Schleifen der Plattformen; eine Antwort, die PHP bis zum nächsten Poll nicht
beendet hat, läuft als Stream über ein Socket-Paar. Navigationen erkennt das Modul an `Upgrade-Insecure-Requests`.
`VIO_WEBVIEW_ENGINE=wpe|gtk` wählt fest. Befunde:
- GTK macht in unserem Prozess seinen eigenen GL-/EGL-Kontext aktuell; jede GLib-Auslieferung stellt vios Kontext wieder her.
- JavaScriptCore übernimmt SIGUSR1 (PHPs Zend-Signals) und meldet das auf stderr; vio gibt das Signal vorher frei.
- Ohne Barrierefreiheits-Bus bzw. ohne Seat (headless weston) warnt GTK; diese beiden Meldungen werden gefiltert.
- GTK3 hört auf XInput2 und ignoriert per `XSendEvent` geschickte Core-Ereignisse; Test 299 nutzt deshalb XTest.
- Ein umgehängtes Override-Redirect-Fenster bekommt keinen Tastaturfokus vom Fenstermanager: ein Klick in die Webview
  nimmt ihn (beide Engines), ein Klick ins vio-Fenster gibt ihn zurück.
- Der erste WebKit-Webprozess nach einem Build braucht im Container über 20 s; die Tests warten bis 60 s.
Nicht automatisch geprüft: sichtbare Webviews unter Wayland (WPE-Subsurface, GTK-Fenster) und Wayland-Eingabe (headless weston
hat keinen Seat). Tastenwiederholung in einer WPE-Webview unter Wayland fehlt (vios Wiederholung ist clientseitig).

### W3 – Laravel-tauglich – ✅ Cocoa (+ Test 298 Weiterleitungen)
Streaming-Antworten (`body` als iterable: ein Chunk je Schleifendurchlauf, die Webview bleibt bedienbar), große
Bodies ohne Komplettkopie, Cookies/Herkunft nach Entscheidung 2, `vio_webview_on_event` (navigated, title, new_window →
Standard: `vio_open_url`), `'transparent' => true` über GPU-Inhalt.
**Tests:** 295 Streaming + SSE, 296 Cookies/Session über zwei Anfragen und einen Neustart der Seite, 297 Webview neben
einem Viewport im selben Fenster (`vio_read_pixels` sieht den GPU-Teil).

### W4 – Beispiel, Doku, Lücken – ✅ bis auf die Folge-Issues
- `examples/webview_app.php` ohne Laravel (für die CI) und `examples/webview_laravel.php <pfad-zur-app>`: Laravel in
  einem Prozess (`$kernel->handle(Request::create(...))`), keine Laravel-Abhängigkeit im Repo.
- `vio.stub.php`, CLAUDE.md (API, Testtabelle, nächste freie Nummer), README, Phase 6 in `DESKTOP-APP-PLAN.md`.
- Folge-Issues für die Lücken aus Abschnitt 5.

## 5. Übergabe Windows (WebView2) – Anweisungen für die nächste Sitzung

Der Rest von W1. Alles andere (Kern, Cookies, Redirects, Streaming, Tests) steht; Windows liefert nur die Plattform-Slots.

**Vorbild:** `src/platform/cocoa/vio_webview_cocoa.m` (ein Kindobjekt je Webview, Request-Tabelle id → Engine-Objekt) und
`src/platform/linux/vio_webview_linux.c` (Antwort sammeln bis `reply_end`, sonst Stream). Vertrag der Slots:
`include/vio_platform.h`, Abschnitt „System webview“.

**Umsetzen** in `src/platform/win32/vio_webview_win32.c` (in `config.w32` neben `vio_platform_win32.c` eintragen), Slots in
`vio_platform_win32` setzen (`webview_create` … `webview_reply_fail`, `webview_set_script`), `available(VIO_PLATFORM_FEATURE_WEBVIEW)`:
1. **Kein SDK zur Build-Zeit** (Grundsatz 5): die nötigen COM-Interfaces als C-Vtables mit lokalen GUIDs in
   `src/platform/win32/vio_webview2.h` (Environment, Environment-Options mit Custom-Scheme-Registrierung, Controller,
   CoreWebView2, Settings, WebResourceRequested/-Response/-Deferral, WebMessageReceived, NavigationCompleted/-Starting,
   DocumentTitleChanged, NewWindowRequested, AddScriptToExecuteOnDocumentCreated). Reihenfolge der Vtable-Einträge exakt wie
   in `WebView2.h` des SDK (zum Abgleich herunterladen, nicht einchecken).
2. **Runtime finden:** Version und Pfad der Evergreen-Runtime aus der Registry (EdgeUpdate `ClientState`,
   `{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}`), `EmbeddedBrowserWebView.dll` per `LoadLibraryW`,
   `CreateWebViewEnvironmentWithOptionsInternal`; Rückfall `WebView2Loader.dll` neben `php.exe`. Fehlt beides:
   `available` = 0, kein Warning.
3. **Custom Schemes** bei der Environment-Erzeugung registrieren (`ICoreWebView2EnvironmentOptions4`,
   `TreatAsSecure`, `HasAuthorityComponent`, Allowed Origins = die eigenen Schemes) und `AddWebResourceRequestedFilter`
   je Scheme. Im Handler: Deferral holen, Anfrage über `vio_webview_request_event` einreihen, offen halten.
   `navigation` = 1, wenn `ResourceContext` == `DOCUMENT`. Antwort: `CreateWebResourceResponse` mit einem `IStream`;
   Ende in einem Dispatch → `SHCreateMemStream`, sonst ein eigener IStream, der bei leerem Puffer `E_PENDING` liefert, bis
   `reply_end`. Header als `"Name: Wert\r\n"`-Liste. `reply_fail`: Antwort mit Status 0 bzw. `Deferral.Complete` ohne Response.
4. **Brücke:** `window.vio.postMessage(v)` → `window.chrome.webview.postMessage(JSON.stringify(v))`,
   `window.__vioPost(s)` → `postMessage(String(s))`; ankommend `TryGetWebMessageAsString` → `vio_webview_message_event`.
   `set_script`: altes Skript per id entfernen, neues hinzufügen (Bridge zuerst).
5. **Ereignisse:** `NavigationStarting`/`ContentLoading` → navigated (URL), `DocumentTitleChanged` → title,
   `NewWindowRequested` → new_window und `Handled = TRUE`. Devtools über `Settings.AreDevToolsEnabled`,
   `transparent` über `DefaultBackgroundColor` (alpha 0).
6. **Event-Schleife:** WebView2 braucht ein STA und den Nachrichten-Pump des Threads – beides gibt es
   (`CoInitializeEx` in `vio_platform_win32.c`, `w32_poll_events`/`MsgWaitForMultipleObjectsEx`). Jeder Engine-Callback
   weckt die Wartezeit wie `vio_wake`, ohne den klebrigen Weck-Zustand zu setzen (vgl. `cocoa_webview_kick`).
   Environment und Controller entstehen asynchron: `webview_create` pumpt Nachrichten, bis der Controller da ist (Timeout 30 s).
7. **Größe:** Controller-`Bounds` in Pixeln (logisch × DPI-Scale des Fensters), ohne Bounds der Client-Bereich, bei `WM_SIZE` nachführen.
   Headless (`WS_POPUP`, unsichtbar): Controller trotzdem anlegen, `IsVisible = TRUE`.

**Prüfen:** Tests 292–298 müssen grün sein (sie skippen bisher auf Windows), 263 erwartet für win32 `webview` = 1 oder 0 –
nach der Umsetzung auf `1` festziehen (`tests/window/263`, Muster `'win32'`). Nach dem Muster von 299 einen Test für echte
Eingabe auf Win32 (`SendInput`/`PostMessageW` per FFI an das Kindfenster des Controllers) als 285 ergänzen. Zusätzlich
`examples/webview_laravel.php <app> --smoke`. Danach in der Windows-CI WebView2 (ist auf den Runnern installiert) über
`VIO_REQUIRE_WEBVIEW=1` zur Pflicht machen, CLAUDE.md (Testtabelle, „nächste freie“) und diesen Plan nachziehen.

**Gelernt auf macOS/Linux, auf Windows zuerst messen:** ob die Engine für eigene Schemes Cookies speichert (vio hält sie
ohnehin selbst, `Cookie`-Header der Engine wird verworfen), ob sie Redirects folgt (vio folgt selbst; dann darf die Engine
nicht zusätzlich), ob POST-Bodies vollständig ankommen, ob der erste Start des Webprozesses länger als 20 s dauert.

## 6. Abgleich mit der NativePHP-API

| NativePHP | vio heute | Folge |
|---|---|---|
| Window | `vio_window`, Fenster-Ereignisse, Unterfenster | ✅; Fensterstile (rahmenlos, versteckte Titelleiste, Vibrancy) eigenes Issue |
| Menu / MenuBar / ContextMenu | `vio_menu_bar`, `vio_popup_menu` | ✅ (Linux: `false`, Entscheidung 1 des Desktop-Plans) |
| Dialog / Alert | `vio_file_dialog`, `vio_message_box` | ✅ |
| Notification | `vio_notify` | ✅ (macOS nur im Bundle) |
| Clipboard | `vio_clipboard_get/set` (Text) | Issue: Bilder und HTML |
| ProgressBar / Dock | `vio_set_progress` | Issue: Dock-Badge (`NSDockTile.badgeLabel`, `ITaskbarList3::SetOverlayIcon`, LauncherEntry `count`) |
| Screen | `vio_monitors`, `vio_monitor_info` | ✅; Cursor-Position in Bildschirmkoordinaten prüfen |
| Shell `openExternal` / `showItemInFolder` | `vio_open_url` / `vio_reveal_file` | ✅ (im Issue als fehlend geführt) |
| GlobalShortcut | – | Issue: `RegisterHotKey`, Carbon `RegisterEventHotKey`, X11 `XGrabKey`, Wayland Portal `GlobalShortcuts` |
| PowerMonitor | – | Issue: `WM_POWERBROADCAST`, `NSWorkspaceWillSleepNotification`, logind `PrepareForSleep` (D-Bus) |
| MenuBar-App (Tray mit Fenster) | `vio_tray` | Issue: Lage des Tray-Icons, Fenster daran ausrichten |
| ChildProcess, Settings, AutoUpdater | – | PHP-Paket |

## 7. Abschluss

Wie im Desktop-Plan: Tests grün auf Linux (Xvfb + weston), macOS und Windows, `vio_platform_info()['webview']` stimmt
überall, Stub/CLAUDE.md/README nachgezogen. Erster Anwender: `phpolygon/editor` ohne Electron.
