@echo off
rem Builds the Metroid Dread PC EXE from this repository (the steps in the README, with checks first).
rem   build.bat                  the release EXE in bin\runtime_win64_win32.exe
rem   build.bat release 0.2.2    also packs a release folder in release\ (needs redist\zlib1.dll and redist\SDL2.dll)
setlocal
cd /d "%~dp0"

if not exist "gcc\Library\bin\gcc.exe" (
  echo gcc\ is missing: put winlibs GCC 14.2.0 ^(UCRT, POSIX threads, x64^) in gcc\ so that gcc\Library\bin\gcc.exe exists.
  exit /b 1
)
if not exist "python\python.exe" (
  echo python\ is missing: put Python 3.12 for Windows ^(the embeddable package is enough^) in python\.
  exit /b 1
)
if "%GLSLANG%"=="" (
  for %%G in (glslang.exe) do set "GLSLANG=%%~$PATH:G"
)
if "%GLSLANG%"=="" (
  echo glslang 16.6.0 was not found: set GLSLANG=C:\path\to\glslang.exe or put glslang.exe on PATH.
  exit /b 1
)
if not exist "%GLSLANG%" (
  echo GLSLANG points to "%GLSLANG%", which does not exist.
  exit /b 1
)
echo Using %GLSLANG%

echo [1/4] Dear ImGui
python\python.exe work-profiler\build_imgui.py || goto :fail
echo [2/4] Native Vulkan renderer
set SWR_NO_DLSS=
python\python.exe work-profiler\build_snxvk2.py || goto :fail
echo [3/4] Runtime EXE
set SWR_RELEASE=1
python\python.exe switchrecomp.py --build-runtime --cc gcc || goto :fail
set SWR_RELEASE=
if /i not "%1"=="release" (
  echo Done: bin\runtime_win64_win32.exe
  exit /b 0
)
if "%2"=="" (
  echo usage: build.bat release X.Y.Z
  exit /b 1
)
echo [4/4] Release %2
python\python.exe dread\make_release.py release release\%2 --version %2 --exe bin\runtime_win64_win32.exe || goto :fail
echo Done: release\%2
exit /b 0

:fail
echo Build failed: see the messages above.
exit /b 1
