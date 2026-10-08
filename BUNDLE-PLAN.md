# Aufgezeichnete Draw-Folgen (Bundles) — OPEN-ITEMS A38

> Stand 2026-10-08. „Recording über Secondary Command Buffers“: PHP ist einfädig, Worker-Threads können keinen
> PHP-Code ausführen. Was sich lohnt, ist das **einmalige Aufzeichnen** statischer Draw-Folgen und ihr billiges
> Wiederabspielen in jedem Frame. Die Messreihe (`examples/bench_open_items.php`) zeigt warum: 2000 Einzel-Draws
> kosten auf einer RTX 2080 1,3–6 ms CPU, `vio_submit_batch` spart davon nichts – die Kosten entstehen im
> Backend (Uniform-Upload in den Frame-Ring, Descriptor-Sets, Varianten-Suche, Kommando-Aufzeichnung), nicht im
> PHP-Aufruf.

## API

```php
$bundle = vio_bundle($ctx, $records);       // Records wie vio_submit_batch: mesh, pipeline, textures, uniforms
vio_draw_bundle($ctx, $bundle);              // im Frame, im offenen Pass (Swapchain oder Render-Target)
vio_bundle_info($bundle);                    // ['draws' => n, 'native' => bool, 'method' => 'secondary_command_buffer' | …]
```

- Ein Bundle ist **statisch**: Uniform-Werte, Texturen, Pipelines und Meshes werden beim Anlegen festgehalten
  (die Objekte per Referenz am Leben gehalten). Was sich pro Frame ändert, bleibt außerhalb oder kommt über
  Storage-Buffer / Bindless (die das Bundle per Referenz liest).
- Ein Record setzt jedes Uniform, auf das er sich verlässt; Werte, die **kein** Record des Bundles setzt, sind
  beim Abspielen unbestimmt. So kann ein nativer Pfad die Bytes beim Aufzeichnen einbacken, und auf OpenGL (kein
  Schatten-Cbuffer, `glUniform` am Programm) gilt derselbe Vertrag. Uniforms, die der Aufrufer zwischen
  Aufzeichnen und `vio_draw_bundle` setzt, ändern das Bundle nicht.
- Ein Bundle passt zu einer Attachment-Signatur (Formate, Samples, Tiefe); es wird für die Signatur des ersten
  `vio_draw_bundle` aufgezeichnet und bei einer anderen Signatur neu aufgezeichnet (Cache je Signatur, max. 4).

## Phasen

| Phase | Inhalt | Prüfung |
|---|---|---|
| 1 | API + **generischer Pfad** auf allen Backends: Records einmal parsen, Uniform-Bytes je Record vorberechnet, beim Abspielen eine C-Schleife über den gemeinsamen Draw-Kern (kein PHP-Hash-Lookup, keine zval-Umwandlung). `native = false`. | Test 207: Bundle ≡ dieselben Einzel-Draws (byte-gleich), Uniforms vor dem Bundle wirken nicht, freigegebene Objekte bleiben gültig, Bundle in Swapchain und RT, falsche Records → `ValueError` |
| 2 | **Vulkan**: Secondary Command Buffer mit `VkCommandBufferInheritanceRenderingInfo` (Dynamic Rendering, A37), bundle-eigener Uniform-Puffer (alle Records, dynamische Offsets fest) und eigene Descriptor-Sets; `vkCmdExecuteCommands`. | 207 nativ, Messreihe: Szenario „Bundle“ |
| 3 | **D3D12**: Bundle-Command-List (`D3D12_COMMAND_LIST_TYPE_BUNDLE`), Root-CBVs auf einen bundle-eigenen Upload-Puffer, SRV-/Sampler-Tabellen in einem reservierten Bereich der shader-sichtbaren Heaps. | dto. |
| 4 | **D3D11**: Deferred Context + `FinishCommandList`, `ExecuteCommandList`; bundle-eigene Konstantenpuffer. | dto. |
| — | Metal (Indirect Command Buffers + Argument-Buffer) und GL bleiben beim generischen Pfad. | — |

## Risiken

- Vulkan: Secondary Command Buffers dürfen den Viewport/Scissor nicht erben → im Bundle gesetzt (Ziel-Extent)
  bzw. `VK_NV_inherited_viewport_scissor` wenn vorhanden.
- D3D12: Bundles können Descriptor-Heaps nicht wechseln; der reservierte Heap-Bereich muss beim Anlegen frei sein.
- Mesh-Shader-, Tessellations-, GS-Pipelines und Indirect-Draws: Phase 1 ja, nativ erst wenn getestet.
