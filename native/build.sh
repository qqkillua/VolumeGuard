#!/bin/sh
# Builds vgaudio.dll (64-bit) with MinGW-w64, on Linux or in MSYS2. Output: build/vgaudio.dll
set -e
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
mkdir -p build
$CXX -O2 -std=c++17 -Wall -Wextra -DNDEBUG -shared \
  -Iinclude -Isrc -I../plugin/src \
  src/Engine.cpp -o build/vgaudio.dll \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--out-implib,build/libvgaudio.dll.a \
  -lole32 -luuid -lavrt -lwinmm
${STRIP:-x86_64-w64-mingw32-strip} build/vgaudio.dll
# smoke test for Windows (CI runs it next to the DLL)
$CXX -O2 -std=c++17 -Wall -Wextra -Wno-cast-function-type -Iinclude tests/smoke.cpp -o build/vgaudio-smoke.exe -static -static-libgcc -static-libstdc++
echo "built build/vgaudio.dll and build/vgaudio-smoke.exe"
