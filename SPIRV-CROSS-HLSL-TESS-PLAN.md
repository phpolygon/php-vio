# SPIRV-CROSS-HLSL-TESS-PLAN — Hull- und Domain-Shader im HLSL-Backend von SPIRV-Cross

Stand 2026-10-03. Ziel: GLSL-Tessellation (`tess_control` + `tess_eval`) läuft auf D3D11/D3D12 ohne
den HLSL-Override aus Test 140. Dafür bekommt SPIRV-Cross upstream (KhronosGroup/SPIRV-Cross) ein
HLSL-Backend für die Tessellations-Stages; vio übernimmt es, sobald es in einem SDK-Tag bzw. in
Homebrew steht, und überbrückt die Zeit mit einem selbst gebauten Stand in `vio-build-deps`.

## Ausgangslage

- `CompilerHLSL` (`spirv_hlsl.cpp`, upstream `main` aa217ae) wirft für `TessellationControl` /
  `TessellationEvaluation` „Unsupported execution model" (`get_inner_entry_point_name`,
  Einstiegs-Signatur). Es gibt nur Splitter: `InvocationId` im TCS ist als
  `SV_OutputControlPointID`-Parameter (`uCPID`) vorgesehen, `fixup_implicit_builtin_block_names`
  kennt beide Modelle.
- Upstream-Issue #905 „HLSL: Implement hull shader" ist seit 2019 offen, ohne Diskussion, ohne PR.
  Niemand arbeitet daran.
- Seit #2603 (xen2, 2026) kann der HLSL-Geometry-Pfad `gl_in[].gl_Position` (`SV_Position`) und
  `gl_InvocationID` (`SV_GSInstanceID`). Das ist dieselbe Lücke, die vio mit `vio_gs_hlsl_rewrite`
  überbrückt; der Umbau bleibt nötig, bis die Windows-CI (Vulkan SDK 1.3.296) und die lokalen Deps
  (1.4.341) einen Stand mit #2603 haben, und ist danach ein No-op.
- Vorbild für Konventionen: das MSL-Backend. Es übersetzt TCS/TES schon (Kernel + Post-Tessellation-
  Vertex-Funktion) und erwartet, dass der Aufrufer fehlende Execution-Modes setzt: die TES bekommt
  `OutputVertices` aus der TCS, die TCS die Domain aus der TES (`set_execution_mode`). vio macht das
  für Metal bereits (`vio_metal_tess_reflect`); dasselbe Muster gilt hier.
- Beitragsregeln: Khronos-CLA und der Abschnitt „AI-Assisted Contributions" in `CONTRIBUTING.md`
  (seit aa217ae). Der Einreicher versichert, die KI-gestützten Teile ausreichend geprüft und
  gestaltet zu haben, um sie als eigene Arbeit zu vertreten. Deshalb: Claude schreibt zu,
  Hendrik prüft, gestaltet nach und reicht den PR selbst ein.

## Entscheidungen (Hendrik, 2026-10-03)

- Patch-Größe des Domain-Shaders kommt aus dem `OutputVertices`-Mode, den der Aufrufer auf der TES
  setzt (wie MSL) – keine neue Option.
- Ursprung: Option `tess_domain_origin_lower_left`; Standard ist Vulkan/D3D-Semantik. Umsetzung nach
  WARP-Messung (siehe „Domain-Ursprung“) anders als bei MSL: nur die Winding wird umgekehrt.
- Patch-Varyings bekommen die Semantik `PATCH<location>` (HS-Ausgabe und DS-Eingabe gleich).
- Arbeitsweise (KI-Richtlinie in `CONTRIBUTING.md`): Design-Entscheidungen trifft Hendrik; jeder
  Schritt ist ein kleiner Diff mit Erklärung, den Hendrik vor dem Commit prüft; gebaut und gegen
  FXC/DXC validiert wird lokal unter Windows; Commits und PR reicht Hendrik selbst ein.

### Hull-Shader (Hendrik, 2026-10-03)

- Patch-Constant-Funktion: gl_out aus dem `OutputPatch` vorbefüllen, dann `tesc_main()` in einer
  Schleife über **alle** Invocations (Patch-Writes jeder Invocation, Faktoren aus anderen
  Kontrollpunkten nach `barrier()`). Machbarkeitstest: handgeschriebenes HS in genau diesem Schema
  kompiliert mit FXC `hs_5_0` und DXC `hs_6_0` (`scratchpad/hs_spike.hlsl`).
