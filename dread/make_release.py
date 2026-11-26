"""Build the Metroid Dread release and the source-only folder.

  make_release.py release OUT_DIR --version X.Y.Z [--exe PATH]   -> OUT_DIR\\Metroid Dread\\ (the game EXE + recompiler + licences) and a .zip
  make_release.py source  OUT_DIR                -> a clean source tree for GitHub (no game files, no generated code, no binaries
                                                    we don't have the right to publish, no NVIDIA files)

The release EXE is built with SWR_RELEASE=1 (no frame generation; DLSS code in, NVIDIA's DLL fetched by the player from NVIDIA):
python\\python.exe switchrecomp.py --build-runtime --cc gcc with SWR_RELEASE=1 set, after work-profiler\\build_imgui.py and
work-profiler\\build_snxvk2.py.
Nothing from Nintendo is copied: setup compiles the player's own files on their PC.
"""
import os, sys, shutil, zipfile, hashlib, fnmatch, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GCC = os.environ.get('SWR_RELEASE_GCC', os.path.join(ROOT, 'gcc', 'Library'))
GV = '14.2.0'; TRIPLE = 'x86_64-w64-mingw32'
# llvm-mingw (github.com/mstorsjo/llvm-mingw, UCRT x86_64): setup compiles the game with it when recompiler\clang is there
CLANG = os.environ.get('SWR_RELEASE_CLANG', os.path.join(ROOT, 'clang'))
RECOMPILER_PY = ['switchrecomp.py', 'codegen.py', 'a64.py', 'nxloader.py', 'metroid_perf.py']


