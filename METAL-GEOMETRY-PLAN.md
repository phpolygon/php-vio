# Metal: Geometry-Stage per Compute-Emulation

Status: ✅ umgesetzt (2026-10-05, `src/backends/metal/vio_metal_kernel.h`, `metal_draw_gs`). Ziel: `VIO_FEATURE_GEOMETRY` (+ `GEOMETRY_INSTANCING`, Adjacency,
`gl_Layer`/`gl_ViewportIndex` aus dem GS) auf Metal, damit die Tests 109, 135, 137, 138, 139 und 143
auch dort laufen statt zu skippen. Metal hat keine Geometry-Stage; MoltenVK emuliert sie ebenfalls
nicht. vio emuliert sie wie die Tessellation: Stages als Compute-Kernel, Ergebnis in Ring-Puffer,
danach ein normaler Draw.

## Ablauf je Draw mit GS-Pipeline

1. **CPU**: Draw de-indizieren (wie `metal_draw_tess`, Mesh-Puffer sind Shared) und Strip-Topologien
   in Listen auflösen. Ergebnis: ein Vertexstrom in einer Ring-Slice, je Primitiv `N` aufeinander-
   folgende Vertices (`N` = 1/2/3/4/6 aus dem GS-Eingabetyp, `lines/triangles_adjacency` direkt).
2. **VS-Kernel** (Grid: Vertex × Instanz): die Vertex-Stage als Compute-Kernel. Liest die
   Attribute aus dem Mesh-Puffer (dicht in Location-Reihenfolge, Locations 3–6 aus dem Instanz-
   Puffer, wie der Metal-Vertex-Deskriptor), schreibt `gl_Position` + alle Outputs als vec4-Slots
   in einen Record-Puffer.
3. **GS-Kernel** (Grid: Primitiv × Invocation, Instanz): liest die `N` Records seines Primitivs in
   die GS-Eingänge (`in T x[]`, `gl_in[]`), führt den GS-Körper aus. `EmitVertex()` schreibt die
   Outputs als Record in den festen Bereich der Invocation, Strips werden dabei zu Listen
   (GL-Strip-Regel: ungerade Dreiecke `(i+1, i, i+2)`). Unbenutzte Slots bekommen eine Position
   außerhalb des Clip-Raums – die Primitiv-Reihenfolge bleibt so wie in GL erhalten (kein Atomic-
   Append).
4. **Draw**: ein generierter Durchreich-Vertex-Shader liest den Record per `gl_VertexIndex` und gibt
   `gl_Position`, `gl_Layer`, `gl_ViewportIndex`, `gl_PointSize` und die GS-Outputs mit ihren
   Locations aus. Der Fragment-Shader bleibt unverändert und linkt über die Locations.
   `drawPrimitives(Point|Line|Triangle, 0, Invocations × Slots)` auf dem offenen Encoder.

## Stage → Kernel (plain C, `src/backends/metal/vio_metal_kernel.h`)

Keine handgeschriebenen SPIR-V-Loads/Stores; die Umbauten sind strukturell, den Rest erledigt GLSL:

1. **SPIR-V-Umbau**: Execution-Model → `GLCompute` (+ `LocalSize`), Stage-Execution-Modes weg,
   alle Interface-Variablen (`Input`/`Output`, auch `gl_PerVertex`-Blöcke und Builtins) werden
   `Private`, `OpEmitVertex`/`OpEndPrimitive` werden Aufrufe zweier leerer Funktionen
   `vio_emit()`/`vio_end_primitive()` (neue IDs).
2. **SPIRV-Cross → GLSL 450** (Vulkan-Semantik wie im Vulkan-Backend). Namen werden vorher fest
   vergeben (`vio_i<loc>`, `vio_o<loc>`, `vio_gl_in`, `vio_Position`, …), damit der Text vorhersagbar
   ist und keine `gl_`-Namen übrig bleiben.
3. **Text**: `void main()` → `void vio_stage_main()`; vio hängt Puffer-Deklarationen, die Körper
   von `vio_emit`/`vio_end_primitive` und ein neues `main()` an (Eingänge laden, Körper aufrufen,
   Ausgänge schreiben bzw. Rest-Slots füllen).
4. **glslang** (Compute) → SPIR-V → **MSL** über den bestehenden Compute-Pfad mit Umnummerierung;
   Uniform-Block der Stage = derselbe Default-Block (gleiche Offsets) → `metal_current_vs_cb` bzw.
   `metal_stage_const[GEOMETRY]`.

Record-Format (vec4-Slots): Slot 0 `gl_Position`, Slot 1 `(layer, viewport, point_size, -)` als Bits,
danach je User-Output in Location-Reihenfolge `ceil(Komponenten/4) × Spalten × Arraylänge` Slots.
VS- und GS-Records nutzen dasselbe Schema (VS-Records werden vom GS über die Location gefunden).

Upstream: SPIRV-Cross #2654 („Complete MSL lowering for geometry shaders“, Draft, Konflikte, an
MoltenVK#2786 gekoppelt; der Maintainer lehnte die Vorgänger-Iteration ab) und #2200 (GS über Metal-
3-Object/Mesh-Stages, CHANGES_REQUESTED, seit 2025 inaktiv) sind die einzigen Ansätze – keiner ist
absehbar mergebar, beide bräuchten Mesh-Shader (macOS 13+). vio emuliert deshalb selbst.

## Grenzen (Warning bei `vio_shader`)

- Sampler in VS/GS einer GS-Pipeline, Interface-Blöcke, Struct-Varyings (wie der D3D-Tess-Pfad).
- `gl_PrimitiveID` aus dem GS erreicht den Fragment-Shader nicht (Metal liefert die Primitiv-
  Nummer des Ersatz-Draws).
- GS + Tessellation in einer Pipeline.
- Strip-Adjacency-Topologien (nur die Listen-Varianten).

## Tests

109, 135 (GS-Teile), 137/138 (`gl_Layer`/`gl_ViewportIndex` aus dem GS), 139 (Instancing +
Adjacency), 143 (`gl_in[].gl_Position`) laufen auf Metal statt `skip (no geometry stage)`;
074 pinnt `GEOMETRY`/`GEOMETRY_INSTANCING = 1`.
