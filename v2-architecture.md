# php-vio v2 – Architektur & Migrationsplan

> **Status:** Design-Dokument, prä-Implementierung
> **Ziel-Release:** v2.0.0 (keine feste Deadline — nach Code Tycoon Launch + iPad-Port)
> **Autor:** Hendrik Mennen, ausgearbeitet in Dialog mit Claude

---

## 1. Vision

**libvio** wird eine eigenständige, in C geschriebene Graphics-, Audio- und Media-Library mit Multi-Backend-Abstraktion (OpenGL, Vulkan, Metal, DirectX 11, DirectX 12). Sie lebt als Upstream-Projekt mit eigenem Repo-Ökosystem, eigener ABI, eigenem Release-Zyklus.

**php-vio** wird zu einem dünnen PHP-Adapter, der libvio zur Release-Zeit gegen pinned Backend-Versionen baut und als monolithisches PHP-Extension-Binary ausliefert. Endnutzer installieren weiterhin `pie install phpolygon/php-vio` und kriegen alle Backends in einem Paket.

**Kernprinzip:** Modularität im Upstream, Einfachheit im Downstream. Die C-Library ist für Contributor und andere Sprachen sauber strukturiert — die PHP-Extension bleibt für Endnutzer ein Install.

### Was das löst

- **Isolation von Backend-Fehlern.** Ein Shader-Bug im DX11-Backend ist kein Breaking Change für macOS-Nutzer. Patch-Release im isolierten Backend-Repo, php-vio zieht ihn beim nächsten Release.
- **Unabhängiger Release-Zyklus pro Backend.** Metal-Optimierung für Apple Silicon muss nicht auf den nächsten Linux-Release warten.
- **Wiederverwendbarkeit außerhalb PHP.** C-Consumer (C++, Rust, Go, Zig) können libvio direkt von Upstream nutzen, ohne PHP-Abhängigkeiten.
- **Schlanke Contribution.** Neue Backends (WebGPU, Software-Renderer) als eigenes Repo, ohne Kern-PR.
- **Saubere Architektur-Dokumentation.** Die Repo-Grenzen machen die ohnehin existierende Vtable-Architektur sichtbar.

### Was das nicht ist

- Keine neue API. Die libvio C-API ist die bereits existierende interne C-API, nur aus dem PHP-Layer extrahiert.
- Keine Positionierung als bgfx-Konkurrent. libvio ist primär das, was PHPolygon/Code Tycoon und php-vio brauchen. Dass andere es nutzen können ist Nebeneffekt, kein Marketingziel.
- Kein Bruch der PHP-API. php-vio 2.0 bleibt für PHP-Code quellkompatibel zu 1.x — die Migration ist eine interne Refaktorisierung, nicht ein API-Rewrite.

---

## 2. Aktuelle Situation (Stand Mai 2026)

### Was bereits stimmt

- **Vtable-Abstraktion** ist sauber in `include/vio_backend.h` definiert (Lifecycle, Surface, Pipeline, Resources, Shader, Drawing, State, Compute, Query). ABI-Check über `api_version`-Feld im Vtable existiert bereits, Backends mit Mismatch werden in `vio_register_backend()` abgelehnt.
- **5 Backends an Bord:** `src/backends/opengl/`, `src/backends/vulkan/`, `src/backends/metal/`, `src/backends/d3d11/`, `src/backends/d3d12/`. Alle registrieren sich gegen die zentrale Vtable — die 1:n-Core-API ist also unter Realbedingungen validiert, nicht nur theoretisch.
- **Auto-Selection** in `vio_backend_registry.c` mit plattformspezifischer Priority (macOS: opengl > metal, Windows: d3d12 > d3d11 > vulkan > opengl, Linux: vulkan > opengl).
- **69 PHPT-Tests** decken alle Backends ab, inkl. Cross-Backend-Parity, Render-Targets, Instanced-Draws, Headless-GPU-Rendering, Multi-Context. Long-run-Memory- und Perf-Regression-Gates sind eingerichtet (Commit `b6bbb2c`).
- **Plugin-System** mit versionierten Vtables vorhanden (`include/vio_plugin.h`).
- **Shader-Toolchain** (GLSL → SPIR-V via glslang, Cross-Compile via SPIRV-Cross) funktioniert backend-agnostisch.

### Bekannte Backend-Reife-Gaps

Bevor in Phase 3 Backend-Repos extrahiert werden, müssen folgende Lücken geschlossen sein — ein halb-stubbed Backend in einem eigenen Repo wäre unsauber.

