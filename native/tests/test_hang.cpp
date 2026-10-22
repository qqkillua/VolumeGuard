// Closing must not hang when Windows audio does not respond (2.8.6). Runs on Windows against a test build of the
// engine, vgaudio-hang.dll (built with -DVG_TEST_HANG_MS=16000: the engine thread's first audio call "hangs" 16 s).
// Before 2.8.6, vg_start and vg_stop waited for that thread without a limit, which hung closing VolumeGuard.
// Build: x86_64-w64-mingw32-g++ -O2 -std=c++17 -Iinclude tests/test_hang.cpp -o vgaudio-hang-test.exe
#include <windows.h>

#include <cstdio>

#include "vgaudio.h"

typedef int (*StartFn)(const wchar_t*, const wchar_t*);
typedef void (*StopFn)(void);

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

static double now()
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

int main()
{
    HMODULE dll = LoadLibraryW(L"vgaudio-hang.dll");
    if (!dll) { std::printf("FAIL: vgaudio-hang.dll not found\n"); return 1; }
    StartFn start = (StartFn)(void*)GetProcAddress(dll, "vg_start");
    StopFn stop = (StopFn)(void*)GetProcAddress(dll, "vg_stop");
    if (!start || !stop) { std::printf("FAIL: exports missing\n"); return 1; }

    // the ids do not matter: the engine thread "hangs" before it looks at the devices
    const double t0 = now();
    double t = now();
    int r = start(L"{test source}", L"{test output}");
    const double startSec = now() - t;
    CHECK(r == VG_ERR_TIMEOUT, "vg_start while the audio call hangs returned %d, want %d (timeout)", r, VG_ERR_TIMEOUT);
    CHECK(startSec < 9.5, "vg_start waited %.1f s (5 s for the start + 3 s to stop, at most)", startSec);

    // the old engine thread is still alive: a new start must not run a second one beside it
    t = now();
    r = start(L"{test source}", L"{test output}");
    const double start2Sec = now() - t;
    CHECK(r == VG_ERR_TIMEOUT, "second vg_start returned %d, want %d (refused)", r, VG_ERR_TIMEOUT);
    CHECK(start2Sec < 3.5, "second vg_start waited %.1f s", start2Sec);

    t = now();
    stop();
    const double stopSec = now() - t;
    CHECK(stopSec < 3.5, "vg_stop waited %.1f s while the audio call hangs (at most 3 s)", stopSec);

    // once the call returns, the thread ends by itself and the next stop cleans up at once
    const double hangEnds = 16.0 + 1.0;
    if (now() - t0 < hangEnds) Sleep((DWORD)((hangEnds - (now() - t0)) * 1000.0));
    t = now();
    stop();
    const double stop2Sec = now() - t;
    CHECK(stop2Sec < 1.0, "vg_stop after the call returned took %.1f s", stop2Sec);

    std::printf("hung audio call: vg_start %.1f s, second vg_start %.1f s (refused), vg_stop %.1f s; after it returns vg_stop %.2f s\n",
                startSec, start2Sec, stopSec, stop2Sec);
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("hang checks passed\n");
    return 0;
}
