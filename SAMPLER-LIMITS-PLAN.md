# SAMPLER-LIMITS-PLAN – Backend-Grenzen statt eigener Textur-/Sampler-Grenzen

Stand: 2026-11-07, Branch `feat/backend-sampler-limits`. Phasen 1–6 umgesetzt (Suite grün, Test 257 erweitert, Test 277 neu); Phase 7 (Metal Argument Buffers) offen. Befunde: FXC begrenzt Sampler-Register auf 16 (D3D12 ohne Shader Model 6), WARP stürzt ab 129 Texturen pro Stufe ab (ohne Debug-Layer) und bekommt 128.

## Ziel

php-vio setzt keine eigenen Grenzen mehr für Textur-Units und Sampler. Jedes Backend meldet,
was Treiber und Grafikkarte im genutzten Binding-Modell pro Shader-Stufe erlauben, und php-vio
richtet Register, Tabellen, Deskriptor-Sets und PHP-Prüfungen danach aus. Die feste Aufteilung
in 12 normale und 4 Vergleichs-Sampler fällt weg: Jede Unit kann normal oder vergleichend sampeln.

## Ist-Zustand (feste Grenzen)

| Stelle | Wert | Wirkung |
|---|---|---|
| `src/vio_shader.h` `VIO_MAX_SAMPLERS` | 16 | Sampler-Tabellen je Shader (Namen, Register, Units) |
| `src/vio_shader_reflect.h` `VIO_SHADOW_SAMPLER_BASE/END` | 12 / 16 | Register-Split normal/Vergleich, D3D12 statische Vergleichs-Sampler s12..s15 |
| `src/vio_shader_reflect.h` `VIO_SAMPLER_PLAN_MAX` | 32 | shaderweiter Sampler-Plan nach Namen |
| `src/vio_context.h` `pending_tex_obj/kind[16]` | 16 | gebundene Texturen je Unit |
| `src/backends/d3d12/vio_d3d12.h` `VIO_D3D12_SRV_TABLE_SIZE` | 16 | SRV-Tabelle der Root Signature, Pending-/Flush-Arrays |
| `src/backends/d3d12/vio_d3d12.h` `VIO_D3D12_SAMPLER_TABLE_SIZE` | 12 | Sampler-Tabelle der Root Signature |
| `src/backends/vulkan/vio_vulkan_3d.h` `VK3D_MAX_SAMPLERS` | 16 | Bindings 2..17 in Set 0, Pool-Größen, Textur-Array |
| `php_vio.c` Unit-Prüfungen (`unit >= 16`, „must be 0..15“) | 16 | PHP-API |

Die Bindless-Tabelle (BINDLESS-PLAN) ist davon unberührt und bleibt, wie sie ist.

## Soll

### Grenzen je Backend

Neuer Vtable-Slot `texture_limits(int *textures, int *samplers)` (pro Stufe). Der Kontext merkt
sich `units = min(textures, samplers)`, weil die API Textur und Sampler pro Unit koppelt.

| Backend | Texturen pro Stufe | Sampler pro Stufe | Quelle |
|---|---|---|---|
| OpenGL | `GL_MAX_TEXTURE_IMAGE_UNITS` (Fragment), `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` | dieselbe Zahl (Unit = Textur + Sampler) | Treiberabfrage |
| D3D11 | `D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT` (128) | `D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT` (16) | Hardware-Modell |
| D3D12 | Tier 1: 128, Tier 2/3: Heap | Tier 1: 16, Tier 2/3: 2048 (shader-sichtbarer Sampler-Heap) | `D3D12_FEATURE_DATA_D3D12_OPTIONS.ResourceBindingTier` |
| Vulkan | `maxPerStageDescriptorSampledImages` | `maxPerStageDescriptorSamplers` | `VkPhysicalDeviceLimits`, zusätzlich `maxPerStageResources`, `maxDescriptorSet*` |
| Metal | 128 (Apple-GPU-Familien) bzw. 31 (ältere) | 16 (Argument-Tabelle ohne Argument Buffers) | `MTLDevice` / GPU-Familie |

Metal und D3D11 bleiben damit bei 16 Units – das ist dort die echte Grenze des Binding-Modells.
Metal-Argument-Buffers (Tier 2: 2048 Sampler) sind eine spätere, eigene Phase (siehe unten).

### Register

