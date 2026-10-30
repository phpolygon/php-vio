# AGILITY-1620-PLAN – D3D12 Agility SDK 1.620.1

Stand 2026-10-30. Ausgang: v2.37.0, lokal und CI auf Agility 1.619.6. Paket: `Microsoft.Direct3D.D3D12` 1.620.1
(`D3D12_SDK_VERSION` 620, Retail, DLLs `1.620.1.0.20261026.1`).

## Grundsätze

- Alles Neue ist optional: fehlt die Runtime (System-D3D12, älteres Agility) oder der Treiber, läuft vio wie bisher.
  Jede neue Fähigkeit wird zur Laufzeit erfragt (`CheckFeatureSupport`, `QueryInterface`), nie aus der SDK-Version geschlossen.
- Kein Feature ohne Nutzen für vio: Performance-Umbauten werden gemessen und nur behalten, wenn der Gewinn belegt ist.
- Tests laufen auf WARP (CI) **und** RTX 2080 (`VIO_D3D_HEADLESS_HARDWARE=1`, `-d vio.debug=1`). Was nur eines von beiden
  kann, wird im Ergebnis so benannt.

## Phase 0 – Header und Referenzlauf

- Die Windows-SDK-Header kennen die neuen Schnittstellen nicht. Die MIT-lizenzierten Header aus `build/native/include`
  (`LICENSE-CODE.txt`) kommen nach `vendor/agility/` (`d3d12.h`, `d3d12sdklayers.h`, `d3dcommon.h`, `dxgiformat.h`) – Vorbild `vendor/ffx-api`.
  Binärdateien (`D3D12Core.dll`, …) kommen nicht ins Repo.
- Die neuen IIDs fehlen in `dxguid.lib`: `vio_d3d12.c` definiert `INITGUID` und erzeugt sie selbst.
- Referenz: volle Suite v2.37.0 auf 1.619.6 und auf 1.620.1 (WARP + RTX 2080, Debug-Layer). Abweichungen sind Befunde, bevor irgendetwas umgebaut wird.
- CI: `build.yml` von 1.619.6 auf 1.620.1 heben.
- Referenz RTX 2080 (v2.37.0): 1.619.6 246 PASS / 9 SKIP / 0 FAIL. Auf 1.620.1 scheitern nur 051 und 236, beide funktional
  richtig: der Debug-Layer meldet neu `DEPRECATED_API` (id=989) und `DEPTH_STENCIL_FORMAT_UNUSED` (id=1477) auf stderr.
  Daraus: `d3d12_check_feature` über `CheckFeatureSupport1` mit der aktiven SDK-Version (begrenzt dadurch nichts), id=1477 in die Deny-Liste.

## Phase 1 – Opacity Micromaps in RayQuery unter SM 6.9

- `D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS` und
  `D3D12_STATE_OBJECT_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS` setzen, wenn der Kontext unter SM 6.9 läuft.
- Heute: `d3d12_hlsl_post` verlangt SM 6.9, sonst ignoriert eine Query die Micromaps (Test 220 D).
- Test: Teil D von 220 unter festgelegtem `shader_model => 66`, in Compute (E) und Fragment (F).
- Befund: Die RTX 2080 (Treiber 617.42) wertet die Micromap unter SM 6.6 auch ohne Flag aus. E/F belegen dort also nur, dass
  das Flag angenommen wird, nicht seine Wirkung.
- Das Flag hat keine Feature-Abfrage und ältere Runtimes kennen es nicht; hier entscheidet ausnahmsweise die aktive SDK-Version.
- Dazu, unabhängig von 1.620: `vio_opacity_micromap_bake` backt die Zustände aus einer Alpha-Textur (Test 252).

## Phase 2 – Diagnose

- `ID3D12DeviceDumpFile`: `GetDeviceErrorCode` im Device-Removed-Bericht neben DRED; `AddBlobToDumpFile` mit vio-Kontext (Backend-Version, letzte Marke).
- Vom Treiber gemeldete Meldungen in den bestehenden Debug-Ausgabepfad.
- `vio_backend_info()['caps']` nennt die erkannten 1.620-Fähigkeiten (`async_commands`, `fence_barriers`, `dump_file`). Jede neue Cap wird in Test 184 gepinnt.

## Phase 3 – Batched Async Commands

- `D3D12_FEATURE_ASYNC_COMMANDS` meldet `NATIVE`/`FALLBACK`; der Fallback wird über `D3D12_DEVICE_FLAG_ASYNC_COMMANDS_FALLBACK_ENABLED` bzw. `ID3D12AsyncCommandsFallback` eingeschaltet.
- Kandidat: `CopyBufferRegions` statt einzelner `CopyBufferRegion` im Upload-Batch (`ubatch_list`, Test 250).
- Maßstab: `examples/bench_update_buffer.php`, RTX 2080, Median. Ohne messbaren Gewinn bleibt es bei Erkennung und Cap.

## Phase 4 – Fence Barriers Tier 1

- `D3D12_FEATURE_FENCE_BARRIERS`, `SignalBarrier`/`WaitBarrier` auf `ID3D12GraphicsCommandList11`.
- Tier 1 kennt nur Signal und Wait auf Listen; ein vio-interner Einsatz muss einen konkreten Queue-Fence oder Wait ersetzen. Ohne solchen Fall bleibt es bei Erkennung und Cap.

## Nicht in diesem Plan

- **Advanced Shader Delivery** (Explicit PSDBs, State Object Database, Partial Graphics Programs): braucht ein Verteilmodell der Spiele, eigene Planung.
- **Shader Model 6.10**: Der Header nennt die Stufe, das Changelog nicht; die Probes bleiben bei `0x69`, bis eine Runtime sie meldet (`SPIRV-CROSS-HLSL-COOPMAT-PLAN.md`).
- **Video-Neuerungen**, DXCGC, GUID-Texture-Layouts: kein Nutzer in vio.

## Test-Kontrakt

- 0 FAIL auf 1.619.6 und 1.620.1, D3D12-Debug-Layer sauber.
- Audit-Gates 070, 099, 212, 246, 247 unverändert grün.
- Neue Tests ab der nächsten freien Nummer; `VIO_REQUIRE_*` nur für Fähigkeiten, die die jeweilige Umgebung wirklich hat.