- **Metal-3D-Pipeline ist Stub.** `create_pipeline`, `create_buffer`, `create_texture`, `draw`, `draw_indexed` sind auf Metal nicht implementiert; ein Metal-Context rendert für 3D-Calls schwarz. Auto-Select auf macOS bevorzugt deshalb OpenGL, nicht Metal (siehe Kommentar `src/vio_backend_registry.c:75-79`). **Blocker für Metal-Repo-Split.**
- **OpenGL-Capability-Check unvollständig.** Die Runtime-Version-Detection (3.3 → 4.6 Ladder, Commit `2accb4f`) füllt `vio_gl.gl_major/gl_minor` korrekt, aber `opengl_supports_feature()` (`src/backends/opengl/vio_opengl.c:243`) gibt noch hardcoded Werte zurück und behauptet fälschlich „GL 4.1 doesn't have compute shaders". Siehe §2.1 unten.

### 2.1 OpenGL als Vollband-Backend (3.3 → aktuelle Version)

**Ziel:** Das OpenGL-Backend soll auf jedem System mit OpenGL 3.3+ laufen und automatisch alle Features nutzen, die der erkannte Context tatsächlich anbietet — keine Compile-Time-Flags, keine festen Mindest-Versionen über 3.3.

**Was schon da ist:**

- GLFW-Context-Negotiation als Ladder von 4.6 herunter bis 3.3 (Window-Layer).
- `glGetIntegerv(GL_MAJOR_VERSION/GL_MINOR_VERSION)` in `vio_opengl_setup_context()` schreibt die effektiv erhaltene Version nach `vio_gl.gl_major/gl_minor`.
- GLAD lädt automatisch alle Funktionspointer, die das gewählte Profil exportiert; höhere Versionen sind über `GLAD_GL_VERSION_X_Y`-Makros zur Laufzeit prüfbar.

**Was fehlt:**

1. **Feature-Gating an die echte Version koppeln.** `opengl_supports_feature()` muss `vio_gl.gl_major/gl_minor` (oder die GLAD-Makros) abfragen, nicht hardcoden:
   - `VIO_FEATURE_COMPUTE` → GL ≥ 4.3
   - `VIO_FEATURE_TESSELLATION` → GL ≥ 4.0
   - `VIO_FEATURE_GEOMETRY` → GL ≥ 3.2 (also in 3.3+ immer true)
   - Weitere Features in `include/vio_types.h:121-132` analog mappen.
2. **Pfad-Dispatch pro Render-Operation.** Wo OpenGL mehrere Wege zum selben Ziel hat (z. B. UBO-Binding via `glBindBufferRange` vs. DSA `glBindBufferRange`/`glNamedBufferData` ab 4.5, Multi-Draw-Indirect ab 4.3), wählt das Backend zur Laufzeit den besten verfügbaren Pfad — nicht den kleinsten gemeinsamen Nenner. 3.3 bleibt aber die garantierte Fallback-Linie.
3. **Shader-Compilation-Profile.** `vio_opengl_get_glsl_version()` liefert bereits die korrekte `#version`-Zahl (`major*100 + minor*10`, Floor 330). Default-Shaders in `src/shaders/` müssen auf 330 core lauffähig bleiben — höhere Versionen dürfen sie nicht voraussetzen.
4. **Conformance-Test pro Major-Version.** CI sollte mindestens 3.3-, 4.1- und 4.6-Contexts forcieren (über `GLFW_CONTEXT_VERSION_MAJOR/MINOR`-Override im Test-Setup), um sicherzustellen, dass das Backend auf der Floor-Version sauber läuft und Feature-Pfade korrekt fallen.

**Reihenfolge:** Punkt 1 (Feature-Gating) ist trivial und sollte sofort gemacht werden — er kostet nichts und entfernt eine aktuell aktiv falsche Aussage des Backends. Punkte 2–4 sind v2-Vorarbeit und können in Phase 1 oder 2 parallel zur Zend-Trennung laufen, da sie rein im Backend stattfinden.

### Was noch aufzuräumen ist

**Die zentrale Erkenntnis:** `src/` ist derzeit **nicht vollständig Zend-frei.** Beispiele:

- `src/vio_context.c` enthält `zend_class_entry`, `zend_object_handlers`, `zend_object_alloc`, `zend_register_internal_class`, `object_properties_init`, Zend-Memory-Management.
- `src/vio_pipeline.c` enthält dasselbe Muster für die Pipeline-Objekt-Registrierung.
- Vermutlich die meisten anderen `src/vio_*.c` ebenso — muss beim Refactoring pro Datei geprüft werden.
- **`php_vio.c` ist mit ~6940 Zeilen der eigentliche Brocken** — sämtliche PHP-Funktionssignaturen, zval-Marshalling und Argument-Parsing leben dort. Die `src/vio_*.c`-Files sind zusammen kleiner als `php_vio.c` allein. Phase 1 muss diesen Hauptteil explizit einplanen, nicht nur die Object-Files.

