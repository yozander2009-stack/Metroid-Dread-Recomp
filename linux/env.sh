#!/bin/bash
# Linux build: shared settings, sourced by the other scripts. Everything stays inside the repository (as on Windows):
#   bin/runtime_linux_x11          the runtime                      (setup_and_build_runtime.sh)
#   games/Metroid Dread*           the installed game folder        (setup_game.sh)
#   linux/test/                    test copies of the game folder   (make_test_game.sh)
# SWR_GAMES and SWR_TEST move the last two (for example to a bigger disk).
W=${SWR_ROOT:-$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)}   # the repository (this script is in linux/)
NAME="Metroid Dread"
RUNTIME="$W/bin/runtime_linux_x11"
GAMES=${SWR_GAMES:-$W/games}
TEST=${SWR_TEST:-$W/linux/test}
CODE_SO="$GAMES/${NAME}_code.so"
PY=${PYTHON:-python3}

need() {   # need CMD PACKAGE-HINT: stop with a clear message when a tool is missing
  command -v "$1" > /dev/null 2>&1 || { echo "error: '$1' not found. Install it first (bash linux/install_deps.sh): $2" >&2; exit 1; }
}

# the tests run on the real GPU; SWR_SOFTGL=1 forces Mesa's software OpenGL (slow, but works without a 4.3 driver)
softgl() {
  if [ "${SWR_SOFTGL:-0}" = 1 ]; then export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe; fi
}
