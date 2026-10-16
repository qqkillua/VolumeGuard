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
typedef int (*FnApps)(wchar_t*, int);
typedef int (*FnRoute)(unsigned int, const wchar_t*);
typedef int (*FnGetRoute)(unsigned int, wchar_t*, int);
typedef int (*FnMeasure)(float*, int*);

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
    FnApps apps = (FnApps)GetProcAddress(dll, "vg_list_apps");
    FnRoute route = (FnRoute)GetProcAddress(dll, "vg_route_app");
    FnGetRoute getRoute = (FnGetRoute)GetProcAddress(dll, "vg_app_route");
    FnMeasure measure = (FnMeasure)GetProcAddress(dll, "vg_measure_latency");
    if (!version || !list || !start || !stop || !params || !status || !error || !apps || !route || !getRoute || !measure)
    {
        std::printf("::error title=vgaudio smoke test::an exported function is missing\n");
        return 1;
    }

    CHECK(version() == VG_API_VERSION, "vg_version() = %d", version());
    CHECK(sizeof(vg_status) == 56, "vg_status is %d bytes, VolumeGuard.exe expects 56", (int)sizeof(vg_status));

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

    // apps that play sound and their output device (API 3)
    {
        const int needApps = apps(nullptr, 0);
        std::vector<wchar_t> a((size_t)needApps + 1);
        CHECK(apps(a.data(), (int)a.size()) == needApps, "vg_list_apps size changed between calls");
        int lines = 0;
        for (wchar_t c : a) lines += c == L'\n';
        std::printf("apps playing sound: %d\n", lines);
        CHECK(route(0, nullptr) == VG_ERR_ARGS, "pid 0 was not refused");
        wchar_t cur[512];
        const int n = getRoute(GetCurrentProcessId(), cur, 512);
        CHECK(n >= 1, "vg_app_route returned %d", n);
        // back to the default device for this test program: harmless, and shows whether the interface exists here
        const int r = route(GetCurrentProcessId(), nullptr);
        std::printf("per-app routing on this Windows: %s (0x%08X)\n", r == VG_OK ? "available" : "not available", (unsigned)r);
    }

    // measuring needs the processing running
    {
        float ms = -1.0f;
        int heard = -1;
        CHECK(measure(&ms, &heard) == VG_ERR_ARGS && heard == 0, "vg_measure_latency without the engine running");
        CHECK(measure(nullptr, nullptr) == VG_ERR_ARGS, "vg_measure_latency(nullptr)");
    }

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
                if (i == 4)
                {
                    float ms = 0.0f;
                    int heard = 0;
                    const int m = measure(&ms, &heard);
                    std::printf("  measured delay: %d, %.1f ms (%d beeps heard)\n", m, ms, heard);
                }
                Sleep(500);
                status(&st, sizeof st);
                std::printf("  state %d, %d -> %d Hz, latency %.1f ms (source %.1f + buffer %.1f + output %.1f + lookahead %.1f), drift %+.0f ppm, gaps %d\n",
                            st.state, st.sourceRate, st.outputRate, st.latencyMs, st.sourceMs, st.bufferMs, st.outputMs, st.lookaheadMs,
                            st.correctionPpm, st.underruns);
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
