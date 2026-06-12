# Implementation Plan: Vulkan Backend 2D + Offscreen Render Targets (php-vio)

> Phased plan to bring php-vio's Vulkan backend from a 2D stub to full parity
> with d3d11/d3d12/metal/opengl, including offscreen render targets and the
> warm-render invariants. Authored from an architecture audit at HEAD `42a9ebf`
> (post-v1.15.1). Read-and-design; not yet implemented.

## Assumptions & ground truth (verified against HEAD `42a9ebf`)

- **Vulkan API version: 1.0** (`VK_API_VERSION_1_0` in `create_instance`, `vio_vulkan.c:80`). Single most consequential constraint — drives the dynamic-rendering decision in Phase 3.
- **2 frames in flight** (`VIO_VK_MAX_FRAMES_IN_FLIGHT = 2`), 1 command buffer + pool + `image_available`/`render_finished` semaphores + `in_flight` fence per frame.
- **VMA wired** (`vio_vma_create_buffer/map/unmap/create_image/destroy_image/destroy_buffer`).
- **glslang SPIR-V path exists** (`vio_compile_glsl_to_spirv`); 2D GLSL shaders live in `src/shaders/shaders_2d.h`. Vulkan consumes SPIR-V directly — no transpile.
- **2D flush is inlined per-backend in `vio_2d.c`**, NOT routed through the vtable `.draw` slot. Most work is a new `#ifdef HAVE_VULKAN` branch in `vio_2d_flush` + a `vio_2d_vulkan.c` helper, not vtable surgery.
- **Main render pass opens in `vulkan_begin_frame`, closes in `vulkan_end_frame`** (`loadOp=CLEAR`, color `finalLayout=PRESENT_SRC_KHR`, + depth). 2D draws must record between those calls.
- Model to mirror: **D3D12** (`vio_2d_d3d12.c`, `vio_d3d12.c`, the `php_vio.c` inline d3d12 RT path). Metal's `current_bound_rt` + no-drawable present-skip is the second reference.

**Cross-cutting decision to lock before Phase 1:** the swapchain currently chooses **sRGB** (`VK_FORMAT_B8G8R8A8_SRGB`); D3D12's 2D PSO targets `R8G8B8A8_UNORM` (linear) with identically-authored colors. Rendering the same vertex colors into an sRGB swapchain makes Vulkan brighter/different. **Recommend standardizing on `B8G8R8A8_UNORM` swapchain + UNORM offscreen** to match the other backends pixel-for-pixel (makes golden-image tests meaningful). Affects Phase 1 (pipeline format), Phase 3 (offscreen format), Phase 5 (test tolerance).

---

## Phase 0 — Scaffolding & state (~0.5 day, low risk)

- `vio_vulkan.h` — add to `vio_vulkan_state`: `int in_frame;`, `void *pending_bound_rt;`, `void *current_bound_rt;` (mirror `vio_d3d12`).
- `vio_2d.h` — add `VIO_2D_BACKEND_VULKAN = 4`; add `void *vulkan_state;` to `vio_2d_state`.
- `vio_render_target.h` — add `#define VIO_RT_BACKEND_VULKAN 5`; add `vulkan_*` fields to `vio_render_target_object`; init to NULL/0 in `vio_render_target_create_object`.
- `vio_vulkan.c` — set `vio_vk.in_frame = 1` at end of `vulkan_begin_frame`, `= 0` in `vulkan_end_frame`.

**Verify:** compiles with `--with-vulkan --with-glslang`; `test-vulkan-minimal.php` still clean under validation layers. No behavior change.

---

## Phase 1 — 2D pipeline, streaming buffers, draw (~3–4 days, MEDIUM-HIGH risk)

Goal: `vio_2d_flush` records real geometry on Vulkan (text/rects/sprites/scissor/transforms → swapchain).

**1a. Shaders → SPIR-V → `VkShaderModule`.** Compile existing `vio_2d_vertex_shader` / `..._shapes` / `..._sprites` via `vio_compile_glsl_to_spirv`, then `vkCreateShaderModule`. **Critical:** the compiler uses `AUTO_MAP_BINDINGS` + `VULKAN_RULES_RELAXED` — auto binding numbers are fragile. Prefer **explicit** `layout(set=0,binding=N)` in a Vulkan shader variant (a binding mismatch is a silent black screen, not a validation error).

**1b. Descriptor set layout + pipeline layout.** Set 0: binding 0 UBO (vertex, projection mat4), binding 1 combined image sampler (fragment). **Recommend push constant for the 64-byte projection** instead of a UBO — sidesteps a per-frame UBO ring; set 0 then holds only the sampler.

