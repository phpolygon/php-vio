# System-Webview – Plan

> **Status:** 📋 geplant (2026-11-06), Issue #120.
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
vio_webview_bounds($wv, $x, $y, $w, $h);
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
| X11 / Wayland | WPE WebKit | `libWPEWebKit` + Backend per `dlopen` | Puffer als Textur bzw. Subsurface, Eingaben von vio weitergereicht |
| GLFW, null | – | – | `NULL` |

- **WebView2 ohne SDK zur Build-Zeit** (Grundsatz 5): die benötigten COM-Interfaces (Environment, Controller,
  CoreWebView2, Settings, WebResourceRequested/-Response, WebMessageReceived, Custom-Scheme-Registration) als
  C-Vtables mit lokalen GUIDs in `src/platform/win32/vio_webview2.h`, wie bei den Datei-Dialogen. Das STA kommt aus der
  vorhandenen `CoInitializeEx`-Stelle der Win32-Plattform. Fehlt die Runtime: `webview` = false mit Grund.
- **Brücken-Skript** beim Dokumentstart: WebView2 `AddScriptToExecuteOnDocumentCreated` + `WebMessageReceived`,
  WKWebView `WKUserScript` + `WKScriptMessageHandler`, WPE `WebKitUserContentManager`. Überall dasselbe
  `window.vio.postMessage(v)` → `JSON.stringify` → PHP-String.
- **Devtools:** WebView2 `AreDevToolsEnabled`, WKWebView `isInspectable` (macOS 13.3+), WPE `enable-developer-extras`.

## 3. Entscheidungen

1. **Linux-Engine: WPE WebKit** (entschieden 2026-11-06). WebKitGTK braucht GTK und dessen Fenster; die X11-/Wayland-
   Plattformen von vio sind kein GTK, und ein GTK-Widget als Kind eines fremden Fensters geht nur unter X11 (GTK3
   `GtkPlug`/XEmbed), unter Wayland gar nicht. WPE (`libWPEWebKit` + WPE-Backend, per `dlopen`) rendert ohne Toolkit in
   EGL-/SHM-Puffer; vio zeigt sie als Textur (bzw. Subsurface) und reicht Maus, Tasten, Scrollen und IME durch. Gleich
   auf X11 und Wayland, Webview über GPU-Inhalt ist eingebaut. Kosten: Eingabe-Weiterleitung, Cursor, Zwischenablage und
   Barrierefreiheit verdrahtet vio selbst; Paketlage je Distribution prüft W0. GMainContext wird in `wait_events`
   eingebunden (`g_main_context_query` → FDs in das vorhandene `poll()`, wie `vio_linux_dbus_fd`).
2. **Herkunft und Cookies** (offen bis W0). Laravel braucht eine stabile, „sichere“ Herkunft für Cookies, `localStorage` und das
   `XSRF-TOKEN`, das axios (Inertia) aus `document.cookie` liest. Custom Schemes verhalten sich je Engine anders
   (WebView2 nur mit `TreatAsSecure` + `HasAuthorityComponent`; WKWebView speichert für eigene Schemes keine Cookies
   im `WKHTTPCookieStore`). Empfehlung: in W0 messen; wo die Engine es nicht kann, führt vio einen Cookie-Speicher je
   Webview (Set-Cookie der Antwort → Cookie der nächsten Anfrage) und spiegelt nicht-`HttpOnly`-Cookies per Skript in
   `document.cookie`. WebView2 könnte stattdessen `https://app.localhost` abfangen – eine einheitliche URL über alle
   Engines ist wichtiger, deshalb überall Custom Scheme.

## 4. Phasen

### W0 – Spike (je Engine ein Wegwerf-Programm)
Beantworten und hier festhalten: POST-Body am Scheme (WKWebView lieferte für `fetch` mit Body zeitweise `nil` →
`HTTPBodyStream`), Cookies/`localStorage`/`document.cookie` am Custom Scheme, Streaming (mehrere Chunks, SSE),
Bodies > 50 MB, Redirects (302 mit `Location`), Webview in einem versteckten/headless Fenster (für die CI), WebView2-
Runtime auf den Windows-Runnern, WPE-Pakete und -Backend auf Ubuntu 24.04 (Xvfb und weston).

### W1 – Win32 und Cocoa
`vio_webview`, `_navigate`, `_eval`, `_bounds`, `_destroy`, `'schemes'` (Antwort als String, Binärdaten), Nachrichten
in beide Richtungen, Devtools, Resize folgt dem Fenster, Freigabe mit dem Fenster.
**Tests:** 277 Vertrag (`vio_platform_info()['webview']`, `false` auf GLFW/null ohne Warnung, Argumentfehler →
`ValueError`, Freigabe vor/nach dem Fenster), 278 Scheme (HTML + JS + CSS + PNG aus PHP, `fetch` POST mit JSON- und
Binär-Body, Header, Status 404/302), 279 Brücke (Seite meldet sich per `postMessage`, PHP antwortet per `eval`, die
Seite bestätigt). Skip mit Grund ohne Runtime.

### W2 – Linux
WPE auf X11 und Wayland: Puffer als Textur, Eingabe, Cursor, Zwischenablage und IME durchgereicht; dieselben Tests 277–279 (Xvfb + weston in der CI). Sandbox im Container
prüfen (`WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS` nur im Test).

### W3 – Laravel-tauglich
Streaming-Antworten (`body` als iterable: ein Chunk je Schleifendurchlauf, die Webview bleibt bedienbar), große
Bodies ohne Komplettkopie, Cookies/Herkunft nach Entscheidung 2, `vio_webview_on_event` (navigated, title, new_window →
Standard: `vio_open_url`), `'transparent' => true` über GPU-Inhalt.
**Tests:** 280 Streaming + SSE, 281 Cookies/Session über zwei Anfragen und einen Neustart der Seite, 282 Webview neben
einem Viewport im selben Fenster (`vio_read_pixels` sieht den GPU-Teil).

### W4 – Beispiel, Doku, Lücken
- `examples/webview_app.php` ohne Laravel (für die CI) und `examples/webview_laravel.php <pfad-zur-app>`: Laravel in
  einem Prozess (`$kernel->handle(Request::create(...))`), keine Laravel-Abhängigkeit im Repo.
- `vio.stub.php`, CLAUDE.md (API, Testtabelle, nächste freie Nummer), README, Phase 6 in `DESKTOP-APP-PLAN.md`.
- Folge-Issues für die Lücken aus Abschnitt 5.

## 5. Abgleich mit der NativePHP-API

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

## 6. Abschluss

Wie im Desktop-Plan: Tests grün auf Linux (Xvfb + weston), macOS und Windows, `vio_platform_info()['webview']` stimmt
überall, Stub/CLAUDE.md/README nachgezogen. Erster Anwender: `phpolygon/editor` ohne Electron.