Das ist keine Schwäche des bisherigen Designs — es war für einen monolithischen PHP-Extension-Build pragmatisch, die Zend-Object-Lifecycle-Handler direkt neben der Backend-Dispatch-Logik zu haben. Für die Extraktion muss aber pro Datei getrennt werden:

- **Reine C-Logik** (Backend-Dispatch, Resource-Management, State) → wandert nach `libvio`.
- **Zend-Wrapper** (Class-Registrierung, Object-Lifecycle, Marshalling von PHP-zval zu C-Structs) → bleibt in php-vio.

Ein realistischer Weg ist, die Zend-Teile in jeder Datei in einen separaten Wrapper zu ziehen (z. B. `ext/vio_context_zend.c` im php-vio-Repo), während der Kernteil als `libvio/src/vio_context.c` lebt und nur C-Typen kennt.

### Build-System-Realität

Das Repo nutzt **Autotools + phpize** (`config.m4`, `config.w32`, `Makefile.frag`), **nicht CMake.** Die in §5 beschriebene CMake-Komposition setzt eine Build-System-Migration voraus, die bisher nicht stattgefunden hat. Diese Migration ist substantieller Aufwand und muss als eigenständiger Vorabschritt von Phase 2 eingeplant werden — entweder als Phase 1.5 oder parallel zur Zend-Trennung in Phase 1. Autotools muss erhalten bleiben, solange php-vio über PIE/`phpize` installierbar sein soll; CMake kommt für libvio-Core und Backend-Repos *zusätzlich*.

### Audio, Video, Streaming, Input

Diese Subsysteme (`vio_audio.c`, `vio_recorder.c`, `vio_stream.c`, `vio_input.c`, `vio_font.c`) gehören konzeptionell zu libvio-core, nicht zu den Render-Backends. Sie werden im Core-Repo verbleiben und sind nicht Teil des Backend-Splits. Gleiches gilt für den `null`-Backend (als Kern-Testing-Utility) und die built-in Default-Shaders unter `src/shaders/`.

---

## 3. Zielarchitektur

```
┌────────────────────────────────────────────────────────────────┐
│                     Consumer-Ebene                             │
│                                                                │
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────────┐   │
│  │   php-vio    │   │  C/C++ Apps  │   │  Rust / Go / Zig │   │
│  │  (Extension) │   │  (Showcase)  │   │  (Bindings)      │   │
│  └──────┬───────┘   └──────┬───────┘   └────────┬─────────┘   │
│         │                  │                    │             │
└─────────┼──────────────────┼────────────────────┼─────────────┘
          │                  │                    │
          └──────────────────┴────────────────────┘
                             │
                             │ libvio C-API
                             │
┌────────────────────────────┼───────────────────────────────────┐
│              libvio-core (eigenes Repo)                        │
│                                                                │
│   Backend-Registry · Resource-Lifecycle · Shader-Toolchain     │
│   Audio · Video · Streaming · Input · Font · Null-Backend      │
│                                                                │
│   Definiert öffentliche Header:                                │
│     libvio/vio.h           — Haupt-API                         │
│     libvio/vio_backend.h   — Backend-Vtable (ABI-relevant)     │
│     libvio/vio_types.h     — Enums, Structs, Deskriptoren      │
│     libvio/vio_plugin.h    — Plugin-System                     │
└──────────────┬────────────────┬──────────┬──────────┬──────────┘
               │                │          │          │
               │ lädt           │          │          │
               │ Backends       │          │          │
               ▼                ▼          ▼          ▼
      ┌────────────────┐ ┌────────────┐ ┌──────┐ ┌────────┐
      │ libvio-opengl  │ │libvio-vk   │ │ metal│ │ d3d11  │ (+ d3d12)
      │  (eigenes Repo)│ │(eig. Repo) │ │(...) │ │(...)   │
      └────────────────┘ └────────────┘ └──────┘ └────────┘
```

### Kern-Design-Entscheidungen

1. **Backend-Repos sind eigenständig**, aber *nicht* runtime-ladbar via `dlopen`. Sie werden zur Build-Zeit gegen libvio-core gelinkt. Das vermeidet ABI-Runtime-Chaos und PIE-Matrix-Hölle und behält „ein Install, alles drin" für php-vio-Endnutzer.

