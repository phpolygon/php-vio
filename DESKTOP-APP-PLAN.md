# Native Desktop-Anwendungen – Plan

> **Status:** 🚧 Phasen 0, 1, 2a (Drop, Dialoge, Message-Box) 2b (Menüs) und 2c (D-Bus, Benachrichtigungen, Fortschritt, Portal, Tray) und 3 (IME auf Win32, Cocoa, X11, Wayland) umgesetzt (2026-11-05, Tests 261–270), 4–5 offen.
> **Baut auf:** `NATIVE-PLATFORM-PLAN.md` (Win32, Cocoa, X11, Wayland hinter `vio_platform`, Tests 212–215) und
> Zwischenablage/Tastennamen (239/240).
> **Ziel:** Mit php-vio sollen sich Werkzeuge, Editoren und Launcher bauen lassen, nicht nur Spiele: sparsam im
> Leerlauf, mit mehreren Fenstern, nativen Dialogen, Menüs, Tray, Drag & Drop, IME-Komposition und Screenreader.

## 1. Ist-Stand und Lücken

| Bereich | Heute | Fehlt |
|---|---|---|
| Event-Loop | `vio_poll_events` (nicht blockierend). Die Plattform-Slots `wait_events` gibt es schon, sie werden aber nur intern genutzt (Vulkan bei minimiertem Fenster). | Warten mit Timeout, Aufwecken aus anderen Quellen, Neuzeichnen während Live-Resize, Fenster-Ereignisse (Schließen-Anfrage, Fokus, Minimieren, DPI-Wechsel) |
| Fenster | ein Fenster pro `VioContext`. Backend-Zustand ist global (`vio_mtl`, `vio_d3d12`, `vio_vk`, `vio_gl` …). | mehrere Fenster auf einem Gerät, Eingabezustand je Fenster |
| Native Integration | Zwischenablage (Text), `vio_native_window_handle` | Datei-/Nachrichten-Dialoge, Menüleiste und Kontextmenü, Tray, Benachrichtigungen, Datei-Drop, Cursor-Formen, Fenster-Icon, „mit Standard-App öffnen“ |
| Texteingabe | `vio_on_char`, `vio_chars_typed`, `vio_ime_backspaces` (iOS) | Preedit-Text mit Cursor/Markierung, Position des Kandidatenfensters, IME ein/aus je Eingabefeld |
| Barrierefreiheit | nichts | Zugänglichkeitsbaum für UIA / NSAccessibility / AT-SPI |

## 2. Grundsätze

1. **Alles über `vio_platform`.** Neue Fähigkeiten sind neue Vtable-Slots. Ein `NULL`-Slot heißt „gibt es hier nicht“:
   die PHP-Funktion liefert `false`/`null` ohne Warnung. Audit-Gate 212 bleibt, kein Plattformcode in `php_vio.c`.
2. **Nur native Plattformen sind Pflicht.** Win32, Cocoa, X11 und Wayland implementieren jede Phase. GLFW bekommt, was
   GLFW 3.4 hergibt (Warten, Drop, Cursor, Icon), und sonst `NULL`. Damit rückt Phase 4 des Native-Platform-Plans
   (GLFW entfernen) näher, ist aber keine Voraussetzung.
3. **`vio_platform_info()`** meldet je Fähigkeit `true`/`false` (`wait_events`, `multi_window`, `file_dialog`, `menu_bar`,
   `tray`, `notifications`, `file_drop`, `ime_preedit`, `accessibility` …), analog zu `vio_feature_info` für Backends.
4. **Ein Thread, Callbacks nur aus `vio_poll_events`/`vio_wait_events`.** Plattform-Ereignisse landen in einer Queue
   je Fenster und werden dort an PHP ausgeliefert, nie aus einem fremden Thread oder einem modalen OS-Loop.