- Eingangs-Patch-Größe: neue HLSL-Option `tess_input_control_points`, 0 = gleich `OutputVertices`.
- Domain / Partitioning / Winding: setzt der Aufrufer als Execution-Modes auf der TCS (wie MSL).
- Liest die Kontrollpunkt-Phase `gl_out[j != gl_InvocationID]`, ist das eine dokumentierte Grenze
  (kein Fehler); die Patch-Phase deckt den üblichen Fall ab.

## Zielbild in HLSL

### Domain-Shader (aus der TES)

```hlsl
struct SPIRV_Cross_Input        { float2 t_uv : TEXCOORD0; float4 gl_Position : SV_Position; };   // je Kontrollpunkt
struct SPIRV_Cross_PatchConstant {
    float edges[3]  : SV_TessFactor;          // tri 3, quad 4, isoline 2
    float inside    : SV_InsideTessFactor;    // tri 1, quad [2], isoline keins
    float4 p_col    : PATCH1;                 // patch-Varyings nach Location
};
[domain("tri")]
SPIRV_Cross_Output main(const OutputPatch<SPIRV_Cross_Input, 3> stage_input,
                        const SPIRV_Cross_PatchConstant patch,
                        float3 gl_TessCoord : SV_DomainLocation, uint gl_PrimitiveID : SV_PrimitiveID)
```

- `gl_TessCoord`: `float3` (tri) bzw. `float2` (quad/isoline, z = 0 aufgefüllt).
- `gl_TessLevelOuter/Inner` lesbar aus `patch.edges/inside` (in statics kopiert).
- Patch-Größe `N` = `OutputVertices`-Mode, den der Aufrufer auf der TES setzt (wie MSL); fehlt er,
  Exception mit klarer Meldung.
- Option `tess_domain_origin_lower_left`: wirkt im Domain-Shader nicht (siehe „Domain-Ursprung“).
- Clip-Space-Fixup (`fixup_depth_convention`) wie beim Vertex-Shader, weil der DS die letzte
  Position schreibende Stage vor dem Rasterizer sein kann.

### Hull-Shader (aus der TCS)

Die Schwierigkeit: GLSL hat **eine** TCS-Funktion für alle Invocations (mit `barrier()`), HLSL
trennt in Control-Point-Funktion und Patch-Constant-Funktion. Ansatz, passend zur bestehenden
HLSL-Architektur (Stage-I/O als `static`-Globals, ein Wrapper `main` kopiert):

```hlsl
static SPIRV_Cross_Input  gl_in[NIn];      // Eingangs-Patch
static SPIRV_Cross_Output gl_out[NOut];    // alle Kontrollpunkte
static float gl_TessLevelOuter[4]; static float gl_TessLevelInner[2];
static float4 p_col;                       // Patch-Outputs
static uint gl_InvocationID;

void tesc_main() { ... unveränderter Körper, barrier() entfällt ... }

SPIRV_Cross_PatchConstant tesc_patch(InputPatch<SPIRV_Cross_Input, NIn> ip,
                                     const OutputPatch<SPIRV_Cross_Output, NOut> op,
                                     uint gl_PrimitiveID : SV_PrimitiveID)
{
    copy ip -> gl_in; copy op -> gl_out;          // Lesezugriffe auf gl_out[j] sehen die echten Werte
    for (uint i = 0; i < NOut; i++) { gl_InvocationID = i; tesc_main(); }
    return { gl_TessLevelOuter / Inner -> SV_TessFactor / SV_InsideTessFactor, Patch-Outputs };
}

[domain("tri")] [partitioning("integer")] [outputtopology("triangle_cw")]
[outputcontrolpoints(NOut)] [patchconstantfunc("tesc_patch")]
SPIRV_Cross_Output main(InputPatch<SPIRV_Cross_Input, NIn> ip, uint uCPID : SV_OutputControlPointID,
                        uint gl_PrimitiveID : SV_PrimitiveID)
{
    copy ip -> gl_in; gl_InvocationID = uCPID; tesc_main(); return gl_out[uCPID];
}
```

- Der Körper wird **einmal** emittiert; beide Phasen rufen ihn auf. Die Control-Point-Phase
  verwirft Patch-Writes, die Patch-Phase verwirft (bzw. überschreibt identisch) Kontrollpunkt-Writes.
  Das ist Mesas Zerlegung (`dxil_nir_split_tess_ctrl`), nur auf Quelltextebene.
