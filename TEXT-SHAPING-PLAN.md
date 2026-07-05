# Text-Shaping-Plan (Weg A) — HarfBuzz + SheenBidi

Ziel: korrektes Rendering der bislang fehlenden Steam-Skripte, allen voran
**Arabisch** (RTL + Joining + Ligaturen) und **Thai** (komplexes Clustering,
Mark-Positioning). Erfordert echtes Shaping *und* BiDi — die Vollausbaustufe.

## Kernentscheidungen

1. **Atlas: statisch-codepoint → lazy-glyph-index.** HarfBuzz liefert Glyph-IDs
   (inkl. Ligaturen/Positionsformen), die kein Codepoint adressiert. Der
   `stbtt_PackFontRanges`-Vorabpack kann sie nicht halten. Neuer Ansatz:
   Glyphen werden bei erster Verwendung einzeln gerastert (`stbtt_MakeGlyphBitmap`)
   und via `stb_rect_pack` (Shelf/Skyline) in einen wachsenden Atlas gelegt.
   `glyph_map` wird nach **Glyph-ID** gekeyt statt nach Codepoint.

2. **Dependency-Split.**
   - **HarfBuzz** = System-/Precompiled-Lib (`--with-harfbuzz`), zu groß zum
     Vendorn. C-API (`hb.h`), C-linkbar; `stdc++` linken wir bereits.
   - **SheenBidi** = **vendored** in `vendor/sheenbidi/` (Apache-2.0, reines C,
     Single-Amalgam `Source/SheenBidi.c`). Passt zum stb/miniaudio-Muster.

3. **Additiv & gegated.** Alles hinter `#ifdef HAVE_HARFBUZZ`. Ohne HarfBuzz
   bleibt der heutige Codepoint-Pfad **byte-identisch** — CI/Builds ohne die
   Dependency bleiben grün. Mit HarfBuzz läuft *aller* Text durch Shaping
   (auch Latin — bekommt dann gratis Kerning/Ligaturen).

