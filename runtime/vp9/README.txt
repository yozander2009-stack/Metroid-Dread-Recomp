libvpx 1.14.1 (BSD-3-Clause, see LICENSE-libvpx.txt), VP9 decoder only, built for Windows x64
with mingw-w64 GCC 10 (Ubuntu 22.04's x86_64-w64-mingw32, MSVCRT), nasm optimisations on, MULTITHREADED with
Windows threads (no winpthreads DLL; the library imports _beginthreadex / WaitForSingleObjectEx from the system):

  CROSS=x86_64-w64-mingw32- ../libvpx/configure --target=x86_64-win64-gcc --as=nasm --enable-multithread \
    --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder --disable-examples --disable-tools --disable-docs \
    --disable-unit-tests --enable-static --disable-shared --disable-webm-io --disable-libyuv
  then HAVE_PTHREAD_H set to 0 in vpx_config.h and config.mk (Windows threads instead of pthreads), make.

The earlier build (before 2026-11-21) was single-threaded (--disable-multithread). Used by runtime/vp9video.inc to
play the game's .webm videos.