2. **libvio-core definiert die Vtable**, die Backend-Repos implementieren sie. Backend-Repos deklarieren eine Mindest-Core-Version in ihrem CMakeLists.txt.

3. **php-vio (Downstream) pinnt Versionen** über eine Manifest-Datei, die Teil des Release-Commits ist. Reproduzierbare Builds sind Pflicht.

4. **Keine PHP-Anhängsel in libvio.** `emalloc`/`efree` raus, reine `malloc`/`free`. `zend_throw_exception` raus, stattdessen Error-Return-Codes (`vio_result_t`) plus Error-String-Accessor (`vio_last_error()`). Das ist die schmerzhafteste Arbeit, und sie passiert einmalig in Phase 2.

---

## 4. Repository-Layout

### Upstream (libvio-Ökosystem)

| Repo | Inhalt | Abhängigkeiten |
|------|--------|----------------|
| `phpolygon/libvio` | Core-Library, Header, Backend-Registry, Audio, Video, Stream, Input, Font, Null-Backend, Shader-Toolchain | glslang, SPIRV-Cross, miniaudio, FFmpeg, GLFW (optional, für Windowing) |
| `phpolygon/libvio-opengl` | OpenGL 4.1 Backend | libvio-core, GLAD |
| `phpolygon/libvio-vulkan` | Vulkan Backend inkl. VMA | libvio-core, Vulkan Loader, VMA |
| `phpolygon/libvio-metal` | Metal Backend (Objective-C) | libvio-core, macOS Metal/QuartzCore Frameworks |
| `phpolygon/libvio-d3d11` | DirectX 11 Backend | libvio-core, Windows SDK |
| `phpolygon/libvio-d3d12` | DirectX 12 Backend | libvio-core, Windows SDK |

### Downstream

| Repo | Inhalt |
|------|--------|
| `phpolygon/php-vio` | PHP-Extension, Zend-Wrapper, Release-Manifest mit gepinnten Upstream-Versionen, CI für den End-to-End-Build |

### Struktur `phpolygon/libvio`

```
libvio/
├── CMakeLists.txt
├── README.md
├── LICENSE
├── include/
│   └── libvio/
│       ├── vio.h              # Haupt-API (Convenience + Pipeline Layer)
│       ├── vio_backend.h      # Backend-Vtable (ABI-relevant!)
│       ├── vio_types.h        # Enums, Deskriptoren, Result-Codes
│       ├── vio_plugin.h       # Plugin-System
│       ├── vio_audio.h
│       ├── vio_video.h
│       ├── vio_stream.h
│       ├── vio_input.h
│       └── vio_font.h
├── src/
│   ├── core/
│   │   ├── context.c
│   │   ├── pipeline.c
│   │   ├── shader.c
│   │   ├── shader_compiler.c
│   │   ├── shader_reflect.c
│   │   ├── buffer.c
│   │   ├── texture.c
│   │   ├── mesh.c
│   │   ├── render_target.c
│   │   ├── cubemap.c
│   │   ├── resource.c
│   │   ├── backend_registry.c
│   │   ├── plugin_registry.c
│   │   ├── 2d.c
│   │   └── error.c
│   ├── media/
│   │   ├── audio.c
│   │   ├── recorder.c
│   │   ├── stream.c
│   │   └── font.c
│   ├── platform/
│   │   ├── window.c           # GLFW-Wrapper, optional
│   │   └── input.c
│   └── backends/
│       └── null.c             # Kern-Testing-Backend, nicht extern
├── tests/
│   └── ...                    # Headless-Tests gegen Null-Backend
├── examples/
│   └── ...                    # C-Beispiele (minimal, in Repo)
└── cmake/
    ├── libvioConfig.cmake.in  # Für find_package(libvio)
    └── FindLibvioBackend.cmake
```

### Struktur `phpolygon/libvio-<backend>` (Template)

```
libvio-opengl/
├── CMakeLists.txt
├── README.md
├── include/
│   └── libvio-opengl/
│       └── backend.h          # Optional: Zusatz-APIs (vio_opengl_native_handle etc.)
├── src/
│   ├── backend.c              # Implementiert vio_backend-Vtable
│   ├── context.c
│   ├── pipeline.c
│   ├── buffer.c
│   ├── texture.c
│   └── ...
├── tests/
│   ├── backend_conformance/   # Testet gegen Vtable-Contract
│   └── php_vio_compatibility/ # Siehe §6 (Kompatibilitätstests)
├── vendor/
│   └── glad/                  # Falls backend-spezifisch vendored
└── cmake/
    └── libvio-openglConfig.cmake.in
```