Ein Shader vergibt seine Register in einer Folge, normale und Vergleichs-Sampler gemischt.
Der Vergleich kommt aus dem Sampler-Zustand der Unit (Vergleichs-Kombination), nicht aus einem
festen Registerbereich. Alle Tabellen werden nach der tatsächlichen Sampler-Zahl des Shaders
bzw. der Backend-Grenze dynamisch angelegt.

### PHP-API

- Unit-Prüfungen gegen die Kontext-Grenze: `ValueError` „must be between 0 and N-1“.
- `vio_backend_info()['samplers']`: `total` = Units, `regular` = Units, `comparison` = Units
  (jede Unit kann vergleichen), neu `textures` und `sampler_states` (Rohwerte des Backends).
- Ein Shader mit mehr Samplern als die Grenze: Fehler beim Erzeugen mit klarer Meldung
  (heute werden überzählige Sampler stillschweigend abgeschnitten).

## Phasen

### Phase 1 – Kern dynamisch, Verhalten unverändert

- Vtable-Slot `texture_limits`, alle Backends melden zunächst 16 / 16.
- `vio_shader`: Sampler-Tabellen dynamisch (`emalloc` nach Reflexion), `VIO_MAX_SAMPLERS` entfällt.
- `vio_context`: `pending_tex_*` dynamisch nach Kontext-Grenze.
- Sampler-Plan (`vio_sampler_plan`) dynamisch, `VIO_SAMPLER_PLAN_MAX` entfällt.
- Unit-Prüfungen in `php_vio.c` gegen die Kontext-Grenze.
- Gate: volle Suite grün auf diesem Host (0 FAIL).

### Phase 2 – Vergleich als Sampler-Zustand, Split entfällt

- D3D12: Vergleichs-Kombinationen im Combo-Heap; statische Vergleichs-Sampler s12..s15 entfallen,
  Unit mit `is_depth` bekommt die Vergleichs-Kombination beim Flush.
- Vulkan: Vergleichs-Sampler-Objekt je Filter, Binding nach Plan statt `VIO_SHADOW_SAMPLER_BASE`.
- D3D11, Metal, OpenGL: Vergleichs-Zustand je Unit (bestehende Pfade prüfen, angleichen).
- `VIO_SHADOW_SAMPLER_BASE/END` entfallen; Register-Vergabe in `vio_shader_reflect.c`,
  `php_vio.c` (Replay) und `vio_vulkan_3d_shader.c` als eine Folge.
- Test 257 erweitert: beliebige Mischung (z. B. 2 normale + 14 Vergleichs-Sampler).

### Phase 3 – D3D12 echte Grenze

- `ResourceBindingTier` abfragen; Tier 2/3: SRV- und Sampler-Tabelle als unbegrenzte Bereiche
  (Root Signature 1.1, `DESCRIPTORS_VOLATILE`), Tier 1: 128 / 16.
- Pending-/Flush-Arrays und Sampler-Set-Cache nach Shader-Sampler-Zahl; der Ring kopiert nur die
  genutzten Einträge.

### Phase 4 – Vulkan echte Grenze

- Grenzen aus `VkPhysicalDeviceLimits`; Set-0-Layout je Pipeline mit genau so vielen
  Sampler-Bindings wie der Shader braucht; Pool-Größen und Textur-Array dynamisch.

### Phase 5 – OpenGL, D3D11, Metal

- OpenGL: Treiberwerte abfragen. D3D11: 128 / 16. Metal: 128 bzw. 31 / 16.

### Phase 6 – Tests, Doku

- Neuer Test: ein Fragment-Shader mit 40 Samplern (wo das Backend es kann; sonst erwarteter
  Fehler), gemischt normal/Vergleich, jede Spalte liest ihre eigene Textur.
- Test 260/259 auf Units über 15 erweitern, wo erlaubt.
- `tests/core/074` (Flags ehrlich), CLAUDE.md, `vio.stub.php`, Doku-Site.

### Phase 7 (später) – Metal Argument Buffers

- Tier 2 (Apple Silicon): Texturen und Sampler über Argument Buffers, Grenze 2048 Sampler.

## Test-Kontrakt

- Volle Suite auf diesem Host nach jeder Phase 0 FAIL; Skips nur Metal/macOS-Vulkan.
- CI (Linux Mesa, macOS Metal, Windows WARP) grün vor dem Merge.
- Audit-Gates 070/099 bleiben eingehalten: neue Fähigkeit nur als Vtable-Slot.
