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
# test build: the engine thread's first audio call "hangs" 16 s, and the program that checks nothing waits for it
$CXX -O2 -std=c++17 -Wall -Wextra -DNDEBUG -DVG_TEST_HANG_MS=16000 -shared \
  -Iinclude -Isrc -I../plugin/src \
  src/Engine.cpp -o build/vgaudio-hang.dll \
  -static -static-libgcc -static-libstdc++ \
  -lole32 -luuid -lavrt -lwinmm
$CXX -O2 -std=c++17 -Wall -Wextra -Iinclude tests/test_hang.cpp -o build/vgaudio-hang-test.exe -static -static-libgcc -static-libstdc++
# smoke test for Windows (CI runs it next to the DLL)
$CXX -O2 -std=c++17 -Wall -Wextra -Wno-cast-function-type -Iinclude tests/smoke.cpp -o build/vgaudio-smoke.exe -static -static-libgcc -static-libstdc++
echo "built build/vgaudio.dll and build/vgaudio-smoke.exe"
