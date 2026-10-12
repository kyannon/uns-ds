#!/bin/sh
# Native build for desktop testing. Needs SDL2 headers (SDL_INC) and libSDL2-2.0.so.0.
set -e
cd "$(dirname "$0")"
SDL_INC=${SDL_INC:-/usr/include/SDL2}
mkdir -p build
clang -O2 -g -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-builtin \
  -I"$SDL_INC" -Isrc src/usds.c -o build/usds-x86_64 -l:libSDL2-2.0.so.0