### Struktur `phpolygon/php-vio` (v2)

```
php-vio/
├── CMakeLists.txt             # Neuer Build: zieht libvio + alle Backends, baut Extension
├── README.md
├── config.m4                  # Autotools-Build, parallel zu CMake erhalten
├── config.w32
├── vio.stub.php
├── php_vio.c                  # Unverändert in Struktur, ruft libvio statt interner src/
├── php_vio.h
├── php_vio_arginfo.h
├── ext/                       # NEU: Zend-Wrapper pro Objekt-Klasse
│   ├── vio_context_zend.c     # Enthält alles Zend-spezifische aus altem src/vio_context.c
│   ├── vio_pipeline_zend.c
│   ├── vio_shader_zend.c
│   └── ...
├── release-manifest.json      # NEU: gepinnte Upstream-Versionen
├── tests/                     # PHPT-Tests bleiben wie gehabt
└── ...
```

### `release-manifest.json` — zentrales Koppelungs-Dokument

```json
{
  "php_vio_version": "2.0.0",
  "libvio_core": {
    "repo": "phpolygon/libvio",
    "version": "1.0.0",
    "commit": "<full-sha>"
  },
  "backends": {
    "opengl":  { "repo": "phpolygon/libvio-opengl",  "version": "1.0.0", "commit": "<sha>" },
    "vulkan":  { "repo": "phpolygon/libvio-vulkan",  "version": "1.0.0", "commit": "<sha>" },
    "metal":   { "repo": "phpolygon/libvio-metal",   "version": "1.0.0", "commit": "<sha>" },
    "d3d11":   { "repo": "phpolygon/libvio-d3d11",   "version": "1.0.0", "commit": "<sha>" },
    "d3d12":   { "repo": "phpolygon/libvio-d3d12",   "version": "1.0.0", "commit": "<sha>" }
  }
}
```

Commit-SHA ist die Wahrheit; Version ist für menschliche Lesbarkeit. Beim Release-Build checkt das Build-Script genau diese SHAs aus — nie „latest tag" oder „branch head".

---

## 5. Build-Integration

### CMake-Komposition

Jedes Upstream-Repo exportiert ein CMake-Target:

- `libvio::core` aus `phpolygon/libvio`
- `libvio::backend::opengl` aus `phpolygon/libvio-opengl`
- `libvio::backend::vulkan` aus `phpolygon/libvio-vulkan`
- `libvio::backend::metal` aus `phpolygon/libvio-metal`
- `libvio::backend::d3d11` aus `phpolygon/libvio-d3d11`
- `libvio::backend::d3d12` aus `phpolygon/libvio-d3d12`

Ein Downstream-Consumer (php-vio, Showcase-App, Third-Party) nutzt:

```cmake
find_package(libvio 1.0 REQUIRED)
find_package(libvio-opengl 1.0 REQUIRED)
find_package(libvio-metal  1.0 REQUIRED OPTIONAL_COMPONENTS)  # nur auf macOS

target_link_libraries(my_app PRIVATE
  libvio::core
  libvio::backend::opengl
  $<$<PLATFORM_ID:Darwin>:libvio::backend::metal>
  $<$<PLATFORM_ID:Windows>:libvio::backend::d3d12>
)
```

### Release-Build von php-vio

Der php-vio-Release-Build läuft in zwei Schritten:

**Schritt 1 — Upstream-Fetch:** Ein CI-Script liest `release-manifest.json` und klont die gepinnten Commits aller Upstream-Repos in ein `build/upstream/`-Verzeichnis. Kein `git submodule` in php-vio — das würde implizite Bindungen erzeugen; das Manifest ist die einzige Wahrheit.

**Schritt 2 — Kombinations-Build:** CMake-Superproject baut libvio-core und alle Backends in `build/upstream/` als statische Libraries, dann die PHP-Extension als `vio.so`/`php_vio.dll`, die alle statisch reinlinkt. Ergebnis ist ein einzelnes Extension-Binary wie heute.

Das heißt: **keine Änderung für den Endnutzer.** PIE-Install-Experience identisch zu 1.x.

### Feature-Flags bleiben

Die Build-Flags (`--with-vulkan`, `--with-metal` etc.) bleiben erhalten und steuern jetzt, welche Upstream-Backend-Repos gezogen und gelinkt werden. Ein Linux-Server-Build ohne Vulkan zieht `libvio-vulkan` gar nicht erst — spart Clone-Zeit und Build-Artefakte.

---

## 6. ABI & Versionierung

### SemVer für alle Upstream-Repos

**libvio-core:**