**1c. Two `VkPipeline`s (shapes, sprites)** mirroring the D3D12 PSO:
- Vertex input: binding 0 stride `sizeof(vio_2d_vertex)`=32, 3 attrs (loc0 `R32G32_SFLOAT` x; loc1 `R32G32_SFLOAT` uv; loc2 `R32G32B32A32_SFLOAT` rgba).
- Topology TRIANGLE_LIST; cull NONE; fill; depth test/write OFF.
- Blend: `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`, alpha `ONE / ONE_MINUS_SRC_ALPHA`, ADD (matches D3D12).
- Dynamic state: VIEWPORT + SCISSOR (per-item scissor + serves both swapchain and offscreen extents).
- **renderPass compatibility:** swapchain pass and offscreen pass (Phase 3) must be render-pass-compatible (same attachment formats/samples) so the SAME pipelines bind in both; else build per-format pipeline variants. Subtlest correctness coupling in the plan.

**1d. Streaming vertex buffer (frame-in-flight ring).** One VMA `HOST_VISIBLE|HOST_COHERENT` `VERTEX_BUFFER`, size `sizeof(vertex)*MAX_VERTICES*2`, persistently mapped. memcpy into `slice = current_frame*slice_size`; bind at slice offset. The `in_flight` fence (waited in `begin_frame`) gates reuse — 2 slices exactly sufficient, don't under-size to 1. Overflow: clamp+warn first; growth-behind-`vkQueueWaitIdle` as follow-up.

**1e. `vulkan_draw`/`vulkan_draw_indexed`** can stay stubs for 2D (flush records `vkCmdDraw` directly); implement only when wiring the 3D mesh path.

**1f. `vio_2d_flush` Vulkan branch** + new `src/vio_2d_vulkan.c`/`.h` (state, init/shutdown). Branch: upload vertices → push projection → bind VB at slice → loop sorted items (scissor change → `vkCmdSetScissor` scaled logical→fb; pipeline change → `vkCmdBindPipeline`; texture change → descriptor (Phase 2); `vkCmdDraw`).

**Risks:** binding mismatch (silent black); render-pass/pipeline incompatibility; sRGB color shift; scissor must be clamped to render area (Vulkan errors if it exceeds, unlike D3D).

**Verify:** `test-d3d12-2d-ascii.php vulkan` — text/sprites/scissor/heavy glyph rows/animated line correct, **zero validation errors**, side-by-side vs d3d11/d3d12.

---

## Phase 2 — Textures, samplers, descriptors (~3–4 days, HIGH risk)

**2a. `vulkan_create_texture`/`_destroy_texture`.** VMA DEVICE_LOCAL `VkImage` (`R8G8B8A8_UNORM`, `SAMPLED|TRANSFER_DST`), staging buffer, `UNDEFINED→TRANSFER_DST` barrier, `vkCmdCopyBufferToImage`, `→SHADER_READ_ONLY`. **Uploads happen outside a frame** (at `vio_texture()`/`vio_font()`): use a one-time-submit transient cmd buffer + `vkQueueWaitIdle` (or transfer fence). Do NOT fold into the frame cmd buffer. Wrap `{VkImage, allocation, VkImageView, VkSampler}`; sampler from `desc->filter`/`wrap`.

**2b. Font atlas.** Atlas is packed once at `vio_font()` (no mid-frame growth today). Expand R8→RGBA8 like D3D12 (white RGB, coverage in alpha) so one sprite pipeline serves sprites+glyphs — new `#ifdef HAVE_VULKAN` block beside the d3d12 one at `php_vio.c:3030`. (Forward-looking: if atlas growth is added, defer-destroy the old image until fences clear.)

**2c. Per-draw descriptor strategy — core difficulty.** **Recommend per-frame descriptor pool + reset:** one `VkDescriptorPool` per frame-in-flight (2), reset at top of `vulkan_begin_frame` after the `in_flight` fence wait (the sync point that makes reset safe). On texture change: allocate set, `vkUpdateDescriptorSets`, `vkCmdBindDescriptorSets`; cache "texture→set" within the frame. This is the analog of the D3D12 per-frame SRV ring. Alternative (persistent per-texture sets) risks updating an in-flight set — prefer per-frame reset.

**Risks (high):** descriptor-set-in-flight overwrite (sync validation catches); pool exhaustion (size generously, handle `OUT_OF_POOL_MEMORY`); sampler/filter mismatch.

