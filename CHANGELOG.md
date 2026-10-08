## [2.31.3](https://github.com/phpolygon/php-vio/compare/v2.31.2...v2.31.3) (2026-10-08)


### Bug Fixes

* **opengl:** load glFramebufferTextureMultiviewOVR without GLAD ([4a1a809](https://github.com/phpolygon/php-vio/commit/4a1a80911c9e6452250ef9c7e75608f1232d661c))

## [2.31.2](https://github.com/phpolygon/php-vio/compare/v2.31.1...v2.31.2) (2026-10-08)


### Bug Fixes

* **d3d12:** re-arm the frame list after a mid-frame capture ([90205ab](https://github.com/phpolygon/php-vio/commit/90205ab151b7a36a674e82b008727d344c618e3c))

## [2.31.1](https://github.com/phpolygon/php-vio/compare/v2.31.0...v2.31.1) (2026-10-08)


### Bug Fixes

* **build:** static builds pick vio's own GLAD, recorder D3D11 include only with FFmpeg ([d1c536b](https://github.com/phpolygon/php-vio/commit/d1c536b1515d0865d5b26b99a9e84d94aa0047dc))

# [2.31.0](https://github.com/phpolygon/php-vio/compare/v2.30.0...v2.31.0) (2026-10-08)


### Bug Fixes

* **build:** link the Cocoa platform's frameworks on the module ([afd9fb5](https://github.com/phpolygon/php-vio/commit/afd9fb5df57c8ca25effbe66ae51233c71aa3f24))
* **core:** include zend_exceptions.h for zend_clear_exception ([655b108](https://github.com/phpolygon/php-vio/commit/655b1085fdc7cdf2d32d331a8bd9710fdbd28041))
* **core:** include zend_exceptions.h for zend_clear_exception ([c672782](https://github.com/phpolygon/php-vio/commit/c6727829989f5718290f705682cc908dfd47a767))
* **core:** include zend_exceptions.h for zend_clear_exception ([0b72e2a](https://github.com/phpolygon/php-vio/commit/0b72e2a30172c7d20173565d3338e1a095d1c534))
* **core:** vio_submit_batch binds pipelines like vio_bind_pipeline ([1147ac4](https://github.com/phpolygon/php-vio/commit/1147ac4ca44fcfde6cffbe8dbaa4cc80c01a7b34))
* **core:** vio_submit_batch binds pipelines like vio_bind_pipeline ([81c4c3a](https://github.com/phpolygon/php-vio/commit/81c4c3a5d018ecd5a4346f0d2f77ece2d0463481))
* **core:** vio_submit_batch binds pipelines like vio_bind_pipeline ([a384cc8](https://github.com/phpolygon/php-vio/commit/a384cc8f5c69447d10f3f3ff0fe79f8cd7847f6f))
* **d3d11:** skip draws before any pipeline is bound ([038f5c2](https://github.com/phpolygon/php-vio/commit/038f5c2952c5beaa9c4869bc4d27424f13c6e8f5))
* **d3d11:** vio_read_pixels inside a frame reads the frame so far ([8cef8e8](https://github.com/phpolygon/php-vio/commit/8cef8e8de00b886e03ed1292bc2d6d296d81308a))
* **d3d12:** a render target replaced without an unbind becomes readable ([4f2ed6c](https://github.com/phpolygon/php-vio/commit/4f2ed6ced0fa1e086fa99f88a765ef8c45803663))
* **d3d12:** FXC compiles with unbounded descriptor tables enabled ([1072912](https://github.com/phpolygon/php-vio/commit/1072912f0c62c31b72cc67945bccac7a4a0b069c))
* **d3d12:** keep the bound pipeline across frames, skip draws without one ([8a5e377](https://github.com/phpolygon/php-vio/commit/8a5e37792870ae0684eefada6c04ab362b29c33d))
* **d3d12:** look up work-graph entry nodes without a debug-layer error ([b2e32c5](https://github.com/phpolygon/php-vio/commit/b2e32c53f6922fe367bf8833dd31d64b97103af7))
* **d3d12:** ray tracing libraries without payload access qualifiers ([79ef51b](https://github.com/phpolygon/php-vio/commit/79ef51b10b8df3916a73e247f7db2cb8b704896b))
* **d3d12:** release pipelines freed between frames on the fence ([86ee9b2](https://github.com/phpolygon/php-vio/commit/86ee9b2a8283bd75b87e07de18e809b68423a551))
* **d3d12:** release pipelines freed between frames on the fence ([5c5b43d](https://github.com/phpolygon/php-vio/commit/5c5b43d7d901b2ee7f7c14193a57fa6954d8e0a8))
* **d3d12:** sampler feedback without a bound map ([3aca718](https://github.com/phpolygon/php-vio/commit/3aca7180e9607aa33d9098fa708b3e712321b330))
* **d3d:** geometry-stage gaps - gl_PrimitiveID in and out, input blocks, vertex textures ([c995fb0](https://github.com/phpolygon/php-vio/commit/c995fb0cb9e35707196829784d297ec0056e8b36))
* **metal:** declare the rate-map resolve after vio_render_target.h ([9e2b641](https://github.com/phpolygon/php-vio/commit/9e2b641865c7634ad51ffca91fb6ad3bf897fb6c))
* **metal:** functions named like a Metal type or keyword get a suffix ([f22a3c7](https://github.com/phpolygon/php-vio/commit/f22a3c7726de496af672dd7191846830793ec243))
* **metal:** geometry emulation - strip adjacency, gl_PrimitiveID, stage textures, interface blocks ([688983d](https://github.com/phpolygon/php-vio/commit/688983d485aa324b2aec840f8143aa8582a449b7))
* **metal:** import AppKit now that GLFW's Cocoa header is gone ([08c7576](https://github.com/phpolygon/php-vio/commit/08c7576b81cdde53344a5ff50c6bae507960511d))
* **metal:** leave texture units unbound that the shader does not sample ([768c6a1](https://github.com/phpolygon/php-vio/commit/768c6a1033a57503c9036292db17da556d89296e))
* **metal:** named GPU sections leave out the gap between command buffers ([37b3517](https://github.com/phpolygon/php-vio/commit/37b35170f0efd72dac4af7960aec44bb3b12a43f))
* **metal:** quiet VideoToolbox probing and archive writes; explicit bilinear in test 208 ([0a235b7](https://github.com/phpolygon/php-vio/commit/0a235b7c9733c55a809f08ccfa040d6534183066))
* **metal:** tessellation varyings line up between the stages ([d758a33](https://github.com/phpolygon/php-vio/commit/d758a3333655e3a9cc0397310b0b442140bc00ad))
* **metal:** the frame's GPU time spans every command buffer of the frame ([83e47f9](https://github.com/phpolygon/php-vio/commit/83e47f98d94e8080b9d2017ee31c6753a5f1ce31))
* **metal:** the frame's GPU time spans every command buffer of the frame ([6e69700](https://github.com/phpolygon/php-vio/commit/6e69700441a10e1b66f60d1cfe3b518376e58802))
* **metal:** the frame's GPU time spans every command buffer of the frame ([e7d0fed](https://github.com/phpolygon/php-vio/commit/e7d0fed2dc8eef1babcf6a99b38c985c13d9a649))
* **opengl:** a re-attach of the bound target drops the multiview attachment state ([6d87758](https://github.com/phpolygon/php-vio/commit/6d87758640bc8517c13484654ce18d0fead41a2c))
* **opengl:** gl_ViewID_OVR is a uint - wrap every use in int() ([37d31eb](https://github.com/phpolygon/php-vio/commit/37d31eb04ee3481f1604535c0ba01490fb292785))
* **opengl:** keep GPU frames shorter than the timer resolution ([52d633a](https://github.com/phpolygon/php-vio/commit/52d633afb45dd7a551fcfa81f5c6657ab28ea474))
* **opengl:** num_views only for the vertex stage; CI requires multiview on GL (llvmpipe) and D3D12 (WARP) ([d40df51](https://github.com/phpolygon/php-vio/commit/d40df51ce1a76b74a89550190121d1f29fa14077))
* **opengl:** redeclare gl_PerVertex in every stage so strict linkers accept clip distances ([f6b91db](https://github.com/phpolygon/php-vio/commit/f6b91db2334ddfe37fc5ddf8d33d61212fd9f96b))
* **opengl:** redeclare gl_PerVertex only in stages that use gl_ClipDistance ([00512d9](https://github.com/phpolygon/php-vio/commit/00512d970bdfe1858d436ccbae0fb468f22e2e7b))
* **opengl:** report subgroups, quad operations and 64-bit atomics as 0 ([6727ca9](https://github.com/phpolygon/php-vio/commit/6727ca9638cc7ee976ac35057f829a770186e000))
* **platform:** refuse WGL contexts below OpenGL 3.0 ([8c9c27e](https://github.com/phpolygon/php-vio/commit/8c9c27e34eb8bf734373d7d5ad235d8e5ed6f0ef))
* **platform:** X11 polls see events another client just sent ([c812f3e](https://github.com/phpolygon/php-vio/commit/c812f3e51649a5caa7b00e7d793724f1011c6c1d))
* **render-target:** multisampled MRT targets on OpenGL and D3D11 ([dbbf067](https://github.com/phpolygon/php-vio/commit/dbbf067b5bbac51c4133aeb8760173f619f4fce2))
* **shader:** flattened interface blocks keep Patch and interpolation decorations ([05317f8](https://github.com/phpolygon/php-vio/commit/05317f8ed3aea7ca413097e3c58bafb0a9b7870b))
* **shader:** keep the subgroup marker out of the GL audit gate ([e7eab65](https://github.com/phpolygon/php-vio/commit/e7eab65825faae4c36c180fb2d75c56a4f1434dc))
* **shader:** vio_pipeline returns false when the backend rejects it ([31a0beb](https://github.com/phpolygon/php-vio/commit/31a0beb42b63b6c9dfa83525272b56326f9808f1))
* **shader:** vio_pipeline returns false when the backend rejects it ([e7cec5b](https://github.com/phpolygon/php-vio/commit/e7cec5bdd5d556146a8df5db37ba8f27ebab1fd0))
* **vulkan:** declare vulkan_update_texture before the glyph-atlas upload uses it ([af1f915](https://github.com/phpolygon/php-vio/commit/af1f915ac4fceeab1d0405a388edec33cef9b08b))
* **vulkan:** enable GL_EXT_mesh_shader for perprimitiveEXT fragment inputs ([ab1fd98](https://github.com/phpolygon/php-vio/commit/ab1fd9875a8cfdb3682e4c595a17b79e4b290898))
* **vulkan:** no ray query on lavapipe/aarch64 (BVH build crashes) ([4b7b603](https://github.com/phpolygon/php-vio/commit/4b7b60303e9d9eb16db5eb1b45b481032a47cac7))
* **vulkan:** ray-query stages round-trip through GLSL 460 ([43643f6](https://github.com/phpolygon/php-vio/commit/43643f6bd961c60a01292fa4da31082b87932f56))
* **vulkan:** readback buffers in cached host memory ([2058191](https://github.com/phpolygon/php-vio/commit/20581910ccac853b62ef2683888991ac99ae53b5))
* **vulkan:** unmap a bundle's upload chunks before parking them ([75db1f1](https://github.com/phpolygon/php-vio/commit/75db1f137711746843025a158cacb79d90d7b5cd))
* **vulkan:** validation errors found on an RTX 2080 ([98396e5](https://github.com/phpolygon/php-vio/commit/98396e557ca6adcef3acca8a0a040aae6f8582c3))


### Features

* **compute:** cooperative matrices (VIO_FEATURE_COOPERATIVE_MATRIX) ([1c04c19](https://github.com/phpolygon/php-vio/commit/1c04c1900ca31e6dc50dcf49e0f8533d59f1eac2))
* **core:** bindless cubemaps and texture arrays ([6e99e25](https://github.com/phpolygon/php-vio/commit/6e99e25d565fe8fbe11ffca51735cf9e180e9779))
* **core:** bindless sampler variants at set 1 bindings 2-4 ([fa585f2](https://github.com/phpolygon/php-vio/commit/fa585f2848c0f18709234b20c7b3711f57e43baf))
* **core:** bindless table in compute kernels ([3d273bf](https://github.com/phpolygon/php-vio/commit/3d273bf98b92af90cb441619822024923ab6a9d0))
* **core:** calibration run picks the fastest backend ([9c199e2](https://github.com/phpolygon/php-vio/commit/9c199e25077c9cb654a2b98b9bb0ca2867e8b1ac))
* **core:** multiview by instancing on D3D11 and OpenGL without OVR ([3f5c982](https://github.com/phpolygon/php-vio/commit/3f5c982e3afe0544b5d20e29bc95707e02faaf40))
* **core:** multiview with geometry and tessellation stages ([44243a5](https://github.com/phpolygon/php-vio/commit/44243a5974f224d41b13823dee81fd579334631c))
* **core:** named GPU timestamps with vio_gpu_timestamp / vio_gpu_timings ([4489899](https://github.com/phpolygon/php-vio/commit/4489899679c17622422ae08b0665589980d5c558))
* **core:** recorded draw sequences - vio_bundle, vio_draw_bundle, vio_bundle_info ([9091806](https://github.com/phpolygon/php-vio/commit/90918067cb4aab6294d0f377605afd615f5d8142))
* **core:** score backends for 'auto' with prefer and require ([2092f51](https://github.com/phpolygon/php-vio/commit/2092f51aa990661ec33896ef65b01534da8ac0e4))
* **core:** separate texture and sampler objects on every backend ([65aabc7](https://github.com/phpolygon/php-vio/commit/65aabc77fd2218336068d8f6a0178f5bc6351e98))
* **core:** vertex input layout follows the mesh layout ([4ed9c47](https://github.com/phpolygon/php-vio/commit/4ed9c4703d773dd8e9e57a66365bfadee30fb3d8))
* **core:** vio_adapters() lists adapters without a context ([d0821df](https://github.com/phpolygon/php-vio/commit/d0821df0d43989728f2831076377758a6c740fc0)), closes [hi#performance](https://github.com/hi/issues/performance)
* **core:** vio_backend_info on D3D11, D3D12, Vulkan and OpenGL ([dbade27](https://github.com/phpolygon/php-vio/commit/dbade27e420a29c28e6223beab91eca379ffae62))
* **core:** vio_bind_buffer feeds uniform blocks of graphics shaders ([7db7d88](https://github.com/phpolygon/php-vio/commit/7db7d88b7e49c4c2b44b3b2789a6db17d47fbf73))
* **core:** vio_feature_info tells native from emulated features ([10c1a60](https://github.com/phpolygon/php-vio/commit/10c1a606eb41a1640c72766cab5c55c71ef7b519))
* **core:** vio_texture_release_index frees a bindless slot ([fa5f4ec](https://github.com/phpolygon/php-vio/commit/fa5f4ec904bb99f216483bbe612ce8466b238f2f))
* **core:** vio_upscale - portable spatial and temporal upscaling, MetalFX behind it ([edc81b2](https://github.com/phpolygon/php-vio/commit/edc81b271755e1a860ba05f57c489d8757916512))
* **d3d11,d3d12:** native bundles on a deferred context and as bundle command lists ([d814965](https://github.com/phpolygon/php-vio/commit/d814965618472759cb77341ade18e70f799a4284))
* **d3d11:** multisampled cube and array render targets ([e31eeeb](https://github.com/phpolygon/php-vio/commit/e31eeebbc05da534e3e32fec1e35fdec7adc137d))
* **d3d11:** multisampled depth-only render targets ([d1f651d](https://github.com/phpolygon/php-vio/commit/d1f651db4b75552fca1e87b03d541eee5413a3dc))
* **d3d12:** depth targets with a mip chain ([0bf3899](https://github.com/phpolygon/php-vio/commit/0bf3899c171c8740e4646f19599a5d2bd6530ab2))
* **d3d12:** draw parameters below Shader Model 6.8 ([ab0cd55](https://github.com/phpolygon/php-vio/commit/ab0cd55d3d1a5b8b5568653095e222a891f911ce))
* **d3d12:** highest shader model 6.x, subgroup operations, DXC stage probe ([b0fcfc1](https://github.com/phpolygon/php-vio/commit/b0fcfc12395ce6a3317839ea89cb1ae2d591aec6))
* **d3d12:** multisampled cube and array render targets ([4033025](https://github.com/phpolygon/php-vio/commit/4033025faf53779cb1c194df828d94f7f8c8cd14))
* **d3d12:** multisampled depth-only render targets ([e0a8963](https://github.com/phpolygon/php-vio/commit/e0a8963bf2789a59213755f7f54d0ac9700f1bf1))
* **d3d12:** pin the shader model with shader_model => 60..69 ([fb0aebc](https://github.com/phpolygon/php-vio/commit/fb0aebc7f56fc294ab8ab577008c779032dc8ac7))
* **d3d12:** sampler feedback for texture streaming ([3df0af4](https://github.com/phpolygon/php-vio/commit/3df0af4c447716378fe7d4ac8f365a367742b309))
* **d3d12:** shading-rate image (VRS Tier 2) ([be5cfa0](https://github.com/phpolygon/php-vio/commit/be5cfa028bfec926ac7b734aed456ae6f3116152))
* **d3d12:** work graphs and Agility SDK device factory ([371d021](https://github.com/phpolygon/php-vio/commit/371d021a95fb4b74663d99bfc2f39b3e503f770f))
* **d3d:** run the suite on a NuGet WARP, an Agility SDK and a given DXC ([1263376](https://github.com/phpolygon/php-vio/commit/1263376e9fa386bfeae35dc01102ad9b95fbfd7e))
* **d3d:** VIO_D3D_HEADLESS_HARDWARE keeps headless contexts on the GPU ([311ff26](https://github.com/phpolygon/php-vio/commit/311ff26dbc79766b397ffbb919bcb874d085a303))
* **media:** hardware video encoders, D3D11 without a CPU copy, reading video back ([84de9c6](https://github.com/phpolygon/php-vio/commit/84de9c683b5b9ed657c18d378a35fb86a53220e6))
* **metal:** 'msl' override for compute kernels, the way to MSL 4 tensors ([29d18a3](https://github.com/phpolygon/php-vio/commit/29d18a34995683bfb34041fe90f682218ebeb946))
* **metal:** depth targets with a mip chain ([bd31828](https://github.com/phpolygon/php-vio/commit/bd31828da0f69c8ba6750d932d703abeb74b55f4))
* **metal:** MSL version ladder and vio_backend_info() ([a17e588](https://github.com/phpolygon/php-vio/commit/a17e58899ee84a5cd3c74833bd8de60a27492032))
* **metal:** multisampled cube and array render targets ([4e73ba7](https://github.com/phpolygon/php-vio/commit/4e73ba7294a28e37b935ce5b40e8242832e4e381))
* **metal:** multisampled depth-only render targets ([1a78ae6](https://github.com/phpolygon/php-vio/commit/1a78ae618f0c6e8ebd503480aaead3656d1d50f5))
* **metal:** pipeline binary archive for the on-disk shader cache ([1f0b19e](https://github.com/phpolygon/php-vio/commit/1f0b19e01a827dbd89ef074f73cc01426aa88d8e))
* **metal:** rasterization rate maps on render targets ([0685588](https://github.com/phpolygon/php-vio/commit/0685588e11553cc5e2f926ce1b1b3078058e6f9e))
* **opengl:** multisampled cube and array render targets ([7242e2e](https://github.com/phpolygon/php-vio/commit/7242e2e8ece78df5e0709c3548c003eeac706a7c))
* **opengl:** multisampled depth-only render targets ([4cc6882](https://github.com/phpolygon/php-vio/commit/4cc68826ce6d51dba8a2d27e21446fff90c77844))
* **opengl:** subgroup and quad operations through the caller's GLSL ([63f4679](https://github.com/phpolygon/php-vio/commit/63f4679fd9fe3aabae3c41134282445688eb1ad3))
* **platform:** native Cocoa platform ([5990b9e](https://github.com/phpolygon/php-vio/commit/5990b9e76975605725337087ebd087a3298e15cd))
* **platform:** native Wayland platform ([7c317fa](https://github.com/phpolygon/php-vio/commit/7c317fabd4ca8f3320792a3f1479994308870c02))
* **platform:** native Win32 platform, OpenGL without GLFW ([c92137e](https://github.com/phpolygon/php-vio/commit/c92137ec3cf528fa460d813808251a9eca1ac4c0))
* **platform:** native X11 platform ([b03d030](https://github.com/phpolygon/php-vio/commit/b03d0301cf5bc9b544e391a0b1adc53d34831b59))
* **platform:** window system behind a vio_platform vtable ([e4391c5](https://github.com/phpolygon/php-vio/commit/e4391c58895fab0038f40be11ad083ca4f7638f9))
* **raytracing:** opacity micromaps (DXR 1.2 / VK_EXT_opacity_micromap) ([b6d12bc](https://github.com/phpolygon/php-vio/commit/b6d12bc97b0849d986a52a020e49731c803e6e5f))
* **raytracing:** several miss shaders, hit groups and callables with shader records ([7f9d5b5](https://github.com/phpolygon/php-vio/commit/7f9d5b532ec6f7f90480a3f42977d162c4416a63))
* **raytracing:** Shader Execution Reordering (Shader Model 6.9 / DXR 1.2) ([022056a](https://github.com/phpolygon/php-vio/commit/022056abed836923d21c1a95c2d4be7b76bdf0b3))
* **raytracing:** vio_acceleration_structure_update refits or rebuilds the top level ([e512dd2](https://github.com/phpolygon/php-vio/commit/e512dd22e355591624a85881b3f9d14756a56c13))
* **raytracing:** vio_rt_bind_texture binds textures for ray tracing stages ([e380a1a](https://github.com/phpolygon/php-vio/commit/e380a1acfafd75d5c330c49d8d5a72bb97477bcd))
* **raytracing:** vio_trace_rays inside vio_begin / vio_end ([d14f478](https://github.com/phpolygon/php-vio/commit/d14f4785cfdc45b47fccda0d7fb332518544a0e8))
* **render-target:** depth targets with a mip chain on OpenGL and D3D11 ([eb9cd56](https://github.com/phpolygon/php-vio/commit/eb9cd56f90f351e3fb456377a4da6f811bc7922b))
* **shader:** 64-bit atomics on storage buffers (D3D12, Vulkan, OpenGL) ([141e773](https://github.com/phpolygon/php-vio/commit/141e77327df306cafc38d5b00991caf28bea8111))
* **shader:** bindless texture table on Metal, Vulkan and D3D12 ([7b5dee1](https://github.com/phpolygon/php-vio/commit/7b5dee10ce779e8b505f528c892950ecd53d3def))
* **shader:** float16, draw parameters and compute derivatives ([ed4c17b](https://github.com/phpolygon/php-vio/commit/ed4c17b556558197c1f397361a21892eb30a1e5e))
* **shader:** inline ray tracing (ray query) with acceleration structures ([9c4c2c0](https://github.com/phpolygon/php-vio/commit/9c4c2c0baadb1646a00fffb073a3f842eee81a57))
* **shader:** mesh and task shaders (D3D12, Vulkan, Metal) ([57d7479](https://github.com/phpolygon/php-vio/commit/57d7479b30e5682356a19610b17f5de96efbbe0a))
* **shader:** multiview on Metal, Vulkan, D3D12 and OpenGL ([8656a82](https://github.com/phpolygon/php-vio/commit/8656a822947569eea1ce35c0520fd921e86ab6da))
* **shader:** per-primitive shading rate (D3D12, Vulkan) ([18b99e3](https://github.com/phpolygon/php-vio/commit/18b99e364d0ee08a7a95437200b51007bf72bc6c))
* **shader:** quad operations and barycentric coordinates ([645d343](https://github.com/phpolygon/php-vio/commit/645d343ab0a697c590daebdca0a363f21837c3d3))
* **shader:** ray tracing pipeline (raygen/miss/hit) on D3D12 and Vulkan ([0af3302](https://github.com/phpolygon/php-vio/commit/0af3302d5b65055e5479640ea314260d499d456c))
* **shader:** sampler feedback from GLSL on every backend with fragment storage ([edacb2b](https://github.com/phpolygon/php-vio/commit/edacb2bb01c2fd976384cbebad46dbda8051cc63))
* **shader:** Shader Model 6.9 long vectors, 'hlsl' override for compute kernels ([ebbc7c9](https://github.com/phpolygon/php-vio/commit/ebbc7c910b6d3eff9a849422c069e443ad5657a2))
* **shader:** subgroup operations on Metal, Vulkan and OpenGL ([de04724](https://github.com/phpolygon/php-vio/commit/de04724ab300e012be18a01b44638c2e944f8582))
* **shader:** textures in mesh and task stages ([5f792e1](https://github.com/phpolygon/php-vio/commit/5f792e1d90ba2f2142afeb85a8f4b4b63247303b))
* **shader:** writable storage buffers in the fragment stage ([2a32c44](https://github.com/phpolygon/php-vio/commit/2a32c44fdeed21be37ce5345c141e365d0f3ec18))
* **tess:** blocks, struct / matrix varyings, clip distances and barrier reads in generated hull / domain shaders ([0c53b2e](https://github.com/phpolygon/php-vio/commit/0c53b2e01b51ddb08f3c089231d1408e47207827))
* **text:** glyph atlas fills on demand with sub-image uploads ([7bab93b](https://github.com/phpolygon/php-vio/commit/7bab93b2208877a9ccd04a682e1ef25ef6e8b22f))
* **texture:** ASTC textures ([56ee270](https://github.com/phpolygon/php-vio/commit/56ee270a8783114d8e2d673b23109e06ae08ee24))
* **texture:** KTX2 cubemaps and 3D textures ([2bcee0d](https://github.com/phpolygon/php-vio/commit/2bcee0d859e7cd4037257716e3eca378ae9063c1))
* **text:** vertical text ([64df4bb](https://github.com/phpolygon/php-vio/commit/64df4bb296f0c289e4f2354e51cb1971a85273e2))
* **vulkan:** barriers through synchronization2, layout transitions with precise scopes ([b72d3fa](https://github.com/phpolygon/php-vio/commit/b72d3fa3a1b44c7d12373d206feaa6cb2ba7ed43))
* **vulkan:** bundles as secondary command buffers ([b3d3be7](https://github.com/phpolygon/php-vio/commit/b3d3be7c93b003bfeba80b8136de83bd1b226b9f))
* **vulkan:** depth targets with a mip chain ([f142d04](https://github.com/phpolygon/php-vio/commit/f142d047578601c490f41d83fa5e53cb419cc9d8))
* **vulkan:** dynamic rendering instead of render-pass and framebuffer objects ([0b669a2](https://github.com/phpolygon/php-vio/commit/0b669a2522d6cbacf0a94ce81bff41d1295d421d))
* **vulkan:** multisampled cube and array render targets ([6f932fc](https://github.com/phpolygon/php-vio/commit/6f932fc348aa4eeb6fa8aa019c862269c69b461d))
* **vulkan:** multisampled depth-only render targets ([8a7553a](https://github.com/phpolygon/php-vio/commit/8a7553a756586c6376bcef4d58020598bcb22794))
* **vulkan:** one timeline semaphore for every submission; render targets read back mid-frame ([9671a72](https://github.com/phpolygon/php-vio/commit/9671a72f49dd547bd495bc56bfbf884f96ae019f))
* **vulkan:** require Vulkan 1.2+ with dynamic rendering, synchronization2 and timeline semaphores ([49550b2](https://github.com/phpolygon/php-vio/commit/49550b2994cadbe641ce57575170a07dd97cf476))
* **vulkan:** shading-rate image on dynamic rendering (A18) ([2317be0](https://github.com/phpolygon/php-vio/commit/2317be098709f4df56effb2fb816c1178de6c6d2))


### Performance Improvements

* **vulkan:** vio_read_pixels waits the frame's fence, not the device ([fdbc605](https://github.com/phpolygon/php-vio/commit/fdbc6051d9535ec976ba76840fbac80db6798a9f))

# [2.30.0](https://github.com/phpolygon/php-vio/compare/v2.29.1...v2.30.0) (2026-10-05)


### Features

* **metal:** geometry stage through compute kernels ([a1c7a85](https://github.com/phpolygon/php-vio/commit/a1c7a8580f84908002b361b83b0ecda38e284c7a))
* **metal:** HDR10 output and frame latency ([289d348](https://github.com/phpolygon/php-vio/commit/289d348a0e5c553c1251fc09b2220dd7f1c1f425))
* **metal:** stencil plane on depth_only, cube and array render targets ([0c71fd5](https://github.com/phpolygon/php-vio/commit/0c71fd5669e8bd5e376ab3b30801df44df50699b))
* **metal:** tessellation isolines and point_mode ([a7764d1](https://github.com/phpolygon/php-vio/commit/a7764d144a23bf8dd6c50145f7370f5e4b517c36))

## [2.29.1](https://github.com/phpolygon/php-vio/compare/v2.29.0...v2.29.1) (2026-10-03)


### Bug Fixes

* **d3d12:** generate mipmaps inside a frame without draining the GPU ([93043c7](https://github.com/phpolygon/php-vio/commit/93043c70f12db4a609baf969c6821a2f9768769b))
* vio_set_uniform("name[i]") for arrays of matrices and vectors ([2e9c1cc](https://github.com/phpolygon/php-vio/commit/2e9c1ccb75d66163023ac3e7339f5d6aabc3df21))

# [2.29.0](https://github.com/phpolygon/php-vio/compare/v2.28.0...v2.29.0) (2026-10-03)


### Bug Fixes

* **metal,opengl:** tessellation conventions found by test 144 in CI ([9c4e0e4](https://github.com/phpolygon/php-vio/commit/9c4e0e49450b8b0beed2ec57e0c87fdca1e6c01c))
* **vulkan:** lower-left tessellation domain origin ([1faf636](https://github.com/phpolygon/php-vio/commit/1faf636c22c7847a16d2e64050da58822113c8d2))


### Features

* **d3d:** hull and domain shaders from GLSL tessellation stages ([19ae153](https://github.com/phpolygon/php-vio/commit/19ae153aa0f4d4ddcc935ee9b2a72ebbf2398c67)), closes [KhronosGroup/SPIRV-Cross#2693](https://github.com/KhronosGroup/SPIRV-Cross/issues/2693) [#2694](https://github.com/phpolygon/php-vio/issues/2694)
* vio_gpu_info through a backend vtable slot ([a1910ce](https://github.com/phpolygon/php-vio/commit/a1910cebee2ea466aa47d7777041ddeabedb7ac3)), closes [#ifdefs](https://github.com/phpolygon/php-vio/issues/ifdefs) [#if](https://github.com/phpolygon/php-vio/issues/if)

# [2.28.0](https://github.com/phpolygon/php-vio/compare/v2.27.0...v2.28.0) (2026-10-03)


### Bug Fixes

* **batch:** bind record pipelines on OpenGL in vio_submit_batch ([92b9fa6](https://github.com/phpolygon/php-vio/commit/92b9fa65c5a0fedc431bdd2b12c265d4ee2fc37a))
* **d3d11:** comparison sampler for depth cubes ([a25fe00](https://github.com/phpolygon/php-vio/commit/a25fe001d0fb32c08cbeb9a4075a2860002cb4db))
* **d3d:** compile geometry / hull / domain stages through the cached path ([e7b2e5c](https://github.com/phpolygon/php-vio/commit/e7b2e5c76e7ae56f94db6f439d95005d3f338efd))
* **d3d:** remap depth on every geometry-shader vertex ([02cb0d0](https://github.com/phpolygon/php-vio/commit/02cb0d05adfef25b311be0564473c9063a35ce5d))
* **metal:** vio_viewport drops the vio_viewports scissors; 141 cube depths inside 0..1 ([3625b3b](https://github.com/phpolygon/php-vio/commit/3625b3bcf2d5b6d53ad7271d667743e6a3249f88))
* **opengl:** comparison sampling for sampler*Shadow ([58fc126](https://github.com/phpolygon/php-vio/commit/58fc1261735819cafbaff40156fb6e234566617c))
* **shader:** declare the viewport/layer extension SPIRV-Cross omits ([2f0cc2e](https://github.com/phpolygon/php-vio/commit/2f0cc2e1de0b2dc2664e9cc72596021d1c12a06c)), closes [#extension](https://github.com/phpolygon/php-vio/issues/extension) [#version](https://github.com/phpolygon/php-vio/issues/version)
* **shader:** patch topology on indirect draws, refuse patches without tessellation ([6b13e9a](https://github.com/phpolygon/php-vio/commit/6b13e9a685fb73100a51dba979d1bc6af2780908))
* **vulkan:** re-binding a render target keeps colour and depth ([29c5940](https://github.com/phpolygon/php-vio/commit/29c5940c0a7e31c4d8e97ada131b3c5f2a73c340))


### Features

* **d3d11:** array and depth-cube render targets ([a497b9c](https://github.com/phpolygon/php-vio/commit/a497b9c0d590294395e7aabbf016fb3ab5253cfa))
* **d3d11:** layered rendering ([a9fee65](https://github.com/phpolygon/php-vio/commit/a9fee6529fcf5fa41bebbe4c382562c0116f4adf))
* **d3d12:** array and depth-cube render targets ([df0c9b7](https://github.com/phpolygon/php-vio/commit/df0c9b75dae186af1577489e9d67c42eca8b4d47))
* **d3d12:** layered rendering ([896371a](https://github.com/phpolygon/php-vio/commit/896371a7538c705a350c22a2c73168d96cdb6515))
* **d3d:** gl_in[].gl_Position and gl_InvocationID in GLSL geometry stages ([659716b](https://github.com/phpolygon/php-vio/commit/659716b00ea69856422d8a48b10f6a1f12d3f62b))
* **d3d:** HLSL overrides for the geometry and tessellation stages ([e51ddaf](https://github.com/phpolygon/php-vio/commit/e51ddafc5d149305771e89ea7bb343d0a3a9aa15))
* **d3d:** multiple viewports ([fb432d9](https://github.com/phpolygon/php-vio/commit/fb432d9c92ad3546877c0c53cd4f6270a26eae4c))
* **metal:** stencil, layered render targets, gl_Layer, multiple viewports ([dd57ce5](https://github.com/phpolygon/php-vio/commit/dd57ce523d7f659cffa189038f6fc53ec13bf283))
* **metal:** tessellation stages via compute kernels and drawPatches ([33002eb](https://github.com/phpolygon/php-vio/commit/33002eb5237ffe4bc6a31d14d034126be686b511))
* **render-target:** array and depth-cube render targets ([df29559](https://github.com/phpolygon/php-vio/commit/df29559d9e4d9ac28932de4b3a5fcd267b374f2f))
* **render-target:** layered rendering with VIO_RT_ALL_LAYERS ([274353e](https://github.com/phpolygon/php-vio/commit/274353e3138b3a6ad7297bcaa5688c6f1b194ad5))
* **shader:** geometry and tessellation stages for vio_shader ([c01044c](https://github.com/phpolygon/php-vio/commit/c01044c70e9927e1bdeb61558f024c1df9469345))
* **shader:** GS instancing flag and adjacency topologies ([b2c573b](https://github.com/phpolygon/php-vio/commit/b2c573b8a54dae6ca237dd8756b2c2fae624cfad))
* **viewport:** vio_viewports for several viewports at once ([db14754](https://github.com/phpolygon/php-vio/commit/db14754514f9f68e585ca56bebad55e755bc0a17))
* **vulkan:** array and depth-cube render targets ([6561510](https://github.com/phpolygon/php-vio/commit/6561510ff709d2d60fc4b4f0ecec1ec48b3a3ea6))
* **vulkan:** geometry and tessellation stages ([1362f9c](https://github.com/phpolygon/php-vio/commit/1362f9ca0001faa37ab80180e5b1f08c68de90ec))
* **vulkan:** layered rendering ([b3df4f1](https://github.com/phpolygon/php-vio/commit/b3df4f19ee0754906b32b62c62ad91fb555d643b))
* **vulkan:** multiple viewports ([d4bba15](https://github.com/phpolygon/php-vio/commit/d4bba1504e06940fc478ae313bc03235aa79458b))
* **vulkan:** texture updates, storage images and async compute in the frame ([39652e8](https://github.com/phpolygon/php-vio/commit/39652e8365848eeea103b69ea6f15ef883c7f56d))

# [2.27.0](https://github.com/phpolygon/php-vio/compare/v2.26.0...v2.27.0) (2026-09-23)


### Features

* **input:** route injected input through the OS event path ([9151812](https://github.com/phpolygon/php-vio/commit/91518121d62e799166f62a4d0ac99b7b8fa76c01))
* **input:** virtual gamepads and input record/replay ([071985c](https://github.com/phpolygon/php-vio/commit/071985c12055c6ed731694a611fdf3b86a0bb679))

# [2.26.0](https://github.com/phpolygon/php-vio/compare/v2.25.4...v2.26.0) (2026-09-19)


### Features

* **headless:** multisample the offscreen surface ([db7cf5b](https://github.com/phpolygon/php-vio/commit/db7cf5b1fecd02a0e47c0e2f48a5813ffa9c665d))

## [2.25.4](https://github.com/phpolygon/php-vio/compare/v2.25.3...v2.25.4) (2026-09-15)


### Bug Fixes

* **readback:** capture the framebuffer, not the size the window was created with ([394a372](https://github.com/phpolygon/php-vio/commit/394a3724dd368d6aa386c7304fd80b5c673058d8))

## [2.25.3](https://github.com/phpolygon/php-vio/compare/v2.25.2...v2.25.3) (2026-09-14)


### Bug Fixes

* **input:** report the cursor in the space vio_window_size describes ([0178ce7](https://github.com/phpolygon/php-vio/commit/0178ce766c187d20b32ad04ad3077faf7d0d06fa))

## [2.25.2](https://github.com/phpolygon/php-vio/compare/v2.25.1...v2.25.2) (2026-09-13)


### Bug Fixes

* **window:** size the window in the space vio_window_size reports ([6b1c3e5](https://github.com/phpolygon/php-vio/commit/6b1c3e54d8c8a8d0a29dbea647f0242b3b8f1af9))

## [2.25.1](https://github.com/phpolygon/php-vio/compare/v2.25.0...v2.25.1) (2026-09-13)


### Bug Fixes

* **font:** pack the currency symbols and general punctuation block ([31bb47f](https://github.com/phpolygon/php-vio/commit/31bb47f52b817d2b070a52b57f11f8d930d496ab))

# [2.25.0](https://github.com/phpolygon/php-vio/compare/v2.24.3...v2.25.0) (2026-09-11)


### Features

* **text:** shaped text as CPU bitmaps via VioFontFace and vio_text_bitmap ([f8b2838](https://github.com/phpolygon/php-vio/commit/f8b2838651647b9244bb49ef9efe9901e0c3e9b2))

## [2.24.3](https://github.com/phpolygon/php-vio/compare/v2.24.2...v2.24.3) (2026-09-11)


### Bug Fixes

* **compute:** keep the params of every async dispatch ([1e6fe0f](https://github.com/phpolygon/php-vio/commit/1e6fe0fc77237744a830c48fb21adace7bc4784b))

## [2.24.2](https://github.com/phpolygon/php-vio/compare/v2.24.1...v2.24.2) (2026-09-11)


### Bug Fixes

* **opengl:** declare the S3TC enums the core-profile glad lacks ([b4d40dc](https://github.com/phpolygon/php-vio/commit/b4d40dc4aa1d1d63da51ef44663c0524e19c4706))

## [2.24.1](https://github.com/phpolygon/php-vio/compare/v2.24.0...v2.24.1) (2026-09-11)


### Bug Fixes

* **compute:** writable seeded storage buffers, slot rebinds, update_buffer offset ([09b0a6a](https://github.com/phpolygon/php-vio/commit/09b0a6a553cee6ffc943a644f7422ebe652ddf48))

# [2.24.0](https://github.com/phpolygon/php-vio/compare/v2.23.0...v2.24.0) (2026-09-10)


### Features

* **vulkan:** HDR10 swapchain output (GAP-PHASE5 Block 10d) ([0383b2e](https://github.com/phpolygon/php-vio/commit/0383b2e389668e4148e53f58bc110d4b93ff3452))

# [2.23.0](https://github.com/phpolygon/php-vio/compare/v2.22.0...v2.23.0) (2026-09-10)


### Bug Fixes

* **core:** a failing 'auto' candidate stays quiet while another one remains ([3f87d5e](https://github.com/phpolygon/php-vio/commit/3f87d5e610ae70659c36ac1e6ab2230d8deb65af))


### Features

* **vulkan:** texture arrays, BC and KTX2 payloads, variable rate shading (GAP-PHASE5 Block 10c) ([0469c61](https://github.com/phpolygon/php-vio/commit/0469c614e84d19308dbf4f3e7366226517c2a9ad))

# [2.22.0](https://github.com/phpolygon/php-vio/compare/v2.21.0...v2.22.0) (2026-09-10)


### Features

* **vulkan:** MRT, MSAA and cube render targets, cubemaps, mipmaps (GAP-PHASE5 Block 10b) ([63adeda](https://github.com/phpolygon/php-vio/commit/63adeda76cc79d9672e916cf207bca42add60aae))

# [2.21.0](https://github.com/phpolygon/php-vio/compare/v2.20.0...v2.21.0) (2026-09-10)


### Features

* **vulkan:** 3D pipeline (GAP-PHASE5 Block 10a) ([4a1d705](https://github.com/phpolygon/php-vio/commit/4a1d705a14e390b95137e745a215757c5275df8f))

# [2.20.0](https://github.com/phpolygon/php-vio/compare/v2.19.0...v2.20.0) (2026-09-10)


### Features

* **d3d12:** variable rate shading (vio_set_shading_rate) ([db48ff0](https://github.com/phpolygon/php-vio/commit/db48ff09dabbe90d72c474b8db5dd4c5b90220e7))

# [2.19.0](https://github.com/phpolygon/php-vio/compare/v2.18.0...v2.19.0) (2026-09-10)


### Features

* **core:** texture arrays, BC-compressed textures and a KTX2 loader ([b9b7ca1](https://github.com/phpolygon/php-vio/commit/b9b7ca1ead595ece485a756840bb2e582b7543e4))

# [2.18.0](https://github.com/phpolygon/php-vio/compare/v2.17.0...v2.18.0) (2026-09-10)


### Features

* **d3d12:** GPU mip generation via compute downsample; Metal honours 'anisotropy' ([e0f8ffb](https://github.com/phpolygon/php-vio/commit/e0f8ffbf081197b3aca46d1b8f752d9dd3905d75))

# [2.17.0](https://github.com/phpolygon/php-vio/compare/v2.16.0...v2.17.0) (2026-09-10)


### Features

* **core:** indirect draws from a storage buffer (vio_draw_indirect) ([f0cfe03](https://github.com/phpolygon/php-vio/commit/f0cfe03eeb9b476030bda9cf380457c0a8c51d86))

# [2.16.0](https://github.com/phpolygon/php-vio/compare/v2.15.0...v2.16.0) (2026-09-10)


### Features

* **d3d12:** Shader Model 6 via DXC (vio_create 'shader_model' => 6) ([487f73e](https://github.com/phpolygon/php-vio/commit/487f73e5367b52813da3a48e146498a0cf3ed0fc))

# [2.15.0](https://github.com/phpolygon/php-vio/compare/v2.14.0...v2.15.0) (2026-09-10)


### Features

* **d3d:** HDR10 swapchain output (RGB10A2 + ST 2084) with PQ-encoded 2D batch ([e068605](https://github.com/phpolygon/php-vio/commit/e06860531202b6ee724c8d398ca02447d9d1e9f0))

# [2.14.0](https://github.com/phpolygon/php-vio/compare/v2.13.0...v2.14.0) (2026-09-10)


### Features

* **core:** on-disk shader and pipeline cache (vio_create 'shader_cache') ([95ebaeb](https://github.com/phpolygon/php-vio/commit/95ebaeb7ae32ff26357f0e51dfd8234df5046ff6))
* **d3d:** waitable swapchain with a frame-latency cap, vio_swapchain_info() ([8612608](https://github.com/phpolygon/php-vio/commit/86126089b6078732af59a97ad0af5d436bfeadb9))

# [2.13.0](https://github.com/phpolygon/php-vio/compare/v2.12.0...v2.13.0) (2026-09-10)


### Features

* **core:** GPU frame time via timestamp queries on every backend ([93a8b89](https://github.com/phpolygon/php-vio/commit/93a8b89e09a0d7bad8f22102483ff9f3aa8599e4))
* **mesh:** 16-bit index buffers when every index fits ([f1201fc](https://github.com/phpolygon/php-vio/commit/f1201fcba4b067ad054f7cabe88dd639ab1270cd))

# [2.12.0](https://github.com/phpolygon/php-vio/compare/v2.11.0...v2.12.0) (2026-09-10)


### Features

* **pipeline:** stencil state on every 3D backend, multisampled render targets on D3D12 ([d8685de](https://github.com/phpolygon/php-vio/commit/d8685deb6498cf6e698132252401098f45a9f45a))

# [2.11.0](https://github.com/phpolygon/php-vio/compare/v2.10.2...v2.11.0) (2026-09-10)


### Features

* **pipeline:** blend mode and colour write mask per attachment (MRT) ([3d2910a](https://github.com/phpolygon/php-vio/commit/3d2910af0d11bb3d4c600a7ae3a493aea6f0fc7f))

## [2.10.2](https://github.com/phpolygon/php-vio/compare/v2.10.1...v2.10.2) (2026-09-10)


### Bug Fixes

* **bind:** the draw-time bind table owns a reference to every pending texture ([63c995f](https://github.com/phpolygon/php-vio/commit/63c995f90e8361322a3af6fcd6824f678081f8cb))

## [2.10.1](https://github.com/phpolygon/php-vio/compare/v2.10.0...v2.10.1) (2026-09-09)


### Bug Fixes

* **opengl:** resolve vio_set_uniform names against SPIRV-Cross's flattened uniform structs ([682a882](https://github.com/phpolygon/php-vio/commit/682a8823b11ae27672f42fcf70b1b779fb204621)), closes [#version](https://github.com/phpolygon/php-vio/issues/version) [#version](https://github.com/phpolygon/php-vio/issues/version)

# [2.10.0](https://github.com/phpolygon/php-vio/compare/v2.9.0...v2.10.0) (2026-09-09)


### Bug Fixes

* **2d:** batch owns texture/font refs; mat4 vertex inputs on D3D; root CBV for storage-instanced draws ([fa43716](https://github.com/phpolygon/php-vio/commit/fa437162d3aa2f3aa3d456110330d85c5546487c))
* **opengl:** per-context generation stamps so stale objects never delete a new context's GL names ([861115d](https://github.com/phpolygon/php-vio/commit/861115d769b7497c9b9f4ecbcaf687cd9717813a))
* **window:** 1:1 headless surfaces and unscaled headless mouse coordinates ([428fc4d](https://github.com/phpolygon/php-vio/commit/428fc4deaa4e3af024df912423c76a3b97e5ec76))


### Features

* **backends:** honest capability flags, 3D-aware auto selection, audit gate 099 ([f490b56](https://github.com/phpolygon/php-vio/commit/f490b5639aabab165c4f35e7e84422d5d61338ee)), closes [#if](https://github.com/phpolygon/php-vio/issues/if)
* **d3d12:** sampler heap, async upload queue, DEFAULT-heap meshes, mip chains ([f5659a5](https://github.com/phpolygon/php-vio/commit/f5659a5ee9862680456abc1eb83b9f4e5e6d73f3))
* **rt:** render targets behind the vtable on D3D11/D3D12; MSAA on D3D11 and OpenGL ([948bae1](https://github.com/phpolygon/php-vio/commit/948bae1eba65d46bc77822f8a09db71d0d65363c))
* **vulkan:** vsync present mode, samplerAnisotropy, persistent transient pool ([ad95271](https://github.com/phpolygon/php-vio/commit/ad9527132e597bc8797cf42ebb8cd4e9fdb0813b))

# [2.9.0](https://github.com/phpolygon/php-vio/compare/v2.8.0...v2.9.0) (2026-09-08)


### Bug Fixes

* **d3d12:** include vio_texture.h for the storage-image bind ([4eeb7fa](https://github.com/phpolygon/php-vio/commit/4eeb7fa55e35fec55217cd78e2036a1d5c3cb6a6))
* **d3d:** defined initial render-target contents; test expectations for D3D12/WARP ([757cc76](https://github.com/phpolygon/php-vio/commit/757cc76de45cf4d6e9ca551d7f60b8700ca0bd9d))
* **d3d:** draw-time texture/cubemap resolution on D3D11/D3D12, D3D12 cubemap upload + render-target readback ([2008331](https://github.com/phpolygon/php-vio/commit/20083318354d24c27beacfec00fba62f9f21a8c5))
* **d3d:** latch vio_clear before vio_begin; make D3D12 follow-ups skip cleanly on Windows CI ([1d98d68](https://github.com/phpolygon/php-vio/commit/1d98d685fb38d1c846a5512f1577a0da2197e6c7))
* **metal:** resolve texture binds at draw time; patch SPIRV-Cross struct-array stride ([b8ba9f3](https://github.com/phpolygon/php-vio/commit/b8ba9f38b55cc163784e7f06774b7cce21208837))


### Features

* **api:** vio_texture_update, pipeline destructor, 1:1 headless metrics; fix flat mesh layouts on OpenGL ([2d085d8](https://github.com/phpolygon/php-vio/commit/2d085d80a6d2c9f76b9010045f0f753fc9d24a0b))
* **compute:** async dispatch inside the frame + vio_compute_wait ([2575141](https://github.com/phpolygon/php-vio/commit/2575141ece72d2a53c2f5daed1fac5fe0992f9b3))
* **compute:** storage images (image2D/image3D) and reflected dispatch geometry ([645cc31](https://github.com/phpolygon/php-vio/commit/645cc31dbda903d95556716262608311c9d9c6d7))
* **metal:** wire the 3D pipeline with D3D11/D3D12 parity ([e0ba90a](https://github.com/phpolygon/php-vio/commit/e0ba90aabeef3363cfd96b51c06aa119462af52c))
* **pipeline:** depth_write, color_mask and premultiplied/multiply/screen/min/max blend modes ([e7fb2f7](https://github.com/phpolygon/php-vio/commit/e7fb2f75b48b9121845b7051f4fc5195da6cd931))
* **rt:** cubemap render targets, per-face binds and vio_generate_mipmaps ([7d1fcaa](https://github.com/phpolygon/php-vio/commit/7d1fcaaddd243e496f60601e32f381830ceb6816))
* **rt:** multiple render targets (up to 4 colour attachments, 8 formats) ([3b30024](https://github.com/phpolygon/php-vio/commit/3b30024ddfaf431cf041f4954e374373dea1ad12))
* **rt:** vio_read_render_target + eager vio_clear on OpenGL + defined initial RT contents ([ecf94b0](https://github.com/phpolygon/php-vio/commit/ecf94b0223f61e6b7f8f618c5d8e436c21f6e6f2))

# [2.8.0](https://github.com/phpolygon/php-vio/compare/v2.7.4...v2.8.0) (2026-07-25)


### Features

* **compute:** vertex-stage storage buffers for readback-free instancing ([80d89c2](https://github.com/phpolygon/php-vio/commit/80d89c29e169a5f4f2c32186ac6d86ff1ee2459b))

## [2.7.4](https://github.com/phpolygon/php-vio/compare/v2.7.3...v2.7.4) (2026-07-17)


### Bug Fixes

* **window:** keep fullscreen windows from auto-minimizing on focus loss ([9b25227](https://github.com/phpolygon/php-vio/commit/9b25227c6215372ee5376a8b6dda452d4f01bd0a))

## [2.7.3](https://github.com/phpolygon/php-vio/compare/v2.7.2...v2.7.3) (2026-07-17)


### Bug Fixes

* **metal:** headless viewport/scissor scale 1:1 (match logical offscreen) ([1d09b18](https://github.com/phpolygon/php-vio/commit/1d09b18948ec0b3368e98b27b3944e750fb6365b))

## [2.7.2](https://github.com/phpolygon/php-vio/compare/v2.7.1...v2.7.2) (2026-07-17)


### Bug Fixes

* **metal:** headless offscreen readback at 1:1 (no Retina 2x quadrant) ([0dd5ec2](https://github.com/phpolygon/php-vio/commit/0dd5ec2b21bdca4096ab27d24f1af933048d001a))

## [2.7.1](https://github.com/phpolygon/php-vio/compare/v2.7.0...v2.7.1) (2026-07-16)


### Bug Fixes

* **opengl:** guard font-atlas deletion against a dead GL context ([701d189](https://github.com/phpolygon/php-vio/commit/701d189dbf4d54f7b00281a7403f6d9913836dab))
* **text:** free per-glyph atlas strings (ZVAL_PTR_DTOR on the shape-atlas hash) ([6d764ac](https://github.com/phpolygon/php-vio/commit/6d764ac76044aaaab6545bb735078817c1eac6ef))

# [2.7.0](https://github.com/phpolygon/php-vio/compare/v2.6.1...v2.7.0) (2026-07-16)


### Features

* **text:** add vio_font_has_glyph for reliable fallback coverage ([7fe20aa](https://github.com/phpolygon/php-vio/commit/7fe20aa8310ea448cb00edb877ae0d5d29d1a33d))

## [2.6.1](https://github.com/phpolygon/php-vio/compare/v2.6.0...v2.6.1) (2026-07-16)


### Bug Fixes

* **vulkan:** compile the VMA wrapper as C++17 (Linux aligned_alloc) ([55538d5](https://github.com/phpolygon/php-vio/commit/55538d5734fe216cacfd68208e240aabc21b3353))

# [2.6.0](https://github.com/phpolygon/php-vio/compare/v2.5.0...v2.6.0) (2026-07-16)


### Bug Fixes

* **metal:** correct headless pixel readback via blit to shared buffer ([8857006](https://github.com/phpolygon/php-vio/commit/885700670cbd2ccbe510042119ceae115523a11b))


### Features

* **window:** extend OpenGL context ladder down to GL 3.0 ([b8d39c5](https://github.com/phpolygon/php-vio/commit/b8d39c5220cc6d381a63b2dfdc991f8e85b2f1cf))

# [2.5.0](https://github.com/phpolygon/php-vio/compare/v2.4.3...v2.5.0) (2026-07-14)


### Bug Fixes

* **test:** normalise path separators so the GL audit gate works on Windows ([eeafd93](https://github.com/phpolygon/php-vio/commit/eeafd938401b5e6a12248815ee7fe6f43f11de10))


### Features

* **d3d12:** make the in-flight frame count configurable ([7c93276](https://github.com/phpolygon/php-vio/commit/7c93276524c9c51adcbb6529aca8be5a3ed6ec3e))

## [2.4.3](https://github.com/phpolygon/php-vio/compare/v2.4.2...v2.4.3) (2026-07-14)


### Performance Improvements

* **d3d11:** stop pushing every frame across PCIe for a readback nobody asked for ([c2cdf11](https://github.com/phpolygon/php-vio/commit/c2cdf11a03247c717cbf8eeb81097e0916583fc1))

## [2.4.2](https://github.com/phpolygon/php-vio/compare/v2.4.1...v2.4.2) (2026-07-14)


### Bug Fixes

* **d3d:** honour the DXGI tearing contract on D3D11 and D3D12 ([751449f](https://github.com/phpolygon/php-vio/commit/751449fa48a667fe79d116d60b1e4d888d362d4f))

## [2.4.1](https://github.com/phpolygon/php-vio/compare/v2.4.0...v2.4.1) (2026-07-05)


### Bug Fixes

* **build:** make --with-harfbuzz opt-in (default no) to fix static builds ([60b7a97](https://github.com/phpolygon/php-vio/commit/60b7a977aef9dbdb73750addd21ce4f1409ca445))

# [2.4.0](https://github.com/phpolygon/php-vio/compare/v2.3.0...v2.4.0) (2026-07-05)


### Features

* **render:** batched draw submission + per-draw SRV-flush dedup (D3D12) ([27e906f](https://github.com/phpolygon/php-vio/commit/27e906fd3a2788571a513ab6c44b84369ef051f7))
* **text:** HarfBuzz + SheenBidi shaping with BiDi and line wrapping ([fd6f835](https://github.com/phpolygon/php-vio/commit/fd6f835ecb470b84e6609578b79da57ad2fb8357)), closes [#14-lite](https://github.com/phpolygon/php-vio/issues/14-lite)

# [2.3.0](https://github.com/phpolygon/php-vio/compare/v2.2.0...v2.3.0) (2026-06-26)


### Features

* **font:** devicePixelRatio scale for crisp HiDPI text ([f557eab](https://github.com/phpolygon/php-vio/commit/f557eabf99cbc08a6e9a87b5efbf5c7de05a81a0))

# [2.2.0](https://github.com/phpolygon/php-vio/compare/v2.1.1...v2.2.0) (2026-06-24)


### Features

* **window:** selectable fullscreen resolution ([b257d33](https://github.com/phpolygon/php-vio/commit/b257d33feb8312612d5e07ae5d11a83aa0c37eec))

## [2.1.1](https://github.com/phpolygon/php-vio/compare/v2.1.0...v2.1.1) (2026-06-22)


### Bug Fixes

* **build:** compile vio_compute_pipeline.c + gate releases on load/test ([affc8b0](https://github.com/phpolygon/php-vio/commit/affc8b033a76275ea8953b7c0b264a1ffc744987))
* **build:** link SPIRV-Tools on aarch64; make GPU tests skip without a context ([c9e69c3](https://github.com/phpolygon/php-vio/commit/c9e69c3c81ac2ef9e8b993ef3274109e3b976cb2))

# [2.1.0](https://github.com/phpolygon/php-vio/compare/v2.0.1...v2.1.0) (2026-06-22)


### Features

* **compute:** cross-backend GPU compute primitive (vio_compute_*) ([2732add](https://github.com/phpolygon/php-vio/commit/2732add16eaa683c5ddd6c1c0432485d0c5bcbfc))


### Performance Improvements

* **uniforms:** O(1) uniform lookup + batch vio_set_uniforms ([204d3ba](https://github.com/phpolygon/php-vio/commit/204d3ba3bc71b5dea339d6d06ec917faf78fa256)), closes [#1](https://github.com/phpolygon/php-vio/issues/1)

## [2.0.1](https://github.com/phpolygon/php-vio/compare/v2.0.0...v2.0.1) (2026-06-20)


### Performance Improvements

* **font:** dynamic + R8 glyph atlas; feat: monitor enumeration ([e2fa0d8](https://github.com/phpolygon/php-vio/commit/e2fa0d86f86b7ac8cb85a3e3ace3c866bc591a78))

# [2.0.0](https://github.com/phpolygon/php-vio/compare/v1.23.0...v2.0.0) (2026-06-19)


* chore!: drop PHP 8.4, require PHP 8.5 ([2c598ca](https://github.com/phpolygon/php-vio/commit/2c598cadfc8315ed4e1336166610cc20a908b8b9))


### BREAKING CHANGES

* PHP 8.4 is no longer supported.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>

# [1.23.0](https://github.com/phpolygon/php-vio/compare/v1.22.0...v1.23.0) (2026-06-19)


### Features

* **font:** async font loading on a background thread (vio_font_load_async) ([ff3a3b3](https://github.com/phpolygon/php-vio/commit/ff3a3b3d060a29026ad800d0e7bfd328cdfb496d))

# [1.22.0](https://github.com/phpolygon/php-vio/compare/v1.21.1...v1.22.0) (2026-06-16)


### Features

* **input:** register VIO_TOUCH_* phase constants ([#14](https://github.com/phpolygon/php-vio/issues/14)) ([8245355](https://github.com/phpolygon/php-vio/commit/8245355f7384e73afe6245fbb3cf2a1056a6f1eb))

## [1.21.1](https://github.com/phpolygon/php-vio/compare/v1.21.0...v1.21.1) (2026-06-16)


### Bug Fixes

* **d3d12:** raise shadow SRV register base so >4 regular samplers don't overlap ([#12](https://github.com/phpolygon/php-vio/issues/12)) ([89e0de0](https://github.com/phpolygon/php-vio/commit/89e0de0cd8a03247577eb44fdec2a1d380eb910e))

# [1.21.0](https://github.com/phpolygon/php-vio/compare/v1.20.1...v1.21.0) (2026-06-15)


### Features

* **texture:** vio_texture_3d — 3D/volume textures across all backends ([#11](https://github.com/phpolygon/php-vio/issues/11)) ([4e234aa](https://github.com/phpolygon/php-vio/commit/4e234aa2d0d35dc6aac570b56806b1d27545e41d))

## [1.20.1](https://github.com/phpolygon/php-vio/compare/v1.20.0...v1.20.1) (2026-06-12)


### Bug Fixes

* **d3d12:** per-frame cbuffer heap race overwrote in-flight frame uniforms ([657cd23](https://github.com/phpolygon/php-vio/commit/657cd23db8e814baa25f647983cdc6ac2e3ef0a4))

# [1.20.0](https://github.com/phpolygon/php-vio/compare/v1.19.1...v1.20.0) (2026-06-10)


### Features

* **d3d12:** configurable RTV format + vio_gpu_info (GPU name/VRAM/RAM) ([400991c](https://github.com/phpolygon/php-vio/commit/400991c199ea603cad3606e9712c8ff283a2141f))

## [1.19.1](https://github.com/phpolygon/php-vio/compare/v1.19.0...v1.19.1) (2026-06-08)


### Bug Fixes

* **window:** exit real fullscreen on setWindowed and restore windowed geometry ([2368211](https://github.com/phpolygon/php-vio/commit/2368211eafc2513cd4302e15f8aea3d4acafe6da))

# [1.19.0](https://github.com/phpolygon/php-vio/compare/v1.18.0...v1.19.0) (2026-06-07)


### Features

* cross-platform thermal sensing + D3D12 swapchain resize fix ([0c5d847](https://github.com/phpolygon/php-vio/commit/0c5d847e6f1fadd02d5867f9c4a4192c92dbab0b))

# [1.18.0](https://github.com/phpolygon/php-vio/compare/v1.17.0...v1.18.0) (2026-06-03)


### Bug Fixes

* **build:** register vio_thermal.c in config.w32 for Windows ([c20cb2e](https://github.com/phpolygon/php-vio/commit/c20cb2eef57f96ece708941b90ca6016c53d38a6))


### Features

* vio_thermal_state() bridge for NSProcessInfo.thermalState ([6038380](https://github.com/phpolygon/php-vio/commit/6038380612fc5d703485038ab27f1ff0d0113605))

## [1.18.1](https://github.com/phpolygon/php-vio/compare/v1.18.0...v1.18.1) (2026-06-03)


### Bug Fixes

* **build:** register vio_thermal.c in config.w32 for Windows ([c20cb2e](https://github.com/phpolygon/php-vio/commit/c20cb2eef57f96ece708941b90ca6016c53d38a6))

# [1.18.0](https://github.com/phpolygon/php-vio/compare/v1.17.0...v1.18.0) (2026-06-03)


### Features

* vio_thermal_state() bridge for NSProcessInfo.thermalState ([6038380](https://github.com/phpolygon/php-vio/commit/6038380612fc5d703485038ab27f1ff0d0113605))

# [1.17.0](https://github.com/phpolygon/php-vio/compare/v1.16.2...v1.17.0) (2026-05-25)


### Bug Fixes

* **ios:** expose logical window size (physical / content scale) ([6995dab](https://github.com/phpolygon/php-vio/commit/6995dabbb9af38cb588eef7f18dae5ebde4333d9))
* **ios:** gate displaySyncEnabled behind TARGET_OS_OSX ([4be8803](https://github.com/phpolygon/php-vio/commit/4be880332e6918370fb9ec6ba6fe2915250da529))
* **ios:** keep 2D design space at config size + scale touch to it ([77d9f2e](https://github.com/phpolygon/php-vio/commit/77d9f2e25220cd64056e5b08ec65557488fcc179))
* **ios:** run shutdown_context teardown on the main thread ([c402e25](https://github.com/phpolygon/php-vio/commit/c402e252cd252ed14c27c3b854b5a9ea5dcd5645))
* **ios:** thread-safe framebuffer size + report real size to PHP ([2efd219](https://github.com/phpolygon/php-vio/commit/2efd219e44952ddfecf9d92a84c3ace388c434d4))
* **ios:** two-pass window discovery for launch-time vio_create ([55dd6a4](https://github.com/phpolygon/php-vio/commit/55dd6a40727ef0e22c475bbc995315adb84b61e0))
* **ios:** unguarded GLFW calls in HAVE_METAL framebuffer-sync block ([5a16a3f](https://github.com/phpolygon/php-vio/commit/5a16a3fe929fdbded8aaee9ae460b736a7841614))


### Features

* **ios:** add iOS / iPadOS backend (UIView + UITouch + CAMetalLayer) ([82b5b9e](https://github.com/phpolygon/php-vio/commit/82b5b9e953d5f6db87c5334f73ecf9ce119cc069))
* **ios:** decouple Metal backend from GLFW + add touch input API ([cbe2bbd](https://github.com/phpolygon/php-vio/commit/cbe2bbde42dc2741a008cb42534416f0d24c1d5e))
* **ios:** on-screen keyboard support (UIKeyInput + text/backspace) ([1aad08e](https://github.com/phpolygon/php-vio/commit/1aad08e446c70c8b4cd7520ad9ed1b9f428c551f))
* **ios:** run setup_context's UIKit work on the main thread ([5cb074f](https://github.com/phpolygon/php-vio/commit/5cb074fc5fdf3f050fe2d3e5779a8e6144220f23))
* **ios:** touch->mouse emulation + logical touch coords ([bf3d943](https://github.com/phpolygon/php-vio/commit/bf3d943660f48e89a89d1f810ff8cb37fe5bc6df))

## [1.16.2](https://github.com/phpolygon/php-vio/compare/v1.16.1...v1.16.2) (2026-05-23)


### Bug Fixes

* **d3d12:** silence benign clear-value-mismatch perf hints (id 820/821) ([15d4a5f](https://github.com/phpolygon/php-vio/commit/15d4a5fd410f6a2d841c2e00c2bcc2e9fb0eecbb)), closes [#9](https://github.com/phpolygon/php-vio/issues/9)

## [1.16.1](https://github.com/phpolygon/php-vio/compare/v1.16.0...v1.16.1) (2026-05-23)


### Bug Fixes

* **d3d11:** warm-render offscreen no longer visible (deferred bind) ([19fb49a](https://github.com/phpolygon/php-vio/commit/19fb49a3654d7570a982b8da9ee036e78bc795a2)), closes [#8](https://github.com/phpolygon/php-vio/issues/8)

# [1.16.0](https://github.com/phpolygon/php-vio/compare/v1.15.1...v1.16.0) (2026-05-23)


### Features

* **vulkan:** full 2D + offscreen render backend (parity with d3d12) ([58653d9](https://github.com/phpolygon/php-vio/commit/58653d96331d8a867b43af6075ffb3416dd022c8))

## [1.15.1](https://github.com/phpolygon/php-vio/compare/v1.15.0...v1.15.1) (2026-05-22)


### Bug Fixes

* **d3d12:** offscreen warm-render no longer flashes or trips the command list ([42a9ebf](https://github.com/phpolygon/php-vio/commit/42a9ebfcbd00ef0111c1f8606a58f1d30b2c028c))

# [1.15.0](https://github.com/phpolygon/php-vio/compare/v1.14.0...v1.15.0) (2026-05-20)


### Features

* expose VIO_FEATURE_* constants + vio_supports_feature() to PHP ([9131c0c](https://github.com/phpolygon/php-vio/commit/9131c0caf0ffc9e57edbf55bfaafa019c62e7883))

# [1.14.0](https://github.com/phpolygon/php-vio/compare/v1.13.0...v1.14.0) (2026-05-20)


### Features

* **metal:** wire render-target binding through the backend vtable ([c708a67](https://github.com/phpolygon/php-vio/commit/c708a67a6dfd8616179dea83972f4681bbeba5d3))

# [1.13.0](https://github.com/phpolygon/php-vio/compare/v1.12.0...v1.13.0) (2026-05-19)


### Features

* **backends:** add VIO_FEATURE_3D_PIPELINE + version-aware OpenGL caps ([9ccf8fc](https://github.com/phpolygon/php-vio/commit/9ccf8fc13cce016b2639cde3870afba716884cfe))
* **backends:** expand vio_feature with 16 capabilities + GL extension cache (Etappe 2) ([9d6b857](https://github.com/phpolygon/php-vio/commit/9d6b857bc7349044e823c9e57a9e9520418aa906)), closes [Issue-#3-part-2](https://github.com/Issue-/issues/3-part-2) [#3](https://github.com/phpolygon/php-vio/issues/3)
* vio_gl_info + GLSL #version check + RT API surface (Etappe 7, closes [#3](https://github.com/phpolygon/php-vio/issues/3) [#4](https://github.com/phpolygon/php-vio/issues/4)) ([4d43201](https://github.com/phpolygon/php-vio/commit/4d432019627e5004bc8eb4bfbeaadc733f29ea1d)), closes [#version](https://github.com/phpolygon/php-vio/issues/version) [#version-mismatch](https://github.com/phpolygon/php-vio/issues/version-mismatch)

# [1.12.0](https://github.com/phpolygon/php-vio/compare/v1.11.0...v1.12.0) (2026-05-17)


### Features

* **opengl:** runtime context-version detection with 4.6→3.3 ladder ([2accb4f](https://github.com/phpolygon/php-vio/commit/2accb4fbf8271463311b47e33b472f7a33df2b16)), closes [#version](https://github.com/phpolygon/php-vio/issues/version) [#version](https://github.com/phpolygon/php-vio/issues/version)

# [1.11.0](https://github.com/phpolygon/php-vio/compare/v1.10.9...v1.11.0) (2026-05-12)


### Bug Fixes

* **context:** always free vio_2d / input state, not just when ctx is initialized ([3b5b66d](https://github.com/phpolygon/php-vio/commit/3b5b66d22ad3bdcaf6aa62ca5ffd1ebd82e13086))
* **render_target:** cache backend-texture wrapper + validate vio_texture data ([2d19696](https://github.com/phpolygon/php-vio/commit/2d19696e42d49fdbfa57f34962fb71da260e8e2d))


### Features

* **d3d12:** InfoQueue drain, staging-heap SRV, in-frame guard ([a070a2c](https://github.com/phpolygon/php-vio/commit/a070a2ca20149f9302250484bb1f0f3fb2c27ab9))

## [1.10.9](https://github.com/phpolygon/php-vio/compare/v1.10.8...v1.10.9) (2026-05-07)


### Bug Fixes

* **2d:** apply current transform to pushed scissor rects ([56a9b3f](https://github.com/phpolygon/php-vio/commit/56a9b3ffae385b244e48ef3a047b8630e1494e77))

## [1.10.8](https://github.com/phpolygon/php-vio/compare/v1.10.7...v1.10.8) (2026-05-07)


### Bug Fixes

* **d3d12:** allocate static SRV descriptors from top of heap (downward) ([61e42a7](https://github.com/phpolygon/php-vio/commit/61e42a7aec07ef3588722ff7da75d00ddd4372de))

## [1.10.7](https://github.com/phpolygon/php-vio/compare/v1.10.6...v1.10.7) (2026-05-07)


### Bug Fixes

* **d3d12:** dynamic SRV-heap partition (no fixed static-slot reservation) ([122e2e5](https://github.com/phpolygon/php-vio/commit/122e2e556ff5a147e62e630e8162b2e98541d3fc)), closes [#65](https://github.com/phpolygon/php-vio/issues/65)

## [1.10.6](https://github.com/phpolygon/php-vio/compare/v1.10.5...v1.10.6) (2026-05-07)


### Bug Fixes

* **ci:** bump humbletim/setup-vulkan-sdk v1.2.0 → v1.2.1 ([1d49af1](https://github.com/phpolygon/php-vio/commit/1d49af1136b7ced11a73476c7006ba2c6722177b))
* **d3d12:** per-frame slicing for 2D VBO and SRV descriptor heap ([89c5b88](https://github.com/phpolygon/php-vio/commit/89c5b883c1a01c4058ede12dd2f2209ba409cf67))

## [1.10.5](https://github.com/phpolygon/php-vio/compare/v1.10.4...v1.10.5) (2026-05-07)


### Bug Fixes

* **ci:** quote windows-x64 build step name to satisfy YAML parser ([753d840](https://github.com/phpolygon/php-vio/commit/753d8406bac2c1cc01c0862f4a6db8075d9e32b1))

## [1.10.4](https://github.com/phpolygon/php-vio/compare/v1.10.3...v1.10.4) (2026-05-06)


### Bug Fixes

* **d3d12:** full GPU sync before cbuffer heap reallocation ([4166a7a](https://github.com/phpolygon/php-vio/commit/4166a7a9c808e799e5953883fa6d392493321e62))

## [1.10.2](https://github.com/phpolygon/php-vio/compare/v1.10.1...v1.10.2) (2026-05-05)


### Bug Fixes

* **windows:** accept --with-glfw=DIR and --with-vulkan=DIR in config.w32 ([aa64bdd](https://github.com/phpolygon/php-vio/commit/aa64bdd62b709c7961be79f947d4c560f6361693))

## [1.10.1](https://github.com/phpolygon/php-vio/compare/v1.10.0...v1.10.1) (2026-05-05)


### Bug Fixes

* **windows:** per-monitor DPI v2 awareness and multi-monitor support ([fce72c4](https://github.com/phpolygon/php-vio/commit/fce72c40844cadf6844002d24757786597b8dbe1))

# [1.10.0](https://github.com/phpolygon/php-vio/compare/v1.9.0...v1.10.0) (2026-04-28)


### Features

* **metal:** SPIR-V→MSL shader pipeline + native window handle accessor ([2f54517](https://github.com/phpolygon/php-vio/commit/2f54517d2e0c0e016e434be860bad5c47b71ce34))

# [1.9.0](https://github.com/phpolygon/php-vio/compare/v1.8.15...v1.9.0) (2026-04-19)


### Bug Fixes

* **d3d:** read_pixels survives Present + D3D11 non-instanced draws render ([989f979](https://github.com/phpolygon/php-vio/commit/989f979f94ae2a0899fa8c1634b9143bf38fa302))


### Features

* **d3d:** complete D3D11/D3D12 backend integration ([16ae78a](https://github.com/phpolygon/php-vio/commit/16ae78a94e45aec7e79469ea0f74d1b8f86cd854)), closes [#include](https://github.com/phpolygon/php-vio/issues/include)

## [1.8.15](https://github.com/phpolygon/php-vio/compare/v1.8.14...v1.8.15) (2026-04-18)


### Bug Fixes

* **ci:** use v-prefixed ref when triggering build workflow ([4d9343e](https://github.com/phpolygon/php-vio/commit/4d9343ed8bccc34f605eb8c8f7cac786e3f34588))

## [1.8.14](https://github.com/phpolygon/php-vio/compare/v1.8.13...v1.8.14) (2026-04-18)


### Bug Fixes

* **vulkan:** gate portability enumeration behind __APPLE__ ([219d5df](https://github.com/phpolygon/php-vio/commit/219d5df3572bac0ae58a28fe31cf5df9255e999a))

## [1.8.13](https://github.com/phpolygon/php-vio/compare/v1.8.12...1.8.13) (2026-04-18)


### Bug Fixes

* **windows:** D3D/Vulkan window visibility + GLFW detection ([4d7c53e](https://github.com/phpolygon/php-vio/commit/4d7c53e0059da6f3d3023eecc793917545502e49))

## [1.8.10](https://github.com/phpolygon/php-vio/compare/1.8.9...1.8.10) (2026-04-16)


### Bug Fixes

* include config.h unconditionally in VMA wrapper ([064a108](https://github.com/phpolygon/php-vio/commit/064a108a73da7dcf886a49c5c23a661bd26030f6))

## [1.8.9](https://github.com/phpolygon/php-vio/compare/1.8.8...1.8.9) (2026-04-16)


### Bug Fixes

* include php.h before HAVE_METAL check in Metal backend ([da3b794](https://github.com/phpolygon/php-vio/commit/da3b7946b7cba50dc20778062bbfa9abd364de8a))

## [1.8.8](https://github.com/phpolygon/php-vio/compare/1.8.7...1.8.8) (2026-04-16)


### Bug Fixes

* revert PHP_GLOBAL_OBJS (doesn't work for .lo targets), keep MSL lib fix ([5c2efde](https://github.com/phpolygon/php-vio/commit/5c2efdebb608d07507440b70557c18685ca31279))

## [1.8.7](https://github.com/phpolygon/php-vio/compare/1.8.6...1.8.7) (2026-04-16)


### Bug Fixes

* add missing spirv-cross-msl.lib to Windows config.w32 ([d25a03f](https://github.com/phpolygon/php-vio/commit/d25a03f45afdd51259a9b293d6af197bd2394a9f))

## [1.8.6](https://github.com/phpolygon/php-vio/compare/1.8.5...1.8.6) (2026-04-16)


### Bug Fixes

* include Metal object in static builds via PHP_GLOBAL_OBJS ([67c5407](https://github.com/phpolygon/php-vio/commit/67c5407397deee464b8e6763825bf1a52d7fa2c9))

## [1.8.5](https://github.com/phpolygon/php-vio/compare/1.8.4...1.8.5) (2026-04-16)


### Bug Fixes

* use correct PHP CFLAGS in Metal Makefile.frag rule ([3739e47](https://github.com/phpolygon/php-vio/commit/3739e47131b8ba1ffbcd97cad3b49ac2d788d5a5))

## [1.8.4](https://github.com/phpolygon/php-vio/compare/1.8.3...1.8.4) (2026-04-16)


### Bug Fixes

* add vio_spirv_get_uniform_offsets stub when SPIRV-Cross unavailable ([ef8254f](https://github.com/phpolygon/php-vio/commit/ef8254f3981ba3b16e0f1384aa91d4fca401069b)), closes [#else](https://github.com/phpolygon/php-vio/issues/else)

## [1.8.3](https://github.com/phpolygon/php-vio/compare/1.8.2...1.8.3) (2026-04-16)


### Bug Fixes

* add missing VIO_2D_MAX_VERTICES constant for D3D vertex buffers ([76c7e05](https://github.com/phpolygon/php-vio/commit/76c7e05356f66ae1bb68d2b23adffadbc30439aa))

## [1.8.2](https://github.com/phpolygon/php-vio/compare/1.8.1...1.8.2) (2026-04-16)


### Bug Fixes

* regenerate GLAD with core GL 4.1 only (no vendor extensions) ([584a36e](https://github.com/phpolygon/php-vio/commit/584a36eb4031b8c6a2dcaefc1547840333417bfe))

## [1.8.1](https://github.com/phpolygon/php-vio/compare/1.8.0...1.8.1) (2026-04-16)


### Bug Fixes

* remove Windows build artifacts from repository ([8c04af2](https://github.com/phpolygon/php-vio/commit/8c04af21112190b2ff73d73b0c1d7c2d20596f3a))

# [1.7.0](https://github.com/phpolygon/php-vio/compare/1.6.0...1.7.0) (2026-04-16)


### Features

* D3D11 correctness — depth convention fixup, shadow sampling, pipeline enhancements ([de5b26c](https://github.com/phpolygon/php-vio/commit/de5b26c7b0e1d314656eaf2c15e13ebe41e9e1d3))
* D3D11 cubemap support, read_pixels, and SRV unbind fix ([7eafda4](https://github.com/phpolygon/php-vio/commit/7eafda400c38b5d1ca271b55f1ed9e8680e383fd))
* D3D11 fragment cbuffers, struct array uniforms, instancing, shadow maps ([96b3659](https://github.com/phpolygon/php-vio/commit/96b36597f242ffeb27cafab63650ad0dc9e323a1))
* D3D11/D3D12 2D rendering + font atlas support ([e872cd0](https://github.com/phpolygon/php-vio/commit/e872cd0a02ab684cd0de929dacf3bb99de2ffc95))
* D3D11/D3D12 rendering pipeline — vtable wiring, shader transpilation, render targets ([e243e6f](https://github.com/phpolygon/php-vio/commit/e243e6f7dee2d47a8f11a8cf88d9fe708fe2ba82))
* D3D12 backend — full feature parity with D3D11 ([686dcb8](https://github.com/phpolygon/php-vio/commit/686dcb81409bf687dbd0d7c871a59608f0595a75))
* D3D12 dynamic cbuffer heap with auto-growth ([2b6d8a0](https://github.com/phpolygon/php-vio/commit/2b6d8a03041b21a76a2f132dc28d6c18b92aae45))
* HDR render target support + color texture SRV infrastructure ([6a16613](https://github.com/phpolygon/php-vio/commit/6a166137177232b8fa3710386f8fb4dab0c9b5e8))
* uniform cbuffer pipeline for D3D — SPIRV reflection + constant buffer upload ([1f43f98](https://github.com/phpolygon/php-vio/commit/1f43f987e2bc7a5ee97390e77c283f6f4c478038))
* vio_set_cursor_mode + mat3 cbuffer padding fix ([267d586](https://github.com/phpolygon/php-vio/commit/267d586a8acd5a4006a1a30702aa873b6da9b6c9))


### Performance Improvements

* vio_draw_instanced accepts packed binary string for zero-copy instancing ([f106f8c](https://github.com/phpolygon/php-vio/commit/f106f8c3e2ab74194ab1d2a9c38ac2a4b646badd))

# [1.6.0](https://github.com/phpolygon/php-vio/compare/1.5.3...1.6.0) (2026-04-15)


### Features

* add vio_gpu_flush() and fix Metal drawable management ([d7e1e0c](https://github.com/phpolygon/php-vio/commit/d7e1e0c00d9f8d90ad6f1c89df017ee36b38ddb8))

## [1.5.3](https://github.com/phpolygon/php-vio/compare/1.5.2...1.5.3) (2026-04-14)


### Bug Fixes

* Metal flickering caused by AppKit layer redraw policy ([a5631e4](https://github.com/phpolygon/php-vio/commit/a5631e4c9b21665b2c2b23611ff04bec7b802002))

## [1.5.2](https://github.com/phpolygon/php-vio/compare/1.5.1...1.5.2) (2026-04-14)


### Bug Fixes

* Metal vsync-off uses offscreen rendering to avoid nextDrawable blocking ([a2be14b](https://github.com/phpolygon/php-vio/commit/a2be14b8b6690b51f41a595c1e56e2778dae55b7))

## [1.5.1](https://github.com/phpolygon/php-vio/compare/1.5.0...1.5.1) (2026-04-14)


### Bug Fixes

* use explicit depth-disabled state in Metal 2D flush ([a57d9da](https://github.com/phpolygon/php-vio/commit/a57d9da9c30aad2c0c9bcc93fec74a729846add5))

# [1.5.0](https://github.com/phpolygon/php-vio/compare/1.4.0...1.5.0) (2026-04-14)


### Bug Fixes

* guard GL texture cleanup and Metal vsync/readback ([1b4261b](https://github.com/phpolygon/php-vio/commit/1b4261bfc915e956353f1397dd9d39b7e1043a8d))


### Features

* implement Metal 2D rendering pipeline ([f265e3f](https://github.com/phpolygon/php-vio/commit/f265e3fa6c0a5168e476aeec69d9e863d8545048))

# [1.4.0](https://github.com/phpolygon/php-vio/compare/1.3.0...1.4.0) (2026-04-14)


### Bug Fixes

* platform-specific backend auto-selection (Metal on macOS, Vulkan on Linux) ([c98400b](https://github.com/phpolygon/php-vio/commit/c98400ba4575faf3f9848cf211c497c12f042d3e))


### Features

* dynamic 2D batch buffer and space glyph fix ([abfd09b](https://github.com/phpolygon/php-vio/commit/abfd09b54f2a4e5982a36703b20b76ce72375e22))

# [1.3.0](https://github.com/phpolygon/php-vio/compare/1.2.5...1.3.0) (2026-04-13)


### Features

* multi-range Unicode font atlas with hashmap glyph lookup ([d85b4c6](https://github.com/phpolygon/php-vio/commit/d85b4c673e93ceacb096c32b949b6aadcd098883))

## [1.2.5](https://github.com/phpolygon/php-vio/compare/1.2.4...1.2.5) (2026-04-13)


### Bug Fixes

* name binary vio.so inside release zips for PIE compatibility ([d725c7d](https://github.com/phpolygon/php-vio/commit/d725c7d922d25aa5c1d231ff5afd89bc200278e1))

## [1.2.1](https://github.com/phpolygon/php-vio/compare/1.2.0...1.2.1) (2026-04-13)


### Bug Fixes

* extend font atlas to Latin-1 and add UTF-8 text decoding ([665885e](https://github.com/phpolygon/php-vio/commit/665885ee9e80ec053f64e036e058e2df87ad8e96))

# [1.2.0](https://github.com/phpolygon/php-vio/compare/1.1.4...1.2.0) (2026-04-12)


### Bug Fixes

* add COBJMACROS/INITGUID for Windows C compilation and link dxguid.lib ([478008c](https://github.com/phpolygon/php-vio/commit/478008cdba8572c9f0fdc682e74b70685043e16e))
* add forward declaration for d3d12_shutdown (fixes C2371 on MSVC) ([abdb4e8](https://github.com/phpolygon/php-vio/commit/abdb4e857b253c5c42e670b37fde4cab347700b4))
* address code review findings for D3D11/D3D12 backends ([ee15dae](https://github.com/phpolygon/php-vio/commit/ee15dae3737b92d2cc691acb1254708e4ad4b1b7))


### Features

* add DirectX 11 and DirectX 12 render backends ([e9341f6](https://github.com/phpolygon/php-vio/commit/e9341f6118c1e1b43dc34934e6c6e03c73e41988))
* DirectX 11 and DirectX 12 render backends ([#1](https://github.com/phpolygon/php-vio/issues/1)) ([90927ee](https://github.com/phpolygon/php-vio/commit/90927ee03cd9f8007cf2c4b7e18238f98d68c7b6))

## [1.1.4](https://github.com/phpolygon/php-vio/compare/1.1.3...1.1.4) (2026-04-12)


### Bug Fixes

* add bare DLL release assets for Windows and simplify Windows install docs ([9775de3](https://github.com/phpolygon/php-vio/commit/9775de314a6d7e71bc72708909be59e9bfa47a47))

## [1.1.3](https://github.com/phpolygon/php-vio/compare/1.1.2...1.1.3) (2026-04-11)


### Bug Fixes

* rename binaries inside ZIP to match asset name for PIE ([7fb6f7e](https://github.com/phpolygon/php-vio/commit/7fb6f7e16150e1c236fc1b77d3272f67926c2fd7))

## [1.1.2](https://github.com/phpolygon/php-vio/compare/1.1.1...1.1.2) (2026-04-11)


### Bug Fixes

* add TSRMLS cache for thread-safe (ZTS) PHP builds on Windows ([93f3831](https://github.com/phpolygon/php-vio/commit/93f3831da3538f7e9f10b047cd4cd012b11b2ae5))

## [1.1.1](https://github.com/phpolygon/php-vio/compare/1.1.0...1.1.1) (2026-04-11)


### Bug Fixes

* use PIE-compatible asset naming for pre-built binaries ([522f9ed](https://github.com/phpolygon/php-vio/commit/522f9ed91787097e020e10b415a93f298902fe91))

# [1.1.0](https://github.com/phpolygon/php-vio/compare/1.0.6...1.1.0) (2026-04-11)


### Features

* build binaries for PHP 8.4 and 8.5 ([06d8a5f](https://github.com/phpolygon/php-vio/commit/06d8a5ffceeb2909d7b7522a14e245f83147e879))

## [1.0.6](https://github.com/phpolygon/php-vio/compare/1.0.5...1.0.6) (2026-04-11)


### Bug Fixes

* use MSVC-compatible visibility macros in stb implementation files ([0492ed4](https://github.com/phpolygon/php-vio/commit/0492ed47d80bf1ae82d7245d58a9427d6276f80d))

## [1.0.5](https://github.com/phpolygon/php-vio/compare/1.0.4...1.0.5) (2026-04-11)


### Bug Fixes

* correct include path for vio_plugin.h on Windows ([b86a787](https://github.com/phpolygon/php-vio/commit/b86a787c80cdd497cfb25793181f45754ee735eb))

## [1.0.4](https://github.com/phpolygon/php-vio/compare/1.0.3...1.0.4) (2026-04-11)


### Bug Fixes

* replace pthread with _beginthreadex on Windows for async texture loading ([7f22100](https://github.com/phpolygon/php-vio/commit/7f22100d6dc8b7f7cc6b42ca359eed9c76e386f2))

## [1.0.3](https://github.com/phpolygon/php-vio/compare/1.0.2...1.0.3) (2026-04-11)


### Bug Fixes

* guard GL calls in vio_begin with vio_gl.initialized check ([49c12c0](https://github.com/phpolygon/php-vio/commit/49c12c09d5baca10cbcbca834c1072baaa2d4883))

## [1.0.2](https://github.com/phpolygon/php-vio/compare/1.0.1...1.0.2) (2026-04-11)


### Bug Fixes

* avoid stb_truetype.h macro conflict with PHP headers on Windows ([98b620a](https://github.com/phpolygon/php-vio/commit/98b620a39e3de35d5da1da78a667dcfdf18f821a))

## [1.0.1](https://github.com/phpolygon/php-vio/compare/1.0.0...1.0.1) (2026-04-11)


### Bug Fixes

* conditional Metal/VMA compilation, Vulkan includes, Windows deps dir ([5c7c847](https://github.com/phpolygon/php-vio/commit/5c7c8477a8afea25c2fd4aea915f62402e9951b4))

# 1.0.0 (2026-04-11)


### Bug Fixes

* trigger build workflow after semantic release via gh workflow run ([4686e86](https://github.com/phpolygon/php-vio/commit/4686e8691c9b0d5fbcc5a21258384110598a02ae))
* use tags without v-prefix for PIE/Composer compatibility ([708df70](https://github.com/phpolygon/php-vio/commit/708df70a23ab57063737c214f6e35ed7d252a2d7))


### Features

* add semantic release workflow for automated versioning ([70d2664](https://github.com/phpolygon/php-vio/commit/70d2664fec9efec01dec1bf6521130d00a9c1de4))

## [0.2.1](https://github.com/phpolygon/php-vio/compare/v0.2.0...v0.2.1) (2026-04-11)


### Bug Fixes

* trigger build workflow after semantic release via gh workflow run ([4686e86](https://github.com/phpolygon/php-vio/commit/4686e8691c9b0d5fbcc5a21258384110598a02ae))

# [0.2.0](https://github.com/phpolygon/php-vio/compare/v0.1.0...v0.2.0) (2026-04-11)


### Features

* add semantic release workflow for automated versioning ([70d2664](https://github.com/phpolygon/php-vio/commit/70d2664fec9efec01dec1bf6521130d00a9c1de4))
