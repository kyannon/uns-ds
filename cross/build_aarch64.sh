#!/bin/sh
# Cross-build usds for aarch64 Linux (ROCKNIX) with clang + lld and no sysroot.
# The binary links dynamically against the device's libSDL2-2.0.so.0 and glibc
# (baseline GLIBC_2.17 symbols only). Needs SDL2 headers in SDL_INC.
set -e
cd "$(dirname "$0")/.."
SDL_INC=${SDL_INC:-/usr/include/SDL2}
T=aarch64-linux-gnu
OUT=build/aarch64
RES=$(clang -print-resource-dir)/include
mkdir -p $OUT
CFLAGS="--target=$T -O2 -ffreestanding -fno-builtin -fPIE -fno-stack-protector -fno-asynchronous-unwind-tables -nostdinc -isystem $RES -Icompat -I$SDL_INC -Isrc"
clang $CFLAGS -Wall -c src/usds.c -o $OUT/usds.o
clang --target=$T -c cross/crt_aarch64.S -o $OUT/crt.o

# stub SDL2: every SDL_ symbol the launcher imports
llvm-nm -u $OUT/usds.o | awk '{print $2}' | grep '^SDL_' | sed 's/.*/void &(void) {}/' > $OUT/stub_sdl.c
clang --target=$T -fPIC -nostdlib -shared -fuse-ld=lld -Wl,-soname,libSDL2-2.0.so.0 $OUT/stub_sdl.c -o $OUT/libSDL2-2.0.so.0
clang --target=$T -fPIC -fno-builtin -nostdlib -shared -fuse-ld=lld -Wl,-soname,libc.so.6 -Wl,--version-script=cross/libc.map \
  cross/stub_libc.c -o $OUT/libc.so.6

ld.lld -pie -z noexecstack -z relro -z now --hash-style=both --dynamic-linker /lib/ld-linux-aarch64.so.1 \
  -o $OUT/usds $OUT/crt.o $OUT/usds.o $OUT/libSDL2-2.0.so.0 $OUT/libc.so.6
llvm-strip $OUT/usds
echo "built $OUT/usds"