- **MAJOR:** Öffentliche Header-Struct-Layouts ändern sich. `vio_backend`-Vtable-Member werden umgeordnet, umbenannt oder entfernt. Enum-Werte ändern numerische Zuordnung. Error-Codes werden umnummeriert.
- **MINOR:** Neue Vtable-Member hinten angefügt (mit Default-NULL erlaubt). Neue Funktionen. Neue Enum-Werte am Ende. Backward-kompatible Feature-Erweiterungen.
- **PATCH:** Bugfixes, Performance, Docs. Keine API-Oberflächenänderung.

**libvio-backend-\*:**

Analog, aber die für Backends öffentliche Oberfläche ist kleiner — im Wesentlichen die Factory-Funktion (`vio_backend_opengl_create()`) und eventuelle Backend-spezifische Zusatz-Header. Alles andere läuft über die Core-Vtable.

Jedes Backend-Repo deklariert in seinem CMakeLists.txt eine **kompatible Core-Version-Range**:

```cmake
find_package(libvio 1.0 REQUIRED)  # akzeptiert alles in 1.x
```

Das garantiert: Backend 1.3 gegen Core 1.5 funktioniert. Backend 1.3 gegen Core 2.0 bricht bewusst.

### Vtable-Versionierung auf Struct-Ebene

`vio_backend` enthält als **erstes Member** eine ABI-Version:

```c
typedef struct vio_backend {
    uint32_t abi_version;  // VIO_BACKEND_ABI_VERSION
    const char *name;
    // ... Funktionspointer
} vio_backend;
```

Core prüft bei `vio_register_backend()`, ob `abi_version` zum Compile-Zeitpunkt von Core passt. Mismatch = hartes Fail mit klarer Fehlermeldung, nicht undefined behavior.

Bei MINOR-Bumps wird `VIO_BACKEND_ABI_VERSION` inkrementiert, und Core akzeptiert sowohl alte als auch neue Versionen (mit NULL-Check für neue Vtable-Member). Bei MAJOR-Bumps wird die Minor-History zurückgesetzt und alte Backends werden kategorisch abgelehnt.

### php-vio SemVer

Eigenständig versioniert, unabhängig von libvio-Versionen. php-vio 2.0 kann libvio-core 1.x + Backends 1.x bündeln. php-vio 3.0 würde libvio-core 2.x erfordern.

Regel: **Die PHP-Oberfläche ist SemVer-relevant, nicht die libvio-Version darunter.** Ein Backend-Bugfix, der libvio-metal 1.0.4 → 1.0.5 auslöst, erfordert nur einen php-vio-Patch-Release (2.0.x).

---

## 7. Kompatibilitätstests (zirkuläre Verifikation)

**Das Problem:** Wenn Backend-Repos unabhängig entwickelt werden, können sie abdriften und erst beim php-vio-Release-Build auffallen. Das ist zu spät.

**Die Lösung:** Jedes Backend-Repo enthält einen Test-Job, der gegen die **aktuelle `main` von php-vio** baut und testet. Dieser Job läuft bei jedem PR im Backend-Repo.

### Konkret pro Backend-Repo

`tests/php_vio_compatibility/` enthält einen CI-Job, der:

1. Die vorgeschlagene Backend-Version (aus dem PR) auscheckt.
2. `phpolygon/libvio` main auscheckt.
3. `phpolygon/php-vio` main auscheckt.
4. Ein Ad-hoc-Manifest erstellt, das php-vio main gegen: libvio-main + diese-Backend-Änderung + stabile andere Backends baut.
5. Den vollen php-vio Test-Suite laufen lässt.

Wenn das fehlschlägt, darf der Backend-PR nicht mergen, ohne dass entweder:

- Die Änderung zurückgenommen wird, oder
- php-vio und/oder libvio-core parallel angepasst werden (dann muss der PR Cross-Repo-Referenzen enthalten), oder
- Bewusst ein Breaking Change markiert wird (→ MAJOR-Bump, erfordert php-vio-Update).

### Ja, das ist ein zirkulärer Bezug

Backend-Repos kennen php-vio. php-vio zieht Backend-Repos. Das klingt zunächst unsauber. Aber:

- Der Zirkelbezug ist **unidirektional in der Produktions-Build-Kette** (php-vio konsumiert Backends, nicht umgekehrt).
- Der Zirkelbezug ist **nur im Testing bidirektional** — Backends testen gegen php-vio als einen Consumer.
- Die Alternative wäre, Inkompatibilitäten erst beim php-vio-Release zu entdecken, was Stunden statt Minuten Debugging bedeutet.

