// VolumeGuard audio engine (vgaudio.dll): WASAPI loopback of the source device → processing → output device.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// Threads:
//   engine thread  — opens both devices, renders to the output (event-driven), restarts after device errors
//   capture thread — every 2 ms takes what the source device played (loopback) and puts it into the ring;
//                    also plays silence into the source device, so Windows keeps delivering packets
//                    (and the clocks keep running) even while the game is quiet
// Both run with MMCSS "Pro Audio" priority.
#define VGAUDIO_BUILD
#include "vgaudio.h"

#include "Bridge.hpp"
#include "Processor.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <propidl.h>
#include <propsys.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

static_assert(sizeof(vg_status) == 44, "VolumeGuard.exe (Processing.cs, VgStatus) expects this layout");

namespace {

// GUIDs spelled out, so the build does not depend on which of them a toolchain's uuid library has.
const CLSID kCLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const IID kIID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const IID kIID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
const IID kIID_IAudioRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
const IID kIID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
const GUID kSubtypePcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
const PROPERTYKEY kPKEY_FriendlyName = { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };

template <class T>
struct Com
{
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    void** putVoid() { reset(); return reinterpret_cast<void**>(&p); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct CoTaskFormat
{
    WAVEFORMATEX* p = nullptr;
    ~CoTaskFormat() { if (p) CoTaskMemFree(p); }
};

double nowSec()
{
    static const double freq = []() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / freq;
}

// ---------------------------------------------------------------------------------------------------------
// audio.log in %APPDATA%\VolumeGuard (next to settings.txt and crash.log)

CRITICAL_SECTION gLogLock;
std::wstring gLogPath;

void logLine(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    const int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\r\n", t.wYear, t.wMonth, t.wDay,
                           t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    EnterCriticalSection(&gLogLock);
    if (gLogPath.empty())
    {
        wchar_t appdata[MAX_PATH];
        const DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            std::wstring dir = std::wstring(appdata) + L"\\VolumeGuard";
            CreateDirectoryW(dir.c_str(), nullptr);
            gLogPath = dir + L"\\audio.log";
        }
    }
    if (!gLogPath.empty())
    {
        // keep it small: start over after 512 KB
        WIN32_FILE_ATTRIBUTE_DATA fa;
        const bool big = GetFileAttributesExW(gLogPath.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 512 * 1024);
        HANDLE h = CreateFileW(gLogPath.c_str(), big ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, big ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD w;
            WriteFile(h, line, (DWORD)std::max(0, std::min(n, (int)sizeof line - 1)), &w, nullptr);
            CloseHandle(h);
        }
    }
    LeaveCriticalSection(&gLogLock);
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring deviceName(IMMDevice* dev)
{
    std::wstring name;
    Com<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put())))
    {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kPKEY_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
        PropVariantClear(&v);
    }
    return name;
}

bool parseFormat(const WAVEFORMATEX* wf, vga::SampleFormat& f)
{
    f.channels = wf->nChannels;
    f.bitsPerSample = wf->wBitsPerSample;
    f.validBits = wf->wBitsPerSample;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) f.isFloat = true;
    else if (wf->wFormatTag == WAVE_FORMAT_PCM) f.isFloat = false;
    else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (IsEqualGUID(x->SubFormat, kSubtypeFloat)) f.isFloat = true;
        else if (IsEqualGUID(x->SubFormat, kSubtypePcm)) f.isFloat = false;
        else return false;
        if (x->Samples.wValidBitsPerSample) f.validBits = x->Samples.wValidBitsPerSample;
    }
    else return false;
    if (f.channels < 1 || f.channels > 32) return false;
    if (f.isFloat) return f.bitsPerSample == 32 || f.bitsPerSample == 64;
    return f.bitsPerSample == 16 || f.bitsPerSample == 24 || f.bitsPerSample == 32;
}

std::string describe(const vga::SampleFormat& f, DWORD rate)
{
    char b[96];
    snprintf(b, sizeof b, "%lu Hz, %d ch, %s%d", (unsigned long)rate, f.channels, f.isFloat ? "float" : "int", f.validBits);
    return b;
}

