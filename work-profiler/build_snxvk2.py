"""Build the re-vendored ScarletNX renderer (runtime/snxvk/src_new, ScarletNX main b5f4f14f2bfc + our hooks) into
runtime/snxvk/libsnxvk_new.a (SWR_NO_DLSS=1: libsnxvk_new_nodlss.a). Same scheme as build_snxvk.py: every global it
defines is renamed snxv_<name>, so its GPU front end sits next to ours; snxvk_glue.c provides what it needs from the
runtime. Also builds and runs upstream's shader decoder generator (tools/shaderdec) first, as its cmake does.

macOS (Apple clang, no binutils): runtime/snxvk/libsnxvk_mac.a, always without DLSS. Apple's ld renames instead of
objcopy: ld -r gives every global an alias snxv_<name> and exports only the aliases, so the originals become local."""
import os, subprocess, sys, concurrent.futures as cf
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
V = os.path.join(ROOT, 'runtime', 'snxvk'); S = os.path.join(V, 'src_new'); NX = os.path.join(S, 'nx')
G = os.path.join(ROOT, 'gcc', 'Library', 'bin')
MAC = sys.platform == 'darwin'
NODLSS = bool(os.environ.get('SWR_NO_DLSS')) or MAC   # NVIDIA DLSS has no macOS side
OUT = os.path.join(V, 'obj_mac' if MAC else 'obj_new_nodlss' if NODLSS else 'obj_new'); os.makedirs(OUT, exist_ok=True)
LIB = os.path.join(V, 'libsnxvk_mac.a' if MAC else 'libsnxvk_new_nodlss.a' if NODLSS else 'libsnxvk_new.a')
# the compilers: the bundled GCC on Windows, Apple clang (Xcode command line tools) on macOS
CC = os.environ.get('CC', 'clang') if MAC else os.path.join(G, 'gcc.exe')
CXX = os.environ.get('CXX', 'clang++') if MAC else os.path.join(G, 'g++.exe')
EXE = '.bin' if MAC else '.exe'   # (not bare: obj_*/shaderdec is the generated-code folder)
# upstream's code generators (GPL-3.0, same project), vendored next to the sources
TOOLS = os.path.join(S, 'tools')
DEPS = {k: os.path.join(V, 'deps', d) for k, d in (('volk', 'volk-vulkan-sdk-1.4.363.0'), ('vma', 'VulkanMemoryAllocator-3.4.0'), ('vkh', 'Vulkan-Headers-1.4.365'))}
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
REDIRECT = {'g_opt_vsync': 'snxvk_opt_vsync'}
GEN = os.path.join(OUT, 'shaderdec')
INC = ['-I' + S, '-I' + NX, '-I' + GEN, '-I' + DEPS['volk'], '-I' + os.path.join(DEPS['vma'], 'include'),
       '-I' + os.path.join(DEPS['vkh'], 'include'), '-I' + os.path.join(ROOT, 'runtime')]
CF = ['-std=gnu2x', '-O2', '-fno-strict-aliasing', '-DVK_NO_PROTOTYPES', '-w'] + INC
if MAC:
    # their g_opt_vsync is our snxvk_opt_vsync (REDIRECT): Apple's ld can't redirect an undefined symbol, the
    # preprocessor can. -fno-common: every global a real definition, so each gets its alias
    CF += ['-fno-common', '-Dg_opt_vsync=snxvk_opt_vsync', '-mmacosx-version-min=12.0']
else:
    CF += ['-DVK_USE_PLATFORM_WIN32_KHR']
ENV = dict(os.environ, PATH=G + os.pathsep + os.environ.get('PATH', ''))


def cc(args):
    r = subprocess.run(args, capture_output=True, text=True, env=ENV)
    if r.returncode:
        sys.exit('build failed: ' + ' '.join(args[-3:]) + '\n' + r.stderr[-4000:])


def shaderdec():
    os.makedirs(GEN, exist_ok=True)
    exe = os.path.join(OUT, 'shaderdec' + EXE)
    cc([CC, '-std=c99', '-O2', os.path.join(TOOLS, 'shaderdec', 'shaderdec.c'), '-o', exe])
    cc([exe, '--enum', os.path.join(GEN, 'maxwell_opcodes.h'), '--decoder', os.path.join(GEN, 'maxwell_decode.inc'),
        os.path.join(NX, 'gpu', 'shader', 'maxwell_opcodes.def')])


def settingsgen():
    exe = os.path.join(OUT, 'settingsgen' + EXE)
    cc([CC, '-std=c99', '-O2', os.path.join(TOOLS, 'settingsgen', 'settingsgen.c'), '-o', exe])
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
    jobs = [(os.path.join(OUT, f.replace('/', '_') + '.o'), [CC] + CF + ['-c', os.path.join(NX, f), '-o', os.path.join(OUT, f.replace('/', '_') + '.o')]) for f in SRC]
    for c in spv_c: jobs.append((c[:-2] + '.o', [CC] + CF + ['-c', c, '-o', c[:-2] + '.o']))
    jobs.append((os.path.join(OUT, 'settings.o'), [CC] + CF + ['-c', os.path.join(GEN, 'settings.c'), '-o', os.path.join(OUT, 'settings.o')]))   # g_settings with its defaults
    jobs.append((os.path.join(OUT, 'volk.o'), [CC] + CF + ['-c', os.path.join(DEPS['volk'], 'volk.c'), '-o', os.path.join(OUT, 'volk.o')]))
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: list(ex.map(lambda j: cc(j[1]), jobs))
    vma = os.path.join(OUT, 'vma.o')
    cc([CXX, '-std=c++17', '-O2', '-w', '-DVK_NO_PROTOTYPES'] + (['-mmacosx-version-min=12.0'] if MAC else []) + INC + ['-c', os.path.join(NX, 'gpu/vk/vma.cpp'), '-o', vma])
    if MAC: return link_mac([o for o, _ in jobs] + [vma])
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


def link_mac(objs):
    """Apple's ld instead of objcopy: one relocatable object in which every global defined here also gets the name
    snxv_<name>, and only those names stay global (ld -r turns the unexported ones local), as the renamed GCC build"""
    merged = os.path.join(OUT, 'snxvk_c.o')
    cc(['ld', '-r', '-o', merged] + objs)
    nm = subprocess.run(['nm', '-gUj', merged], capture_output=True, text=True).stdout.split()
    defs = sorted(set(n for n in nm if n.startswith('_')))
    with open(os.path.join(OUT, 'aliases.txt'), 'w') as f:
        for n in defs: f.write('%s _snxv_%s\n' % (n, n[1:]))
    with open(os.path.join(OUT, 'exported.txt'), 'w') as f:
        for n in defs: f.write('_snxv_%s\n' % n[1:])
    renamed = os.path.join(OUT, 'snxvk_renamed.o')
    cc(['ld', '-r', '-o', renamed, merged, '-alias_list', os.path.join(OUT, 'aliases.txt'),
        '-exported_symbols_list', os.path.join(OUT, 'exported.txt')])
    if os.path.exists(LIB): os.remove(LIB)
    cc(['ar', 'rcs', LIB, renamed])
    und = subprocess.run(['nm', '-uj', renamed], capture_output=True, text=True).stdout.split()
    ext = sorted(x[1:] for x in set(und) if x.startswith('_'))
    open(os.path.join(OUT, 'externals.txt'), 'w').write('\n'.join(ext) + '\n')
    print(f'{LIB}: {len(defs)} symbols renamed; {len(ext)} external references (externals.txt)')


main()