Pragmatisch überwiegt der Nutzen.

### libvio-core testet sich selbst

libvio-core hat eigene Tests über das Null-Backend und optional einen synthetischen Test-Backend. Diese Tests kennen php-vio nicht. Nur die Render-Backend-Repos haben den zusätzlichen php-vio-Kompatibilitätstest.

---

## 8. Migrationspfad

Die Migration läuft in Phasen, von denen jede in sich stabil ist und nicht auf den Abschluss der nächsten wartet. Zwischenzustände funktionieren — kein „Big Bang Rewrite".

### Phase 0 — Vorbereitung (parallel zu aktuellem Betrieb)

- Dieses Dokument ist das Artefakt von Phase 0.
- Keine Code-Änderungen. Nur Planung.
- Status: ✅ abgeschlossen mit Commit dieses Dokuments.

### Phase 1 — Zend-Trennung im Monorepo

**Ziel:** `src/` ist zu 100% Zend-frei, innerhalb des aktuellen php-vio-Repos. Kein Extract in separate Repos.

Schritte:

1. Vollständiger Audit pro `src/vio_*.c`: wo tauchen Zend-Symbole auf? Liste führen.
2. Neues `ext/`-Verzeichnis im php-vio-Repo anlegen.
3. Pro Datei: Zend-spezifischen Code (Class-Registrierung, Object-Lifecycle, zval-Marshalling) nach `ext/vio_*_zend.c` verschieben. Reine C-Logik bleibt in `src/`.
4. `emalloc`/`efree` → `malloc`/`free` in `src/`.
5. `zend_throw_exception` → Error-Return-Code (`vio_result_t`) in `src/`. Zend-Wrapper übersetzt Fehler in PHP-Exceptions.
6. Build funktioniert wie vorher, alle PHPT-Tests grün.
7. Ein Commit: „feat: separate Zend wrappers from core C code (pre-v2)."

**Nach Phase 1 ist die Extraktion trivial** — Phase 2 ist dann hauptsächlich ein `git mv` + Repo-Split.

### Phase 2 — Extraktion libvio-core

1. Neues Repo `phpolygon/libvio` anlegen.
2. `src/`, `include/`, Vendored-Dependencies (`vendor/glad`, `vendor/stb`, `vendor/vma`, `vendor/miniaudio`), null-Backend, Audio, Video, Stream, Input, Font, Shader-Toolchain nach libvio migrieren.
3. CMake-Projekt in libvio aufsetzen, exportiert `libvio::core`.
4. Render-Backends (opengl, vulkan, metal, d3d11, d3d12) **bleiben vorerst** in libvio, als Unterverzeichnisse von `src/backends/`. Der Backend-Split folgt in Phase 3.
5. php-vio wird umgestellt: statt gegen internen `src/` zu bauen, linkt es gegen libvio über CMake-Find-Package.
6. `release-manifest.json` wird eingeführt, pinnt libvio-Version.
7. End-to-End-Test: php-vio-Build mit externer libvio reproduziert 1:1 die 1.x-Funktionalität. Alle PHPT-Tests grün.

### Phase 3 — Backend-Repo-Splits

Pro Backend (beginnend mit dem am wenigsten kritischen):

1. Neues Repo `phpolygon/libvio-<backend>` anlegen.
2. Backend-Code aus `libvio/src/backends/<backend>/` in das neue Repo migrieren.
3. Backend-CMake exportiert `libvio::backend::<backend>`, deklariert `find_package(libvio 1.0 REQUIRED)`.
4. Test-Suite pro Backend anlegen (Backend-Conformance-Tests gegen Vtable-Contract).
5. php-vio-Kompatibilitätstest-Job im Backend-Repo einrichten (§7).
6. `release-manifest.json` um den neuen Backend-Eintrag erweitern.
7. End-to-End-Test php-vio: Build zieht jetzt dieses Backend extern. Verhalten unverändert.

Reihenfolge der Splits (Vorschlag, umstellbar):

1. **opengl** — am stabilsten, gut testbar, niedrigstes Risiko. Erster Test des gesamten Backend-Split-Prozesses.
2. **metal** — macOS-only, isoliert, kein Overlap mit Windows-Backends.
3. **vulkan** — komplexestes Backend, bewusst nach OpenGL/Metal (Erfahrung sammeln).
4. **d3d11** + **d3d12** — gemeinsam oder separat, je nachdem wie getrennt die Codebasen sind.

### Phase 4 — Stabilisierung & v2.0-Release