// ---------------------------------------------------------------------------------------------------------

class Engine
{
public:
    Engine()
    {
        InitializeCriticalSection(&lock_);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    int start(const wchar_t* sourceId, const wchar_t* outputId)
    {
        if (!sourceId || !outputId || !*sourceId || !*outputId) return fail(VG_ERR_ARGS, "no device selected");
        if (wcscmp(sourceId, outputId) == 0)
            return fail(VG_ERR_SAME, "source and output are the same device (the sound would feed back into itself)");

        EnterCriticalSection(&lock_);
        stopLocked();
        sourceId_ = sourceId;
        outputId_ = outputId;
        firstResult_ = VG_OK;
        ResetEvent(stopEvent_);
        ResetEvent(readyEvent_);
        state_.store(VG_STATE_STARTING);
        thread_ = CreateThread(nullptr, 0, &Engine::engineThread, this, 0, nullptr);
        if (!thread_)
        {
            state_.store(VG_STATE_STOPPED);
            LeaveCriticalSection(&lock_);
            return fail(VG_ERR_SYSTEM, "could not create the engine thread");
        }
        const DWORD w = WaitForSingleObject(readyEvent_, 5000);
        int result = w == WAIT_OBJECT_0 ? firstResult_ : VG_ERR_TIMEOUT;
        if (result != VG_OK)
        {
            if (result == VG_ERR_TIMEOUT) setError("the engine did not start within 5 s");
            stopLocked();
        }
        LeaveCriticalSection(&lock_);
        return result;
    }

    void stop()
    {
        EnterCriticalSection(&lock_);
        stopLocked();
        LeaveCriticalSection(&lock_);
    }

    void setParams(const vga::Params& p)
    {
        pMode_.store(p.mode);
        pAmount_.store(p.amount);
        pCeiling_.store(p.ceilingDb);
        pGain_.store(p.inputGainDb);
        pRelease_.store(p.releaseMs);
        pBypass_.store(p.bypass);
    }

    void status(vg_status& s)
    {
        s.state = state_.load();
        s.sourceRate = srcRate_.load();
        s.outputRate = outRate_.load();
        s.underruns = (int)underrunsTotal_.load();
        s.restarts = restarts_.load();
        s.inPeakDb = std::max(-100.0f, vgl::gainToDb(inPeak_.exchange(0.0f)));
        s.outPeakDb = std::max(-100.0f, vgl::gainToDb(outPeak_.exchange(0.0f)));
        s.reductionDb = reduction_.exchange(0.0f);
        s.latencyMs = latencyMs_.load();
        s.bufferMs = bufferMs_.load();
        s.correctionPpm = correctionPpm_.load();
    }

    int lastError(wchar_t* buf, int cap)
    {
        EnterCriticalSection(&gLogLock);
        std::wstring e = error_;
        LeaveCriticalSection(&gLogLock);
        if (buf && cap > 0)
        {
            const int n = std::min((int)e.size(), cap - 1);
            wmemcpy(buf, e.c_str(), (size_t)n);
            buf[n] = 0;
        }
        return (int)e.size();
    }

    int fail(int code, const char* what)
    {
        setError(what);
        return code;
    }

    void setError(const char* what)
    {
        logLine("error: %s", what);
        std::wstring w;
        for (const char* c = what; *c; ++c) w += (wchar_t)(unsigned char)*c;
        EnterCriticalSection(&gLogLock);
        error_ = w;
        LeaveCriticalSection(&gLogLock);
    }

private:
    void stopLocked()
    {
        if (!thread_) return;
        SetEvent(stopEvent_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
        state_.store(VG_STATE_STOPPED);
        logLine("stopped");
    }

    static DWORD WINAPI engineThread(LPVOID self)
    {
        static_cast<Engine*>(self)->engineMain();
        return 0;
    }

    static DWORD WINAPI captureThread(LPVOID self)
    {
        static_cast<Engine*>(self)->captureMain();
        return 0;
    }

    void engineMain()
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        timeBeginPeriod(1);
        bool first = true;
        while (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT)
        {
            int code = VG_OK;
            const bool ran = runSession(first, code);
            if (first && !ran)
            {
                // the first attempt failed: report to vg_start and quit
                firstResult_ = code;
                SetEvent(readyEvent_);
                break;
            }
            first = false;
            if (WaitForSingleObject(stopEvent_, 0) != WAIT_TIMEOUT) break;
            // a device was unplugged, disabled or switched to another format: try again every 2 s
            state_.store(VG_STATE_RECONNECTING);
            restarts_.fetch_add(1);
            if (WaitForSingleObject(stopEvent_, 2000) != WAIT_TIMEOUT) break;
        }
        timeEndPeriod(1);
        if (SUCCEEDED(co)) CoUninitialize();
    }

    // Returns true if audio was flowing at some point (so a later failure is a reason to reconnect).
    bool runSession(bool first, int& code)
    {
        Com<IMMDeviceEnumerator> en;
        HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, en.putVoid());
        if (FAILED(hr)) { code = VG_ERR_SYSTEM; return report(first, "CoCreateInstance(MMDeviceEnumerator)", hr); }

        Com<IMMDevice> srcDev, outDev;
        if (FAILED(hr = en->GetDevice(sourceId_.c_str(), srcDev.put()))) { code = VG_ERR_SOURCE; return report(first, "source device not found", hr); }
        if (FAILED(hr = en->GetDevice(outputId_.c_str(), outDev.put()))) { code = VG_ERR_OUTPUT; return report(first, "output device not found", hr); }

        // --- source: loopback capture + a silent stream that keeps it running
        Com<IAudioClient> capClient, keepClient, renClient;
        if (FAILED(hr = srcDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, capClient.putVoid())))
        { code = VG_ERR_SOURCE; return report(first, "source: Activate", hr); }
        CoTaskFormat capFmt;
        if (FAILED(hr = capClient->GetMixFormat(&capFmt.p))) { code = VG_ERR_SOURCE; return report(first, "source: GetMixFormat", hr); }
        if (!parseFormat(capFmt.p, capFormat_)) { code = VG_ERR_FORMAT; return report(first, "source: unsupported sample format", E_FAIL); }
        if (FAILED(hr = capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, capFmt.p, nullptr)))
        { code = VG_ERR_SOURCE; return report(first, "source: Initialize(loopback)", hr); }
        if (FAILED(hr = capClient->GetService(kIID_IAudioCaptureClient, capture_.putVoid())))
        { code = VG_ERR_SOURCE; return report(first, "source: GetService(capture)", hr); }

        keepFrames_ = 0;
        keep_.reset();
        if (SUCCEEDED(srcDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, keepClient.putVoid()))
            && SUCCEEDED(keepClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, capFmt.p, nullptr))
            && SUCCEEDED(keepClient->GetBufferSize(&keepFrames_))
            && SUCCEEDED(keepClient->GetService(kIID_IAudioRenderClient, keep_.putVoid())))
            keepClient_ = keepClient.p;
        else
        {
            keepFrames_ = 0;
            keep_.reset();
            keepClient_ = nullptr;
            logLine("note: could not open a keep-alive stream on the source device; quiet moments may cause short gaps");
        }

        // --- output: event-driven playback
        if (FAILED(hr = outDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, renClient.putVoid())))
        { code = VG_ERR_OUTPUT; return report(first, "output: Activate", hr); }
        CoTaskFormat renFmt;
        if (FAILED(hr = renClient->GetMixFormat(&renFmt.p))) { code = VG_ERR_OUTPUT; return report(first, "output: GetMixFormat", hr); }
        vga::SampleFormat outFormat;
        if (!parseFormat(renFmt.p, outFormat)) { code = VG_ERR_FORMAT; return report(first, "output: unsupported sample format", E_FAIL); }
        if (FAILED(hr = renClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, renFmt.p, nullptr)))
        { code = VG_ERR_OUTPUT; return report(first, "output: Initialize", hr); }
        HANDLE renEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!renEvent) { code = VG_ERR_SYSTEM; return report(first, "CreateEvent", E_FAIL); }
        struct EventCloser { HANDLE h; ~EventCloser() { CloseHandle(h); } } closeEvent{ renEvent };
        Com<IAudioRenderClient> render;
        UINT32 renFrames = 0;
        if (FAILED(hr = renClient->SetEventHandle(renEvent)) || FAILED(hr = renClient->GetBufferSize(&renFrames))
            || FAILED(hr = renClient->GetService(kIID_IAudioRenderClient, render.putVoid())))
        { code = VG_ERR_OUTPUT; return report(first, "output: setup", hr); }

        const DWORD srcRate = capFmt.p->nSamplesPerSec, outRate = renFmt.p->nSamplesPerSec;
        srcRate_.store((int)srcRate);
        outRate_.store((int)outRate);
        logLine("start: source \"%s\" (%s) -> output \"%s\" (%s), output buffer %u frames%s",
                narrow(deviceName(srcDev.p)).c_str(), describe(capFormat_, srcRate).c_str(),
                narrow(deviceName(outDev.p)).c_str(), describe(outFormat, outRate).c_str(), renFrames,
                keep_ ? "" : ", no keep-alive");

        // --- bridge and processing
        ring_.init((size_t)srcRate);                       // 1 s of room
        vga::DriftResampler rs;
        rs.prepare(srcRate, outRate, 25.0);
        vga::Processor proc;
        proc.prepare(outRate);
        vga::Params params = currentParams();
        proc.setParams(params);
        std::vector<float> l(renFrames), r(renFrames);
        const float latencyExtraMs = 10.0f + 1000.0f * (float)proc.latencyFrames() / (float)outRate;   // loopback packet + lookahead

        // pre-fill the output with silence, then start everything
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(renFrames, &data))) render->ReleaseBuffer(renFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        captureHr_.store(S_OK);
        captureFailed_.store(false);
        sessionStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (keepClient_) keepClient_->Start();
        if (FAILED(hr = capClient->Start())) { cleanupCapture(); code = VG_ERR_SOURCE; return report(first, "source: Start", hr); }
        if (FAILED(hr = renClient->Start())) { capClient->Stop(); cleanupCapture(); code = VG_ERR_OUTPUT; return report(first, "output: Start", hr); }
        HANDLE capThread = CreateThread(nullptr, 0, &Engine::captureThread, this, 0, nullptr);
        if (!capThread)
        {
            capClient->Stop(); renClient->Stop(); cleanupCapture();
            code = VG_ERR_SYSTEM;
            return report(first, "could not create the capture thread", E_FAIL);
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        state_.store(VG_STATE_BUFFERING);
        if (first) { firstResult_ = VG_OK; SetEvent(readyEvent_); }

        uint64_t lastUnderruns = 0;
        double lastUnderrunLog = -1e9, lastEvent = nowSec();
        HANDLE waits[2] = { renEvent, stopEvent_ };
        hr = S_OK;
        for (;;)
        {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (w == WAIT_OBJECT_0 + 1) break;
            if (captureFailed_.load()) { hr = captureHr_.load(); logLine("source failed: 0x%08lX", (unsigned long)hr); break; }
            const double now = nowSec();
            if (w == WAIT_TIMEOUT)
            {
                if (now - lastEvent > 2.0) { hr = AUDCLNT_E_DEVICE_INVALIDATED; logLine("output stopped asking for audio"); break; }
                continue;
            }
            lastEvent = now;

            UINT32 padding = 0;
            if (FAILED(hr = renClient->GetCurrentPadding(&padding))) { logLine("output: GetCurrentPadding 0x%08lX", (unsigned long)hr); break; }
            const UINT32 avail = renFrames - std::min(renFrames, padding);
            if (avail == 0) continue;

            const vga::Params p = currentParams();
            if (p != params) { params = p; proc.setParams(p); }

            rs.read(ring_, l.data(), r.data(), (int)avail, now);
            proc.process(l.data(), r.data(), (int)avail);

            if (FAILED(hr = render->GetBuffer(avail, &data))) { logLine("output: GetBuffer 0x%08lX", (unsigned long)hr); break; }
            vga::fromStereo(l.data(), r.data(), avail, outFormat, data);
            if (FAILED(hr = render->ReleaseBuffer(avail, 0))) { logLine("output: ReleaseBuffer 0x%08lX", (unsigned long)hr); break; }

            // status for the UI
            raiseMax(inPeak_, proc.takeInputPeak());
            raiseMax(outPeak_, proc.takeOutputPeak());
            lowerMin(reduction_, proc.takeReductionDb());
            const float bufMs = (float)rs.fillMs();
            bufferMs_.store(bufMs);
            correctionPpm_.store((float)(rs.correction() * 1e6));
            latencyMs_.store(bufMs + 1000.0f * (float)(padding + avail) / (float)outRate + latencyExtraMs);
            state_.store(rs.running() ? VG_STATE_RUNNING : VG_STATE_BUFFERING);
            const uint64_t u = rs.underruns();
            if (u != lastUnderruns)
            {
                underrunsTotal_.fetch_add(u - lastUnderruns);
                lastUnderruns = u;
                if (now - lastUnderrunLog > 10.0)
                {
                    lastUnderrunLog = now;
                    logLine("gap in the sound (total %llu in this session), buffer target now %.0f ms",
                            (unsigned long long)u, rs.targetMs());
                }
            }
        }

        SetEvent(sessionStop_);
        WaitForSingleObject(capThread, INFINITE);
        CloseHandle(capThread);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        renClient->Stop();
        capClient->Stop();
        cleanupCapture();
        if (FAILED(hr)) logLine("session ended: 0x%08lX, reconnecting", (unsigned long)hr);
        return true;
    }

    void cleanupCapture()
    {
        if (keepClient_) keepClient_->Stop();
        keepClient_ = nullptr;   // owned by runSession's Com<>
        keep_.reset();
        capture_.reset();
        if (sessionStop_) { CloseHandle(sessionStop_); sessionStop_ = nullptr; }
    }

    void captureMain()
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        std::vector<float> lr(4096 * 2);
        const int blockAlign = capFormat_.blockAlign();
        HRESULT hr = S_OK;
        while (WaitForSingleObject(sessionStop_, 2) == WAIT_TIMEOUT)
        {
            // keep-alive: keep half of its buffer filled with silence
            if (keep_)
            {
                UINT32 pad = 0;
                if (SUCCEEDED(keepClient_->GetCurrentPadding(&pad)) && pad < keepFrames_ / 2)
                {
                    const UINT32 n = keepFrames_ / 2 - pad;
                    BYTE* d = nullptr;
                    if (SUCCEEDED(keep_->GetBuffer(n, &d))) keep_->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }
            for (;;)
            {
                UINT32 next = 0;
                if (FAILED(hr = capture_->GetNextPacketSize(&next))) break;
                if (next == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                if (frames > 0)
                {
                    if (lr.size() < (size_t)frames * 2) lr.resize((size_t)frames * 2);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data || blockAlign <= 0)
                        std::fill(lr.begin(), lr.begin() + (size_t)frames * 2, 0.0f);
                    else
                        vga::toStereo(data, frames, capFormat_, lr.data());
                    ring_.write(lr.data(), frames, nowSec());
                }
                if (FAILED(hr = capture_->ReleaseBuffer(frames))) break;
            }
            if (FAILED(hr))
            {
                captureHr_.store(hr);
                captureFailed_.store(true);
                break;
            }
        }
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (SUCCEEDED(co)) CoUninitialize();
    }

    bool report(bool first, const char* what, HRESULT hr)
    {
        char b[256];
        snprintf(b, sizeof b, "%s (0x%08lX)", what, (unsigned long)hr);
        if (first) setError(b);
        else logLine("reconnect failed: %s", b);
        return false;
    }

    vga::Params currentParams() const
    {
        vga::Params p;
        p.mode = pMode_.load();
        p.amount = pAmount_.load();
        p.ceilingDb = pCeiling_.load();
        p.inputGainDb = pGain_.load();
        p.releaseMs = pRelease_.load();
        p.bypass = pBypass_.load();
        return p;
    }

    static void raiseMax(std::atomic<float>& a, float v)
    {
        float cur = a.load();
        while (v > cur && !a.compare_exchange_weak(cur, v)) {}
    }
    static void lowerMin(std::atomic<float>& a, float v)
    {
        float cur = a.load();
        while (v < cur && !a.compare_exchange_weak(cur, v)) {}
    }

    CRITICAL_SECTION lock_;
    HANDLE thread_ = nullptr, stopEvent_ = nullptr, readyEvent_ = nullptr, sessionStop_ = nullptr;
    std::wstring sourceId_, outputId_, error_;
    int firstResult_ = VG_OK;

    // session objects shared with the capture thread
    vga::FrameRing ring_;
    vga::SampleFormat capFormat_;
    Com<IAudioCaptureClient> capture_;
    Com<IAudioRenderClient> keep_;
    IAudioClient* keepClient_ = nullptr;
    UINT32 keepFrames_ = 0;
    std::atomic<bool> captureFailed_{ false };
    std::atomic<HRESULT> captureHr_{ S_OK };

    // parameters (written by vg_set_params)
    std::atomic<int> pMode_{ 1 };
    std::atomic<float> pAmount_{ 50.0f }, pCeiling_{ -1.0f }, pGain_{ 0.0f }, pRelease_{ 100.0f };
    std::atomic<bool> pBypass_{ false };

    // status (read by vg_get_status)
    std::atomic<int> state_{ VG_STATE_STOPPED }, srcRate_{ 0 }, outRate_{ 0 }, restarts_{ 0 };
    std::atomic<uint64_t> underrunsTotal_{ 0 };
    std::atomic<float> inPeak_{ 0.0f }, outPeak_{ 0.0f }, reduction_{ 0.0f };
    std::atomic<float> latencyMs_{ 0.0f }, bufferMs_{ 0.0f }, correctionPpm_{ 0.0f };
};

