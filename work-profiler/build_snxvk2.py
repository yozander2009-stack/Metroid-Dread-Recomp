"""Build the re-vendored ScarletNX renderer (runtime/snxvk/src_new, ScarletNX main b5f4f14f2bfc + our hooks) into
runtime/snxvk/libsnxvk_new.a (SWR_NO_DLSS=1: libsnxvk_new_nodlss.a). Same scheme as build_snxvk.py: every global it
defines is renamed snxv_<name>, so its GPU front end sits next to ours; snxvk_glue.c provides what it needs from the
runtime. Also builds and runs upstream's shader decoder generator (tools/shaderdec) first, as its cmake does."""
import os, subprocess, sys, concurrent.futures as cf
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
V = os.path.join(ROOT, 'runtime', 'snxvk')
# SWR_SNX_SRC: the source folder under runtime/snxvk (default src_new); SWR_SNX_TAG: the lib/obj name (default new)
S = os.path.join(V, os.environ.get('SWR_SNX_SRC', 'src_new')); NX = os.path.join(S, 'nx')
TAG = os.environ.get('SWR_SNX_TAG', 'new')
G = os.path.join(ROOT, 'gcc', 'Library', 'bin')
NODLSS = bool(os.environ.get('SWR_NO_DLSS'))
OUT = os.path.join(V, 'obj_' + TAG + ('_nodlss' if NODLSS else '')); os.makedirs(OUT, exist_ok=True)
LIB = os.path.join(V, 'libsnxvk_' + TAG + ('_nodlss.a' if NODLSS else '.a'))
NEW = os.path.isfile(os.path.join(NX, 'gpu', 'shader', 'structure.c'))   # upstream after 2026-10-22 (common/ helpers)
# upstream's code generators (GPL-3.0, same project), vendored next to the sources
TOOLS = os.path.join(S, 'tools')
DEPS = {k: os.path.join(V, 'deps', d) for k, d in (('volk', 'volk-vulkan-sdk-1.4.363.0'), ('vma', 'VulkanMemoryAllocator-3.4.0'), ('vkh', 'Vulkan-Headers-1.4.365'), ('xxh', 'xxHash-0.8.3'))}
# upstream src/nx/CMakeLists.txt: the gpu, shader and vk components (LLVM macro JIT, FFmpeg video and the video
# component left out: our runtime has its own video; the JIT is a stub, the interpreter runs every macro)
SRC = ('gpu/gpu.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/mme_hle.c gpu/maxwell/mme_jit_stub.c '
       'gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c gpu/maxwell/write_pages.c gpu/maxwell/maxwell3d.c '
       'gpu/renderer.c gpu/nvn_trace.c gpu/shipped_pass.c '
       'gpu/shader/shader.c gpu/shader/decode.c gpu/shader/corpus.c gpu/shader/shipped.c gpu/shader/spirv_translate.c '
       'gpu/shader/spirv_compute.c gpu/shader/spirv_emit.c '
       'gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/record.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c '
       'gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/pipeline.c gpu/vk/pipeline_file.c gpu/vk/render.c gpu/vk/release.c '
       'gpu/vk/query.c gpu/vk/compute.c gpu/vk/pending_pages.c gpu/vk/memory.c gpu/vk/renderdoc.c gpu/vk/astc.c '
       'gpu/vk/blocklinear.c gpu/vk/texconv.c gpu/vk/owned.c gpu/vk/dma.c gpu/vk/blit.c gpu/vk/ui.c '
       'gpu/texture/blocklinear.c gpu/texture/depth.c gpu/texture/etc2.c gpu/vk/vic.c gpu/vk/gpuprof.c '
       'gpu/vk/hdr.c gpu/vk/swr_settings.c').split() + (['gpu/vk/dlss_stub.c'] if NODLSS else ['gpu/vk/dlss.c'])
if NEW:   # ScarletNX main 112db409: spirv_emit and the Vulkan device helpers moved to common/, XXH3 hashing, structured
          # control flow (structure.c), the FTZ probe; etc2.c went to the tests
    SRC = [f for f in SRC if f not in ('gpu/shader/spirv_emit.c', 'gpu/texture/etc2.c')]
    SRC += ['gpu/shader/structure.c', 'gpu/vk/ftz_probe.c', 'gpu/vk/xxh3.c', '../common/utilities/spirv_emit.c',
            '../common/gpu/vk/vk_common.c', '../common/gpu/vk/ring.c', '../common/gpu/vk/swapchain.c']