5. **Keine neuen Pflicht-Abhängigkeiten zur Build-Zeit.** Linux-Dienste (Portal-Dialoge, Benachrichtigungen, Tray,
   AT-SPI) laufen über D-Bus. `libdbus-1.so.3` wird zur Laufzeit per `dlopen` geladen, so wie heute `libGL`/`libEGL`.
   Gemeinsamer Helfer in `src/platform/linux/vio_dbus.h`. Fehlt die Bibliothek, sind die Slots `NULL`.
6. **Testbar über echte OS-Ereignisse**, wo möglich wie in 213/214 (`PostMessageW`, `XSendEvent` im Kindprozess per
   FFI). Wo das nicht geht, gibt es `vio_inject_*`-Gegenstücke, die denselben Pfad nehmen wie das OS-Ereignis.

## 3. Phasen

Reihenfolge nach Nutzen pro Aufwand. Phase 0 ist klein und bringt sofort Akkulaufzeit. Phase 4 ist der große
Backend-Umbau und kommt deshalb erst, wenn die API der kleineren Phasen steht.

### Phase 0 – Event-Loop und Fenster-Ereignisse

```php
vio_wait_events($ctx, $timeout = -1.0);   // blockiert bis Ereignis, Timeout (s) oder vio_wake()
vio_wake();                               // threadsicher: weckt ein wartendes vio_wait_events
vio_on_close($ctx, fn() => $dirty ? false : true);   // false = Schließen abgelehnt
vio_on_focus($ctx, fn(bool $focused) => …);
vio_on_minimize($ctx, fn(bool $minimized) => …);
vio_on_scale($ctx, fn(float $sx, float $sy) => …);   // DPI-Wechsel (Monitorwechsel)
vio_on_refresh($ctx, fn() => render());   // OS will neu gezeichnet haben (Expose, Live-Resize)
```

- **Win32:** `MsgWaitForMultipleObjectsEx` mit Timeout und Weck-Event. Live-Resize/-Move läuft in einer modalen
  Schleife; dort kommt `WM_SIZE`/`WM_PAINT` nur über die WndProc, also ruft vio dort `on_refresh` auf (der einzige
  Callback, der innerhalb eines OS-Loops feuern darf, dokumentiert). `WM_DPICHANGED` → `on_scale` + vorgeschlagenes
  Rechteck übernehmen.
- **Cocoa:** `nextEventMatchingMask:untilDate:`, Aufwecken per `postEvent:` eines leeren `NSEventTypeApplicationDefined`.
  Live-Resize über `viewDidEndLiveResize`/`setNeedsDisplay` → `on_refresh`.
- **X11:** `poll()` auf `ConnectionNumber` + eine Self-Pipe für `vio_wake`. Timeout auch für Tasten-Wiederholung.
- **Wayland:** `wl_display_prepare_read` + `poll()` auf Display-FD, Wiederhol-Timer (`timerfd`) und Self-Pipe.
- **GLFW:** `glfwWaitEventsTimeout` / `glfwPostEmptyEvent`.
- Empfohlenes Muster: `while (!vio_should_close($ctx)) { vio_wait_events($ctx); if ($dirty) render(); }`.
- **Umgesetzt:** `vio_wake` gilt prozessweit und klebt, bis eine Wartezeit ihn nimmt (`vio_poll_events` lässt ihn liegen);
  ohne Display wartet jede Plattform nur auf ihn. Ereignisse feuern nur bei einer Änderung, nicht beim Anlegen des
  Fensters. X11 meldet Minimieren über `WM_STATE` und keinen DPI-Wechsel, Wayland kein Minimieren.
  `vio_inject_window_event` nimmt denselben Funnel wie die Plattformen.
- **Tests (261, 262):** Timeout, Wecken (auch durch ein echtes Ereignis auf Win32/X11), CPU im Leerlauf < 50 ms über
  0,5 s; Schließen mit Veto, Fokus, Minimieren, Neuzeichnen über echte Nachrichten (Win32, X11) und Injection.

### Phase 1 – Fenster-Details

