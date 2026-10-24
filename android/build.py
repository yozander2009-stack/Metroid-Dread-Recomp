#!/usr/bin/env python3
"""Build a personal ARM64 APK from an extracted Metroid Dread 2.1.0 ExeFS."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parent.parent
ANDROID = ROOT / 'android'
LOCAL = ANDROID / 'local'
BUILD = ANDROID / 'build'
NAME = 'MetroidDread'
PACKAGE = 'com.switchrecomp.metroiddread'
APK = ANDROID / 'MetroidDread.apk'


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def sdk_paths():
    sdk = Path(os.environ.get('ANDROID_HOME', '/opt/homebrew/share/android-commandlinetools'))
    ndk = Path(os.environ.get('ANDROID_NDK_HOME', sdk / 'ndk/27.1.12297006'))
    cmake = sdk / 'cmake/3.22.1/bin'
    llvm = next(p for p in (ndk / 'toolchains/llvm/prebuilt').iterdir() if p.is_dir()) / 'bin'
    return sdk, ndk, cmake, llvm


def checkout(name, url, commit):
    dst = LOCAL / name
    if not (dst / '.git').exists():
        run(['git', 'init', dst])
        run(['git', '-C', dst, 'remote', 'add', 'origin', url])
        run(['git', '-C', dst, 'fetch', '--depth', '1', 'origin', commit])
        run(['git', '-C', dst, 'checkout', '--detach', 'FETCH_HEAD'])
    actual = subprocess.check_output(['git', '-C', str(dst), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != commit:
        raise SystemExit(f'{dst}: expected dependency revision {commit}, found {actual}')
    return dst


def dependencies(jobs):
    sdk, ndk, cmake, llvm = sdk_paths()
    header = ANDROID / 'headers/GL/glcorearb.h'
    if not header.exists():
        header.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve('https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/6af574a14089ccfee87efe230ebcdd8742859813/api/GL/glcorearb.h', header)
    if hashlib.sha256(header.read_bytes()).hexdigest() != '65fae555a8b3b5709099837001e0c14228b3e45651e2d0fa2ad22649833921e1':
        raise SystemExit('Khronos GL header checksum changed; review the upstream update before building.')
    vpx = checkout('libvpx', 'https://chromium.googlesource.com/webm/libvpx', '1024874c5919305883187e2953de8fcb4c3d7fa6')
    if not (vpx / 'libvpx.a').exists():
        env = dict(os.environ, CC=str(llvm / 'aarch64-linux-android28-clang'),
                   CXX=str(llvm / 'aarch64-linux-android28-clang++'),
                   AR=str(llvm / 'llvm-ar'), STRIP=str(llvm / 'llvm-strip'))
        run(['./configure', '--target=arm64-android-gcc', '--disable-examples', '--disable-tools',
             '--disable-docs', '--disable-unit-tests', '--disable-vp8', '--disable-vp9-encoder',
             '--enable-pic', '--disable-shared'], cwd=vpx, env=env)
        run(['make', f'-j{jobs}'], cwd=vpx, env=env)
    oh = checkout('openh264', 'https://github.com/cisco/openh264.git', '652bdb7719f30b52b08e506645a7322ff1b2cc6f')
    if not (oh / 'libdecoder.a').exists():
        run(['make', f'-j{jobs}', 'OS=android', 'ARCH=arm64', f'NDKROOT={ndk}',
             'TARGET=android-28', 'libdecoder.a', 'libcommon.a'], cwd=oh)


def adreno_dependencies():
    source = checkout('libadrenotools', 'https://github.com/bylaws/libadrenotools.git',
                      '8fae8ce254dfc1344527e05301e43f37dea2df80')
    run(['git', '-C', source, 'submodule', 'update', '--init', '--recursive'])


def generate(exefs):
    sys.path.insert(0, str(ROOT))
    sys.path.insert(0, str(ROOT / 'dread'))
    import codegen
    import dread_setup
    import nxloader
    import switchrecomp
    dread_setup.PATCHES = str(ROOT / 'dread/dread_v210_patches.json')
    if dread_setup.nso_build_id(str(exefs / 'main')) != dread_setup.BUILD_ID:
        raise SystemExit('Expected original Metroid Dread 2.1.0 ExeFS (build 646761F643AFEBB379EDD5E6A5151AF2CEF93DC1).')
    if dread_setup.npdm_program_id(str(exefs / 'main.npdm')) != dread_setup.TITLE_ID:
        raise SystemExit('The ExeFS program ID is not Metroid Dread.')
    dst = LOCAL / 'game/MetroidDread_build'
    inputs = [ROOT / p for p in ('a64.py', 'codegen.py', 'nxloader.py', 'metroid_perf.py',
                                'switchrecomp.py', 'dread/dread_v210_patches.json')]
    inputs += sorted(p for p in exefs.iterdir() if p.is_file())
    h = hashlib.sha256()
    for p in inputs:
        h.update(p.name.encode()); h.update(p.read_bytes())
    marker = dst / '.generated'
    if not marker.exists() or marker.read_text() != h.hexdigest():
        dst.mkdir(parents=True, exist_ok=True)
        mods, npdm, _ = nxloader.load_program(str(exefs))
        prog = codegen.Program(mods, [])
        prog.analyze(print)
        switchrecomp.gen_module(prog, mods, npdm, str(dst), NAME, 30000, print)
        dread_setup.apply_patches(str(dst))
        marker.write_text(h.hexdigest())
    assets = LOCAL / 'assets/exefs'
    assets.mkdir(parents=True, exist_ok=True)
    for p in exefs.iterdir():
        if p.is_file():
            shutil.copy2(p, assets / p.name)
    return dst


def package():
    sdk, ndk, cmake, llvm = sdk_paths()
    tools = sdk / 'build-tools/35.0.0'
    stage = LOCAL / 'apk'
    stage.mkdir(parents=True, exist_ok=True)
    assets = LOCAL / 'assets'
    config = assets / 'switchrecomp'
    config.mkdir(parents=True, exist_ok=True)
    (config / 'name.txt').write_text(NAME + '\n')
    enabled = 'SWR_ANDROID_VULKAN:BOOL=ON' in (BUILD / 'CMakeCache.txt').read_text()
    (config / 'capabilities.txt').write_text(f'vulkan={int(enabled)}\n')
    shutil.copy2(ANDROID / 'game.ini', config / 'game.ini')
    licenses = assets / 'licenses'
    licenses.mkdir(parents=True, exist_ok=True)
    for p in [ROOT / 'LICENSE.txt', ROOT / 'NOTICE.txt', ROOT / 'runtime/bcn/LICENSE_texture2ddecoder.txt',
              LOCAL / 'libvpx/LICENSE', LOCAL / 'openh264/LICENSE', ndk / 'NOTICE.toolchain']:
        if p.is_file():
            shutil.copy2(p, licenses / (p.parent.name + '-' + p.name))
    if enabled:
        for p in [ROOT / 'LICENSE-GPL-3.0.txt', ROOT / 'runtime/snxvk/LICENSE', ROOT / 'runtime/snxvk/NOTICE',
                  LOCAL / 'libadrenotools/LICENSE', LOCAL / 'libadrenotools/lib/linkernsbypass/LICENSE']:
            shutil.copy2(p, licenses / (p.parent.name + '-' + p.name))
    unsigned = stage / 'unsigned.apk'
    classes = LOCAL / 'classes'
    shutil.rmtree(classes, ignore_errors=True)
    classes.mkdir(parents=True, exist_ok=True)
    java = sorted((ANDROID / 'java').rglob('*.java'))
    run(['javac', '-source', '8', '-target', '8', '-classpath', sdk / 'platforms/android-35/android.jar', '-d', classes, *java])
    run([tools / 'd8', '--min-api', '28', '--lib', sdk / 'platforms/android-35/android.jar',
         '--output', stage, *sorted(classes.rglob('*.class'))])
    unsigned.unlink(missing_ok=True)
    run([tools / 'aapt', 'package', '-f', '-M', ANDROID / 'AndroidManifest.xml',
         '-I', sdk / 'platforms/android-35/android.jar', '-A', assets, '-F', unsigned])
    libs = [BUILD / 'libdread.so', BUILD / 'libMetroidDread_code.so',
            next((ndk / 'toolchains/llvm/prebuilt').glob('*/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'))]
    if enabled:
        hooks = BUILD / 'adrenotools/src/hook'
        libs += [hooks / ('lib' + name + '.so') for name in
                 ('main_hook', 'hook_impl', 'file_redirect_hook', 'gsl_alloc_hook')]
    with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_DEFLATED) as apk:
        apk.write(stage / 'classes.dex', 'classes.dex')
        for source in libs:
            name = source.name
            stripped = stage / name
            shutil.copy2(source, stripped)
            run([llvm / 'llvm-strip', '--strip-unneeded', stripped])
            apk.write(stripped, 'lib/arm64-v8a/' + name)
    aligned = stage / 'aligned.apk'
    run([tools / 'zipalign', '-f', '-p', '4', unsigned, aligned])
    key = ANDROID / 'debug.keystore'
    if not key.exists():
        run(['keytool', '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass',
             'android', '-alias', 'androiddebugkey', '-dname', 'CN=Android Debug',
             '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000'])
    run([tools / 'apksigner', 'sign', '--ks', key, '--ks-pass', 'pass:android', '--out', APK, aligned])
    run([tools / 'apksigner', 'verify', '--verbose', APK])
    print(f'APK: {APK}', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exefs', type=Path, help='Extracted original Dread 2.1.0 ExeFS')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--package-only', action='store_true')
    ap.add_argument('--vulkan', action='store_true', help='Build the experimental native Vulkan renderer')
    a = ap.parse_args()
    LOCAL.mkdir(parents=True, exist_ok=True)
    if not a.package_only:
        if not a.exefs:
            ap.error('--exefs is required unless --package-only is used')
        dependencies(a.jobs)
        if a.vulkan:
            run([sys.executable, ANDROID / 'build_vulkan.py', '--jobs', a.jobs])
        game = generate(a.exefs.resolve())
        sdk, ndk, cmake, llvm = sdk_paths()
        run([cmake / 'cmake', '-S', ANDROID, '-B', BUILD, '-G', 'Ninja',
             f'-DCMAKE_MAKE_PROGRAM={cmake / "ninja"}',
             f'-DCMAKE_TOOLCHAIN_FILE={ndk / "build/cmake/android.toolchain.cmake"}',
             '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_shared',
             '-DCMAKE_BUILD_TYPE=Release', f'-DGAME_BUILD_DIR={game}',
             '-DSWR_ANDROID_VULKAN=' + ('ON' if a.vulkan else 'OFF')])
        run([cmake / 'cmake', '--build', BUILD, f'-j{a.jobs}'])
    package()


if __name__ == '__main__':
    main()