if os.environ.get('SWR_SNX_ANY'):   # bisecting upstream: whichever of these files that version has
    _all = SRC + ['gpu/shader/spirv_emit.c', 'gpu/texture/etc2.c', 'gpu/shader/structure.c', 'gpu/vk/ftz_probe.c',
                  'gpu/vk/xxh3.c', '../common/utilities/spirv_emit.c', '../common/gpu/vk/vk_common.c',
                  '../common/gpu/vk/ring.c', '../common/gpu/vk/swapchain.c']
    SRC = [f for i, f in enumerate(_all) if f not in _all[:i] and os.path.isfile(os.path.join(NX, f))]
    NEW = 'SNX_SHIP_WALK' in open(os.path.join(NX, 'gpu', 'shipped_pass.c'), encoding='utf-8', errors='replace').read()
REDIRECT = {'g_opt_vsync': 'snxvk_opt_vsync'}
GEN = os.path.join(OUT, 'shaderdec')
INC = ['-I' + S, '-I' + NX, '-I' + GEN, '-I' + DEPS['volk'], '-I' + os.path.join(DEPS['vma'], 'include'),
       '-I' + os.path.join(DEPS['vkh'], 'include'), '-I' + DEPS['xxh'], '-I' + os.path.join(ROOT, 'runtime')]
CF = ['-std=gnu2x', '-O2', '-fno-strict-aliasing', '-DVK_NO_PROTOTYPES', '-DVK_USE_PLATFORM_WIN32_KHR', '-w'] + INC
if NEW:   # cmake/ship_walk.cmake: the shipped-shader walk's version, the hash of its sources
    import hashlib
    _h = ''.join(hashlib.sha256(open(os.path.join(NX, f), 'rb').read()).hexdigest() for f in
                 ('gpu/shipped_pass.c', 'gpu/shipped_pass.h', 'gpu/shader/shipped.c', 'gpu/shader/shipped.h'))
    CF.append('-DSNX_SHIP_WALK="%s"' % hashlib.sha256(_h.encode()).hexdigest()[:16])
ENV = dict(os.environ, PATH=G + os.pathsep + os.environ.get('PATH', ''))


def cc(args):
    r = subprocess.run(args, capture_output=True, text=True, env=ENV)
    if r.returncode:
        sys.exit('build failed: ' + ' '.join(args[-3:]) + '\n' + r.stderr[-4000:])


def shaderdec():
    os.makedirs(GEN, exist_ok=True)
    exe = os.path.join(OUT, 'shaderdec.exe')
    cc([os.path.join(G, 'gcc.exe'), '-std=c99', '-O2', os.path.join(TOOLS, 'shaderdec', 'shaderdec.c'), '-o', exe])
    cc([exe, '--enum', os.path.join(GEN, 'maxwell_opcodes.h'), '--decoder', os.path.join(GEN, 'maxwell_decode.inc'),
        os.path.join(NX, 'gpu', 'shader', 'maxwell_opcodes.def')])


def settingsgen():
    exe = os.path.join(OUT, 'settingsgen.exe')
    cc([os.path.join(G, 'gcc.exe'), '-std=c99', '-O2', os.path.join(TOOLS, 'settingsgen', 'settingsgen.c'), '-o', exe])
    cc([exe, '--out', GEN, os.path.join(NX, 'runtime', 'settings.txt')])


# glslang 16.6.0 (the version ScarletNX's cmake pins): GLSLANG=path, or glslang on PATH, or third_party\glslang\bin
import shutil
GLSLANG = (os.environ.get('GLSLANG') or shutil.which('glslang')
           or os.path.join(ROOT, 'third_party', 'glslang', 'bin', 'glslang.exe'))
SPV = [('gpu/texture/astc.comp', 'astc_spv', 'nx/gpu/texture/astc_spv.h', []),
       ('gpu/texture/blocklinear.comp', 'blocklinear_spv', 'nx/gpu/texture/blocklinear_spv.h', []),
       ('gpu/texture/texconv.comp', 'texconv_spv', 'nx/gpu/texture/texconv_spv.h', []),
       ('gpu/video/vic.comp', 'vic_spv', 'nx/gpu/video/vic_spv.h', []),
       ('gpu/video/vic.comp', 'vic_unaligned_spv', 'nx/gpu/video/vic_spv.h', ['VIC_UNALIGNED']),
       ('gpu/vk/query_predicate.comp', 'query_predicate_spv', 'nx/gpu/vk/query_predicate_spv.h', [])]