- Cursor-Formen: `vio_set_cursor($ctx, VIO_CURSOR_ARROW|IBEAM|HAND|CROSSHAIR|RESIZE_EW|RESIZE_NS|RESIZE_NWSE|RESIZE_NESW|MOVE|NOT_ALLOWED|WAIT)`,
  eigene aus RGBA (`vio_cursor_create($rgba, $w, $h, $hotX, $hotY)`).
- Fenster-Icon (`vio_set_window_icon`, mehrere Größen), Minimal-/Maximalgröße, Seitenverhältnis, „immer oben“,
  Transparenz/Deckkraft. Der Fortschritt in Taskleiste/Dock wandert nach Phase 2: unter Linux braucht er D-Bus.
- `vio_open_url($url)` / `vio_reveal_file($path)`: `ShellExecuteW`, `NSWorkspace`, Portal `OpenURI` (Fallback `xdg-open`).
- **Umgesetzt:** `vio_set_cursor`, `vio_set_cursor_image`, `vio_set_window_icon`, `vio_set_window_size_limits`
  (`vio_set_window_size` klemmt auf jeder Plattform), `vio_set_window_aspect_ratio`, `vio_set_window_floating`,
  `vio_set_window_opacity`, `vio_open_url`, `vio_reveal_file`, `vio_platform_info()`. macOS hat keine Fenster-Icons
  (das Bild wird das Dock-Icon), Wayland kein Icon, kein Seitenverhältnis, kein „oben“, keine Deckkraft. X11-Cursor und
  -Bilder brauchen libXcursor (`dlopen`). Linux zeigt bei `vio_reveal_file` nur den Ordner.
- **Tests (263, 264):** jede Fähigkeit wie von `vio_platform_info()` gemeldet, das Fenstersystem bestätigt sie per FFI
  (Win32, X11, Cocoa); Opener über `VIO_OPEN_COMMAND` (macOS, Linux).

### Phase 2 – Native Integration

| Funktion | Win32 | Cocoa | Linux (X11 + Wayland) |
|---|---|---|---|
| `vio_file_dialog($ctx, ['mode' => 'open'\|'save'\|'folder', 'multiple', 'filters', 'default_path', 'title'])` → `string[]\|null` | `IFileOpenDialog`/`IFileSaveDialog` | `NSOpenPanel`/`NSSavePanel` (als Sheet) | Portal `org.freedesktop.portal.FileChooser` über D-Bus |
| `vio_message_box($ctx, $title, $text, ['buttons', 'kind'])` → Index | `TaskDialogIndirect` | `NSAlert` | `false` (das Portal hat keine Message-Box) |
| Datei-Drop: `vio_on_drop($ctx, fn(array $paths, float $x, float $y) => …)`, `vio_on_drag_over` | `IDropTarget` (`RegisterDragDrop`) | `registerForDraggedTypes` + `NSPasteboard` | X11 XDND, Wayland `wl_data_device` (`text/uri-list`) |
| Menüleiste: `vio_menu_bar($ctx, $tree, fn(string $id) => …)` | `HMENU` + `WM_COMMAND`, Tastenkürzel über Accelerator-Tabelle | `NSApp.mainMenu` inkl. App-Menü (Über, Beenden, Ausblenden) | `false`, die App zeichnet ihr Menü selbst |
| Kontextmenü: `vio_popup_menu($ctx, $tree, $x, $y)` → `id\|null` | `TrackPopupMenuEx` | `popUpMenuPositioningItem` | `false` |
| Tray: `vio_tray($icon, $tooltip, $menuTree, fn(string $id) => …)` → `VioTray` | `Shell_NotifyIconW` | `NSStatusItem` | `org.kde.StatusNotifierItem` über D-Bus (GNOME nur mit Erweiterung → `false`, wenn kein Watcher da ist) |
| `vio_set_progress($ctx, ?float $value, $state)` (Taskleiste/Dock, aus Phase 1) | `ITaskbarList3` | `NSDockTile` mit Fortschrittsanzeige | Unity `LauncherEntry` über D-Bus, sonst `false` |
| `vio_notify($title, $body, ['icon', 'id'])`, Klick über `vio_on_notification` | Toast (WinRT `ToastNotificationManager`, braucht AUMID; ohne: Tray-Balloon) | `UNUserNotificationCenter` (im App-Bundle; CLI-PHP zur Entwicklung: `false` mit Grund) | `org.freedesktop.Notifications` |

