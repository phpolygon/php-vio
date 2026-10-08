# SM69-PLAN — Shader Model 6.9 vollständig, Windows-CI auf aktuellem WARP

Stand 2026-10-08. Ziel: alles, was das **finale** Shader Model 6.9 (Agility SDK 1.619, DXC 1.9.2602) bringt, als
portable vio-Features – GLSL bleibt die Quelle, wo es eine GLSL-Form gibt – und ein CI-Beleg dafür, der über
SM 6.2 hinausgeht. Fortsetzung von `SHADER-FEATURES-PLAN.md` (Phasen 6c, 8b).

## Was zu SM 6.9 gehört (Retail)

| Feature | HLSL | D3D12-Gate | Vulkan | Metal / GL |
|---|---|---|---|---|
| Shader Execution Reordering (DXR 1.2) | `dx::HitObject`, `dx::MaybeReorderThread` (Raygen) | SM 6.9 + `OPTIONS5.RaytracingTier ≥ 1_2` | `VK_EXT_ray_tracing_invocation_reorder` (NV-Vorläufer), GLSL `GL_EXT_shader_invocation_reorder` (`hitObjectEXT`, `reorderThreadEXT`) | – |
| Opacity Micromaps (DXR 1.2) | `RAY_FLAG_FORCE_OMM_2_STATE`, `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`, Pipeline-Flag `ALLOW_OPACITY_MICROMAPS` | `RaytracingTier ≥ 1_2` | `VK_EXT_opacity_micromap`, GLSL `GL_EXT_opacity_micromap` | – |
| Long Vectors | `vector<T, N>` mit 5 ≤ N ≤ 1024 | SM 6.9 | `VK_EXT_shader_long_vector`, GLSL `GL_EXT_long_vector` (SPV_EXT_long_vector) | – (Metal/GL: max. 4 Komponenten) |
| 16-Bit-Sonderwerte | `isnan/isinf/isfinite` für `half`, neu `isnormal` | SM 6.9 + 16-Bit-Ops | – (Vulkan rechnet ohnehin korrekt) | – |
| Pflicht-Caps | Wave-Ops, Int64, native 16-Bit werden für 6.9 verbindlich | – | – | – |

**Nicht** in 6.9: Cooperative Vectors – im Retail zugunsten der SM-6.10-Linearalgebra (`dx::linalg`) gestrichen,
die eigener Plan ist (`SPIRV-CROSS-HLSL-COOPMAT-PLAN.md`).

## Phase 0 — Beleg-Basis: Agility SDK + WARP aus NuGet + DXC 1.9 (M)

Die Windows-CI rendert auf dem WARP des Runner-Betriebssystems (SM 6.2) – alles darüber ist dort nur
compile-geprüft. Microsoft liefert WARP als NuGet-Paket `Microsoft.Direct3D.WARP`; ab 1.0.18 mit SM 6.9, DXR 1.2
(SER, OMM), Long Vectors. Damit wird die CI zum echten Beleg für die D3D12-Features oberhalb von 6.2.

- **0a** `VIO_D3D12_AGILITY_SDK` / `VIO_D3D12_AGILITY_SDK_VERSION`: Umgebungs-Defaults für die `vio_create`-Option
  `agility_sdk` – damit läuft die **ganze Suite** auf der Agility-Runtime, nicht nur Tests, die die Option setzen.
- **0b** `VIO_D3D12_WARP=<pfad zu d3d10warp.dll>`: vio lädt diese WARP-DLL vor dem ersten Device
  (`LoadLibraryW`), wie Microsoft es für das NuGet-WARP vorsieht – kein Kopieren neben `php.exe`.
  `vio_gpu_info()` / `vio_backend_info()` melden die WARP-Version.
- **0c** Messen: welche `VIO_FEATURE_*` meldet WARP 1.0.2x unter Agility 1.619 + DXC 1.9 (lokal und in der CI)?
  Daraus die `VIO_REQUIRE_*`-Liste des neuen CI-Laufs.
- **0d** Windows-CI: zweiter D3D-Durchlauf „D3D12 auf NuGet-WARP + Agility 1.619 + DXC 1.9“ über alle
  D3D12-Feature-Tests mit `VIO_REQUIRE_*` für alles, was 0c misst. Der bestehende Lauf auf dem System-WARP
  bleibt (Untergrenze SM 6.2).
- **0e** Lokal (`fullrun.bat`): RTX-Lauf zusätzlich mit Agility 1.619 (bzw. 1.721-preview) und den neuen
  `VIO_REQUIRE_*`.

