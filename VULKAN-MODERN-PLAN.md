# Vulkan 1.2+ — Timeline-Semaphores, Synchronization2, Dynamic Rendering (OPEN-ITEMS A37, danach A18)

> **Status: ✅ umgesetzt (2026-10-08), Phasen 1–5.** Abweichung vom Kontrakt: `pipeline_variants` in
> `vio_backend_info` entfällt (die Caps sind Booleans); der Varianten-Schlüssel war schon die Attachment-Signatur.

> Stand 2026-10-08. Ziel: das Vulkan-Backend auf den Kern moderner Treiber stellen. Mindestanforderung wird
> **Vulkan 1.3**, oder **1.2 mit `VK_KHR_dynamic_rendering` + `VK_KHR_synchronization2`** (Timeline-Semaphores
> sind Kern 1.2). Alle Desktop-Treiber seit 2022 und MoltenVK erfüllen das. Ein Gerät ohne die drei Features wird
> bei der Gerätewahl übersprungen; `vio_create('vulkan')` scheitert dann mit Warnung, `auto` nimmt den nächsten
> Kandidaten (Registry-Verhalten seit GAP-PLAN).

## Warum

- **Render-Pass- und Framebuffer-Objekte fallen weg.** Heute hält jedes Render-Target bis zu `layers × levels`
  Framebuffer, eigene Passes für ohne Tiefe, Multiview (2/3/4 Views), Depth-Resolve und Depth-Mips; dazu der
  LOAD-„Resume"-Pass der Swapchain. Pipelines sind Varianten **je Render-Pass-Handle**. Mit Dynamic Rendering ist
  ein Pass ein `vkCmdBeginRendering` mit Views + Load-Ops, Pipelines hängen nur noch an Formaten, Sample-Zahl und
  View-Maske — weniger Varianten, kein Objekt-Lebenszyklus.
- **A18 (Shading-Rate-Bild)** wird ein Feld von `VkRenderingInfo` (`VkRenderingFragmentShadingRateAttachmentInfoKHR`)
  statt einer zweiten Varianten-Achse über alle Render-Passes.
- **Timeline-Semaphore** ersetzt die Zoo-Fences (`in_flight` je Slot, `transient_fence`, `midframe_fence`,
  `capture_fence`): jede Übergabe signalisiert einen fortlaufenden Wert, Warten heißt „bis Wert N". Readback,
  Grab-Freigabe und Upload-Ordnung werden Vergleiche statt Fence-Buchhaltung.
- **Synchronization2** macht Barrieren präzise (Stage + Access je Barriere statt Masken-Paare über alles) und ist
  Voraussetzung für `vkQueueSubmit2`, das binäre (Present) und Timeline-Semaphoren in einer Übergabe mischt.

## Phasen

| Phase | Inhalt | Prüfung |
|---|---|---|
| 1 | Instanz/Gerät: API 1.3 bzw. 1.2 + Extensions, Features `timelineSemaphore`, `synchronization2`, `dynamicRendering` einschalten, Einstiegspunkte über `vkGetDeviceProcAddr` (Kern- oder KHR-Name). `vio_backend_info` meldet `dynamic_rendering`, `synchronization2`, `timeline_semaphore`. Geräte ohne → übersprungen. | Test 206 (Info + Pass-Folge, s. u.) |
| 2 | Timeline: ein `VkSemaphore` Typ TIMELINE + Zähler. Frame-Slots, Transient-Uploads, Mid-Frame-Flush und Capture merken sich ihren Wert; `vkWaitSemaphores` statt `vkWaitForFences`. Binäre Semaphoren bleiben nur für Acquire/Present. Übergaben über `vkQueueSubmit2`. | volle Suite, 188 (Readback in flight), 202/098 (Compute im Frame) |
| 3 | Synchronization2: `vio_vk_image_barrier*` und alle `vkCmdPipelineBarrier` → `vkCmdPipelineBarrier2` mit Stage/Access je Übergang. | volle Suite + `vio.debug=1` (Sync-Validation) |
| 4 | Dynamic Rendering: eine Pass-Beschreibung (`vio_vk_pass`: Farb-/Tiefen-Views, Bild + Bereich, Ruhe-Layout, Load-Op, Resolve-View, Layer, View-Maske, Fläche). `begin` = Barrieren Ruhe → Attachment + `vkCmdBeginRendering`, `end` = `vkCmdEndRendering` + Barrieren zurück; „Resume" = derselbe Pass mit LOAD. Ruhe-Layouts wie heute (RT-Farbe `SHADER_READ_ONLY`, MSAA-Farbe `COLOR_ATTACHMENT`, Tiefe `ATTACHMENT` bzw. `READ_ONLY` bei depth_only, Swapchain `PRESENT_SRC`). Pipelines (3D, 2D, Depth-Mip, Depth-Resolve) mit `VkPipelineRenderingCreateInfo`, Varianten-Schlüssel = (Formate, Tiefenformat, Samples, View-Maske, Stride). Alle Render-Pass-/Framebuffer-Objekte entfallen. | volle Suite + Debug; 090–098, 101–105, 135–148, 182/183, 193–196 decken die Pass-Arten ab |
| 5 | A18: `vio_set_shading_rate_image` auf Vulkan über das Rendering-Attachment (`attachmentFragmentShadingRate`), Kachelgröße aus den Geräteeigenschaften. | Test 160 läuft auf Vulkan |

## Test-Kontrakt

- **Neu 206** (`tests/backends/206_vulkan_modern_core.phpt`): auf Vulkan meldet `vio_backend_info` die drei Features;
  ein Frame mit 64 Wechseln zwischen Swapchain, einem MSAA-Target, einer Cube-Face, einem Array-Layer
  (`VIO_RT_ALL_LAYERS`) und einem depth_only-Target, mit Zeichnungen, Mid-Frame-Readback eines Targets, einem
  async Compute-Dispatch und `vio_compute_wait` dazwischen, liefert pixelgenau dasselbe wie D3D12/GL; die Zahl der
  Pipeline-Varianten bleibt bei gleichen Formaten 1 (`vio_backend_info()['pipeline_variants']`, nur Vulkan).
- Bestehende Suite unverändert grün auf RTX und mit `vio.debug=1` (Validation inkl. Synchronisation).
- Kein Backend-Zweig in `php_vio.c` (Audit-Gate 099).

## Risiken

- Layout-Buchhaltung, die heute implizit in den Passes steckt, muss vollständig in Barrieren auftauchen — die
  Sync-Validation (`vio.debug=1`) ist der Prüfstein.
- MoltenVK: Dynamic Rendering + Sync2 als Extensions; Belege nur über die macOS-CI (Vulkan dort per MoltenVK, Tests
  skippen bisher wegen SIP — Gerätewahl trotzdem prüfen).