Engine* gEngine = nullptr;

} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        InitializeCriticalSection(&gLogLock);
        gEngine = new Engine();
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        // FreeLibrary: the app should call vg_stop first; threads cannot be joined safely from here.
    }
    return TRUE;
}

extern "C" {

VG_API int vg_version(void) { return VG_API_VERSION; }

VG_API int vg_list_devices(wchar_t* buffer, int capacity)
{
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE on an STA thread is fine
    std::wstring out;
    {
        Com<IMMDeviceEnumerator> en;
        Com<IMMDeviceCollection> list;
        if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, en.putVoid()))
            && SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, list.put())))
        {
            std::wstring defId;
            Com<IMMDevice> def;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, def.put())))
            {
                LPWSTR id = nullptr;
                if (SUCCEEDED(def->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); }
            }
            UINT count = 0;
            list->GetCount(&count);
            for (UINT i = 0; i < count; ++i)
            {
                Com<IMMDevice> dev;
                if (FAILED(list->Item(i, dev.put()))) continue;
                LPWSTR id = nullptr;
                if (FAILED(dev->GetId(&id)) || !id) continue;
                std::wstring name = deviceName(dev.p);
                for (wchar_t& c : name) if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
                out += id;
                out += L'\t';
                out += name;
                out += defId == id ? L"\t1\n" : L"\t0\n";
                CoTaskMemFree(id);
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    const int need = (int)out.size() + 1;
    if (buffer && capacity >= need) wmemcpy(buffer, out.c_str(), (size_t)need);
    return need;
}

VG_API int vg_start(const wchar_t* sourceId, const wchar_t* outputId) { return gEngine->start(sourceId, outputId); }

VG_API void vg_stop(void) { gEngine->stop(); }

VG_API void vg_set_params(int mode, float amountPercent, float ceilingDb, float inputGainDb, float releaseMs, int bypass)
{
    vga::Params p;
    p.mode = mode;
    p.amount = amountPercent;
    p.ceilingDb = ceilingDb;
    p.inputGainDb = inputGainDb;
    p.releaseMs = releaseMs;
    p.bypass = bypass != 0;
    gEngine->setParams(p.sanitized());
}

VG_API int vg_get_status(vg_status* status, int size)
{
    if (!status || size < (int)sizeof(vg_status)) return VG_ERR_ARGS;
    gEngine->status(*status);
    return VG_OK;
}

VG_API int vg_last_error(wchar_t* buffer, int capacity) { return gEngine->lastError(buffer, capacity); }

} // extern "C"