**Verify:** `test-d3d12-2d-ascii.php vulkan` (glyphs + 3 sprite icons); `test-d3d12-lazy-sprite.php vulkan` (mid-run texture, no corruption). Run with `VK_LAYER_KHRONOS_validation` + **synchronization validation**.

---

## Phase 3 — Offscreen render targets (~4–5 days, HIGH risk)

**3a. Render pass vs dynamic rendering — DECISION: use render-pass + framebuffer**, NOT `VK_KHR_dynamic_rendering`. Instance is `VK_API_VERSION_1_0`; dynamic rendering needs 1.3 or several KHR extensions. The swapchain path is already render-pass based. (Revisit only if the instance is bumped to 1.3.)

**3b. Per-target resources (`vulkan_create_render_target`).** Wire `.create/.bind/.unbind/.destroy_render_target` into the vtable (currently absent). Per RT: VMA DEVICE_LOCAL color `VkImage` (UNORM, or `R16G16B16A16_SFLOAT` HDR) usage `COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC`; optional depth; `VkImageView`(s); a `VkRenderPass` **render-pass-compatible with the swapchain 2D pipelines**, color `finalLayout=SHADER_READ_ONLY_OPTIMAL` (cleaner than D3D12's barrier-on-unbind); `VkFramebuffer` at the RT extent; a `VkSampler` + persistent `VkDescriptorSet` for sampling. Store on `vio_render_target_object`. Add the `HAVE_VULKAN` branch to `ZEND_FUNCTION(vio_render_target)` (parallel to d3d12 ~5470+).

**3c. Mid-frame target switch — structural difference from D3D.** Vulkan can't switch render pass without ending it. In-frame `bind`: `vkCmdEndRenderPass` (swapchain) → `vkCmdBeginRenderPass(offscreen)` → set viewport/scissor to RT extent → `current_bound_rt = rt`. In-frame `unbind`: end offscreen pass (→ `SHADER_READ_ONLY`), begin swapchain pass (use a `loadOp=LOAD` variant so prior swapchain draws aren't wiped) + restore viewport/scissor, clear `current_bound_rt`. (warmRender never resumes the swapchain, so the LOAD variant matters only for true composite frames.)

