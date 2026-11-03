"""Build runtime/imgui/libimgui.a: Dear ImGui (MIT, third_party/imgui-1.92.5) core + the Win32 and OpenGL3 backends,
for the launcher built into the game EXE (runtime/launcher_ui.cpp). Headers are copied next to the library so the
runtime build needs no path into third_party."""
import os, shutil, subprocess, sys, concurrent.futures as cf
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'third_party', 'imgui-1.92.5'); OUT = os.path.join(ROOT, 'runtime', 'imgui')
OBJ = os.path.join(OUT, 'obj'); G = os.environ.get('SWR_GCC_BIN') or os.path.join(ROOT, 'gcc', 'Library', 'bin')
FILES = ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp',
         'backends/imgui_impl_win32.cpp', 'backends/imgui_impl_opengl3.cpp', 'backends/imgui_impl_dx11.cpp']
HEADERS = ['imgui.h', 'imconfig.h', 'imgui_internal.h', 'imstb_rectpack.h', 'imstb_textedit.h', 'imstb_truetype.h',
           'backends/imgui_impl_win32.h', 'backends/imgui_impl_opengl3.h', 'backends/imgui_impl_dx11.h', 'backends/imgui_impl_opengl3_loader.h', 'LICENSE.txt']
CF = ['-O2', '-w', '-fno-exceptions', '-fno-rtti', '-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS',
      '-DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS', '-DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD', '-I' + SRC]   # controllers: our own reader

def cc(job):
    src, o = job
    r = subprocess.run([os.path.join(G, 'g++.exe')] + CF + ['-c', src, '-o', o], capture_output=True, text=True)
    if r.returncode: sys.exit('build failed: %s\n%s' % (src, r.stderr[-3000:]))
    return o

def main():
    os.makedirs(OBJ, exist_ok=True)
    env_path = G + os.pathsep + os.environ.get('PATH', ''); os.environ['PATH'] = env_path
    jobs = [(os.path.join(SRC, f), os.path.join(OBJ, os.path.basename(f).replace('.cpp', '.o'))) for f in FILES]
    with cf.ThreadPoolExecutor() as ex: objs = list(ex.map(cc, jobs))
    lib = os.path.join(OUT, 'libimgui.a')
    if os.path.exists(lib): os.remove(lib)
    r = subprocess.run([os.path.join(G, 'ar.exe'), 'rcs', lib] + objs, capture_output=True, text=True)
    if r.returncode: sys.exit(r.stderr)
    for h in HEADERS: shutil.copy2(os.path.join(SRC, h), os.path.join(OUT, os.path.basename(h)))
    # sources for the Linux runtime, which compiles ImGui itself (its build only copies runtime/)
    os.makedirs(os.path.join(OUT, 'src'), exist_ok=True)
    for f in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'backends/imgui_impl_opengl3.cpp'):
        shutil.copy2(os.path.join(SRC, f), os.path.join(OUT, 'src', os.path.basename(f)))
    print(lib, os.path.getsize(lib))

if __name__ == '__main__':
    main()