4. **Atlas-Upload: build-once, stabiler Handle.** (Revidiert gegenüber dem
   ursprünglichen „lazy re-upload"-Plan.) Beim Durchdenken zeigte sich: ein
   Mid-Frame-Recreate der Atlas-Textur ist auf deferred Backends (D3D12/Vulkan)
   **gefährlich** (Destroy einer noch in-flight referenzierten Textur) und bricht
   zudem früher gepushte 2D-Items (die den Textur-Handle beim Push kopieren).
   Lösung: **alle Glyphen des Fonts einmal bei `vio_font()` packen** (keyed nach
   Glyph-Index, `stb_rect_pack`) und **einmal** hochladen — exakt der bestehende
   Single-Upload-at-Creation-Pfad. Danach ist der Handle für die Font-Lebenszeit
   stabil; kein Re-Upload, keine Backend-Änderung, überall korrekt. Ligaturen/
   Positionsformen sind enthalten, weil sie Glyph-Indizes haben (nur keine
   Codepoints). Gleicher Tradeoff wie heute (die CJK/Hangul-Ranges werden auch
   jetzt schon vorab gepackt).

## Neue/geänderte Font-State (`vio_font.h`)

```c
/* HarfBuzz (gegated) */
hb_blob_t *hb_blob;  hb_face_t *hb_face;  hb_font_t *hb_font;
/* Lazy-Raster */
stbtt_fontinfo   info;          /* eigenes fontinfo für Einzelglyph-Raster */
unsigned char   *atlas_cpu;     /* CPU-Kopie, für Re-Upload beim Wachsen */
stbrp_context    packer;  stbrp_node *packer_nodes;
int              atlas_dirty;   /* seit letztem Upload verändert? */
/* glyph_map: jetzt glyph_id -> { atlas-rect, bearing_x/y, w, h } */
```
`ttf_data` bleibt (Backing für `hb_blob` + `stbtt_fontinfo`). `xadvance` aus
`packedchar` entfällt — Advances kommen aus HarfBuzz.

## Shaping-Pipeline (neue Datei `src/vio_text_shape.c`)

Input: UTF-8. Output: Liste positionierter Glyphen `{glyph_id, x, y}` in
Pixeln, plus Gesamt-Advance/Extents für `vio_text_measure`.

1. **BiDi (SheenBidi):** `SBAlgorithm` → `SBParagraph` (base direction auto) →
   resolved levels je Zeichen.
2. **Itemisierung:** in Runs gleichen BiDi-Levels schneiden; Richtung = Level
   gerade→LTR / ungerade→RTL. Script/Language pro Run via
   `hb_buffer_guess_segment_properties()` (v1; feinere Script-Grenzen innerhalb
   eines Levels = spätere Verfeinerung).
3. **Reorder:** Runs nach BiDi in **visuelle** Reihenfolge bringen.
4. **Shape je Run:** `hb_buffer_add_utf8` → Direction/Script/Language setzen →
   `hb_shape(hb_font, buf, NULL, 0)`. `hb_font_set_scale(size*64, size*64)`,
   Positionen sind 26.6 → `/64.0`. Aus `glyph_infos` (id, cluster) +
   `glyph_positions` (x/y_advance, x/y_offset) den Pen fortschreiben.
5. **Emit:** je Glyph `atlas_ensure_glyph(font, glyph_id)` (lazy raster) →
   Quad aus Pen + `x_offset/y_offset` + Bearing/Atlas-Rect → in den bestehenden
   `vio_2d`-Batch pushen (`VIO_2D_TEXT`). Transform-Stack wie heute anwenden.

## Phasen

- **P0 — Build.** `--with-harfbuzz` in `config.m4` (pkg-config `harfbuzz` +
  Fallback-Pfade, `HAVE_HARFBUZZ`); `vendor/sheenbidi/` droppen + `SheenBidi.c`
  in `PHP_NEW_EXTENSION`, Include-Dir + Build-Dir. `stb_rect_pack.h` vendoren.
  Windows: HarfBuzz-Lib nach `deps/`. → Build ohne Codeänderung grün.
- **P1 — Atlas-Umbau.** Lazy-Raster + `stb_rect_pack` + glyph-id-Keying in
  `vio_font.c`; `hb_font`-Aufbau in `vio_font()`; Free-Pfad
  (`hb_*_destroy`, `packer_nodes`, `atlas_cpu`). Async-Pack-Pfad anpassen
  (Init des leeren Atlas statt Vorabpack). Legacy-Pack bleibt im `#else`.
- **P2 — Shaping-Stage.** `vio_text_shape.c` (BiDi + Itemize + Shape + Reorder),
  reine Datenausgabe, ohne GL.
- **P3 — Verdrahtung.** `vio_text` / `vio_text_measure` auf die Stage umstellen
  (unter `#ifdef HAVE_HARFBUZZ`, sonst heutiger Code). Dirty-Atlas-Re-Upload
  vor Flush.
- **P4 — Tests.** PHPT: Arabisch (RTL-Reihenfolge, Joining-Formen ≠ isoliert),
  Thai-Cluster, Latin-Ligatur (`fi`), BiDi-Mixed (LTR+RTL im selben String).
  Headless-VRT-Screenshot gegen Referenz. Skip, wenn `HAVE_HARFBUZZ` fehlt.
  Regression: bestehende Font-Tests müssen grün bleiben.
- **P5 — Doku.** `CLAUDE.md` (Font-Abschnitt + Dependency-Tabelle + config-Flag),
  `vio.stub.php` unverändert (API-kompatibel), README-Notiz.

## Status (Stand dieser Session)

- **P0 Build** ✅ — SheenBidi + `stb_rect_pack` vendored, `--with-harfbuzz` in
  `config.m4` + `config.w32`, HarfBuzz 14.2.1 via vcpkg unter
  `C:\vcpkg\installed\x64-windows` gebaut.
- **P1 Atlas** ✅ — glyph-index build-once Atlas in `vio_text_shape.c`, Font-State
  in `vio_font.h/.c`.
- **P2 Shaping** ✅ — BiDi (SheenBidi) + Itemize + Shape (HarfBuzz) + Emit.
- **P3 Verdrahtung** ✅ — `vio_font()`/`vio_text()`/`vio_text_measure()` gated,
  Konstante `VIO_HAS_SHAPING`.
- **P4 Tests** ✅ — `081_text_shaping.phpt`, `082_text_bidi_arabic.phpt`.
- **P5 Doku** ✅ — CLAUDE.md.
- **Build + Verifikation** ✅ — gebaut mit VS 2022 Pro (MSVC **14.44**, passend
  zum PHP-8.5.5-Core; VS18/14.50 erzeugt eine nicht-ladbare DLL) via
  phpize+configure+nmake, HarfBuzz aus vcpkg. DLL lädt, `VIO_HAS_SHAPING==1`.
  Headless-Smoke: Latin + Arabisch „مرحبا" (Advance 52.8, Joining greift) +
  gemischter BiDi-String rendern korrekt. **Testsuite: 77 pass, 6 skip, 2 fail**
  — beide Fails vorbestehend/umgebungsbedingt (070 Audit-Gate flaggt
  OpenGL-Backend-Dateien wegen Windows-`\`-Pfaden; 051 D3D12-Debug-Layer-Output),
  **nicht** durch diese Änderung. Neue Tests 081/082 grün, Font-Regression
  (030/055) grün.

### Windows-Build-Rezept (verifiziert)
```
call "…\VS 2022\Professional\VC\Auxiliary\Build\vcvars64.bat"   :: 14.44, NICHT VS18/14.50
set PATH=C:\php-sdk\phpmaster\msys2\usr\bin;%PATH%
set _CL_=/FS /MP1
cd D:\PhpstormProjects\php-vio
call "C:\php-sdk\php-8.5.5-devel-vs17-x64\phpize.bat"
configure --enable-vio=shared --with-php-build=C:\php-sdk\php-8.5.5-devel-vs17-x64 ^
  --with-glfw=C:\php-sdk\vio-build-deps --with-glslang=C:\php-sdk\vio-build-deps ^
  --with-spirv-cross=C:\php-sdk\vio-build-deps --with-vulkan=C:\php-sdk\vio-build-deps ^
  --with-ffmpeg=C:\php-sdk\vio-build-deps --with-d3d11=yes --with-d3d12=yes ^
  --with-harfbuzz=C:\vcpkg\installed\x64-windows
nmake
:: Laufzeit: C:\vcpkg\installed\x64-windows\bin (harfbuzz.dll + freetype/brotli/…) auf PATH
```

## Risiken / offene Punkte

- **HarfBuzz auf Windows/CI:** Precompiled-Lib beschaffen; ohne sie greift der
  `#else`-Fallback (kein Shaping, aber Build grün).
- **Atlas-Wachstum:** v1 volles Re-Upload pro Dirty-Frame — bei vielen neuen
  Glyphen/Frame teuer. Später Sub-Image-Upload-Vtable.
- **Zeilenumbruch/Wrapping:** ✅ implementiert — harte `\n`-Umbrüche immer,
  weicher Wortumbruch via `['max_width' => px]`, `line_height`-Override,
  BiDi-korrekt (Umbruch auf logischem Text, dann pro Zeile reordern).
  `vio_text_measure(..., $opts)` liefert zusätzlich `lines`. Umbruchgelegenheiten
  über kompakten **UAX-#14-lite**-Klassifikator (`lb_class`/`lb_break_between`):
  Whitespace (Latin), zwischen Ideogrammen (**CJK**, mit Basis-Kinsoku) und an
  **Thai**-Cluster-Grenzen. Thai wörterbuchfrei → Cluster- statt echte
  Wortgrenzen; Überlangsegment läuft über. Tests: `083_text_wrapping.phpt`,
  `084_text_wrap_cjk_thai.phpt`. **Vertikaltext (CJK vertical):** außerhalb Scope.
- **`render_scale`/devicePixelRatio:** HarfBuzz-Scale = `font_size` (bereits
  `size*scale`); die `inv_rs`-Division in `vio_text` bleibt analog erhalten.
