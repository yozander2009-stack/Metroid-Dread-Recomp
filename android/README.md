# Android / Retroid build

Builds an ARM64 APK with a Java settings launcher, NativeActivity game process,
AAudio, physical gamepad input, OpenH264 and libvpx video decoding. OpenGL ES 3.2
is the default renderer; native Vulkan is an optional experimental build.
Requires Android 9/API 28 or newer and OpenGL ES 3.2.

Use an original extracted **Metroid Dread 2.1.0** ExeFS containing `main`,
`main.npdm`, `rtld`, `sdk`, and `subsdk0`, plus its merged 2.1.0 RomFS.
The scripts verify the executable build ID, upstream code patch hashes, and
all RomFS file hashes. No game files or keys are downloaded by these scripts.

Prerequisites: Python 3, Git, make, a JDK with keytool, and Android SDK packages
`platform-tools`, `platforms;android-35`, `build-tools;35.0.0`,
`cmake;3.22.1`, `ndk;27.1.12297006`.
`ANDROID_HOME` defaults to Homebrew's `/opt/homebrew/share/android-commandlinetools`;
`ANDROID_NDK_HOME` can override the NDK path.

From the repository root:

```sh
python3 android/build.py --exefs /path/to/exefs --jobs 8
python3 android/deploy.py --serial YOUR_ADB_SERIAL --romfs /path/to/romfs

# Optional native Vulkan renderer (GPL-3.0-or-later ScarletNX components):
python3 android/build.py --exefs /path/to/exefs --jobs 8 --vulkan
```

The first build fetches pinned codec sources and checksum-verifies the Khronos
OpenGL declarations. Generated C, assets and dependency builds live in ignored
`android/local/`; native objects live in ignored `android/build/`.
The signed personal APK is `android/MetroidDread.apk`. It contains your translated
game code and ExeFS, so keep it private. The local debug signing key is ignored.
Subsequent builds reuse generated sources and native objects. `--package-only`
repackages existing libraries. Subsequent deployment can omit `--romfs` to retain
already installed game data and saves. Do not uninstall the app when updating.

Game data, saves, settings, shader caches and logs are isolated in this app's
private `files/` directory. They do not touch emulator installations or saves.
For diagnostics:

```sh
adb -s YOUR_ADB_SERIAL logcat -s SwitchRecomp AndroidRuntime DEBUG
adb -s YOUR_ADB_SERIAL shell run-as com.switchrecomp.metroiddread cat files/MetroidDread.log
adb -s YOUR_ADB_SERIAL exec-out run-as com.switchrecomp.metroiddread cat files/MetroidDread.ini
```

Defaults use the 4:3 render-size patch (`docked=1`, `aspect=4:3`, 1200x900 base),
a 120 FPS cap, vsync, an on-screen FPS counter, and physical controls with the
touch overlay hidden. The cap is a target; gameplay can run slower depending on
the scene. Original cinematics can still run at 30 FPS. Change `aspect=16:9` and
`docked=0` for the original handheld picture. Aspect and docked changes require
a restart. Android face buttons are mapped by physical position to Switch
controls. The launcher offers renderer, resolution, FPS limit, shadows, FXAA,
anisotropic filtering, VSync, aspect, volume and cutscene options. Performance
and Quality buttons fill in graphics presets; the FPS limit stays your choice.
Save settings or Play applies the selections on the next launch.

Touch controls can be disabled entirely or hidden automatically whenever a
physical gamepad/joystick is detected, including the Nova's built-in controller.
Touch events cannot bring the overlay back while disabled. The touch overlay is
currently drawn by OpenGL ES only; use a physical controller with Vulkan.
Android Back closes the game process and returns to settings. Home, screen-off,
and activity pause suspend guest execution and rendering; resuming preserves
the guest clock rather than advancing through time spent in the background.

The Vulkan option builds the vendored ScarletNX renderer into the APK and
compiles its compute shaders using host glslang (pinned source built if absent).
It requires Vulkan 1.3 and device features accepted by that renderer. Check the
runtime log for `renderer: native Vulkan`; initialization failure falls back to
OpenGL ES. The Vulkan build includes the relevant GPL license and notices.
The Vulkan build includes [libadrenotools](https://github.com/bylaws/libadrenotools)
(pinned revision `8fae8ce254dfc1344527e05301e43f37dea2df80`) and its pinned
linker-namespace dependency, with their BSD notices. The APK extracts its native
libraries so the loader can locate the required hooks. DLSS, frame generation,
and desktop front-end menus are not supported on Android.

### Custom Vulkan drivers

In the launcher, scroll to **Graphics driver**, choose **Import custom driver ZIP**,
and select a compatible package using Android's document picker. The format is
AdrenoTools-style `meta.json` (schema 1), naming an ARM64 ELF driver in
`libraryName`, plus that library and its supporting files. Select the imported
package in the Vulkan-driver dropdown, then Save settings or Play. Compatible
Qualcomm, Turnip and other AdrenoTools packages use the same importer. Binary compatibility still depends on the GPU, Android version and driver.
OpenGL ES always uses the system driver.

Imports copy into private app storage; the original ZIP can be moved afterward.
The importer checks metadata/minimum Android API, ELF architecture, archive size,
duplicate paths and directory traversal before completing installation. Shader
caches are separated by package ID. **Remove selected custom driver** removes the
app's copy and selects System; it does not delete the original ZIP.

The **Last launch** status reports which library loaded. The native Vulkan path
verifies the selected file is mapped after initialization. A reported custom-driver
initialization failure returns to the launcher with an error; it does not silently
switch to the system renderer. Select System or import another compatible driver
when recovering from a failed launch. Installed drivers and selection survive APK
updates. Driver binaries are user-supplied and are not bundled in this repository.

Validated on the Retroid Pocket Nova (Android 13, Adreno 740, 1280x960 at 120 Hz):
all 9,774 RomFS files passed on-device SHA-1 verification; the game loaded an
existing save, rendered 4:3 gameplay, accepted controls, and initialized AAudio
and VP9 playback. The owner's active gameplay experience averages in the 40s FPS.
The title screen reached 120 FPS, and mostly static scenes sometimes reached
roughly 100 FPS; those are not representative gameplay averages. This build
does not deliver sustained 120 FPS gameplay. See [Nova performance notes](PERFORMANCE.md)
for settings, evidence, and limitations of these observations.