## Phase 1 — Profil SM 6.9 und 16-Bit-Sonderwerte (S)

- Probe und `shader_model => 69` gibt es schon (Probe startet bei 6.9); prüfen, dass DXC 1.9 unter 6.9 die
  SPIRV-Cross-Ausgabe annimmt (SPIRV-Cross kennt Profile bis 6.8 – die HLSL bleibt 6.8-Syntax, DXC kompiliert
  sie als `x_6_9`).
- Test: `isnan/isinf/isfinite` auf `float16_t` (GLSL) liefern für NaN/±Inf/Normale/Denormale die richtigen
  Werte auf jedem Backend mit `SHADER_FLOAT16`; `isnormal` nur über den HLSL-Override (GLSL hat es nicht).

## Phase 2 — Long Vectors `VIO_FEATURE_LONG_VECTOR` (M)

- Quelle: GLSL `GL_EXT_long_vector` (`vector<float, 8>`-Äquivalent) im Compute-Kernel; glslang des Vulkan SDK
  1.4.341 prüfen.
- Vulkan: `VK_EXT_shader_long_vector` + Feature-Bit; Kernel läuft unverändert.
- D3D12: SPIRV-Cross übersetzt SPIR-V-Vektoren > 4 nicht nach HLSL ⇒ entweder SPIRV-Cross-Patch (Fork-Regeln:
  Hendrik entscheidet und sieht jeden Diff) oder der HLSL-Override `vio_compute_pipeline(['hlsl' => …])`
  (Gegenstück zu `'msl'`, A17). Entscheidung nach einem Prototyp.
- Metal/GL: Flag 0. Test: 16er-Vektor laden, elementweise rechnen, speichern; Ergebnis wie CPU.

## Phase 3 — Shader Execution Reordering `VIO_FEATURE_SHADER_EXECUTION_REORDER` (M)

- D3D12: RT-Bibliotheken unter `lib_6_9`, wenn das Flag gilt; HLSL-Raygen nutzt `dx::HitObject::TraceRay`,
  `dx::MaybeReorderThread(hit, hint, bits)`, `HitObject::Invoke`. Kein neuer API-Einstieg – das Flag sagt, dass
  die Bibliothek SER nutzen darf.
- Vulkan: `VK_EXT_ray_tracing_invocation_reorder` (Fallback NV) im Device, GLSL-Raygen mit
  `GL_EXT_shader_invocation_reorder`.
- Test: Raygen über HitObject + Reorder schreibt dasselbe Bild wie der gewöhnliche `TraceRay` (Test 164-Szene);
  der Reorder-Hinweis darf das Ergebnis nicht ändern.

## Phase 4 — Opacity Micromaps `VIO_FEATURE_OPACITY_MICROMAP` (L)

- API: `vio_acceleration_structure`-Meshes bekommen `'opacity_micromap' => ['subdivision' => L,
  'format' => VIO_OMM_2_STATE | VIO_OMM_4_STATE, 'states' => string]` (Zustände je Mikrodreieck, 2 bzw. 4 Bit,
  Dreiecke in Index-Reihenfolge). Zustände: transparent, opak, unbekannt-transparent, unbekannt-opak.
- D3D12: OMM-Array bauen (`D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY`), Geometrie als
  `OMM_TRIANGLES` mit Verweis, Pipeline-Flag `ALLOW_OPACITY_MICROMAPS`; RayQuery mit
  `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`.
- Vulkan: `VK_EXT_opacity_micromap` (`vkBuildMicromapsEXT`, Triangles-Opacity-Micromap im pNext der Geometrie).
- Test: ein Quad, dessen OMM die linke Hälfte transparent setzt – ohne Any-Hit sieht ein Ray Query (163) bzw.
  der Trace (164) dort durch, rechts trifft er; Unbekannt-Zustände rufen den Any-Hit auf.

## Test- und Gate-Kontrakt

- Je Feature `tests/render3d/NNN_*_all_backends.phpt`, `skip (no …)` wo das Flag 0 ist; Flag in `074`/`171`.
- `VIO_REQUIRE_<FEATURE>=d3d12` im NuGet-WARP-Lauf der CI, `=d3d12,vulkan` lokal (RTX: SER/OMM nur, wenn der
  Treiber Tier 1.2 meldet – sonst ehrlich 0 und lokal nur WARP).
- Audit-Gate 099 bleibt: neue Fähigkeiten über Vtable-Slots und Feature-Flags.
