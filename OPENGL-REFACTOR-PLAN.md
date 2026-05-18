# OpenGL als eigenständiges Backend — Refactor-Plan

> **Status:** Implementations-Plan, Branch `feat/opengl-standalone-backend`
> **Schließt:** [#2](https://github.com/phpolygon/php-vio/issues/2), [#3](https://github.com/phpolygon/php-vio/issues/3), [#4](https://github.com/phpolygon/php-vio/issues/4)
> **Vorlage:** `v2-architecture.md` §2.1, §8 Phase 1 + Phase 3 (OpenGL-spezifisch)

---

## 1. Ziel

OpenGL wird vom „über `src/` verstreuten Quasi-Backend" zu einem **echten kapselten Backend hinter der Vtable**, sodass nach dem Refactor folgendes gilt:

- `grep -rE 'gl[A-Z]|GL_[A-Z]' src/ include/ php_vio.c` liefert **0 Treffer außerhalb von `src/backends/opengl/`** — bewacht durch einen CI-Audit-Gate-Test.
- Jede heute hartcodierte OpenGL-Verzweigung in `php_vio.c` wird ersetzt durch `if (be->method)` plus `backend_has(be, VIO_FEATURE_*)` — call-sites entscheiden via Capability-Check, nicht via Backend-Name-`strcmp`.
- Die Feature-Erkennung kennt **Core-Version *und* Extension-Fallback** (Issue #3 Part 2): `has_compute_shader` ist true auf GL 4.3 *oder* `GL_ARB_compute_shader`.
- Die PHP-API gewinnt die Render-Target-Convenience-Funktionen aus Issue #4 (`vio_create_render_target` / `vio_set_render_target` / `vio_push_render_target` / `vio_pop_render_target` / `vio_destroy_render_target`) **zusätzlich** zur bestehenden Surface — keine Breaking Changes.
- Der Build-Pfad für VMA-C++-Wrapper und Metal-ObjC-Source funktioniert auch in static-php-cli-Builds (Issue #2).

PHP-API-Kompatibilität ist nicht verhandelbar: alle 71 `vio_*`-Funktionen verhalten sich nach dem Refactor semantisch identisch.

---

## 2. Audit-Befund (Stand 2026-05-18)

Direkte OpenGL-Aufrufe außerhalb von `src/backends/opengl/`:

| Datei | GL-Hits | Charakter |
|-------|---------|-----------|
| `php_vio.c` | 188 | sämtliche PHP-Funktionen, die State setzen, lesen oder dispatchen |
| `src/vio_2d.c` | ~40 | kompletter 2D-Batch-Renderer hardcoded GL (VAO/VBO/glDrawArrays/Scissor/Blend) |
| `src/vio_mesh.c` | 3 | `glDelete*` im Destructor |
| `src/vio_render_target.c` | 3 | `glDelete*` im Destructor |
| `src/vio_texture.c` | 1 | `glDeleteTextures` im Destructor |
| `src/vio_buffer.c` | 1 | `glDeleteBuffers` im Destructor |
| `src/vio_shader.c` | 1 | `glDeleteProgram` im Destructor |
| `src/vio_font.c` | 1 | `glDeleteTextures` im Destructor |
| `src/vio_cubemap.c` | 1 | `glDeleteTextures` im Destructor |

Die Cluster in `php_vio.c` zerfallen in 7 Themen-Blöcke, die in Etappe 5 schrittweise gelift't werden.

---

## 3. Etappen

### Etappe 0 — Static-Build-Fix für VMA + Metal (Issue #2)

**Problem:** `Makefile.frag` registriert `vio_vma_wrapper.cpp` und `vio_metal.m` nur in `shared_objects_vio`. In static-php-cli-Builds wird `PHP_GLOBAL_OBJS` genutzt → die Objekte sind orphaned → Linker-Fail.

**Vorgehensweise:**

1. `config.m4`: VMA-Wrapper und Metal-Source via `PHP_ADD_SOURCES_X` mit per-Source-Flags registrieren — funktioniert für statische *und* shared Builds.
   - VMA: `-std=c++14 -DVMA_STATIC_VULKAN_FUNCTIONS=0`
   - Metal: `-x objective-c -fobjc-arc`
2. `Makefile.frag`: VMA- und Metal-Regeln daraus entfernen (Duplikat-Definitionen vermeiden).
3. Verifikation mit lokalem `phpize`-Shared-Build (Default) **und** simuliertem Static-Build (Manual-Test via static-php-cli-Compatibility-Mode oder dokumentierter Workaround-Override).

**Akzeptanzkriterium:**
- Shared-Build auf macOS PHP 8.4 + 8.5 unverändert grün.
- `nm modules/vio.so | grep _vma_` und `nm modules/vio.so | grep _metal_` zeigen non-stub Symbole.
- 69 PHPT-Tests grün (Baseline: 61 pass / 2 env-fail / 6 skip).

**Aufwand:** 4-6h.

**Reihenfolge:** Voraussetzung für sauberen Branch — wenn wir später `vio_2d_opengl.c` hinzufügen und das Build-System inkonsistent ist, häufen sich Bug-Quellen.

---

### Etappe 1 — Destructors über die Vtable (Issue #3 Part 1, mechanisch)

**Was sich ändert:**

`include/vio_backend.h`: neue Funktionspointer, alle dürfen `NULL` sein:

```c
void (*destroy_mesh)(void *mesh);            /* mesh = vio_mesh_object* */
void (*destroy_render_target)(void *rt);     /* rt   = vio_render_target_object* */
void (*destroy_cubemap)(void *cm);           /* cm   = vio_cubemap_object* */
void (*destroy_font_atlas)(void *font);      /* font = vio_font_object* */
```

`destroy_buffer`, `destroy_texture`, `destroy_shader` existieren bereits.

`src/backends/opengl/vio_opengl.c`: implementiert die 4 neuen Destructors mit dem heutigen `glDelete*`-Code.

`src/vio_mesh.c`, `src/vio_render_target.c`, `src/vio_cubemap.c`, `src/vio_font.c`: ersetzen direkte `glDelete*` durch `if (obj->backend && obj->backend->destroy_xxx) obj->backend->destroy_xxx(obj);`. Object-Structs brauchen einen `backend`-Pointer falls noch nicht vorhanden (additiv, PHP-API-unsichtbar).

`src/vio_buffer.c`, `src/vio_texture.c`, `src/vio_shader.c`: schon teilweise vtable-getrieben; deren GL-Destruct-Pfad konsequent auf vtable umstellen.

**Akzeptanzkriterium:**
- `grep -E 'glDelete' src/vio_*.c` liefert leer.
- 69 PHPT-Tests grün.

**Aufwand:** 3h. Rein mechanisch.

**Parallelisierbar?** Ja, unabhängig von Etappe 2.

---

### Etappe 2 — Capability-Enum + Extension-Fallback (Issue #3 Part 2)

**Was sich ändert:**

`include/vio_types.h`: Enum `vio_feature` um folgende Werte erweitert (am Ende, keine Umnummerierung):

```c
VIO_FEATURE_READ_PIXELS         = 6,
VIO_FEATURE_INSTANCED_DRAW      = 7,
VIO_FEATURE_RENDER_TARGET       = 8,
VIO_FEATURE_RENDER_TARGET_HDR   = 9,
VIO_FEATURE_RENDER_TARGET_DEPTH = 10,
VIO_FEATURE_RENDER_TARGET_MSAA  = 11,
VIO_FEATURE_CUBEMAP             = 12,
VIO_FEATURE_DEPTH_BIAS          = 13,
VIO_FEATURE_SCISSOR             = 14,
VIO_FEATURE_TEXTURE_SWIZZLE     = 15,
VIO_FEATURE_NATIVE_2D_BATCH     = 16,
VIO_FEATURE_DEBUG_OUTPUT        = 17,
VIO_FEATURE_DSA                 = 18,
VIO_FEATURE_BUFFER_STORAGE      = 19,
VIO_FEATURE_TEXTURE_STORAGE     = 20,
VIO_FEATURE_SEPARATE_SHADERS    = 21,
```

`src/backends/opengl/`:

1. Im `vio_gl`-Struct (siehe `src/backends/opengl/vio_opengl.c`) drei neue Felder:
   ```c
   int     extension_count;
   char  **extensions;        /* heap, freed at shutdown */
   /* Cached caps, gefüllt 1× in vio_opengl_setup_context() */
   struct {
       int has_compute_shader;
       int has_tessellation;
       int has_separate_shader_objects;
       int has_debug_output;
       int has_dsa;
       int has_buffer_storage;
       int has_texture_storage;
       /* ... weitere nach Bedarf */
   } caps;
   ```
2. Neuer Helper `static int gl_has_ext(const char *name)`: bin-search oder linear über das gecachte Array. Extension-Liste wird einmal beim Setup via `glGetIntegerv(GL_NUM_EXTENSIONS)` + `glGetStringi(GL_EXTENSIONS, i)` geholt — das Modern-GL-Idiom, nicht das deprecated `glGetString(GL_EXTENSIONS)`.
3. Caps-Init:
   ```c
   vio_gl.caps.has_compute_shader = gl_ge(4,3) || gl_has_ext("GL_ARB_compute_shader");
   vio_gl.caps.has_tessellation   = gl_ge(4,0) || gl_has_ext("GL_ARB_tessellation_shader");
   /* ... */
   ```
4. `opengl_supports_feature()` mappt auf die gecachten Caps statt jeden Call einen Versions-Check zu rechnen.
5. Alle anderen Backends (Vulkan, Metal, D3D11, D3D12, Null): `supports_feature()` um die neuen Cases erweitern. Pragmatische Defaults: D3D11/12 für RT/Cubemap/Read-Pixels true (Code existiert), Metal/Vulkan für 3D-Features false (Stub), Null überall false.

**Begründung pro neuem Feature** — keine Featuritis, jeder Wert entspricht einer konkreten Call-Site, die heute hartcoded GL ruft oder per `strcmp(backend_name, ...)` verzweigt:

| Feature | Anfragende Call-Site | Backend-Realität |
|---|---|---|
| `READ_PIXELS` | `vio_read_pixels`, `vio_save_screenshot`, `vio_recorder_capture`, `vio_stream_push` | Vulkan/Metal stub |
| `INSTANCED_DRAW` | `vio_draw_instanced` | aktuell `strcmp` |
| `RENDER_TARGET` | `vio_render_target`, `vio_bind_render_target`, neue API #4 | Vulkan/Metal stub |
| `RENDER_TARGET_HDR` | `vio_render_target([..., "hdr" => true])` | braucht `RGBA16F` |
| `RENDER_TARGET_DEPTH` | Depth-Only-RTs (Shadow Maps) | Metal stub |
| `RENDER_TARGET_MSAA` | `vio_render_target([..., "samples" => N])` | Backend-Probe |
| `CUBEMAP` | `vio_cubemap`, `vio_bind_cubemap` | Metal stub |
| `DEPTH_BIAS` | `vio_pipeline([..., "depth_bias" => ...])` | `glPolygonOffset` ist GL-Konzept |
| `SCISSOR` | 2D-Renderer Push-Scissor-Pfad | Null hat es nicht |
| `TEXTURE_SWIZZLE` | Font-Atlas (`R8` → Alpha) | D3D-Pfad expandiert CPU |
| `NATIVE_2D_BATCH` | `vio_2d_flush()`-Dispatch | Backend signalisiert eigenen Pfad |
| `DEBUG_OUTPUT` | `vio_gl_info()`-Feldfüllung (#3 Part 3) | GL 4.3 / KHR_debug |
| `DSA` | Pfad-Dispatch in Backend-Internals | GL 4.5 / ARB_direct_state_access |
| `BUFFER_STORAGE` | `vio_uniform_buffer` Persistent-Mapped-Pfad | GL 4.4 / ARB_buffer_storage |
| `TEXTURE_STORAGE` | Backend-internes Immutable-Storage | GL 4.2 / ARB_texture_storage |
| `SEPARATE_SHADERS` | Shader-Pipeline-Objects | GL 4.1 / ARB_separate_shader_objects |

`php_vio.c` bekommt einen Helper:

```c
static inline int backend_has(const vio_backend *b, vio_feature f) {
    return b && b->supports_feature && b->supports_feature(f);
}
```

…den die Call-Sites in Etappe 5+ als Guard nutzen.

**Akzeptanzkriterium:**
- Alle 5 Backends melden für die neuen Features konsistente Werte (siehe Capabilities-Matrix in §5).
- `vio_gl.caps.*` ist nach Setup gefüllt; keine `gl*`-Aufrufe in `supports_feature()` mehr.
- 69 PHPT-Tests grün.

**Aufwand:** 3h.

**Parallelisierbar?** Ja, unabhängig von Etappe 1.

---

### Etappe 3 — `vio_2d.c` Lift → `src/backends/opengl/vio_2d_opengl.c`

**Was sich ändert:**

Neue Dateien `src/backends/opengl/vio_2d_opengl.c` und `.h`, analog zu existierenden `vio_2d_d3d11.c` / `vio_2d_d3d12.c`:

- `vio_2d_opengl_state` (VAO, VBO, shader_shapes, shader_sprites, vbo_capacity).
- `vio_2d_opengl_init(state)`, `vio_2d_opengl_shutdown(state)`, `vio_2d_opengl_flush(state, items, vertices, ...)`.

`src/vio_2d.c`: der `#ifdef HAVE_GLFW`-Block wird zu einem Aufruf in die neue Datei reduziert. Public-API bleibt 1:1 gleich (`vio_2d_init`/`vio_2d_shutdown`/`vio_2d_flush`).

`config.m4`: neue Source registrieren.

**Akzeptanzkriterium:**
- `grep -E 'gl[A-Z]' src/vio_2d.c` liefert leer.
- 2D-PHPT-Tests grün: `012_2d_shapes`, `013_2d_sprite`, `030_font_text`, `031_2d_pixel_verify`, `053_2d_state_stacks`, `054_rounded_rect`, sowie Long-Run-Perf-Gate aus `069_perf_regression_gates`.

**Aufwand:** 4-6h. Mechanisches Verschieben.

**Vorsicht:** VAO-Setup-Reihenfolge — der OpenGL-Kontext muss vor `vio_2d_*_init` initialisiert sein. Aktuell ist das via `vio_gl.initialized`-Guard sichergestellt; beim Lift bleibt das stabil, aber im Code-Review explizit prüfen.

**Parallelisierbar?** Ja, parallel zu 1 und 2. Aber **sequenziell vor Etappe 5**.

---

### Etappe 4 — Vtable-Erweiterung für Frame-Lifecycle, RT, Read-Back, State

**Was sich ändert:**

`include/vio_backend.h` — neue Funktionspointer, alle dürfen `NULL` sein:

```c
/* Render-Target — heute hardcoded pro Backend in php_vio.c */
void *(*create_render_target)(int width, int height, int hdr, int depth_only, int msaa_samples);
void  (*bind_render_target)(void *rt);
void  (*unbind_render_target)(void);

/* Cubemap */
void *(*create_cubemap)(int width, int height, const void *face_data[6]);
void  (*bind_cubemap)(void *cm, int slot);

/* Headless / Pixel-Read */
int   (*setup_headless)(int width, int height);    /* return 0 on success */
void  (*teardown_headless)(void);
int   (*read_pixels)(int width, int height, void *out_rgba);

/* State-Knobs heute direkt in php_vio.c als GL-Calls */
void  (*set_blend_mode)(vio_blend_mode mode);
void  (*set_depth_test)(int enabled, vio_depth_func func);
void  (*set_cull_mode)(vio_cull_mode mode);
void  (*set_polygon_offset)(float constant_bias, float slope_scaled_bias);
void  (*set_scissor)(int x, int y, int w, int h);  /* w<=0 disables */

/* Font-Atlas — R8 + optional Alpha-Swizzle */
void *(*create_font_atlas)(int width, int height, const unsigned char *r8_data, int swizzle_red_to_alpha);

/* Mesh-Create — wird heute direkt in php_vio.c gebaut */
void *(*create_mesh)(const void *vertex_data, int vertex_count, int stride,
                     const vio_vertex_attrib *layout, int attrib_count,
                     const unsigned int *indices, int index_count);

/* Instancing — heute mit hartcodierter glVertexAttribDivisor-Logik in php_vio.c */
void  (*draw_instanced)(void *mesh, const float *matrices_4x4, int instance_count);
```

**Was sich ändert pro Backend:**

- OpenGL: implementiert alle 14 Methoden via Code-Lift aus heutigem `php_vio.c`.
- D3D11/D3D12: implementieren `create_render_target`/`bind_render_target`/`unbind_render_target`/`create_cubemap`/`bind_cubemap`/`read_pixels` via Lift des heute in `php_vio.c` lebenden D3D-Codes. `setup_headless` → NULL (Backbuffer ist Headless-Surface).
- Vulkan: NULL für die meisten neuen Methoden bis 3D-Pfad ausimplementiert ist.
- Metal: NULL für 3D-Methoden (Metal-3D ist Stub). 2D bleibt unangetastet.
- Null-Backend: alle NULL.

**Akzeptanzkriterium:**
- Vtable kompiliert auf allen Plattformen.
- Backend-Registration funktioniert.
- 69 PHPT-Tests grün (Code in `php_vio.c` ist noch nicht entfernt — die Vtable-Methoden sind parallel verfügbar).

**Aufwand:** 8-10h. Größtenteils Code-Lift, wenig neue Logik.

**Parallelisierbar?** Nach Etappe 1+2 (Vtable-Konventionen etabliert), unabhängig von Etappe 3.

---

### Etappe 5 — `php_vio.c` Cluster-Lift (sequenziell)

Nach Etappe 4 stehen die Vtable-Methoden bereit. Jetzt wird in `php_vio.c` Cluster für Cluster der direkte GL-Call durch `ctx->backend->xxx()` ersetzt, mit `if (...->xxx) { ... }` + `backend_has()`-Guard. Reihenfolge bewusst pragmatisch:

**5a — Frame-Lifecycle / Viewport / Headless-FBO**
- `vio_create`: FBO-Setup → `if (be->setup_headless) be->setup_headless(...)`.
- `vio_destroy`: `be->teardown_headless()`.
- `vio_begin` / `vio_unbind_render_target`: GL-spezifischer Z. 426 + Z. 481-482 Block wandert komplett ins OpenGL-`begin_frame`.

**5b — Mesh-Create + Draw**
- `vio_mesh`: `mesh->backend_mesh = be->create_mesh(...)`. GL-Felder `vao`/`vbo`/`ebo` werden zu opake `void *backend_mesh`.
- `vio_draw`: nutzt konsequent `be->draw(&cmd)`.

**5c — Pipeline-Bind State**
- `vio_bind_pipeline`: Cull/Depth/Blend/Polygon-Offset über `be->set_cull_mode` / `be->set_depth_test` / `be->set_blend_mode` / `be->set_polygon_offset`.

**5d — Uniform / Bind-Texture / Uniform-Buffer**
- `vio_texture`: GL-Pfad komplett in `be->create_texture` (Vtable existiert, OpenGL-Backend stub'd heute mit `return NULL`).
- `vio_uniform_buffer`: dito via `be->create_buffer`.
- `vio_set_uniform`: über die existierende `set_uniform`-Vtable; OpenGL-Backend cached die `glGetUniformLocation`-Ergebnisse per Shader-Programm.
- `vio_bind_texture`: `be->bind_texture` (existiert).

**5e — Render-Target / Cubemap / Font-Atlas + neue API (Issue #4)**

Lift der existierenden Funktionen:
- `vio_render_target`: → `be->create_render_target(...)`.
- `vio_bind_render_target` / `vio_unbind_render_target`: → `be->bind_render_target` / `be->unbind_render_target`.
- `vio_cubemap` / `vio_bind_cubemap`: → `be->create_cubemap` / `be->bind_cubemap`.
- `vio_font`: → `be->create_font_atlas(w, h, r8_data, swizzle_red_to_alpha=1)`.

Zusätzlich neue PHP-Funktionen (Issue #4):

```php
$rt = vio_create_render_target($ctx, $width, $height, [
    'format'  => 'rgba8',     // 'rgba8' | 'rgba16f' (HDR)
    'depth'   => true,
    'samples' => 1,
]);
vio_set_render_target($ctx, $rt);     // null = window default
vio_destroy_render_target($rt);

// Stack-Variante analog zu pushScissor/popScissor:
vio_push_render_target($ctx, $rt);
vio_pop_render_target($ctx);
```

Implementierung: dünne Aliase auf die schon refactorten Vtable-Methoden. RT-Stack lebt im VioContext-Struct (max-Tiefe z.B. 8, Overflow → Warning + Stack-Top-Replace).

**Bestehende Funktionen bleiben erhalten** — keine PHP-API-Brüche. CHANGELOG dokumentiert die neuen Funktionen als „bevorzugte Schreibweise ab v1.13".

**5f — Read-Back / Recording / Streaming**

Vier nahezu identische Read-Pixels-Pfade in `php_vio.c`. Werden alle reduziert auf:

```c
if (!backend_has(be, VIO_FEATURE_READ_PIXELS)) {
    RETURN_FALSE;
}
be->read_pixels(w, h, out_buf);
/* vertical flip im PHP-Layer, backend-agnostisch */
```

Betroffen: `vio_read_pixels`, `vio_save_screenshot`, `vio_recorder_capture`, `vio_stream_push`.

**5g — Instancing**

`vio_draw_instanced`: vereinfacht zu `be->draw_instanced(mesh, matrices, count)`. Vier-Attribute-Divisor-Aufbau lebt im OpenGL-Backend.

**Akzeptanzkriterium pro Sub-Etappe:**
- Nach **jedem** Sub-Schritt 69 PHPT-Tests grün. Bewusst kein Big-Bang.
- Endkriterium: `grep -E 'gl[A-Z]|GL_' php_vio.c` liefert leer (außer `#ifdef HAVE_GLFW`-Kommentare oder Konstanten, die als String an PHP exportiert werden).

**Aufwand:** 16-22h total. Sub-Schritte je 2-4h.

**Parallelisierung:** 5a ist Voraussetzung für b-g. 5b/5c/5d sind unabhängig parallel. 5e und 5f sequenziell nach 5a. 5g isoliert.

---

### Etappe 6 — Audit-Gate + Include-Cleanup

**Was sich ändert:**

- `#include <glad/glad.h>` aus `php_vio.c`, `src/vio_2d.c`, `src/vio_mesh.c`, `src/vio_texture.c`, `src/vio_render_target.c`, `src/vio_cubemap.c`, `src/vio_font.c`, `src/vio_shader.c`, `src/vio_buffer.c` entfernen — sollten nach Etappe 1+3+5 ungenutzt sein.
- Neuer PHPT-Test `070_no_gl_outside_backend.phpt` (oder besser ein Shell-Test im `tests/`-Verzeichnis), der prüft:
  ```sh
  cd $(dirname $0)/..
  COUNT=$(grep -rE 'gl[A-Z]|GL_[A-Z]' src/ include/ php_vio.c \
          | grep -v 'src/backends/opengl/' \
          | grep -v '^\s*\*' \
          | wc -l)
  [ "$COUNT" -eq 0 ]
  ```
- CI-Workflow zieht den Test mit — Regression-Schutz für die Zukunft.

**Akzeptanzkriterium:**
- Audit-Gate-Test passt: 0 GL-Calls außerhalb `src/backends/opengl/`.
- 69 + 1 PHPT-Tests grün.

**Aufwand:** 2h.

---

### Etappe 7 — Compatibility-Surface (Issue #3 Part 3)

**Was sich ändert:**

1. Neue PHP-Funktion `vio_gl_info($ctx): array | false`:
   ```php
   [
       'version'    => '4.6',
       'glsl'       => '460',
       'renderer'   => 'AMD Radeon Pro 555X OpenGL Engine',
       'vendor'     => 'AMD',
       'profile'    => 'core',
       'extensions' => ['GL_ARB_compute_shader', ...],
       'features'   => [
           'compute_shader'           => true,
           'tessellation'             => true,
           'separate_shader_objects'  => true,
           'debug_output'             => true,
           'dsa'                      => true,
           'buffer_storage'           => true,
           'texture_storage'          => true,
       ],
   ]
   ```
   Implementiert in `php_vio.c`, fragt das OpenGL-Backend ab via neuer Vtable-Methode `gl_info` (oder backend-spezifischer Convenience-Header). Auf Non-OpenGL-Backends `false` mit Warning.

2. GLSL-Version-Mismatch-Check in `vio_shader()`:
   - Vor Compile: Parse `#version N` aus dem User-Shader-Source.
   - Vergleiche mit `vio_gl.glsl_version`.
   - Bei `N > glsl_version`: `php_error_docref(E_WARNING, "shader requires GLSL >= %d, runtime context provides %d", N, glsl_version)` + `RETURN_FALSE`.
   - Bei `N <= glsl_version`: weiter wie bisher (SPIRV-Cross transpiliert ggf. runter).

3. `CLAUDE.md`: neue Tabelle „OpenGL-Feature-Ladder", die pro Core-Version (3.3, 4.0, 4.1, 4.3, 4.4, 4.5, 4.6) auflistet, welche `VIO_FEATURE_*` sicher verfügbar sind und welche per Extension-Fallback geprüft werden.

4. Neuer PHPT-Test `071_gl_info_and_shader_version.phpt`:
   - `vio_gl_info($ctx)` liefert array mit erwarteten Keys.
   - Submit eines `#version 999 core` Shaders → erwartete Warning + `false` Return.

**Akzeptanzkriterium:**
- `vio_gl_info($ctx)` getestet auf macOS (GL 4.1 via Apple-Treiber).
- Shader-Version-Mismatch produziert lesbare Warning, keinen Driver-Crash.
- CLAUDE.md aktualisiert.

**Aufwand:** 3-4h.

---

## 4. PHP-API-Erweiterungen (Issue #4)

Neue Funktionen — alle additiv:

| Funktion | Beschreibung |
|---|---|
| `vio_create_render_target($ctx, $w, $h, $opts): VioRenderTarget\|false` | Alias / Convenience für `vio_render_target`, mit explizitem Format/MSAA/Depth |
| `vio_set_render_target($ctx, $rt \| null): void` | Ersetzt die zwei-Funktionen-Variante; `null` = Window-Default |
| `vio_push_render_target($ctx, $rt): void` | Stack-Push |
| `vio_pop_render_target($ctx): void` | Stack-Pop |
| `vio_destroy_render_target($rt): void` | Explizites Destroy (Default: GC) |
| `vio_gl_info($ctx): array\|false` | Diagnostics (Issue #3 Part 3) |

Bestehende Funktionen bleiben unverändert:
- `vio_render_target($ctx, $opts)` → wirkt fortan als interner Alias auf `vio_create_render_target`.
- `vio_bind_render_target($ctx, $rt)`, `vio_unbind_render_target($ctx)` → bleiben für Bestandscode.

`vio.stub.php` und `php_vio_arginfo.h` werden entsprechend ergänzt.

---

## 5. Capabilities-Matrix nach Refactor

| Capability | OpenGL 3.3 | OpenGL 4.3+ | Vulkan | Metal | D3D11 | D3D12 | Null |
|---|---|---|---|---|---|---|---|
| `COMPUTE` | 0 | 1 | 1 | 1 | 1 | 1 | 0 |
| `RAYTRACING` | 0 | 0 | 1 | 0 | 0 | 1 | 0 |
| `TESSELLATION` | 0 (3.3) / 1 (4.0+) | 1 | 1 | 1 | 1 | 1 | 0 |
| `GEOMETRY` | 1 | 1 | 1 | 0 | 1 | 1 | 0 |
| `MULTIVIEW` | 0 | 0 (Ext) | 1 | 0 | 0 | 0 | 0 |
| `3D_PIPELINE` | 1 | 1 | 1 | 0 | 1 | 1 | 0 |
| `READ_PIXELS` | 1 | 1 | 0 (Stub) | 0 (Stub) | 1 | 1 | 0 |
| `INSTANCED_DRAW` | 1 | 1 | 1 | 0 | 1 | 1 | 0 |
| `RENDER_TARGET` | 1 | 1 | 0 (Stub) | 0 (Stub) | 1 | 1 | 0 |
| `RENDER_TARGET_HDR` | 1 | 1 | 0 | 0 | 1 | 1 | 0 |
| `RENDER_TARGET_DEPTH` | 1 | 1 | 0 | 0 | 1 | 1 | 0 |
| `RENDER_TARGET_MSAA` | 1 | 1 | 0 | 0 | 1 | 1 | 0 |
| `CUBEMAP` | 1 | 1 | 0 | 0 | 1 | 1 | 0 |
| `DEPTH_BIAS` | 1 | 1 | 1 | 1 | 1 | 1 | 0 |
| `SCISSOR` | 1 | 1 | 1 | 1 | 1 | 1 | 0 |
| `TEXTURE_SWIZZLE` | 1 | 1 | 1 | 1 | 0 | 0 | 0 |
| `NATIVE_2D_BATCH` | 1 | 1 | 0 | 1 | 1 | 1 | 0 |
| `DEBUG_OUTPUT` | 0 (Ext) | 1 | 1 | 0 | 1 | 1 | 0 |
| `DSA` | 0 (Ext) | 0 (4.5) | n/a | n/a | n/a | n/a | 0 |
| `BUFFER_STORAGE` | 0 (Ext) | 0 (4.4) | n/a | n/a | n/a | n/a | 0 |
| `TEXTURE_STORAGE` | 0 (Ext) | 1 (4.2) | n/a | n/a | n/a | n/a | 0 |
| `SEPARATE_SHADERS` | 0 (Ext) | 1 (4.1) | n/a | n/a | n/a | n/a | 0 |

Felder mit „n/a" sind backend-spezifische OpenGL-Konzepte und für andere Backends nicht definiert — sie geben 0 zurück, was korrekt als „nicht unterstützt" interpretiert wird. Aufrufer fragen sie ohnehin nur in OpenGL-Code-Pfaden ab (innerhalb des OpenGL-Backends selbst).

---

## 6. Risiken & Mitigationen

### Performance-Regression

1. **`vio_set_uniform`-Hot-Path:** Vtable-Indirektion + Name-Lookup pro Call. **Mitigation:** OpenGL-Backend cached `glGetUniformLocation` per Shader-Programm in einer kleinen Hash-Map — heute tat `php_vio.c` das *nicht*. Nettoeffekt neutral bis leicht positiv.
2. **`vio_draw`-Hot-Path:** Direkter `glDrawArrays` → `be->draw(&cmd)`. Eine indirekte Funktion mehr, aber identisch zur heutigen D3D-Indirektion. Perf-Test `069_perf_regression_gates` muss vergleichbar bleiben.
3. **2D-Batch-Lift:** Identische Logik in neuer Datei. Inlining-Verhalten könnte minimal abweichen. `069_perf_regression_gates` + Long-Run-Test (`068`) müssen die Baseline halten.

### PHP-API-Bruch

- **Keinen.** Alle bestehenden Funktionen behalten Signatur und Semantik.
- **Subtil:** `vio_render_target` und `vio_cubemap` geben heute auf unsupported Backends `false` mit `E_WARNING`. Das Verhalten muss exakt erhalten bleiben, wenn `supports_feature(VIO_FEATURE_RENDER_TARGET)` 0 ist.

### Backend-Stubs werden expliziter sichtbar

- Vulkan/Metal `read_pixels` ist heute schon Stub (CLAUDE.md dokumentiert). Nach Refactor melden sie das via Capability sauber. Kein Verhaltenswechsel.
- Vulkan-No-Op-Funktionen, die heute als no-op-Funktionen statt `NULL` registriert sind, sollten beim Refactor konsistent auf `NULL` gestellt werden. Sonst greift der `if (be->method)`-Guard nicht.

### Build-System

- Etappe 0 fixt VMA + Metal Static-Build. Neue Source `vio_2d_opengl.c` ist plain C → unkritisch via `PHP_NEW_EXTENSION`.
- Etappe 0 muss vor Etappe 3 (neue Source) abgeschlossen sein.

---

## 7. Was wir in diesem Branch NICHT tun

- **Kein Repo-Split.** `src/backends/opengl/` bleibt im Monorepo. Phase 2 + 3 aus dem v2-Architektur-Doc sind separate Arbeiten.
- **Kein API-Bruch.** PHP-API und interne C-API (außer Vtable-Erweiterungen) unverändert.
- **Kein Backend-Loader via dlopen.** Statisch gelinkt bleibt statisch gelinkt.
- **Keine neuen Backends.** WebGPU, Software-Renderer etc. außerhalb Scope.
- **Keine Zend-Trennung.** Phase-1-Hälfte „src/ Zend-frei" ist orthogonal und kann separat laufen.
- **Kein CMake.** Phase 1.5 aus dem v2-Doc bleibt unberührt.
- **Keine Aufräumarbeit an `tests/*.sh`-Artefakten.** Separates Cleanup-Ticket.
- **Keine Audio/Recorder/Streaming/Input-Änderungen.** Nur Frame-Capture-Read-Pixels-Pfade in `php_vio.c` ändern sich indirekt durch Etappe 5f.

---

## 8. Reihenfolge & Parallelisierung

```
Phase A — Vorbereitung (sequenziell vor allem anderen):
  Etappe 0: Static-Build-Fix (#2)               4-6h
    ↓
Phase B — Mechanische Vorbereitung (parallel):
  Etappe 1: Destructors → Vtable                3h
  Etappe 2: Capability-Enum + Extensions (#3)   3h
            ↓ ↓
Phase C — Backend-Code-Lift (parallel):
  Etappe 3: vio_2d.c → vio_2d_opengl.c          4-6h
  Etappe 4: Vtable-Erweiterung + Backends       8-10h
            ↓ ↓
Phase D — php_vio.c Cluster-Lift (16-22h):
  5a Frame-Lifecycle  →  Voraussetzung für b-g
  5b Mesh        ┐
  5c Pipeline    ┼─ parallelisierbar
  5d Uniforms    ┘
  5e RT/Cubemap/Font (inkl. neue API #4)
  5f Read-back
  5g Instancing
    ↓
Phase E — Abschluss:
  Etappe 6: Audit-Gate + Cleanup                2h
  Etappe 7: vio_gl_info + Shader-Check (#3)     3-4h
```

**Gesamtaufwand grob:** 43-57 Entwicklerstunden. Verteilt über 1.5-2.5 Wochen mit Test-Cycles realistisch.

---

## 9. Tracking

| Issue | Schließt durch | Sub-Etappen |
|---|---|---|
| [#2](https://github.com/phpolygon/php-vio/issues/2) | Etappe 0 | – |
| [#3](https://github.com/phpolygon/php-vio/issues/3) | Etappe 2 (Part 1+2), Etappe 7 (Part 3) | Caps, gl_info, Shader-Version-Check |
| [#4](https://github.com/phpolygon/php-vio/issues/4) | Etappe 5e + Etappe 7 (Dokumentation) | RT-Stack, neue API-Funktionen |

PR-Body referenziert: `Closes #2, Closes #3, Closes #4`.

---

*Plan ist lebendig — Anpassungen während der Implementierung via Commit zu dieser Datei mit Begründung im Message.*