- **2a umgesetzt** (Tests 265, 266): Datei-Drop auf allen Plattformen, Datei-Dialog und Message-Box auf Win32 und Cocoa.
  Der Linux-Datei-Dialog (Portal) kommt mit dem D-Bus-Helfer in 2c. Testbar über `VIO_TEST_DIALOG=<ms>:<aktion>`, das den
  echten Dialog nach einer Wartezeit schließt.
- **2b umgesetzt** (Test 267): Menüleiste und Kontextmenü auf Win32 und Cocoa, Kürzel mit `Primary`; macOS bekommt ein
  App-Menü, dessen Beenden `vio_on_close` fragt.
- **2c-1 umgesetzt** (Test 268): D-Bus-Helfer (libdbus per `dlopen`), Benachrichtigungen (Linux, macOS im Bundle),
  Fortschritt (Win32 `ITaskbarList3`, macOS Dock-Kachel, Linux `LauncherEntry` mit `vio.app_id`), Portal-Dateidialog.
  `vio_platform_info()` folgt der Sitzung (Slot `available`).
- **2c-2 umgesetzt** (Test 269): `vio_tray` / `vio_tray_remove` – ein Icon je Prozess (Win32 `Shell_NotifyIconW`, macOS
  `NSStatusItem`, Linux StatusNotifierItem + dbusmenu, ohne Watcher `false`); Windows-Benachrichtigungen als Balloon am Tray-Icon.
  Ursprünglich für **2c**: D-Bus-Helfer (`dlopen` libdbus), Portal-Dialoge, Tray, Benachrichtigungen,
  Fortschritt in Taskleiste/Dock.
- Menü-Baum als PHP-Array: `['id', 'label', 'shortcut' => 'Ctrl+S', 'checked', 'enabled', 'children', 'separator']`.
  Kürzel plattformgerecht (`Ctrl` → `Cmd` auf macOS über `'shortcut' => 'Primary+S'`).
- Dialoge blockieren (modal). Rendern pausiert, `on_refresh` feuert weiter (Phase 0). Async-Variante erst bei Bedarf.
- Neue Zend-Klasse `VioTray` (hält Icon und Callback). Menüs bleiben Arrays.
- **Tests (264–268):** Drop über echte OS-Ereignisse (Win32: `IDataObject` per `DoDragDrop`-freiem `IDropTarget`-Aufruf
  aus FFI ist zu aufwendig → `vio_inject_drop` über denselben Pfad; X11: XDND-Nachrichten per `XSendEvent`),
  Menü-Befehl per `WM_COMMAND` bzw. `performActionForItemAtIndex:`, Tray-Lebenszyklus, Dialoge nur Argumentvertrag
  (modal, CI kann nicht klicken) plus Win32 `IFileDialog` mit vorab per Timer geschlossenem Dialog.

### Phase 3 – IME-Preedit

```php
vio_on_preedit($ctx, fn(string $text, int $caretStart, int $caretEnd) => …);   // '' = Komposition beendet
vio_ime_rect($ctx, $x, $y, $w, $h);   // Eingabefeld/Caret in Fensterkoordinaten → Kandidatenfenster
vio_ime_enable($ctx, bool $on);       // aus für Spielsteuerung, an für Textfelder
```

- Der fertige Text kommt weiter über `vio_on_char`/`vio_chars_typed`; Preedit ist nur Anzeige.
- **Win32:** IMM32 `WM_IME_STARTCOMPOSITION`/`WM_IME_COMPOSITION` (`GCS_COMPSTR`, `GCS_CURSORPOS`, `GCS_COMPATTR` für
  die Markierung), `ImmSetCandidateWindow`/`ImmSetCompositionWindow`, `ImmAssociateContextEx` für ein/aus.