def cp(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copy2(src, dst)


def cptree(src, dst, ignore=None):
    shutil.copytree(src, dst, dirs_exist_ok=True, ignore=ignore)


def gcc_version():
    versions_dir = os.path.join(GCC, 'lib', 'gcc', TRIPLE)
    versions = [v for v in os.listdir(versions_dir) if os.path.isdir(os.path.join(versions_dir, v))] if os.path.isdir(versions_dir) else []
    if GV in versions: return GV
    if len(versions) == 1: return versions[0]
    raise SystemExit('Expected one GCC version in ' + versions_dir)


def slim_gcc(dst):
    """only what compiling C and linking one DLL needs (the full package has clang, gdb, cmake ...: 1.5 GB)"""
    version = gcc_version()
    b = os.path.join(GCC, 'bin'); lx = os.path.join(GCC, 'libexec', 'gcc', TRIPLE, version); lg = os.path.join(GCC, 'lib', 'gcc', TRIPLE, version)
    for f in ('gcc.exe', 'as.exe', 'ld.exe', 'ld.bfd.exe', 'libiconv-2.dll', 'libwinpthread-1.dll', 'libintl-8.dll', 'zlib1.dll', 'libzstd.dll'):
        cp(os.path.join(b, f), os.path.join(dst, 'bin', f))
    for f in os.listdir(lx):
        if f in ('cc1.exe', 'collect2.exe', 'lto-wrapper.exe', 'lto1.exe', 'liblto_plugin.dll') or f.endswith('.dll'):
            cp(os.path.join(lx, f), os.path.join(dst, 'libexec', 'gcc', TRIPLE, version, f))
    for f in ('crtbegin.o', 'crtend.o', 'crtfastmath.o', 'libgcc.a', 'libgcc_eh.a'):
        cp(os.path.join(lg, f), os.path.join(dst, 'lib', 'gcc', TRIPLE, version, f))
    for d in ('include', 'include-fixed'):
        cptree(os.path.join(lg, d), os.path.join(dst, 'lib', 'gcc', TRIPLE, version, d))
    cptree(os.path.join(GCC, TRIPLE, 'include'), os.path.join(dst, TRIPLE, 'include'))
    cptree(os.path.join(GCC, TRIPLE, 'lib'), os.path.join(dst, TRIPLE, 'lib'))
    for f in ('as.exe', 'ld.exe', 'ld.bfd.exe', 'libiconv-2.dll', 'libintl-8.dll', 'libwinpthread-1.dll', 'libzstd.dll', 'zlib1.dll'):
        cp(os.path.join(GCC, TRIPLE, 'bin', f), os.path.join(dst, TRIPLE, 'bin', f))
    cp(os.path.join(GCC, 'version_info.txt'), os.path.join(dst, 'version_info.txt'))


def clang_major():
    d = os.path.join(CLANG, 'lib', 'clang')
    v = [x for x in os.listdir(d) if x.isdigit()] if os.path.isdir(d) else []
    if len(v) != 1: raise SystemExit('Expected one Clang version in ' + d + ' (set SWR_RELEASE_CLANG)')
    return v[0]


def slim_clang(dst):
    """llvm-mingw for x86_64 C only: the compiler, lld, the C headers and import libraries, compiler-rt builtins (the
    full package, 736 MB, has three more targets, C++ headers, lldb, clangd ...)"""
    v = clang_major(); b = os.path.join(CLANG, 'bin')
    for f in ('clang.exe', 'clang-%s.exe' % v, 'clang-target-wrapper.exe', 'ld.lld.exe', 'libLLVM-%s.dll' % v,
              'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll', 'x86_64-w64-windows-gnu.cfg', 'mingw32-common.cfg'):
        if os.path.isfile(os.path.join(b, f)): cp(os.path.join(b, f), os.path.join(dst, 'bin', f))
    cptree(os.path.join(CLANG, 'include'), os.path.join(dst, 'include'), ignore=shutil.ignore_patterns('c++'))
    cptree(os.path.join(CLANG, TRIPLE, 'lib'), os.path.join(dst, TRIPLE, 'lib'))
    cptree(os.path.join(CLANG, 'lib', 'clang', v, 'include'), os.path.join(dst, 'lib', 'clang', v, 'include'))
    rt = os.path.join('lib', 'clang', v, 'lib', 'windows', 'libclang_rt.builtins-x86_64.a')
    cp(os.path.join(CLANG, rt), os.path.join(dst, rt))
    cp(os.path.join(CLANG, 'LICENSE.TXT'), os.path.join(dst, 'LICENSE.TXT'))


LICENSES = [
    ('LICENSE.txt', 'SwitchRecomp-Apache-2.0.txt'), ('NOTICE.txt', 'SwitchRecomp-NOTICE.txt'),
    ('runtime/snxvk/LICENSE', 'ScarletNX-GPL-3.0.txt'), ('runtime/snxvk/NOTICE', 'ScarletNX-NOTICE.txt'),
    ('runtime/imgui/LICENSE.txt', 'DearImGui-MIT.txt'), ('runtime/vp9/LICENSE-libvpx.txt', 'libvpx-BSD.txt'),
    ('runtime/oh264/LICENSE', 'OpenH264-BSD.txt'), ('runtime/bcn/LICENSE_texture2ddecoder.txt', 'texture2ddecoder.txt'),
    ('python/LICENSE_PYTHON.txt', 'Python-PSF.txt'), ('runtime/xegtao/LICENSE.txt', 'XeGTAO-MIT.txt'), ('runtime/ryujinx-reverb/LICENSE.txt', 'Ryujinx-reverb-MIT.txt'),
]
ZLIB_TXT = """zlib (zlib1.dll) and SDL2 (SDL2.dll) are under the zlib licence:

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held
liable for any damages arising from the use of this software. Permission is granted to anyone to use this software for
any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following
restrictions:
1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If
   you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

zlib: Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler.  SDL2: Copyright (C) 1997-2024 Sam Lantinga.
"""
GCC_TXT = """recompiler\\gcc is GCC %s and GNU binutils (MinGW-w64, built by Brecht Sanders / winlibs.com).
GCC and binutils are GPL-3.0; the GCC runtime library exception covers code they compile. The MinGW-w64 runtime and headers
are under the ZPL / public domain / MIT-style terms listed in their headers. Source: https://gcc.gnu.org, https://sourceware.org/binutils,
https://www.mingw-w64.org, https://github.com/brechtsanders/winlibs_mingw (the exact version is in recompiler\\gcc\\version_info.txt).
"""

README = """METROID DREAD - recompiled for PC
=================================

This is not the game. It turns YOUR OWN copy of Metroid Dread (version 2.1.0) into a native Windows game:
setup compiles it on your PC. No Nintendo files, keys or firmware are included.

1. Dump Metroid Dread and its 2.1.0 update from your own Switch and extract them, with the update applied, into a
   folder with "exefs" and "romfs" inside (an emulator's "Extract RomFS" / "Extract ExeFS" with the update installed
   gives this too). This program does not dump or decrypt anything itself.
2. Start "Metroid Dread.exe". The first time it asks for that folder (and, if you extracted the update separately,
   the update folder), checks every file against the 2.1.0 file list and compiles the game with the bundled Clang
   (GCC when Clang is not there).
   That takes a few minutes and about 7 GB of space next to the EXE (less when your game files are on the same drive).
3. Then it opens to the Play screen. Settings, controller set-up, mods and save backups are all there.
   In the game, F1 (or the Home / Guide button) opens the menu; most settings change right away.

Controllers: Xbox controllers work as they are; SDL2.dll (included) adds Switch Pro, Joy-Con, DualShock 4, DualSense
and 8BitDo over USB or Bluetooth. Keyboard: arrows = D-pad, WASD = stick, X/Z/C/V = A/B/X/Y, Enter = +, Backspace = -.

Made by yozander2009-stack (project lead) with Claude by Anthropic (AI pair programmer).
Credits: SwitchRecomp by MZ Labs (recompiler and runtime); ScarletNX (native Vulkan renderer); theboy181 (60 FPS cutscenes,
21:9); Krydar, Nova77, xSeRosiSx (button-icon mods, downloaded on request; art: Xelu's FREE Controller Prompts); program icon: Metroid icon created by Smashicons - Flaticon (https://www.flaticon.com/free-icons/metroid); Intel XeGTAO (ambient occlusion); Dear ImGui; SDL2; zlib; libvpx; OpenH264; GCC / MinGW-w64; Python. Licences are in the licenses folder; the EXE is
GPL-3.0 (it contains the ScarletNX renderer) and its source is published with this release.
Metroid Dread is (c) Nintendo. This project is not affiliated with or endorsed by Nintendo.
"""


def redist(name):
    """a DLL the game ships beside it: redist\\ (a clone's), else games\\ (the development folder's)"""
    for d in ('redist', 'games'):
        p = os.path.join(ROOT, d, name)
        if os.path.isfile(p): return p
    raise SystemExit('%s not found: put it in %s. zlib1.dll: zlib 1.3.1 built for x64 (MinGW); SDL2.dll: SDL 2.x x64 '
                     'release (github.com/libsdl-org/SDL/releases)' % (name, os.path.join(ROOT, 'redist')))


def update_files(out, version):
    """update-<version>.zip and update.txt for the release in OUT/Metroid Dread (also: make_release.py update)"""
    dst = os.path.join(out, 'Metroid Dread')
    # in-place updates (runtime/dread_ui/fe_update.cpp): every file the release owns except the bundled compiler and
    # Python, in update-<version>.zip, and update.txt listing each with SHA-256 and size. The installed copy keeps
    # update.txt as the record of what is installed. Publish update.txt and the update zip as release assets too
    own = []
    for dp, dn, fn in os.walk(dst):
        rel_dir = os.path.relpath(dp, dst).replace(os.sep, '/')
        if rel_dir.startswith(('recompiler/gcc', 'recompiler/clang', 'recompiler/python')):   # compilers: not updated in place
            continue
        for n in fn:
            if rel_dir == '.' and n == 'update.txt': continue   # the manifest itself
            own.append(n if rel_dir == '.' else rel_dir + '/' + n)
    own.sort()
    pkg = 'update-%s.zip' % version
    pz = os.path.join(out, pkg)
    with zipfile.ZipFile(pz, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for r in own:
            zf.write(os.path.join(dst, r.replace('/', os.sep)), r)
    def sha(f):
        with open(f, 'rb') as h:
            return hashlib.sha256(h.read()).hexdigest()
    lines = ['# Metroid Dread PC update manifest (make_release.py)', 'version %s' % version, 'tag v%s' % version,
             'package %s %s %d' % (pkg, sha(pz), os.path.getsize(pz))]
    for r in own:
        f = os.path.join(dst, r.replace('/', os.sep))
        lines.append('file %s %d %s' % (sha(f), os.path.getsize(f), r))
    # the bundled Clang as a package of its own (extra / xfile lines, which updaters before 0.5.10 skip): an install
    # without recompiler\clang (updated from 0.5.0 or older, or a launcher's older download) fetches it once, then the
    # game code is rebuilt with it. Updates themselves stay small. Publish clang-<version>.zip as a release asset too
    cl = os.path.join(dst, 'recompiler', 'clang')
    if os.path.isdir(cl):
        xs = []
        for dp, dn, fn in os.walk(cl):
            for n in fn:
                xs.append(os.path.relpath(os.path.join(dp, n), dst).replace(os.sep, '/'))
        xs.sort()
        xpkg = 'clang-%s.zip' % version
        xz = os.path.join(out, xpkg)
        with zipfile.ZipFile(xz, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for r in xs:
                zf.write(os.path.join(dst, r.replace('/', os.sep)), r)
        lines.append('extra %s %s %d' % (xpkg, sha(xz), os.path.getsize(xz)))
        for r in xs:
            f = os.path.join(dst, r.replace('/', os.sep))
            lines.append('xfile %s %d %s' % (sha(f), os.path.getsize(f), r))
        print('compiler: %s (%.1f MB, %d files)' % (xz, os.path.getsize(xz) / 2**20, len(xs)))
    for f in (os.path.join(dst, 'update.txt'), os.path.join(out, 'update.txt')):
        with open(f, 'w', newline='\n') as h:
            h.write('\n'.join(lines) + '\n')
    print('update: %s (%.1f MB, %d files) + update.txt' % (pz, os.path.getsize(pz) / 2**20, len(own)))


def release(out, exe, version):
    dst = os.path.join(out, 'Metroid Dread')
    if os.path.exists(dst):
        raise SystemExit('%s exists: choose a new folder' % dst)
    os.makedirs(dst)
    cp(exe, os.path.join(dst, 'Metroid Dread.exe'))
    for f in ('zlib1.dll', 'SDL2.dll'):
        cp(redist(f), os.path.join(dst, f))
    # Native Vulkan's pipeline list (shader hashes and pipeline state, no game code): its pipelines built at start
    # (and the vertex layouts its pipelines need where the GPU has no dynamic vertex input, as on AMD's driver)
    for f in ('pipeline_recipes_shipped.bin', 'pipeline_recipes_layouts.bin'):
        cp(os.path.join(ROOT, 'dread', f), os.path.join(dst, 'vulkan_native', f))
    rc = os.path.join(dst, 'recompiler')
    for f in RECOMPILER_PY:
        cp(os.path.join(ROOT, f), os.path.join(rc, f))
    cp(os.path.join(ROOT, 'dread', 'dread_setup.py'), os.path.join(rc, 'dread_setup.py'))
    for f in ('dread_v210_patches.json', 'romfs_v210.manifest', 'randovania.py'):
        cp(os.path.join(ROOT, 'dread', f), os.path.join(rc, 'dread', f))
    for f in ('cpu.h', 'cpu_helpers.c'):
        cp(os.path.join(ROOT, 'runtime', f), os.path.join(rc, 'runtime', f))
    cptree(os.path.join(ROOT, 'python'), os.path.join(rc, 'python'), ignore=shutil.ignore_patterns('__pycache__'))
    slim_gcc(os.path.join(rc, 'gcc', 'Library'))   # kept: SWR_SETUP_GCC=1, and installs updated from 0.5 and older
    slim_clang(os.path.join(rc, 'clang'))
    lic = os.path.join(dst, 'licenses')
    for s, d in LICENSES:
        cp(os.path.join(ROOT, s), os.path.join(lic, d))
    open(os.path.join(lic, 'zlib-and-SDL2.txt'), 'w').write(ZLIB_TXT)
    open(os.path.join(lic, 'GCC-binutils-MinGW.txt'), 'w').write(GCC_TXT % gcc_version())
    cp(os.path.join(CLANG, 'LICENSE.TXT'), os.path.join(lic, 'LLVM-mingw-Apache-2.0-with-LLVM-exception.txt'))
    open(os.path.join(lic, 'theboy181-patches.txt'), 'w').write(
        'The 60 FPS cutscene and 21:9 patches are ports of theboy181\'s Metroid Dread v2.1.0 patches\n'
        '(https://github.com/theboy181/switch-ptchtxt-mods); 21:9 commissioned by Nikorasu081.\n')
    open(os.path.join(dst, 'README.txt'), 'w', newline='\r\n').write(README)
    # RecompLauncher (recomplauncher.com): how it starts the game, its settings and where the saves are; next to the EXE
    cp(os.path.join(ROOT, 'dread', 'recomplauncher.json'), os.path.join(dst, 'recomplauncher.json'))
    update_files(out, version)
    # one zip to download (RecompLauncher picks a release file by "Windows" and "x64" in its name)
    z = os.path.join(out, 'MetroidDread-PC-v%s-Windows-x64.zip' % version)
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for dp, dn, fn in os.walk(dst):
            for n in fn:
                p = os.path.join(dp, n); zf.write(p, os.path.relpath(p, out))
    size = sum(os.path.getsize(os.path.join(dp, n)) for dp, dn, fn in os.walk(dst) for n in fn)
    print('release: %s (%.0f MB), zip %s (%.0f MB)' % (dst, size / 2**20, z, os.path.getsize(z) / 2**20))


SRC_IGNORE = ['*.o', '*.a', '*.obj', '*.exe', '*.dll', '__pycache__', 'obj', 'obj_nodlss', 'obj_new', 'obj_new_nodlss',
              '*.tmp', '*.before*', '*.orig', '*.rej', 'externals.txt',
              'lsfg.c', 'lsfg_spvc.cpp', 'spvc']   # no frame generation (it loads Lossless Scaling's DLL) in the public source


def snx_keep(s2=None):
    """the renderer files the build reads (runtime/snxvk/src_new): the sources build_snxvk2.py compiles, everything they
    #include (C and GLSL), the generator inputs and tools, our shader sources. Upstream ScarletNX's other parts (its game
    -dump extractor and decryption, GUI, recompiler) are not built here and stay out of the public source"""
    import re
    s2 = s2 or os.path.join(ROOT, 'runtime', 'snxvk', 'src_new'); nx = os.path.join(s2, 'nx')
    b = open(os.path.join(ROOT, 'work-profiler', 'build_snxvk2.py'), encoding='utf-8').read()
    src = ''.join(re.findall(r"'([^']*)'", re.search(r"SRC = \((.*?)\)\.split\(\)", b, re.S).group(1))).split()
    for extra in re.findall(r"SRC \+= \[(.*?)\]", b, re.S):   # ScarletNX main 112db409's (common/ among them)
        src += re.findall(r"'([^']+)'", extra)
    src += ['gpu/vk/dlss.c', 'gpu/vk/vma.cpp', 'gpu/shader/maxwell_opcodes.def', 'runtime/settings.txt']
    src += re.findall(r"\('([^']+\.comp)'", b)
    todo = [os.path.join(nx, f) for f in src]
    for d in (os.path.join(nx, 'gpu', 'vk', 'shaders'), os.path.join(s2, 'tools')):
        for dp, dn, fn in os.walk(d): todo += [os.path.join(dp, n) for n in fn]
    keep = set()
    while todo:
        f = os.path.normpath(todo.pop())
        if f in keep or not os.path.isfile(f): continue
        keep.add(f)
        if f.endswith(('.c', '.h', '.cpp', '.comp', '.glsl', '.vert', '.frag', '.inc')):
            for inc in re.findall(r'#include "([^"]+)"', open(f, encoding='utf-8', errors='replace').read()):
                for base in (s2, nx, os.path.dirname(f)):
                    q = os.path.normpath(os.path.join(base, inc))
                    if os.path.isfile(q): todo.append(q); break
    return keep


_SNX_KEEP = None


def src_ignore(d, names):
    out = set(shutil.ignore_patterns(*SRC_IGNORE)(d, names))
    n = os.path.normpath(d)
    if n == os.path.normpath(os.path.join(ROOT, 'runtime')): out |= {'imgui'} & set(names)   # made by build_imgui.py
    if n == os.path.normpath(os.path.join(ROOT, 'runtime', 'snxvk')): out |= {'src'} & set(names)   # the old renderer: src_new is it
    # the renderer's dependencies: only what the build reads, and their licences. Their docs and tools are not needed,
    # and their deep paths passed Windows' 260-character limit in a clone (MEASURED 2026-10-22)
    s2 = os.path.normpath(os.path.join(ROOT, 'runtime', 'snxvk', 'src_new'))
    if n == s2 or n.startswith(s2 + os.sep):   # only what the build reads (snx_keep)
        global _SNX_KEEP
        if _SNX_KEEP is None: _SNX_KEEP = snx_keep()
        for x in names:
            q = os.path.join(n, x)
            if os.path.isdir(q):
                if not any(k.startswith(q + os.sep) for k in _SNX_KEEP): out.add(x)
            elif q not in _SNX_KEEP: out.add(x)
    if os.path.dirname(n) == os.path.normpath(os.path.join(ROOT, 'runtime', 'snxvk', 'deps')):
        keep = {'include', 'LICENSE.md', 'LICENSE.txt', 'LICENSES', 'README.md', 'volk.c', 'volk.h',
                'LICENSE', 'xxhash.h', 'xxhash.c', 'xxh3.h', 'xxh_x86dispatch.c', 'xxh_x86dispatch.h'}   # + xxHash 0.8.3
        out |= set(names) - keep
    # ScarletNX's game-file driver (container extraction and decryption): none of it is in our build, only
    # its headers are included on the way to fs.h, so the public source carries the headers and not the code
    drv = os.path.normpath(os.path.join(ROOT, 'runtime', 'snxvk', 'src_new', 'nx', 'runtime', 'driver'))
    if n == drv or n.startswith(drv + os.sep):
        out |= {x for x in names if x.endswith('.c')}
    return out


def source(out):
    if os.path.exists(out) and os.listdir(out):
        raise SystemExit('%s is not empty' % out)
    os.makedirs(out, exist_ok=True)
    for f in RECOMPILER_PY + ['LICENSE.txt', 'NOTICE.txt']:
        cp(os.path.join(ROOT, f), os.path.join(out, f))
    cptree(os.path.join(ROOT, 'runtime'), os.path.join(out, 'runtime'), ignore=src_ignore)
    # prebuilt third-party libraries the build links (BSD licences, sources linked in their READMEs)
    for rel in ('runtime/vp9/win/libvpx.a', 'runtime/oh264/win/libdecoder.a', 'runtime/oh264/win/libcommon.a'):
        if os.path.isfile(os.path.join(ROOT, rel)):
            cp(os.path.join(ROOT, rel), os.path.join(out, rel))
    for f in ('dread_setup.py', 'make_release.py', 'make_patches.py', 'make_manifest.py', 'randovania.py', 'dread_v210_patches.json', 'romfs_v210.manifest', 'DESIGN.md'):
        cp(os.path.join(ROOT, 'dread', f), os.path.join(out, 'dread', f))
    im = os.path.join(ROOT, 'third_party', 'imgui-1.92.5')
    for f in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'imgui.h', 'imconfig.h', 'imgui_internal.h',
              'imstb_rectpack.h', 'imstb_textedit.h', 'imstb_truetype.h', 'LICENSE.txt'):
        cp(os.path.join(im, f), os.path.join(out, 'third_party', 'imgui-1.92.5', f))
    for f in ('imgui_impl_win32', 'imgui_impl_opengl3', 'imgui_impl_dx11'):
        for ext in ('.cpp', '.h'):
            cp(os.path.join(im, 'backends', f + ext), os.path.join(out, 'third_party', 'imgui-1.92.5', 'backends', f + ext))
    cp(os.path.join(im, 'backends', 'imgui_impl_opengl3_loader.h'), os.path.join(out, 'third_party', 'imgui-1.92.5', 'backends', 'imgui_impl_opengl3_loader.h'))
    for f in ('build_imgui.py', 'build_snxvk2.py', 'gen_spv_headers.py', 'update_test.py', 'test_randovania_loader.py'):
        cp(os.path.join(ROOT, 'work-profiler', f), os.path.join(out, 'work-profiler', f))
    for f in os.listdir(os.path.join(ROOT, 'linux')):
        p = os.path.join(ROOT, 'linux', f)
        if os.path.isfile(p) and (f.endswith('.sh') or f.endswith('.h') or f.endswith('.txt')):
            cp(p, os.path.join(out, 'linux', f))
    for f in ('run.sh', 'README-SteamDeck.txt', 'Metroid Dread.ini'):   # the Steam Deck package's own files (linux/package.sh)
        cp(os.path.join(ROOT, 'linux', 'dist', f), os.path.join(out, 'linux', 'dist', f))
    for f in ('AppRun', 'metroid-dread.desktop'):   # the AppImage's own files (linux/make_appimage.sh)
        cp(os.path.join(ROOT, 'linux', 'appimage', f), os.path.join(out, 'linux', 'appimage', f))
    cp(os.path.join(ROOT, 'runtime', 'snxvk', 'LICENSE'), os.path.join(out, 'LICENSE-GPL-3.0.txt'))
    open(os.path.join(out, '.gitignore'), 'w').write('\n'.join(['gcc/', 'clang/', 'python/', 'bin/', 'games/', '*_build/', '*_data/', '*_code.dll', '*.o', '*.a',
        '!runtime/vp9/win/libvpx.a', '!runtime/oh264/win/*.a', 'runtime/imgui/', 'redist/', 'cleanbuild_test/', 'runtime/snxvk/obj*/', '__pycache__/', 'release/', '*.log', 'shader_cache/', 'vulkan_native/', '',
        '# Linux build output (linux/*.sh)', '*.so', 'linux/test/', 'linux/dist/MetroidDread-Linux/', 'linux/dist/*.tar.gz', 'linux/dist/*.AppImage', 'linux/dist/update-linux.txt', 'linux/appimage/cache/', 'linux/appimage/AppDir/', '',
        '# never commit binaries, archives or game files', '*.exe', '*.dll', '*.pdb', '*.zip', '*.7z', '*.rar', '*.nsp', '*.xci',
        '*.nca', '*.nso', '*.keys', 'exefs/', 'romfs/', 'ExeFS/', 'RomFS/', 'mods/', '']))
    # line endings exactly as written (some runtime files are CRLF by rule, the Linux scripts must stay LF)
    open(os.path.join(out, '.gitattributes'), 'w', newline='\n').write('* -text\n')
    cp(os.path.join(ROOT, 'DLSS.md'), os.path.join(out, 'DLSS.md'))
    # files that only belong in the repository: at its root since the repository became the source tree
    for f in ('README.md', 'CHANGELOG.md', 'CONTRIBUTING.md', 'build.bat'):
        cp(os.path.join(ROOT, f), os.path.join(out, f))
    cptree(os.path.join(ROOT, '.github'), os.path.join(out, '.github'))
    cptree(os.path.join(ROOT, 'docs'), os.path.join(out, 'docs'))
    n = sum(len(fn) for dp, dn, fn in os.walk(out))
    print('source: %s (%d files)' % (out, n))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    if sys.argv[1] == 'release':
        exe = os.path.join(ROOT, 'bin', 'runtime_win64_win32.exe'); version = None
        args = sys.argv[3:]
        while args:   # --exe PATH or --exe=PATH (the spaced form was ignored before, which packaged bin\'s EXE)
            a = args.pop(0)
            if a == '--exe' and args: exe = args.pop(0)
            elif a.startswith('--exe='): exe = a[6:]
            elif a == '--version' and args: version = args.pop(0)
            elif a.startswith('--version='): version = a[10:]
            else: raise SystemExit('unknown argument: %s' % a)
        # the release EXE has the Play screen and no frame generation (SWR_RELEASE=1 with the front end)
        data = open(exe, 'rb').read()
        if b'RECOMPILED FOR PC' not in data or b'Lossless.dll' in data:
            raise SystemExit('%s is not a release build: build it with SWR_RELEASE=1 (and the front end)' % exe)
        if not version or not all(p.isdigit() for p in version.split('.')):
            raise SystemExit('--version X.Y.Z is required (it names the zips and the update manifest, e.g. --version 0.2.2)')
        release(os.path.abspath(sys.argv[2]), exe, version)
    elif sys.argv[1] == 'update':   # remake the update files for an existing release folder: update OUT_DIR --version X.Y.Z
        args = sys.argv[3:]; version = args[1] if len(args) == 2 and args[0] == '--version' else (args[0][10:] if args and args[0].startswith('--version=') else None)
        if not version or not all(p.isdigit() for p in version.split('.')):
            raise SystemExit('usage: make_release.py update OUT_DIR --version X.Y.Z')
        update_files(os.path.abspath(sys.argv[2]), version)
    elif sys.argv[1] == 'source':
        source(os.path.abspath(sys.argv[2]))
    else:
        raise SystemExit(__doc__)
