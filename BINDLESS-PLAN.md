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

## Phase 4b (offen)

Freigabe von Slots (`vio_texture_release_index`), Samplerwahl je Eintrag, Arrays/Cubes, Compute-Stages,
Vertex-Stage-Zugriff auf Metal ohne `useResources`-Kosten (Residency Sets, Metal 4).