**3d. Out-of-frame bind/unbind — warm-render ordering (mirror D3D12).** `warmRender` binds **before** `vio_begin`, unbinds **after** `vio_end`. `bind` before `begin`: stash `pending_bound_rt` (don't record); apply it in `vio_begin` after the swapchain pass opens (parallel to d3d12 block at `php_vio.c:488`). `unbind` after `end`: cmd buffer already submitted — just drop `pending/current_bound_rt` (parallel to d3d12 at `php_vio.c:5991`). Guard `backend_type == VIO_RT_BACKEND_VULKAN && vio_vk.initialized`, branch on `vio_vk.in_frame`.

**3e. `vio_render_target_texture`** — `HAVE_VULKAN` branch returning a texture wrapper around the RT color view+sampler+persistent descriptor set; build once + cache (avoid the per-frame leak the d3d11/d3d12 comments warn about).

**Risks (high):** the end/begin-render-pass dance (validation flags draws outside a pass / present of an image still in COLOR_ATTACHMENT layout); render-pass-compatibility coupling; layout transitions.

**Verify:** render-to-texture script (offscreen → `vio_render_target_texture` → fullscreen sprite to swapchain), zero validation errors.

---

## Phase 4 — Warm-render correctness (~1.5–2 days, MEDIUM risk; the known bug class)

**4a. Present-skip when offscreen-bound (bug #2).** `vulkan_present` today always presents. When `current_bound_rt` set at end-of-frame, the swapchain image was never drawn → presenting = flash. Fix (mirror `d3d12_present:1616`/metal): skip `vkQueuePresentKHR` + image rotation, still advance `current_frame`.

**4b. Acquire/present-semaphore problem (Vulkan-specific, NO D3D analog — HIGHEST RISK).** For a pure-offscreen frame that won't present: don't signal `render_finished` (submit with `signalSemaphoreCount=0`); ideally don't `vkAcquireNextImageKHR` at all. Options: (1) still acquire but don't wait on `image_available` and handle the unused semaphore — fiddly; (2) defer the swapchain acquire/pass-begin out of `begin_frame` to first swapchain use — larger change, eliminates the dangling-semaphore class. **Recommend (1) for warm-render parity first** (warm frames are throwaway), note (2) as the proper fix; decide based on what synchronization validation reports.

**4c. Destroy-while-in-flight (bug #3).** `vulkan_destroy_render_target` must `vkDeviceWaitIdle` (or wait both `in_flight` fences) before destroying image/view/framebuffer/render-pass/sampler/memory (a present-skipped warm frame has no present to throttle). Mirror `d3d12_destroy_render_target:1238`. Also null `pending/current_bound_rt` if they point at the RT.

**Verify (acceptance gate):** code-tycoon (or minimal warmRender repro) on Vulkan — no splash flash, no validation/sync errors, clean shutdown.

---

## Phase 5 — Integration & verification (~1.5 days, LOW-MEDIUM risk)

**5a. Feature flags** in `vulkan_supports_feature`: `NATIVE_2D_BATCH→1` (after P1–2), `RENDER_TARGET→1` (after P3), `RENDER_TARGET_HDR/_DEPTH` per build. Implement `read_pixels` (`vkCmdCopyImageToBuffer`→HOST_VISIBLE staging, top-down RGBA8), flip `READ_PIXELS→1`.

**5b. Engine-side:** remove the Vulkan exclusion at `code-tycoon SettingsPanel.php:565` (add `'vulkan'`), gated behind the feature query for clean rollback.

**5c. Test harness (validation-layer-first):** every test with `VK_LAYER_KHRONOS_validation` + GPU-assisted + synchronization validation; any message = hard fail. Per-phase repros: `test-vulkan-minimal.php`, `test-d3d12-2d-ascii.php vulkan`, `test-d3d12-lazy-sprite.php vulkan`, new render-to-texture, warmRender repro. Headless golden-image via `vio_read_pixels` vs D3D12 golden (UNORM standardization makes the tolerance meaningful). Edge cases: resize swapchain recreate mid-2D-scene, minimize→restore, high vertex volume, device-lost smoke. RenderDoc capture of one warm frame to confirm offscreen pass recorded + present skipped.

---

## Effort & risk summary

| Phase | Scope | Effort | Risk |
|---|---|---|---|
| 0 | State scaffolding (enums, tracking fields) | ~0.5d | Low |
| 1 | 2D pipelines, push-constant projection, VBO ring, flush branch | ~3–4d | Med-High |
| 2 | Textures/samplers, font atlas, per-frame descriptor ring | ~3–4d | **High** |
| 3 | Offscreen RT, render-pass switch mid-frame, warm-render ordering | ~4–5d | **High** |
| 4 | Present-skip + acquire/semaphore-for-non-present + destroy-wait | ~1.5–2d | Med (one no-analog item) |
| 5 | Feature flags, read_pixels, engine un-gate, golden tests | ~1.5d | Low-Med |

**Total: ~14–17 dev-days.**

**Highest-risk areas, in order:**
1. **Phase 4b** — acquire/present-semaphore handling for a non-presenting frame. No D3D analog; a dangling swapchain semaphore desyncs the 2-frame ring; passes a quick run, fails under sync validation / on other drivers. Lean on synchronization validation as the oracle.
2. **Phase 2c** — per-frame descriptor ring. A descriptor set updated while in flight is a use-after-write; per-frame-pool-reset is the safe path.
3. **Phase 3c** — mid-frame render-pass switching + render-pass-compatibility coupling with Phase 1 pipelines (easy to get a silently-wrong layout or forget the `loadOp=LOAD` resume variant).

## Key files

- `src/backends/vulkan/vio_vulkan.c` / `.h` — state, present-skip, RT impls, texture/descriptor code
- `src/vio_2d.c` / `.h` — new Vulkan flush branch + enum/state field
- `src/vio_2d_vulkan.c` / `.h` (NEW) — pipelines, VBO ring, descriptor ring (mirrors `vio_2d_d3d12.c`)
- `src/vio_render_target.h` / `.c` — `VIO_RT_BACKEND_VULKAN` + `vulkan_*` fields
- `php_vio.c` — RT create/bind/unbind dispatch, deferred-bind in `vio_begin`, font-atlas Vulkan block (~3030), `vio_render_target_texture`, `read_pixels`
- `src/shaders/shaders_2d.h` — possible explicit-binding Vulkan GLSL variant
- `code-tycoon-php/src/Panels/SettingsPanel.php:565` — remove Vulkan exclusion
- Reference (read-only): `vio_2d_d3d12.c`, `vio_d3d12.c` (`vio_d3d12_flush_srv_table`, `d3d12_present`, `d3d12_destroy_render_target`), `backends/opengl/vio_2d_opengl.c`

## Open decision to lock before Phase 1
Swapchain/offscreen color format: **standardize on `B8G8R8A8_UNORM`** (recommended, matches other backends + meaningful golden tests) vs keep existing sRGB.
