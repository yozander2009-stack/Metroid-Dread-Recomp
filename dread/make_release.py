"""Build the Metroid Dread release and the source-only folder.

  make_release.py release OUT_DIR --version X.Y.Z [--exe PATH]   -> OUT_DIR\\Metroid Dread\\ (the game EXE + recompiler + licences) and a .zip
  make_release.py source  OUT_DIR                -> a clean source tree for GitHub (no game files, no generated code, no binaries
                                                    we don't have the right to publish, no DLSS)

The release EXE is built with SWR_RELEASE=1 (no DLSS, no frame generation): python\\python.exe switchrecomp.py --build-runtime
--cc gcc with SWR_RELEASE=1 set, after work-profiler\\build_imgui.py and SWR_NO_DLSS=1 work-profiler\\build_snxvk.py.
Nothing from Nintendo is copied: setup compiles the player's own files on their PC.
"""
import os, sys, shutil, zipfile, hashlib, fnmatch, time, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GCC = os.path.join(ROOT, 'gcc', 'Library')
GV = '14.2.0'; TRIPLE = 'x86_64-w64-mingw32'
RECOMPILER_PY = ['switchrecomp.py', 'codegen.py', 'a64.py', 'nxloader.py', 'metroid_perf.py']


def cp(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copy2(src, dst)


def cptree(src, dst, ignore=None):
    shutil.copytree(src, dst, dirs_exist_ok=True, ignore=ignore)


def slim_gcc(dst):
    """only what compiling C and linking one DLL needs (the full package has clang, gdb, cmake ...: 1.5 GB)"""
    b = os.path.join(GCC, 'bin'); lx = os.path.join(GCC, 'libexec', 'gcc', TRIPLE, GV); lg = os.path.join(GCC, 'lib', 'gcc', TRIPLE, GV)
    for f in ('gcc.exe', 'as.exe', 'ld.exe', 'ld.bfd.exe', 'libiconv-2.dll', 'libwinpthread-1.dll', 'libintl-8.dll', 'zlib1.dll', 'libzstd.dll'):
        cp(os.path.join(b, f), os.path.join(dst, 'bin', f))
    for f in os.listdir(lx):
        if f in ('cc1.exe', 'collect2.exe', 'lto-wrapper.exe', 'lto1.exe', 'liblto_plugin.dll') or f.endswith('.dll'):
            cp(os.path.join(lx, f), os.path.join(dst, 'libexec', 'gcc', TRIPLE, GV, f))
    for f in ('crtbegin.o', 'crtend.o', 'crtfastmath.o', 'libgcc.a', 'libgcc_eh.a'):
        cp(os.path.join(lg, f), os.path.join(dst, 'lib', 'gcc', TRIPLE, GV, f))
    for d in ('include', 'include-fixed'):
        cptree(os.path.join(lg, d), os.path.join(dst, 'lib', 'gcc', TRIPLE, GV, d))
    cptree(os.path.join(GCC, TRIPLE, 'include'), os.path.join(dst, TRIPLE, 'include'))
    cptree(os.path.join(GCC, TRIPLE, 'lib'), os.path.join(dst, TRIPLE, 'lib'))
    for f in ('as.exe', 'ld.exe', 'ld.bfd.exe', 'libiconv-2.dll', 'libintl-8.dll', 'libwinpthread-1.dll', 'libzstd.dll', 'zlib1.dll'):
        cp(os.path.join(GCC, TRIPLE, 'bin', f), os.path.join(dst, TRIPLE, 'bin', f))
    cp(os.path.join(GCC, 'version_info.txt'), os.path.join(dst, 'version_info.txt'))


LICENSES = [
    ('LICENSE.txt', 'SwitchRecomp-Apache-2.0.txt'), ('NOTICE.txt', 'SwitchRecomp-NOTICE.txt'),
    ('runtime/snxvk/LICENSE', 'ScarletNX-GPL-3.0.txt'), ('runtime/snxvk/NOTICE', 'ScarletNX-NOTICE.txt'),
    ('runtime/imgui/LICENSE.txt', 'DearImGui-MIT.txt'), ('runtime/vp9/LICENSE-libvpx.txt', 'libvpx-BSD.txt'),
    ('runtime/oh264/LICENSE', 'OpenH264-BSD.txt'), ('runtime/bcn/LICENSE_texture2ddecoder.txt', 'texture2ddecoder.txt'),
    ('python/LICENSE_PYTHON.txt', 'Python-PSF.txt'),
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
""" % GV

README = """METROID DREAD - recompiled for PC
=================================

This is not the game. It turns YOUR OWN copy of Metroid Dread (version 2.1.0) into a native Windows game:
setup compiles it on your PC. No Nintendo files, keys or firmware are included.

1. Dump Metroid Dread and its 2.1.0 update from your own Switch and extract them, with the update applied, into a
   folder with "exefs" and "romfs" inside (an emulator's "Extract RomFS" / "Extract ExeFS" with the update installed
   gives this too). This program does not dump or decrypt anything itself.
2. Start "Metroid Dread.exe". The first time it asks for that folder (and, if you extracted the update separately,
   the update folder), checks every file against the 2.1.0 file list and compiles the game with the bundled GCC.
   That takes a few minutes and about 7 GB of space next to the EXE (less when your game files are on the same drive).
3. Then it opens to the Play screen. Settings, controller set-up, mods and save backups are all there.
   In the game, F1 (or the Home / Guide button) opens the menu; most settings change right away.

Controllers: Xbox controllers work as they are; SDL2.dll (included) adds Switch Pro, Joy-Con, DualShock 4, DualSense
and 8BitDo over USB or Bluetooth. Keyboard: arrows = D-pad, WASD = stick, X/Z/C/V = A/B/X/Y, Enter = +, Backspace = -.

Made by yozander2009-stack (project lead) with Claude by Anthropic (AI pair programmer).
Credits: SwitchRecomp by MZ Labs (recompiler and runtime); ScarletNX (native Vulkan renderer); theboy181 (60 FPS cutscenes,
21:9); Krydar, Nova77, xSeRosiSx (button-icon mods, downloaded on request; art: Xelu's FREE Controller Prompts); program icon: Metroid icon created by Smashicons - Flaticon (https://www.flaticon.com/free-icons/metroid); Dear ImGui; SDL2; zlib; libvpx; OpenH264; GCC / MinGW-w64; Python. Licences are in the licenses folder; the EXE is
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
        if rel_dir.startswith('recompiler/gcc') or rel_dir.startswith('recompiler/python'):
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
    rc = os.path.join(dst, 'recompiler')
    for f in RECOMPILER_PY:
        cp(os.path.join(ROOT, f), os.path.join(rc, f))
    cp(os.path.join(ROOT, 'dread', 'dread_setup.py'), os.path.join(rc, 'dread_setup.py'))
    for f in ('dread_v210_patches.json', 'romfs_v210.manifest'):
        cp(os.path.join(ROOT, 'dread', f), os.path.join(rc, 'dread', f))
    for f in ('cpu.h', 'cpu_helpers.c'):
        cp(os.path.join(ROOT, 'runtime', f), os.path.join(rc, 'runtime', f))
    cptree(os.path.join(ROOT, 'python'), os.path.join(rc, 'python'), ignore=shutil.ignore_patterns('__pycache__'))
    slim_gcc(os.path.join(rc, 'gcc', 'Library'))
    lic = os.path.join(dst, 'licenses')
    for s, d in LICENSES:
        cp(os.path.join(ROOT, s), os.path.join(lic, d))
    open(os.path.join(lic, 'zlib-and-SDL2.txt'), 'w').write(ZLIB_TXT)
    open(os.path.join(lic, 'GCC-binutils-MinGW.txt'), 'w').write(GCC_TXT)
    open(os.path.join(lic, 'theboy181-patches.txt'), 'w').write(
        'The 60 FPS cutscene and 21:9 patches are ports of theboy181\'s Metroid Dread v2.1.0 patches\n'
        '(https://github.com/theboy181/switch-ptchtxt-mods); 21:9 commissioned by Nikorasu081.\n')
    open(os.path.join(dst, 'README.txt'), 'w', newline='\r\n').write(README)
    update_files(out, version)
    # one zip to download
    z = os.path.join(out, 'MetroidDread-PC-v%s.zip' % version)
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for dp, dn, fn in os.walk(dst):
            for n in fn:
                p = os.path.join(dp, n); zf.write(p, os.path.relpath(p, out))
    size = sum(os.path.getsize(os.path.join(dp, n)) for dp, dn, fn in os.walk(dst) for n in fn)
    print('release: %s (%.0f MB), zip %s (%.0f MB)' % (dst, size / 2**20, z, os.path.getsize(z) / 2**20))


SRC_IGNORE = ['*.o', '*.a', '*.obj', '*.exe', '*.dll', '__pycache__', 'obj', 'obj_nodlss', 'obj_new', 'obj_new_nodlss',
              '*.tmp', '*.before*', '*.orig', '*.rej', 'externals.txt',
              'lsfg.c', 'lsfg_spvc.cpp', 'spvc']   # no frame generation (it loads Lossless Scaling's DLL) in the public source


def snx_keep():
    """the renderer files the build reads (runtime/snxvk/src_new): the sources build_snxvk2.py compiles, everything they
    #include (C and GLSL), the generator inputs and tools, our shader sources. Upstream ScarletNX's other parts (its game
    -dump extractor and decryption, GUI, recompiler) are not built here and stay out of the public source"""
    import re
    s2 = os.path.join(ROOT, 'runtime', 'snxvk', 'src_new'); nx = os.path.join(s2, 'nx')
    b = open(os.path.join(ROOT, 'work-profiler', 'build_snxvk2.py'), encoding='utf-8').read()
    src = ''.join(re.findall(r"'([^']*)'", re.search(r"SRC = \((.*?)\)\.split\(\)", b, re.S).group(1))).split()
    src += ['gpu/vk/dlss.c', 'gpu/vk/dlss_stub.c', 'gpu/vk/vma.cpp', 'gpu/shader/maxwell_opcodes.def', 'runtime/settings.txt']
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
        keep = {'include', 'LICENSE.md', 'LICENSE.txt', 'LICENSES', 'README.md', 'volk.c', 'volk.h'}
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
    for f in ('dread_setup.py', 'make_release.py', 'make_patches.py', 'make_manifest.py', 'dread_v210_patches.json', 'romfs_v210.manifest', 'DESIGN.md'):
        cp(os.path.join(ROOT, 'dread', f), os.path.join(out, 'dread', f))
    im = os.path.join(ROOT, 'third_party', 'imgui-1.92.5')
    for f in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'imgui.h', 'imconfig.h', 'imgui_internal.h',
              'imstb_rectpack.h', 'imstb_textedit.h', 'imstb_truetype.h', 'LICENSE.txt'):
        cp(os.path.join(im, f), os.path.join(out, 'third_party', 'imgui-1.92.5', f))
    for f in ('imgui_impl_win32', 'imgui_impl_opengl3', 'imgui_impl_dx11'):
        for ext in ('.cpp', '.h'):
            cp(os.path.join(im, 'backends', f + ext), os.path.join(out, 'third_party', 'imgui-1.92.5', 'backends', f + ext))
    cp(os.path.join(im, 'backends', 'imgui_impl_opengl3_loader.h'), os.path.join(out, 'third_party', 'imgui-1.92.5', 'backends', 'imgui_impl_opengl3_loader.h'))
    for f in ('build_imgui.py', 'build_snxvk2.py', 'gen_spv_headers.py', 'update_test.py'):
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
    open(os.path.join(out, '.gitignore'), 'w').write('\n'.join(['gcc/', 'python/', 'bin/', 'games/', '*_build/', '*_data/', '*_code.dll', '*.o', '*.a',
        '!runtime/vp9/win/libvpx.a', '!runtime/oh264/win/*.a', 'runtime/imgui/', 'redist/', 'cleanbuild_test/', 'runtime/snxvk/obj*/', '__pycache__/', 'release/', '*.log', 'shader_cache/', 'vulkan_native/', '',
        '# Linux build output (linux/*.sh)', '*.so', 'linux/test/', 'linux/dist/MetroidDread-Linux/', 'linux/dist/*.tar.gz', 'linux/dist/*.AppImage', 'linux/appimage/cache/', 'linux/appimage/AppDir/', '',
        '# never commit binaries, archives or game files', '*.exe', '*.dll', '*.pdb', '*.zip', '*.7z', '*.rar', '*.nsp', '*.xci',
        '*.nca', '*.nso', '*.keys', 'exefs/', 'romfs/', 'ExeFS/', 'RomFS/', 'mods/', '']))
    # line endings exactly as written (some runtime files are CRLF by rule, the Linux scripts must stay LF)
    open(os.path.join(out, '.gitattributes'), 'w', newline='\n').write('* -text\n')
    open(os.path.join(out, 'README.md'), 'w', encoding='utf-8').write(SOURCE_README)
    cp(os.path.join(ROOT, 'dread', 'DLSS.md'), os.path.join(out, 'DLSS.md'))
    # files that only belong in the repository (CONTRIBUTING.md, build.bat, .github issue forms)
    repo = os.path.join(ROOT, 'dread', 'repo')
    for dp, dn, fn in os.walk(repo):
        for n in fn:
            p = os.path.join(dp, n); cp(p, os.path.join(out, os.path.relpath(p, repo)))
    n = sum(len(fn) for dp, dn, fn in os.walk(out))
    print('source: %s (%d files)' % (out, n))


SOURCE_README = """# Metroid Dread — recompiled for PC

> **Work in progress.** This project was shared on Reddit and elsewhere before it was ready: the planned first proper
> release is 1.0.0, and current builds (0.x) are early previews. Expect crashes, setup problems and rough edges on
> hardware it has not been tested on. If something breaks, please open an issue and attach the `.log` file from the
> game folder, plus your CPU, GPU, RAM and Windows version. I'll do the best I can to resolve the errors and make this
> a good experience.

One Windows EXE that sets up, configures and runs a native PC build of **your own** copy of Metroid Dread 2.1.0.
It is the SwitchRecomp runtime (by MZ Labs) with a Metroid Dread front end built in: first-run setup that compiles
the game with a bundled GCC, a Play / settings screen, and an in-game menu (F1 or the Home / Guide button).

![The Play screen](docs/images/play-screen.png)

**No Nintendo files are in this repository**: no game code, no RomFS, no keys, no generated C.

- `dread/dread_v210_patches.json` holds small anchored edits that setup applies to the C it generates from the player's ExeFS.
- `dread/romfs_v210.manifest` lists RomFS file names, sizes and SHA-1 hashes, so setup can verify the player's files.

## Layout

| path | what |
|---|---|
| `switchrecomp.py`, `codegen.py`, `a64.py`, `nxloader.py`, `metroid_perf.py` | SwitchRecomp: ARM64 → C translator (Apache-2.0) |
| `runtime/` | the runtime: kernel, services, GPU → OpenGL, audio, video, input |
| `runtime/dread_ui/` | front end: `fe.cpp` (setup, Play, settings), `ov.cpp` (in-game menu), `ui.cpp` (theme, widgets, settings table) |
| `runtime/snxvk/` | ScarletNX's native Vulkan renderer (GPL-3.0, main b5f4f14) in `src_new/`, vendored, symbols prefixed `snxv_`; our DLSS code in `src_new/nx/gpu/vk/dlss.c` |
| `dread/` | setup driver run by the EXE, patch set, RomFS manifest, release script, design notes |
| `third_party/imgui-1.92.5/` | Dear ImGui (MIT) |
| `linux/` | scripts for the Linux / Steam Deck build |

## Setting up the game (players)

Download the release zip, not this repository. On first start the EXE asks for your game files and compiles the game.
It needs **both** parts of your own dumped copy of Metroid Dread **2.1.0**:

- the **ExeFS** (the game code: the files `main`, `main.npdm`, ...)
- the **RomFS** (the game data: `system`, `packs`, ...), with the 2.1.0 update merged in

A RomFS dump on its own is not enough. Choose the folder that holds both; they can be a few folders down (a dump
folder with one folder per title is fine). If you have the base game and the update as separate dumps, choose the
update as "Update folder". Every RomFS file is checked against the 2.1.0 file list before anything is compiled.

**Updates**: the Play screen checks this repository's releases and offers **UPDATE** when there is a
new version. Only the game's own files that changed are downloaded (a few MB), each one is checked against its
SHA-256 before anything is replaced, and the replaced files are kept so **UNDO** can go back. If the game-code patches
changed, the game is rebuilt from the files setup already installed, without asking for your game folder again.
Saves, settings and mods are never touched. It can be switched off on the Play screen.

**Not sure which settings to pick?** Press **AUTO-SET** on the Play screen. It reads your graphics card, screen and
memory and picks resolution, frame rate and quality to match; a new installation gets these automatically after
setup. Anything can still be changed afterwards.

Found a bug? See [CONTRIBUTING.md](CONTRIBUTING.md): please attach the `.log`.

## Screenshots

| Graphics settings | Controls |
|---|---|
| ![Graphics settings](docs/images/graphics-settings.png) | ![Controls](docs/images/controls.png) |

## Build from source (Windows)

The toolchain is **not** in this repository and never was: the paths below are where you put your own downloads.
The release zip bundles the same GCC and Python so that setup can compile the game on the player's PC.

- `gcc\\` in the repository root: [winlibs](https://winlibs.com) GCC 14.2.0, UCRT, POSIX threads, x64
- `python\\` in the repository root: Python 3.12 for Windows (the embeddable package is enough)
- glslang 16.6.0 (Vulkan SDK or KhronosGroup release), for the renderer's compute shaders: `GLSLANG=` or on PATH

`build.bat` checks for all three and runs the steps below (`build.bat release X.Y.Z` also packs a release):

```
python\\python.exe work-profiler\\build_imgui.py
set GLSLANG=<path to glslang.exe 16.6.0> && set SWR_NO_DLSS=1 && python\\python.exe work-profiler\\build_snxvk2.py
set SWR_RELEASE=1 && python\\python.exe switchrecomp.py --build-runtime --cc gcc
python\\python.exe dread\\make_release.py release <empty folder> --version X.Y.Z --exe bin\\runtime_win64_win32.exe
```

The release step also needs `redist\\zlib1.dll` (zlib 1.3.1, x64) and `redist\\SDL2.dll` (SDL 2.x x64 release), the two
DLLs the game ships beside it. They are not in this repository.

## Build and play on Linux

Native Linux (X11 or Wayland through XWayland, OpenGL 4.3), built from this repository with the system's GCC. The
scripts in `linux/` keep everything inside the repository: the runtime in `bin/`, the installed game in `games/`.
You need your own Metroid Dread 2.1.0 extracted to an ExeFS and a RomFS folder, as for Windows.

```
bash linux/install_deps.sh                    # gcc, X11/OpenGL headers, static libvpx, SDL2 (asks for sudo)
bash linux/setup_and_build_runtime.sh         # bin/runtime_linux_x11, with the in-game menu
bash linux/setup_game.sh GAME_FOLDER [UPDATE_FOLDER]   # checks the files, compiles games/Metroid Dread_code.so
bash linux/run.sh                             # play; settings in games/Metroid Dread.ini, log in games/Metroid Dread.log
```

`linux/run.sh` opens the same Play / setup / settings screens as the Windows EXE (`--play` skips them, `--launcher`
brings them back when skipped). `linux/build_game_so.sh` recompiles only what changed (after editing the patches or the
translator), `linux/package.sh` makes the Steam Deck package in `linux/dist/` (no game data inside), and `run_test.sh`,
`menu_test.sh` and `run_route_linux.sh` run the automated tests on a copy of the game folder (`SWR_SOFTGL=1` for
Mesa's software OpenGL). Not on Linux yet: native Vulkan, updates from the releases page, the Steam shortcut button,
the mouse free aim and F7 recording.

### AppImage

`bash linux/make_appimage.sh` builds `linux/dist/Metroid_Dread-x86_64.AppImage`: one file for players, like the Windows
EXE. It has the runtime with the Play / setup screens, the recompiler, a relocatable GCC 14.3
([Bootlin](https://toolchains.bootlin.com)) and Python 3.12
([python-build-standalone](https://github.com/astral-sh/python-build-standalone)), so nothing needs to be installed;
no game files are inside. The downloads are checked against their SHA-256 and kept in `linux/appimage/cache/`. The game
is installed in `~/.local/share/MetroidDreadRecomp` (or `METROID_DREAD_HOME`). It needs glibc 2.38 or newer, SDL2 for
controllers and an OpenGL 4.3 driver.

## What is in this repository

- Source code only, plus three prebuilt third-party static libraries, kept so the build does not need MSYS2:
  `runtime/vp9/win/libvpx.a` (libvpx 1.14.1, VP9 decoder only, build options in `runtime/vp9/README.txt`) and
  `runtime/oh264/win/*.a` (OpenH264 decoder). Rebuild them from the upstream sources if you prefer.
- `.gitignore` keeps out toolchains, build output, EXEs, DLLs, archives and anything from a game folder.
- `dread/dread_v210_patches.json` holds **our** edits only: for each patched file, the SHA-256 of the C that setup
  generates from the player's ExeFS, the line range to replace, and the replacement lines we wrote (frame pacing,
  vector growth, zlib, cutscene skip). It contains no game code, no data from the game and no decryption.
- `dread/romfs_v210.manifest` lists RomFS file names, sizes and SHA-1 hashes, used only to check the player's files.

## Contributors

- **yozander2009-stack** — project lead
- **Claude** (Anthropic) — AI pair programmer: runtime fixes and performance work, the front end and in-game menu,
  setup, the Linux build, testing and documentation

Button-icon packs (Controls > Button icons) are community mods downloaded from GameBanana when a player asks for
them, not included here: Switch to XBOX Layout by Krydar, Switch to PS4 Layout by Nova77, Switch to PS5 Layout by
xSeRosiSx, with PlayStation art from Xelu's FREE Controller Prompts.

The program icon is the [Metroid icon created by Smashicons - Flaticon](https://www.flaticon.com/free-icons/metroid)
(Flaticon licence, free with attribution).

Third-party work this builds on is credited below and on the game's Credits page.

## AI use

Large parts of this project were written with Claude (Anthropic), not only boilerplate: runtime fixes, performance
work, the front end, setup and the build scripts. Every change was built and tested on real hardware before release,
and each fix was measured. The source is all here: read it, build it yourself, and report anything that looks wrong.

## Licences

**The released EXE is distributed under GPL-3.0** (`LICENSE-GPL-3.0.txt`), because it contains the ScarletNX renderer.
Each source file keeps its own licence; Apache-2.0 and MIT code is GPL-3.0-compatible.

- SwitchRecomp: Apache-2.0. The credit "SwitchRecomp by MZ Labs" and NOTICE.txt must be kept.
- ScarletNX renderer: GPL-3.0, which makes the combined EXE GPL-3.0.
- Dear ImGui: MIT.
- libvpx and OpenH264: BSD.
- SDL2 and zlib: zlib licence.

NVIDIA DLAA: the code is here (`dlss.c`), NVIDIA's files are not, and the release is built without it. To add it to
your own build, see [DLSS.md](DLSS.md). A build with DLSS is for your own use only: never share it.

Metroid Dread is © Nintendo. This project is not affiliated with or endorsed by Nintendo.
"""

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