1. Alle Backends extrahiert, alle Tests grün.
2. Tags `v1.0.0` für libvio und jedes Backend-Repo.
3. `release-manifest.json` enthält ausschließlich stabile Tags.
4. Release `php-vio v2.0.0` mit diesem Manifest.
5. Doku-Update: libvio bekommt eigene README + API-Doku. php-vio-README verweist auf libvio als Upstream.
6. Ankündigung (minimal, konsistent mit der Haltung „Produkt positioniert sich selbst").

---

## 9. Timing & Prioritäten

Zur Einordnung im größeren Kontext (Stand April 2026):

- **Code Tycoon Next Fest (15.–22. Juni 2026):** Blocker für alle Architekturarbeit an libvio.
- **iPad-Port Code Tycoon:** Folgt auf Next Fest. Testet implizit die Backend-Vtable auf neue Plattform-Anforderungen (iOS-Lifecycle, POSIX-Restriktionen). Erkenntnisse hieraus fließen in die Backend-ABI, **bevor** sie in Phase 3 eingefroren wird.
- **PHPolygon Sky/Atmosphere-System:** Separates Projekt, läuft parallel. Unabhängig von libvio-Extraktion.

**Empfohlene Reihenfolge:**

1. Bis Juni: keine libvio-Arbeit außer Phase 0 (dieses Dokument).
2. Nach Next Fest: Phase 1 (Zend-Trennung im Monorepo) parallel zu iPad-Port-Arbeit.
3. Nach iPad-Port: Phase 2 (libvio-Extraktion) und Phase 3 (Backend-Splits).
4. Phase 4 & v2-Release: wenn alles stabil ist, ohne feste Deadline.

Der Zeitrahmen ist bewusst lose. v2 ist eine strategische Aufräumaktion, kein Feature, das unter Zeitdruck steht.

---

## 10. Offene Fragen

Punkte, die bei der Umsetzung auftauchen werden und bewusst noch nicht vorgreifend entschieden sind:

- **Shader-Toolchain-Scope.** Bleibt `glslang` + `SPIRV-Cross` in libvio-core, oder wird das zu einem separaten `libvio-shadertool` Repo? Argumentation beides möglich. **Default:** bleibt in Core, bis es einen konkreten Grund zur Trennung gibt.
- **Plugin-System vs. Backend-Ökosystem.** Aktuell: Plugins (Output/Input/Filter) und Backends teilen konzeptionell die Vtable-Idee, leben aber unterschiedlich. Frage: Werden Third-Party-Plugins analog zu Backends in eigene Repos extrahiert? **Default:** nein, Plugins sind weiterhin dynamisch ladbar, Backends statisch gelinkt.
- **Documentation-Hosting.** libvio mit eigener API-Doku (Doxygen? Eigene Website wie phpolygon.github.io?). **Default:** Doxygen-generiert, Hosting als GitHub Pages am libvio-Repo. Entscheidung in Phase 4.
- **Logo und Branding.** libvio braucht ein eigenes visuelles Identität, wenn es eigenständig steht. **Default:** geerbt von phpolygon-Branding, bis jemand anderes Interesse zeigt.
- **License-Konsistenz.** Aktuell MIT. Bleibt in allen neuen Repos MIT. Null Flexibilität hier — keine LGPL/GPL/BSL-Experimente.
- **CI-Provider.** GitHub Actions konsistent für alle Repos. Windows-Runner für D3D-Backends, macOS-Runner für Metal-Backend.

---

## 11. Nicht-Ziele

Explizit zur Abgrenzung, damit Scope-Creep während der Umsetzung ausbleibt:

- **Kein neues API-Design.** v2 ist eine Repo-/Build-Refaktorisierung, nicht ein API-Rewrite. Funktionssignaturen bleiben stabil.
- **Kein Ersatz für existierende Abstraktionen.** libvio konkurriert nicht mit bgfx, sokol, WebGPU. Es ist die extrahierte Core-Library hinter php-vio und PHPolygon.
- **Keine Runtime-Plugin-Loader-Infrastruktur** für Backends. Backends werden statisch gelinkt. Nur das bestehende Plugin-System (`vio_plugin.h`) erlaubt dynamisches Laden von Output/Input/Filter-Plugins.
- **Keine neuen Backends in v2.** WebGPU, Software-Renderer etc. sind für v2.x oder v3 denkbar, nicht Scope von v2.0.
- **Keine Sprachbindings von uns.** Wenn jemand Rust-/Go-/Zig-Bindings für libvio schreiben will, ist das großartig, aber wir pflegen sie nicht.

---

*Ende des Dokuments. Änderungen via PR ins php-vio-Repo, mit Begründung im Commit-Message.*