- **Cocoa:** die View implementiert `NSTextInputClient` (`setMarkedText`, `unmarkText`, `firstRectForCharacterRange`,
  `insertText`), Tasten laufen über `interpretKeyEvents`. Das ersetzt die heutige Textgewinnung aus `NSEvent.characters`.
- **X11:** XIM mit `XIMPreeditCallbacks` (Draw/Caret/Done), Kandidatenposition über `XNSpotLocation`.
- **Wayland:** `zwp_text_input_v3` (`preedit_string`, `commit_string`, `set_cursor_rectangle`, `enable`/`disable`);
  Protokoll-Glue über `wayland-scanner` wie die übrigen.
- iOS: `UITextInput` `setMarkedText` auf dieselbe Callback-Form; `vio_ime_backspaces` bleibt.
- **3a umgesetzt** (Test 270): Win32 (IMM32) und Cocoa (`NSTextInputClient`, Tasten über `interpretKeyEvents`). GLFW meldet keine
  Komposition (`vio_platform_info()['ime_preedit']` false).
- **3b umgesetzt:** X11 (XIM-Preedit-Callbacks, wenn die Eingabemethode sie anbietet) und Wayland (`text-input-v3`). Nur per
  Injection geprüft: weder Xvfb noch der Test-weston haben eine Eingabemethode.
- **Tests (ursprünglich 269):** `vio_inject_preedit` über denselben Pfad; Win32 zusätzlich echt per `ImmSetCompositionStringW` auf
  dem Kontext des Fensters (geht ohne installierte IME nur eingeschränkt, Skip mit Grund). X11/Wayland in der CI nicht
  echt prüfbar (kein IME-Server) – bewusst nur Injection.

### Phase 4 – Mehrere Fenster

Der teuerste Teil: Backends halten heute genau eine Swapchain im globalen Zustand.

```php
$ctx  = vio_create('auto', ['width' => 1280, 'height' => 800]);   // Hauptfenster wie bisher
$tool = vio_window($ctx, ['width' => 320, 'height' => 600, 'title' => 'Inspector', 'parent' => null]);
vio_begin($ctx, ['window' => $tool]); /* … */ vio_end($ctx);
vio_key_pressed($tool, VIO_KEY_A);   // Eingabe-Funktionen nehmen VioContext|VioWindow
```

- Neue Zend-Klasse `VioWindow`: Plattform-Fenster, eigene Swapchain, eigener `vio_input_state`, eigene Callbacks.
  Der `VioContext` besitzt das Gerät und das Hauptfenster (intern ebenfalls ein `VioWindow`, damit es nur einen Pfad gibt).
  Ressourcen (Meshes, Texturen, Pipelines, RTs) gelten für alle Fenster des Kontexts.
- Neue Backend-Slots: `create_swapchain(window) → handle`, `destroy_swapchain`, `resize_swapchain`, `set_swapchain`
  (vor `begin_frame`). Bestehende Swapchain-Felder wandern in eine Struktur je Fenster
  (`vio_d3d12_swapchain`, `vio_vk_swapchain`, …); `resize(int, int)` bekommt das Fenster.
- Je Backend: D3D11/D3D12 eine `IDXGISwapChain` je HWND auf demselben Gerät (Frame-Slots und Fences bleiben je
  Gerät, Back-Buffer je Fenster); Vulkan `VkSurfaceKHR` + `VkSwapchainKHR` je Fenster, Acquire-/Present-Semaphoren je
  Fenster; Metal ein `CAMetalLayer` je `NSWindow`; OpenGL ein geteilter Kontext je Fenster (`wglShareLists`, geteilte
  `NSOpenGLContext`, `glXCreateContextAttribsARB` mit Share, EGL-Share) – VAOs und FBOs sind nicht teilbar, deshalb
  zuletzt und mit Kennzeichnung in `vio_platform_info()`.
- Unterfenster/Popups: `'parent'` (Win32 Owner, Cocoa `addChildWindow`, X11 `WM_TRANSIENT_FOR`, Wayland `xdg_popup`
  bzw. Child-Toplevel), `'modal' => true`.
