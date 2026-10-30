Native Vulkan renderer (ini renderer=vulkan_native), taken from ScarletNX
(https://github.com/scarletnx/ScarletNX, main branch commit 112db409, fetched 2026-10-29; before that b5f4f14f2bfc of
2026-10-21), itself a fork of SwitchRecomp. The sources are in src_new/, only the files the build reads.

LICENSE: the code in this folder is GPL-3.0-or-later (LICENSE; MZ Labs' original parts Apache-2.0, LICENSE.apache,
NOTICE). A runtime EXE built with libsnxvk_new.a linked in is therefore a GPL-3.0 work: fine for personal use; anyone
given such an EXE must also be offered this source. A build with SWR_NO_SNXVK=1 leaves it out.

deps/: Vulkan-Headers 1.4.365, volk 1.4.363, VulkanMemoryAllocator 3.4.0, xxHash 0.8.3, the versions and SHA-256
hashes ScarletNX's cmake/dependencies.cmake pins (verified).
src_new/tools/: upstream's shaderdec and settingsgen code generators (same project, same licence).

Local changes to upstream (marked "SWR" in the code):
  gpu/gpu.c             plat_prof_mark_gpu (our profiler samples the renderer's GPU thread); no save-state schema
                        under GCC (upstream builds with Clang's __builtin_dump_struct; our runtime has no save states)
  gpu/vk/hdr.c          HDR output, the FXAA / sharpening present pass and the averaged shrink (vk_shrink); the pass
                        records through the recording thread's ring
  gpu/vk/renderer.c     the present pass, DLSS frame hook, "slow present" step timing (logs over 250 ms)
  gpu/vk/render.c, texture.c, device.c, spirv_translate.c, common/utilities/spirv_emit.c: DLSS hooks (dlss.c;
                        dlss_stub.c in the public build), the jitter on every viewport, motion-vector draws paired by
                        guest address; vk_rt_prefiltered (glow passes read an averaged copy); ShTex.use
  gpu/vk/pipeline.c     pipeline recipes (pipeline_recipes.bin) for the shipped-shader pass
  gpu/shader/shipped.c, gpu/shipped_pass.c: Metroid Dread's MSHD shader containers
  gpu/vk/swr_settings.c, gpu/maxwell/mme_jit_stub.c: our settings and no LLVM macro JIT (the interpreter runs every macro);
                        swr_settings.c also gives snxvk_glue.c the renderer's entries (swr_r_*), so the glue never
                        mirrors upstream's Renderer struct
  gpu/vk/render.c       SWR_VK_STREAM_MERGE (default 1): vertex streams over the same memory staged once; SWR_VK_VCLAMP
                        (default 2 = upstream da6776e). Both made the game's particle pool overflow at the acid enemy
                        until main.c pace_update got its guard (2026-10-29); staging there fell from 23-63 to <1 MB a frame
  gpu/vk/surface.c      pitch-linear targets aren't written back at each release without memory traps (we have none)
  gpu/gpu.c, vk/owned.c SWR_VK_GW / SWR_VK_GW_WATCH: logs GPU writes into guest memory (diagnostic, inert by default)

Build: set GLSLANG=<glslang.exe 16.6.0> and run python\python.exe work-profiler\build_snxvk2.py
(SWR_NO_DLSS=1: runtime\snxvk\libsnxvk_new_nodlss.a, the public build), then the normal --build-runtime links it and
compiles runtime\snxvk_glue.c (the adapter between this renderer and our runtime; SWR_SNXVK_V2 is defined for it when
src_new is main 112db409 or later). Check for the build's "symbols renamed" line: without it nothing was rebuilt.
Every global symbol of the ScarletNX objects is renamed with the prefix snxv_ so its GPU front end can sit next to ours.

Re-vendoring: a git clone of upstream, a branch at the old commit with this folder's files copied over it (the SWR
changes as one commit), then a merge of upstream main; dread\make_release.py's snx_keep lists what the build reads.