if os.path.isfile(os.path.join(NX, 'gpu', 'vk', 'ftz_probe.comp')): SPV.append(('gpu/vk/ftz_probe.comp', 'ftz_probe_spv', 'nx/gpu/vk/ftz_probe_spv.h', []))


def spirv():   # cmake/glsl.cmake + spirv_source.cmake: the SPIR-V words in a C array beside its header
    out = []
    for src, name, header, defs in SPV:
        words = os.path.join(GEN, name + '.inc')
        cc([GLSLANG, '-V', '--target-env', 'vulkan1.1', '-g0', '-x'] + ['-D' + d for d in defs] + ['-o', words, os.path.join(NX, src)])
        c = os.path.join(GEN, name + '.c')
        with open(c, 'w') as f:
            f.write('/* generated from %s by glslang (build_snxvk2.py) */\n#include "%s"\n' % (src, header))
            f.write('const u32 %s[] = {\n#include "%s.inc"\n};\nconst u64 %s_size = sizeof %s;\n' % (name, name, name, name))
        out.append(c)
    return out


def main():
    shaderdec()
    settingsgen()
    spv_c = spirv()
    jobs = [(os.path.join(OUT, f.replace('../', '').replace('/', '_') + '.o'), [os.path.join(G, 'gcc.exe')] + CF + ['-c', os.path.join(NX, f), '-o', os.path.join(OUT, f.replace('../', '').replace('/', '_') + '.o')]) for f in SRC]
    for c in spv_c: jobs.append((c[:-2] + '.o', [os.path.join(G, 'gcc.exe')] + CF + ['-c', c, '-o', c[:-2] + '.o']))
    jobs.append((os.path.join(OUT, 'settings.o'), [os.path.join(G, 'gcc.exe')] + CF + ['-c', os.path.join(GEN, 'settings.c'), '-o', os.path.join(OUT, 'settings.o')]))   # g_settings with its defaults
    jobs.append((os.path.join(OUT, 'volk.o'), [os.path.join(G, 'gcc.exe')] + CF + ['-c', os.path.join(DEPS['volk'], 'volk.c'), '-o', os.path.join(OUT, 'volk.o')]))
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: list(ex.map(lambda j: cc(j[1]), jobs))
    vma = os.path.join(OUT, 'vma.o')
    cc([os.path.join(G, 'g++.exe'), '-std=c++17', '-O2', '-w', '-DVK_NO_PROTOTYPES'] + INC + ['-c', os.path.join(NX, 'gpu/vk/vma.cpp'), '-o', vma])
    merged = os.path.join(OUT, 'snxvk_c.o')
    cc([os.path.join(G, 'ld.exe'), '-r', '-o', merged] + [o for o, _ in jobs])
    nm = subprocess.run([os.path.join(G, 'nm.exe'), '--defined-only', '-g', merged], capture_output=True, text=True).stdout.split('\n')
    defs = sorted({l.split()[-1] for l in nm if len(l.split()) == 3})
    with open(os.path.join(OUT, 'rename.txt'), 'w') as f:
        for n in defs: f.write(f'{n} snxv_{n}\n')
        for a, b in REDIRECT.items(): f.write(f'{a} {b}\n')
    renamed = os.path.join(OUT, 'snxvk_renamed.o')
    cc([os.path.join(G, 'objcopy.exe'), '--redefine-syms=' + os.path.join(OUT, 'rename.txt'), merged, renamed])
    if os.path.exists(LIB): os.remove(LIB)
    vma_r = os.path.join(OUT, 'vma_renamed.o')
    cc([os.path.join(G, 'objcopy.exe'), '--redefine-syms=' + os.path.join(OUT, 'rename.txt'), vma, vma_r])
    cc([os.path.join(G, 'ar.exe'), 'rcs', LIB, renamed, vma_r])
    und = subprocess.run([os.path.join(G, 'nm.exe'), '-u', renamed], capture_output=True, text=True).stdout.split()
    ext = sorted(x for x in set(und) if x != 'U' and not x.startswith('__imp_') and not x.startswith('___'))
    open(os.path.join(OUT, 'externals.txt'), 'w').write('\n'.join(ext) + '\n')
    print(f'{LIB}: {len(defs)} symbols renamed; {len(ext)} external references (externals.txt)')


main()
