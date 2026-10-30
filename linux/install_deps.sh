#!/bin/bash
# Linux build, step 0: the compiler and the development libraries the runtime links against (X11, OpenGL, static
# libvpx for the .webm videos), plus SDL2 (controllers and audio, loaded at run time) and zlib. Asks for sudo.
set -e
if command -v apt-get > /dev/null; then
  sudo apt-get update
  sudo apt-get install -y build-essential python3 libx11-dev libgl-dev libvpx-dev libsdl2-2.0-0 zlib1g binutils
elif command -v dnf > /dev/null; then
  sudo dnf install -y gcc gcc-c++ make python3 libX11-devel mesa-libGL-devel libvpx-devel libvpx-static SDL2 zlib binutils || \
  sudo dnf install -y gcc gcc-c++ make python3 libX11-devel mesa-libGL-devel libvpx-devel SDL2 zlib binutils
elif command -v pacman > /dev/null; then
  # Arch ships no libvpx.a: the runtime is then built without the VP9 videos (see setup_and_build_runtime.sh)
  sudo pacman -S --needed --noconfirm base-devel python libx11 libglvnd mesa libvpx sdl2 zlib binutils
else
  echo "Unknown package manager. Install by hand: gcc/g++, python3, X11 and OpenGL headers, libvpx (static), SDL2, zlib." >&2
  exit 1
fi
gcc --version | head -1
