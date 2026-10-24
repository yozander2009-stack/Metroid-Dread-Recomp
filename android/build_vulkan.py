#!/usr/bin/env python3
"""Cross-compile the vendored ScarletNX renderer; retain its snxv_ namespace."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import shutil
import subprocess
import build

VENDOR = build.ROOT / 'runtime/snxvk'
SOURCE = VENDOR / 'src_new'
NX = SOURCE / 'nx'
OUT = build.LOCAL / 'vulkan'
GEN = OUT / 'generated'
SOURCES = '''gpu/gpu.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/mme_hle.c
gpu/maxwell/mme_jit_stub.c gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c
gpu/maxwell/write_pages.c gpu/maxwell/maxwell3d.c gpu/renderer.c gpu/nvn_trace.c gpu/shipped_pass.c
gpu/shader/shader.c gpu/shader/decode.c gpu/shader/corpus.c gpu/shader/shipped.c
gpu/shader/spirv_translate.c gpu/shader/spirv_compute.c gpu/shader/spirv_emit.c
gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/record.c gpu/vk/format.c
gpu/vk/surface.c gpu/vk/texture.c gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/pipeline.c
gpu/vk/pipeline_file.c gpu/vk/render.c gpu/vk/release.c gpu/vk/query.c gpu/vk/compute.c
gpu/vk/pending_pages.c gpu/vk/memory.c gpu/vk/renderdoc.c gpu/vk/astc.c gpu/vk/blocklinear.c
gpu/vk/texconv.c gpu/vk/owned.c gpu/vk/dma.c gpu/vk/blit.c gpu/vk/ui.c
gpu/texture/blocklinear.c gpu/texture/depth.c gpu/texture/etc2.c gpu/vk/vic.c
gpu/vk/gpuprof.c gpu/vk/hdr.c gpu/vk/swr_settings.c gpu/vk/dlss_stub.c'''.split()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    build.adreno_dependencies()
    sdk, ndk, cmake, llvm = build.sdk_paths()
    OUT.mkdir(parents=True, exist_ok=True)
    GEN.mkdir(parents=True, exist_ok=True)
    glslang = os.environ.get('GLSLANG') or shutil.which('glslang')
    if not glslang:
        src = build.checkout('glslang', 'https://github.com/KhronosGroup/glslang.git',
                             'e1b562a8bed273a02f30b59b66a5d499793cede5')
        host = build.LOCAL / 'glslang-build'
        build.run([cmake / 'cmake', '-S', src, '-B', host, '-G', 'Ninja',
                   f'-DCMAKE_MAKE_PROGRAM={cmake / "ninja"}', '-DCMAKE_BUILD_TYPE=Release',
                   '-DENABLE_OPT=OFF', '-DBUILD_TESTING=OFF', '-DENABLE_GLSLANG_BINARIES=ON',
                   '-DENABLE_HLSL=OFF'])
        build.run([cmake / 'cmake', '--build', host, f'-j{args.jobs}'])
        glslang = host / 'StandAlone/glslang'
    host_cc = os.environ.get('HOST_CC', 'cc')
    for name in ('shaderdec', 'settingsgen'):
        exe = OUT / name
        build.run([host_cc, '-std=c99', '-O2', SOURCE / f'tools/{name}/{name}.c', '-o', exe])
        if name == 'shaderdec':
            build.run([exe, '--enum', GEN / 'maxwell_opcodes.h', '--decoder', GEN / 'maxwell_decode.inc',
                       NX / 'gpu/shader/maxwell_opcodes.def'])
        else:
            build.run([exe, '--out', GEN, NX / 'runtime/settings.txt'])
    generated = [GEN / 'settings.c']
    shaders = [('gpu/texture/astc.comp', 'astc_spv', []),
               ('gpu/texture/blocklinear.comp', 'blocklinear_spv', []),
               ('gpu/texture/texconv.comp', 'texconv_spv', []),
               ('gpu/video/vic.comp', 'vic_spv', []),
               ('gpu/video/vic.comp', 'vic_unaligned_spv', ['VIC_UNALIGNED']),
               ('gpu/vk/query_predicate.comp', 'query_predicate_spv', [])]
    for src, name, defines in shaders:
        build.run([glslang, '-V', '--target-env', 'vulkan1.1', '-g0', '-x',
                   *['-D' + d for d in defines], '-o', GEN / (name + '.inc'), NX / src])
        c = GEN / (name + '.c')
        c.write_text(f'#include <stdint.h>\nconst uint32_t {name}[]={{\n#include "{name}.inc"\n}};\n'
                     f'const uint64_t {name}_size=sizeof {name};\n')
        generated.append(c)
    includes = [SOURCE, NX, GEN, VENDOR / 'deps/volk-vulkan-sdk-1.4.363.0',
                VENDOR / 'deps/VulkanMemoryAllocator-3.4.0/include',
                VENDOR / 'deps/Vulkan-Headers-1.4.365/include', build.ROOT / 'runtime']
    flags = ['-O2', '-fPIC', '-fno-strict-aliasing', '-DVK_NO_PROTOTYPES',
             '-DVK_USE_PLATFORM_ANDROID_KHR', '-w', *['-I' + str(p) for p in includes]]
    sources = [NX / p for p in SOURCES] + generated + [includes[3] / 'volk.c']
    def compile_one(src):
        obj = OUT / (src.name + '.' + str(sources.index(src)) + '.o')
        build.run([llvm / 'aarch64-linux-android28-clang', '-std=gnu2x', *flags, '-c', src, '-o', obj])
        return obj
    with ThreadPoolExecutor(args.jobs) as pool:
        objects = list(pool.map(compile_one, sources))
    vma = OUT / 'vma.o'
    build.run([llvm / 'aarch64-linux-android28-clang++', '-std=c++17', *flags,
               '-c', NX / 'gpu/vk/vma.cpp', '-o', vma])
    merged = OUT / 'renderer.o'
    build.run([llvm / 'ld.lld', '-r', '-o', merged, *objects])
    symbols = subprocess.check_output([str(llvm / 'llvm-nm'), '-g', '--defined-only',
                                       '--format=posix', str(merged)], text=True)
    names = sorted({line.split()[0] for line in symbols.splitlines() if len(line.split()) >= 2})
    mapping = OUT / 'rename.txt'
    mapping.write_text(''.join(f'{name} snxv_{name}\n' for name in names)
                       + 'g_opt_vsync snxvk_opt_vsync\noverlay_draw snxvk_overlay_draw\n')
    renamed = []
    for obj in (merged, vma):
        dst = obj.with_suffix('.renamed.o')
        build.run([llvm / 'llvm-objcopy', '--redefine-syms=' + str(mapping), obj, dst])
        renamed.append(dst)
    library = OUT / 'libsnxvk.a'
    library.unlink(missing_ok=True)
    build.run([llvm / 'llvm-ar', 'rcs', library, *renamed])
    print(f'Vulkan renderer built: {library}')


if __name__ == '__main__':
    main()
