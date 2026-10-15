// Windows smoke test for vgaudio.dll: loads it like VolumeGuard.exe does (by name, exported symbols),
// checks the API answers sensibly without audio hardware, and — if the machine has two playback
// devices — runs the engine for a few seconds. Build: x86_64-w64-mingw32-g++ -Iinclude tests/smoke.cpp
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "vgaudio.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("::error title=vgaudio smoke test::" __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

typedef int (*FnVersion)(void);
typedef int (*FnList)(wchar_t*, int);
typedef int (*FnStart)(const wchar_t*, const wchar_t*);
typedef void (*FnStop)(void);
typedef void (*FnParams)(int, float, float, float, float, int);
typedef int (*FnStatus)(vg_status*, int);
typedef int (*FnError)(wchar_t*, int);

int main()
{
    HMODULE dll = LoadLibraryW(L"vgaudio.dll");
    if (!dll) { std::printf("::error title=vgaudio smoke test::LoadLibrary failed (%lu)\n", GetLastError()); return 1; }
    FnVersion version = (FnVersion)GetProcAddress(dll, "vg_version");
    FnList list = (FnList)GetProcAddress(dll, "vg_list_devices");
    FnStart start = (FnStart)GetProcAddress(dll, "vg_start");
    FnStop stop = (FnStop)GetProcAddress(dll, "vg_stop");
    FnParams params = (FnParams)GetProcAddress(dll, "vg_set_params");
    FnStatus status = (FnStatus)GetProcAddress(dll, "vg_get_status");
    FnError error = (FnError)GetProcAddress(dll, "vg_last_error");
    if (!version || !list || !start || !stop || !params || !status || !error)
    {
        std::printf("::error title=vgaudio smoke test::an exported function is missing\n");
        return 1;
    }

    CHECK(version() == VG_API_VERSION, "vg_version() = %d", version());
    CHECK(sizeof(vg_status) == 44, "vg_status is %d bytes, VolumeGuard.exe expects 44", (int)sizeof(vg_status));

    // devices
    const int need = list(nullptr, 0);
    std::vector<wchar_t> buf((size_t)need + 1);
    list(buf.data(), (int)buf.size());
    std::vector<std::wstring> ids;
    std::wstring all(buf.data());
    size_t pos = 0;
    while (pos < all.size())
    {
        size_t nl = all.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = all.size();
        std::wstring line = all.substr(pos, nl - pos);
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos && tab > 0) ids.push_back(line.substr(0, tab));
        pos = nl + 1;
    }
    std::printf("playback devices: %d\n", (int)ids.size());

    // argument checks
    CHECK(start(L"x", L"x") == VG_ERR_SAME, "same device was not refused");
    CHECK(start(nullptr, L"x") == VG_ERR_ARGS, "missing id was not refused");
    const int bogus = start(L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}", L"{0.0.0.00000000}.{11111111-0000-0000-0000-000000000000}");
    CHECK(bogus == VG_ERR_SOURCE || bogus == VG_ERR_SYSTEM, "unknown device gave %d", bogus);
    wchar_t err[256];
    error(err, 256);
    std::printf("last error for an unknown device: %ls\n", err);
    CHECK(err[0] != 0, "no error text");

    vg_status st;
    CHECK(status(&st, sizeof st) == VG_OK && st.state == VG_STATE_STOPPED, "state after a failed start: %d", st.state);
    CHECK(status(&st, 8) == VG_ERR_ARGS, "a too small status struct was accepted");
    params(9, NAN, NAN, 1e9f, -5.0f, 1);   // nonsense must be clamped, not crash
    stop();
    stop();

    // with real devices (developer machines; CI runners usually have none)
    if (ids.size() >= 2)
    {
        params(VG_MODE_TACTICAL, 50, -1, 0, 100, 0);
        const int r = start(ids[0].c_str(), ids[1].c_str());
        std::printf("start on two real devices: %d\n", r);
        if (r == VG_OK)
        {
            for (int i = 0; i < 6; ++i)
            {
                Sleep(500);
                status(&st, sizeof st);
                std::printf("  state %d, %d -> %d Hz, latency %.1f ms, buffer %.1f ms, drift %+.0f ppm, gaps %d\n", st.state,
                            st.sourceRate, st.outputRate, st.latencyMs, st.bufferMs, st.correctionPpm, st.underruns);
            }
            stop();
            status(&st, sizeof st);
            CHECK(st.state == VG_STATE_STOPPED, "still running after vg_stop");
        }
    }

    FreeLibrary(dll);
    if (failures) return 1;
    std::printf("vgaudio smoke test passed\n");
    return 0;
}