- Die Schleife über alle Invocations fängt Patch-Writes auch dann, wenn nicht Invocation 0 sie
  schreibt; mehrfaches Schreiben ist in GLSL ohnehin undefiniert („letzter gewinnt").
- Nicht abbildbar (Exception bzw. dokumentierte Grenze): Lesen von `gl_out[j != gl_InvocationID]`
  in der Control-Point-Phase, bevor `j` geschrieben hat – in GLSL nur nach `barrier()` erlaubt,
  in der CP-Phase von HLSL gar nicht. Die Patch-Phase deckt den üblichen Fall (Faktoren aus allen
  Kontrollpunkten) ab.
- Attribute: `domain`/`partitioning`/`outputtopology` aus den Execution-Modes (Triangles/Quads/
  Isolines, SpacingEqual → `integer`, FractionalEven/Odd → `fractional_even/odd`, VertexOrderCw/Ccw
  → `triangle_cw/ccw`, PointMode → `point`). Fehlen sie auf der TCS (GLSL deklariert sie auf der
  TES), muss der Aufrufer sie setzen – wie bei MSL.
- `NIn` (Größe des Eingangs-Patches, = Patch-Topologie im Input-Assembler) steht nicht in der SPIR-V
  (glslang dimensioniert `gl_in[]` auf `gl_MaxPatchVertices`). Neue Option
  `tess_input_control_points` (HLSL), Pflicht für TCS.
- `barrier()` (`OpControlBarrier`) wird in TCS nicht emittiert.
- `gl_PatchVerticesIn` = `NIn` als Konstante.

## Domain-Ursprung (gemessen 2026-10-03)

Auf WARP gegen das OpenGL-Backend gemessen (handgeschriebene HS/DS, Quad / Dreieck / Isolines,
Farbe kodiert Kanten-Marker und v, unabhängig von der Readback-Orientierung):

- D3D erzeugt **dieselben Domain-Koordinaten** wie OpenGL, und `SV_TessFactor[i]` unterteilt dieselbe
  Kante wie `gl_TessLevelOuter[i]` (Quad/Dreieck `[1]` ↔ v = 0; Isolines `[0]` = Linienzahl, Linien bei
  v = 0, ¼, …).
- Nur die **Winding** unterscheidet sich: GL `ccw` = HLSL `triangle_cw`, für Dreiecke **und** Quads.

GL-Semantik auf D3D heißt also: `outputtopology` umkehren, `gl_TessCoord` nie spiegeln. Das MSL-Backend
spiegelt v für Quads/Isolines (MoltenVK kehrt nur bei Dreiecken die Winding um); auf D3D würde das
`outer[1]`/`outer[3]` vertauschen. #2693/#2694 sind entsprechend korrigiert. Ob Metal wie D3D
parametrisiert (dann hätte vio-Metal mit der MSL-Option vertauschte Quad-Faktoren), ist offen und wird
mit Test 144 auf der macOS-CI gemessen.

## Phasen

| Phase | Inhalt | Wo | Aufwand | Beleg / Stand |
|---|---|---|---|---|
| 0 | SPIRV-Cross lokal bauen (CMake, VS 2022), `test_shaders.py` für `shaders-hlsl` grün, FXC/DXC als Validator einrichten | Fork `hlsl-tessellation` | S | ✅ Baseline 169/169, FXC/DXC aus Windows SDK 10.0.26100 |
| 1 | Domain-Shader: Einstieg, `OutputPatch`, Patch-Constant-Struct, `SV_DomainLocation`, Tess-Levels lesen, Origin-Option, Depth-Fixup | Fork | M | ✅ Commits 91201b06…6c275f1e; 4 TES-Referenzen, FXC `ds_5_1` + DXC `ds_6_0`; dazu GS-Fix `gl_in[i].gl_Position` (f522b9b3, eigener Branch) und eine Kopierschleife für GS/DS |
| 2 | Hull-Shader: Statics + einmaliger Körper, CP-Funktion, Patch-Constant-Funktion mit Invocation-Schleife, Attribute, `tess_input_control_points`, `barrier()` | Fork `hlsl-tessellation-hull` | L | ✅ H1–H7: 8 asm-Tests `shaders-hlsl/asm/tesc/` (normal + opt), FXC `hs_5_0/5_1` + DXC `hs_6_0`, HS+DS-Signaturen verglichen; Winding-Umkehr bei Lower-Left für Dreiecke und Quads |
| 3 | C-API: Option `SPVC_COMPILER_OPTION_HLSL_TESS_INPUT_CONTROL_POINTS`, `..._TESS_DOMAIN_ORIGIN_LOWER_LEFT` | Fork | S | ✅ 95 (Origin), 96 (Eingangs-Kontrollpunkte); API-Version bumpt upstream der Maintainer |
| 4 | Upstream-PRs mit Bezug auf #905 | KhronosGroup | – | ✅ eingereicht 2026-10-03: #2692 GS `gl_in`, #2693 Domain-Shader, #2694 Hull-Shader; CI grün (alle 13 Testvarianten, mit den CI-Werkzeugständen aus `checkout_glslang_spirv_tools.sh`); Review offen |
| 5 | vio-Integration, siehe unten (5a–5f) | php-vio | L | ✅ 5a/5c/5d/5f, 5e Vulkan: 110/135/144 auf D3D11/D3D12 (auch SM 6) ohne Override, 140 unverändert, volle Suite grün. Offen: 5b nativer Pfad, Metal-Messung (macOS-CI), D3D-Isolines |
| 6 | Fork-Stand in `C:\php-sdk\vio-build-deps` bauen, um den nativen Pfad (5b) lokal zu prüfen; die Windows-CI bleibt beim SDK-SPIRV-Cross und nutzt den Generator (5c) | php-vio | S | lokaler Lauf mit nativem Pfad |

## vio-Integration (Phase 5)

Zwei Wege zum selben HLSL-Vertrag, gewählt beim Build bzw. zur Laufzeit:

- **Nativ** (SPIRV-Cross mit #2693/#2694): `config.m4`/`config.w32` prüfen, ob `spirv_cross_c.h`
  `SPVC_COMPILER_OPTION_HLSL_TESS_INPUT_CONTROL_POINTS` deklariert, und setzen dann
  `HAVE_SPVC_HLSL_TESS`. Nur dann werden die Enum-Namen benutzt; damit hängt vio nicht an Nummern,
  die der Maintainer beim Merge noch ändern kann.
- **Generator** (jedes andere SPIRV-Cross, also heute überall): vio baut Hull/Domain selbst.

### 5a Gemeinsame Schnittstelle

Neue Datei `src/vio_tess_hlsl.c` (+ `.h`):
`char *vio_tess_to_hlsl(stage, tcs_spirv, tes_spirv, input_points, shader_model, fixup_depth, &err)`.
Sie bekommt **beide** Module: die TCS braucht Domain/Spacing/Winding/PointMode der TES, die TES
`OutputVertices` der TCS (wie `vio_metal_tess_reflect`). GL-Semantik ist fest eingestellt (Winding-Umkehr,
kein Koordinaten-Flip). Register-/Binding-Vergabe und Optionen kommen aus demselben Code wie
`vio_spirv_to_hlsl_ex` (wird dafür in einen internen Helfer mit Vor-/Nach-Hooks zerlegt), damit
Uniforms und Texturen in HS/DS genauso gebunden werden wie heute.

### 5b Nativer Pfad

Execution-Modes per `spvc_compiler_set_execution_mode(_with_arguments)` setzen, Optionen
`TESS_INPUT_CONTROL_POINTS` und `TESS_DOMAIN_ORIGIN_LOWER_LEFT`, kompilieren. Die Stage-Probe bekommt
dafür gepaarte Probe-Module (heute probt sie TCS/TES ohne Modes und scheitert auch am Fork).

### 5c Generator

SPIR-V-Umbau wie `vio_gs_hlsl_rewrite`, dann SPIRV-Cross als **Vertex-Stage**, dann Text-Wrapper:

1. Execution-Model → Vertex, Tessellations-Modes entfernen, `OpControlBarrier`/`OpMemoryBarrier` im
   TCS entfernen.
2. Interface-Variablen → `Private`: TCS alle Ein- und Ausgänge, TES alle Eingänge (die TES-Ausgänge
   bleiben echte VS-Ausgänge; damit erzeugt SPIRV-Cross `SPIRV_Cross_Output` und den Depth-Fixup
   selbst). Pointer-Typen mit; BuiltIn-/Location-/Patch-Dekorationen dieser Variablen und die
   Block-/BuiltIn-Dekorationen der Eingangs-`gl_PerVertex`-Structs entfernen (glslang legt Ein- und
   Ausgangs-`gl_PerVertex` als getrennte Typen an). Namen per `spvc_compiler_set_name` setzen, nach dem
   Kompilieren die endgültigen Namen per `spvc_compiler_get_name` lesen.
3. SPIRV-Cross liefert Statics + `vert_main()` + ein triviales `main`.
4. Wrapper wie in #2694: TCS → Patch-Constant-Funktion (Ausgänge vorbefüllen, `vert_main()` je
   Invocation) + Kontrollpunkt-`main`; TES → `[domain]`-Signatur mit `OutputPatch`, Patch-Struct und
   `SV_DomainLocation`, Kopien vor `vert_main()`.
5. Structs: Kontrollpunkt-Struct und Patch-Struct werden **aus der TCS** gebaut (alle Ausgänge nach
   Location, Position als `SV_Position`, Patch-Varyings `PATCH<loc>`, Faktoren nach Domain) und sind in
   HS und DS identisch – auch wenn die TES nur einen Teil liest. Liest die TES eine Location, die die
   TCS nicht schreibt: Fehler. HS-Eingang = TCS-Eingänge (wie SPIRV-Cross beim GS).
6. Grenzen v1 (Warnung + `false`, HLSL-Override bleibt der Ausweg): Interface-Blöcke als Varyings,
   Matrix-/Struct-Varyings, `gl_ClipDistance`/`gl_CullDistance`, Lesen fremder Kontrollpunkte in der
   CP-Phase (wie upstream dokumentiert).

### 5d D3D11/D3D12

- `compile_shader` übersetzt HS/DS über `vio_tess_to_hlsl` (wenn kein Override), mit
  `input_points = OutputVertices`, und hält die beiden SPIR-V-Module für Varianten.
- `create_pipeline`: weicht `patch_vertices` davon ab, wird eine HS-Variante kompiliert (D3D11: eigenes
  `ID3D11HullShader` in der Pipeline; D3D12: HS-Blob der PSO-Vorlage). Der DXBC-/DXIL-Cache greift über
  den HLSL-Text, der `NIn` enthält.
- `TESSELLATION`-Flag: Probe über den neuen Pfad (gepaarte Probe-Module, FXC `hs/ds_5_0|5_1`).
- SM 6 (DXC) mit beiden Pfaden prüfen.

### 5e Vulkan und Metal

- Vulkan setzt heute keinen Domain-Ursprung (Standard oben links) → GLSL-Tessellation hat die
  umgekehrte Winding und verschwindet bei Backface-Culling. Fix:
  `VkPipelineTessellationDomainOriginStateCreateInfo` mit `LOWER_LEFT` an den Tessellation-State hängen
  (Core 1.1, die Instanz läuft schon mit 1.1).
- Metal: Test 144 misst Kanten und Winding; vertauscht die MSL-Option Quad-Faktoren, wird sie durch
  Winding-Umkehr im Pipeline-State ersetzt.

### 5f Tests

- **144** (neu, alle Backends): GL-Semantik der Tessellation – asymmetrische Kantenfaktoren
  (welche Kante wird unterteilt), Winding unter `VIO_CULL_BACK`, Isolines (v-Werte), Patch-Varyings,
  `patch_vertices` ≠ `vertices` (HS-Variante), Uniform in HS und DS.
- 110/135 laufen auf D3D ohne Override (Flag wird 1); 140 bleibt unverändert.
- Nebenbefund: D3D11-Isolines zeichnen in vio unzuverlässig (gleicher Lauf mal vier Linien, mal
  keine) – in 5d mit untersuchen.

## Testbarkeit

SPIRV-Cross: alle 13 CTest-Varianten mit den CI-Werkzeugständen (`external/`, gebaut aus
`checkout_glslang_spirv_tools.sh`), dazu FXC/DXC aus dem Windows SDK. vio: Pixeltests auf der RTX 2080
(D3D11/D3D12) und WARP; Vulkan lokal, Metal über die macOS-CI.

## Risiken

- FXC und indizierte `static`-Arrays von Structs in Hull-Shadern (Fork/Join-Phasen): früh in Phase 2
  gegen FXC prüfen; Ausweichen auf flache Arrays je Varying.
- Semantik-Kollisionen zwischen Kontrollpunkt- und Patch-Signatur: Patch-Varyings bekommen eigene
  Semantik (`PATCH<loc>`), DS liest dieselbe.
- Reviewer-Vorbehalte gegen die Invocation-Schleife: Alternative wäre, nur Invocation 0 auszuführen;
  die Schleife ist korrekter und wird im PR begründet.