- **Aufteilung nach der Bestandsaufnahme (2026-11-05):** D3D11 und Metal halten den Swapchain-Zustand als abgrenzbare
  Feldgruppe im globalen Backend-Zustand. Ein Backend-Slot `set_swapchain` tauscht diese Gruppe je Fenster aus und wieder ein, die
  vielen Lesestellen bleiben unverändert. Bei D3D12 ist der Frame-Slot der Back-Buffer-Index der Swapchain, und daran hängen
  Allokatoren, Fences, Heap-Slices, Ringe und Freigaben. Bei Vulkan hängen Gerätewahl und Present-Queue an der ersten Surface,
  Acquire-Semaphoren und Offscreen-Flags gehen von einer Swapchain aus. Bei OpenGL legt keine Plattform geteilte Kontexte an,
  und VAOs/FBOs sind nicht teilbar.
  - **4a ✅ (2026-11-05, Test 271):** Frontend (`VioWindow`, Input je Fenster, `VioContext|VioWindow`, Tausch von
    Fenster/Größe/2D-Zustand um `vio_begin`/`vio_end` und `vio_read_pixels`), Metal, D3D11. `VioWindow` hält einen
    Kontext-Datensatz (`view`) mit Fenster, Input, Schließ-Flag und Größen; die Fenster- und Input-Funktionen laufen
    darauf unverändert. Nebenbefund Metal: der 2D-Batch schrieb jede Ausgabe in denselben Vertex-Puffer, ein späterer
    Frame überschrieb die Daten eines noch laufenden (jetzt Per-Frame-Ring).
  - **4b ✅ (2026-11-05):** Vulkan. Surface, Swapchain, Tiefe, `render_finished` je Bild und der Headless-Capture je
    Fenster; Frame-Slots, Command-Buffer, Acquire-Semaphoren und die Timeline bleiben je Gerät (die Acquire-Semaphore
    eines Slots wartet die Übergabe des Frames, der mit ihr acquired hat). Ein Fenster, dessen Surface die Present-Queue
    nicht kann oder ein anderes Format als die Hauptswapchain bekommt (die 2D-Pipelines sind dafür gebaut), wird
    abgelehnt.
  - **4c ✅ (2026-11-05):** D3D12. Der Frame-Slot (`frame_index`: Allokator, Fence, Heap-Slices, Ringe) rückt nach jedem
    Present um eins weiter, der Back-Buffer ist ein eigenes Feld (`back_index`) der geladenen Swapchain. Swapchain,
    Back-Buffer mit RTV-Heap, Tiefe mit DSV-Heap, Größen und Flags je Fenster. Mit HDR-Komposit (Ebene in der Größe des
    Hauptfensters) und bei anderem Swapchain-Format lehnt `vio_window` ab.
  - **4d:** OpenGL (geteilte Kontexte in allen Plattformen, VAOs/FBOs je Kontext).
  - Backends ohne Umbau melden `VIO_FEATURE_MULTI_WINDOW` = false, `vio_window` liefert dort `false`. Headless-Fenster bekommen dieselben Slots, damit die Tests
  ohne Bildschirm laufen.
- **Tests (270–272):** zwei und drei Fenster in einem Frame-Zyklus, je Fenster eigener Clear/Draw und
  `vio_read_pixels($window)`; Resize eines Nebenfensters stört das Hauptfenster nicht; Schließen und Freigeben
  eines Fensters mitten im Betrieb; Eingabe landet nur im fokussierten Fenster (echte OS-Ereignisse wie 213/214).
  Läuft auf jedem Backend (`*_all_backends`-Muster).

### Phase 5 – Barrierefreiheit (AccessKit)

- **Anbindung:** `accesskit-c` (MIT/Apache-2.0, vorgebaute statische Bibliotheken je Plattform), Build-Schalter
  `--with-accesskit`, `HAVE_ACCESSKIT`. AccessKit bringt die Adapter selbst mit: Windows UIA (Subclassing des HWND),
  macOS NSAccessibility (an der View), Unix AT-SPI über D-Bus. Die Plattform reicht ihm nur Fenster-Handle und
  Ereignisse (Fokus, Aktivierung) durch.
