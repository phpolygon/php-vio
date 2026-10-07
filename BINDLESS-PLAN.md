# BINDLESS-PLAN — eine Texturtabelle je Kontext (SHADER-FEATURES-PLAN Phase 4)

Stand 2026-10-07. Ziel: Materialsysteme, die pro Instanz/Draw einen Texturindex wählen, statt Texturen
umzubinden. GLSL bleibt die Quelle; jedes Backend bekommt dieselbe Semantik.

## API (Phase 4a, umgesetzt)

```php
$i = vio_texture_index($ctx, $tex);   // int: stabiler Slot in der Kontext-Tabelle (0..1023)
```

- Erster Aufruf trägt die Textur in die Tabelle des Kontexts ein und gibt ihren Slot zurück; jeder weitere
  Aufruf für dieselbe Textur liefert denselben Slot. Die Tabelle **hält eine Referenz**: eine eingetragene
  Textur lebt bis `vio_destroy`, damit kein Slot je auf freigegebenen Speicher zeigt (kein Wiederverwenden
  von Slots in 4a). Ohne `VIO_FEATURE_BINDLESS` oder bei voller Tabelle: Warning + `false`.
- Nur 2D-Texturen (keine Arrays, 3D, Cubemaps, Render-Target-Wrapper) in 4a.

GLSL-Vertrag (fest, alle Backends):

```glsl
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 1, binding = 0) uniform texture2D vio_textures[];
layout(set = 1, binding = 1) uniform sampler   vio_sampler;     // linear, repeat
layout(set = 1, binding = 2) uniform sampler   vio_sampler_nearest;         // 4b: nearest, repeat
layout(set = 1, binding = 3) uniform sampler   vio_sampler_clamp;           // 4b: linear, clamp
layout(set = 1, binding = 4) uniform sampler   vio_sampler_nearest_clamp;   // 4b: nearest, clamp
layout(set = 1, binding = 5) uniform textureCube    vio_cubes[];            // 4b: VioCubemap-Slots
layout(set = 1, binding = 6) uniform texture2DArray vio_texture_arrays[];   // 4b: Slots von Array-Texturen
... texture(sampler2D(vio_textures[nonuniformEXT(i)], vio_sampler), uv)
```

Set 1 ist für die Tabelle reserviert; die normale Umnummerierung (Set 0) fasst es nicht an. Getrennte
Texturen + Sampler, weil SPIRV-Cross-MSL kombinierte `sampler2D[]` mit Laufzeitgröße nicht kann.

## Backends

| Backend | Weg | Gate |
|---|---|---|
| D3D12 | Root-Parameter [15]: Deskriptortabelle, unbegrenzter SRV-Bereich `t0, space1`; statischer Sampler `s1, space1`. 1024 Deskriptoren am oberen Ende des shader-sichtbaren SRV-Heaps fest reserviert, Eintrag per `CopyDescriptorsSimple` aus dem Staging-SRV der Textur. Läuft mit FXC 5.1 und DXC. | `ResourceBindingTier >= 2` |
| Vulkan | Globales Set-Layout (Set 1): Binding 0 `SAMPLED_IMAGE` × 1024 (`PARTIALLY_BOUND`, `UPDATE_AFTER_BIND`), Binding 1 unveränderlicher Sampler; ein Set aus einem Update-after-bind-Pool; Pipeline-Layouts bekommen Set 1, wenn der Shader es nutzt. | Descriptor-Indexing-Features (`runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingPartiallyBound`, `descriptorBindingSampledImageUpdateAfterBind`) |
| Metal | SPIRV-Cross-Argument-Buffer nur für Set 1 (Set 0 bleibt diskret), `device`-Adressraum, `[[buffer(21)]]`; Einträge sind `gpuResourceID`s in einem Shared-Buffer, `useResources` je Draw; `vio_sampler` wird ein `constexpr sampler`. | Metal3 + Argument Buffers Tier 2 + MSL ≥ 3.0 |
| OpenGL | — GLSL für GL kennt keine getrennten Texturen; `ARB_bindless_texture` bräuchte eigene Handles und ist nicht überall vorhanden. | 0 |
| D3D11 | — keine unbegrenzten Tabellen. | 0 |

Test 161: 64 einfarbige Texturen, ein Quad je Textur, Index pro Instanz → nicht-uniformer Index im
Fragment-Shader; dazu der Vertrag (Slot stabil, Referenz hält die Textur, ohne Flag abgelehnt).

## Phase 4b

- ✅ Freigabe von Slots (`vio_texture_release_index`, Test 176): der Slot bleibt `VIO_BINDLESS_RETIRE_FRAMES` (4)
  `vio_begin` lang reserviert, dann schreibt das Backend einen leeren Eintrag (D3D12 Null-SRV, Vulkan 1×1-Dummy,
  Metal Null-Resource-ID) und der Slot wird neu vergeben.
- ✅ Samplerwahl (Test 177): Set 1 Binding 2–4 sind feste Varianten (nearest / clamp / nearest + clamp); der
  Shader wählt je Zugriff. D3D12 statische Sampler `s1`–`s4, space1`, Vulkan unveränderliche Sampler im Set-1-
  Layout, Metal `constexpr`-Sampler je Binding.
- ✅ Cubemaps und Texture-Arrays (Test 178): `vio_texture_index` nimmt `VioCubemap` und Array-Texturen in denselben
  Slot-Raum; der Shader liest einen Slot über das Array seiner Art. D3D12: dieselbe Tabelle zusätzlich als
  `TextureCube[]` (`t0, space3`) und `Texture2DArray[]` (`t0, space4`) — drei unbegrenzte Bereiche mit Offset 0;
  Vulkan: Bindings 5/6 im Set-1-Layout; Metal: die MSL-Übersetzung legt sie in Set 2/3, je ein Argument-Buffer
  (`[[buffer(17)]]` / `[[buffer(18)]]`), weil unbegrenzte Arrays sich keinen Argument-Buffer teilen können.
- ✅ Compute-Stages (Test 179, synchron und async im Frame): D3D12 spiegelt die Tabelle über eine CPU-Kopie
  (`bindless_cpu_heap`) in einen Block hinter den Dispatch-Blöcken des Compute-Heaps, Kernels mit Set 1 bekommen
  Root-Tabelle [4] und die vier statischen Sampler; Vulkan hängt das Set-1-Layout an das Compute-Pipeline-Layout
  (Stage-Flags jetzt mit COMPUTE); Metal teilt den Bindless-Teil der MSL-Übersetzung (`metal_msl_bindless`) mit
  den Grafik-Stages und bindet die Puffer am Compute-Encoder.

Offen (Metal 4, nicht ohne neuere macOS-Runner prüfbar): Samplerwahl je Eintrag, Arrays/Cubes, Compute-Stages,
Vertex-Stage-Zugriff auf Metal ohne `useResources`-Kosten (Residency Sets, Metal 4).