- **API:** Die App beschreibt ihre Oberfläche als Baum und schickt bei Änderungen nur die geänderten Knoten.
  ```php
  vio_a11y_update($window, [
      'focus' => 'save',
      'nodes' => [
          ['id' => 'root', 'role' => 'window', 'children' => ['save', 'name']],
          ['id' => 'save', 'role' => 'button', 'label' => 'Speichern', 'bounds' => [10, 10, 80, 24], 'actions' => ['click']],
          ['id' => 'name', 'role' => 'text_input', 'label' => 'Name', 'value' => 'Hendrik', 'bounds' => [100, 10, 200, 24]],
      ],
  ]);
  vio_on_a11y_action($window, fn(string $id, string $action, $value) => …);   // Klick, Fokus, Wert setzen
  ```
  String-IDs werden intern auf `accesskit_node_id` (u64) abgebildet. Der Baum wird erst gebaut, wenn ein Screenreader
  fragt (AccessKits Lazy-Init), sonst kostet die API nichts.
- Abhängig von Phase 4 (Baum je `VioWindow`) und Phase 3 (Textfelder melden Preedit/Selektion).
- Wayland ist abgedeckt, weil AT-SPI nicht am Fenstersystem hängt.
- **Tests (273, 274):** Baum anlegen/ändern/Fokus; danach vom Kindprozess abfragen: Windows über UIA
  (`IUIAutomation::ElementFromHandle`, Name/Rolle der Kinder), macOS über `AXUIElementCreateApplication`
  (braucht Bedienungshilfen-Freigabe → Skip mit Grund), Linux über AT-SPI-D-Bus auf der CI (at-spi2-core installieren).
  Aktion vom Kindprozess (`Invoke`) ruft `vio_on_a11y_action`.

## 4. Entscheidungen

1. **Linux ohne Portal/Dienst:** `false` mit Grund in `vio_platform_info()`, kein selbst gezeichneter Fallback.
   Gilt für Message-Box ohne Portal, Menüleiste, Kontextmenü und Tray ohne StatusNotifierWatcher.
2. **macOS wird als App-Bundle ausgeliefert, CLI-PHP nur zur Entwicklung.** vio erkennt das Bundle über
   `NSBundle.mainBundle.bundleIdentifier`. Im Bundle läuft alles (Benachrichtigungen, Dock, App-Menü mit Bundle-Namen).
   Ohne Bundle melden bundle-pflichtige Funktionen (`vio_notify`) `false` mit Grund, ohne Warnung. Der Rest
   (Dialoge, Menü, Tray, Drop) geht auch in der CLI. Das Bundle selbst (Info.plist, Signatur, Notarisierung) baut
   das Packaging von PHPolygon, nicht php-vio.
3. **Eingabe- und Fensterfunktionen nehmen `VioContext|VioWindow`.** Mit `$ctx` ist das Hauptfenster gemeint,
   bestehender Code bleibt unverändert. Funktionen, die das Gerät betreffen (`vio_begin`, Ressourcen), bleiben bei
   `VioContext`; das Ziel-Fenster wählt `vio_begin($ctx, ['window' => $w])`.
4. **Barrierefreiheit über AccessKit** (`accesskit-c`), statisch gelinkt hinter `--with-accesskit`, in den
   Release-Binaries an. Ohne den Schalter ist `vio_platform_info()['accessibility']` `false`, die `vio_a11y_*`-Funktionen
   tun nichts.

## 5. Abschluss je Phase

Tests grün auf Linux (Xvfb + weston), macOS und Windows; `vio_platform_info()` meldet die Fähigkeit auf jeder
Plattform richtig; `vio.stub.php`, CLAUDE.md (API-Abschnitt, Testtabelle, nächste freie Nummer) und README nachgezogen.
