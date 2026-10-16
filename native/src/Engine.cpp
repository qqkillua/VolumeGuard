// VolumeGuard audio engine (vgaudio.dll): WASAPI loopback of the source device → processing → output device.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// One audio thread (MMCSS "Pro Audio"), waking every ~1 ms ("push mode"):
//   1. takes every packet the source device played (loopback; 2.7 ms packets when the driver allows it),
//   2. converts it (clock drift, sample rate), processes it and queues it in the output device at once,
//   3. keeps the output queue just above what the device takes per period (OutputPacer, Bridge.hpp).
// So there is one buffer, not two (a ring between the devices + the output queue), which roughly halves the
// delay. It also plays silence into the source device, so Windows keeps delivering packets while the game
// is quiet. The engine thread restarts everything after device errors.
#define VGAUDIO_BUILD
#include "vgaudio.h"

#include "Bridge.hpp"
#include "Processor.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmreg.h>
#include <propidl.h>
#include <propsys.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

static_assert(sizeof(vg_status) == 56, "VolumeGuard.exe (Processing.cs, VgStatus) expects this layout");

namespace {

// GUIDs spelled out, so the build does not depend on which of them a toolchain's uuid library has.
const CLSID kCLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
const IID kIID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
const IID kIID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
const IID kIID_IAudioClient3 = { 0x7ED4EE07, 0x8E67, 0x4CD4, { 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42 } };
const IID kIID_IAudioRenderClient = { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };
const IID kIID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
const GUID kSubtypePcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
const IID kIID_IAudioClock = { 0xCD63314F, 0x3FBA, 0x4A1B, { 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7 } };
const IID kIID_IAudioSessionManager2 = { 0x77AA99A0, 0x1BD6, 0x484F, { 0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F } };
const IID kIID_IAudioSessionControl2 = { 0xBFB7FF88, 0x7239, 0x4FC9, { 0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D } };
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

/// Opens a shared-mode stream on dev. With lowLatency, asks for the smallest engine period the driver
/// supports (IAudioClient3, Windows 10+): the device then works in packets of e.g. 2.7 ms instead of 10 ms,
/// which shortens the whole path. Falls back to a normal stream if that is not possible.
/// periodFrames: how often (in frames) the device engine runs for this stream.
HRESULT openStream(IMMDevice* dev, DWORD flags, REFERENCE_TIME bufferHns, const WAVEFORMATEX* fmt, bool lowLatency,
                   Com<IAudioClient>& client, UINT32& periodFrames, bool& gotLow)
{
    gotLow = false;
    HRESULT hr;
    if (lowLatency)
    {
        Com<IAudioClient3> c3;
        if (SUCCEEDED(dev->Activate(kIID_IAudioClient3, CLSCTX_ALL, nullptr, c3.putVoid())))
        {
            UINT32 def = 0, fund = 0, minP = 0, maxP = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(fmt, &def, &fund, &minP, &maxP)) && minP > 0 && minP < def
                && SUCCEEDED(c3->InitializeSharedAudioStream(flags, minP, fmt, nullptr)))
            {
                UINT32 cur = minP;
                WAVEFORMATEX* curFmt = nullptr;
                if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&curFmt, &cur)) && curFmt) CoTaskMemFree(curFmt);
                periodFrames = cur ? cur : minP;
                gotLow = true;
                client.reset();
                return c3->QueryInterface(kIID_IAudioClient, client.putVoid());
            }
        }
    }
    // a failed Initialize can leave a client unusable: always start from a fresh one
    if (FAILED(hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, client.putVoid()))) return hr;
    if (FAILED(hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferHns, 0, fmt, nullptr))) return hr;
    REFERENCE_TIME defP = 100000;
    client->GetDevicePeriod(&defP, nullptr);
    periodFrames = (UINT32)((double)defP * fmt->nSamplesPerSec / 1e7 + 0.5);
    return S_OK;
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
        {
            auto it = srcChoice_.find(sourceId_);
            srcMode_ = it != srcChoice_.end() ? it->second : 0;
            trialScore0_ = -1.0;
            restartQuick_ = false;
        }
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
        s.sourceMs = sourceMs_.load();
        s.outputMs = outputMs_.load();
        s.lookaheadMs = lookaheadMs_.load();
    }

    /// Measures the real delay of the running processing: plays short beeps into the source device and
    /// finds them in what the output device plays (its loopback), both on the QPC clock. Blocks ~2.5 s.
    int measure(float* ms, int* valid)
    {
        EnterCriticalSection(&lock_);
        const bool running = thread_ != nullptr && state_.load() == VG_STATE_RUNNING;
        const std::wstring src = sourceId_, out = outputId_;
        LeaveCriticalSection(&lock_);
        if (!running) return fail(VG_ERR_ARGS, "measure: the processing is not running");
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const int r = measureOn(src, out, ms, valid);
        if (SUCCEEDED(co)) CoUninitialize();
        return r;
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
            if (restartQuick_) { restartQuick_ = false; continue; }   // switching the source mode
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
        CoTaskFormat capFmt;
        {
            Com<IAudioClient> probe;
            if (FAILED(hr = srcDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, probe.putVoid())))
            { code = VG_ERR_SOURCE; return report(first, "source: Activate", hr); }
            if (FAILED(hr = probe->GetMixFormat(&capFmt.p))) { code = VG_ERR_SOURCE; return report(first, "source: GetMixFormat", hr); }
        }
        if (!parseFormat(capFmt.p, capFormat_)) { code = VG_ERR_FORMAT; return report(first, "source: unsupported sample format", E_FAIL); }

        // The keep-alive stream goes first: opened with the smallest engine period, it makes the source device
        // run in small packets, so the loopback below delivers the game's sound in small packets too.
        keepFrames_ = 0;
        keep_.reset();
        keepClient_ = nullptr;
        UINT32 srcPeriod = 0;
        bool srcLow = false;
        if (SUCCEEDED(openStream(srcDev.p, 0, 1000000, capFmt.p, srcMode_ == 0, keepClient, srcPeriod, srcLow))
            && SUCCEEDED(keepClient->GetBufferSize(&keepFrames_))
            && SUCCEEDED(keepClient->GetService(kIID_IAudioRenderClient, keep_.putVoid())))
            keepClient_ = keepClient.p;
        else
        {
            keepFrames_ = 0;
            keep_.reset();
            logLine("note: could not open a keep-alive stream on the source device; quiet moments may cause short gaps");
        }

        UINT32 capPeriod = 0;
        bool capLow = false;
        if (FAILED(hr = openStream(srcDev.p, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, capFmt.p, false, capClient, capPeriod, capLow)))
        { code = VG_ERR_SOURCE; return report(first, "source: Initialize(loopback)", hr); }
        if (FAILED(hr = capClient->GetService(kIID_IAudioCaptureClient, capture_.putVoid())))
        { code = VG_ERR_SOURCE; return report(first, "source: GetService(capture)", hr); }
        if (keepClient_) capPeriod = std::min(capPeriod, srcPeriod);   // the engine runs at the smaller of the two

        // --- output: event-driven playback, smallest engine period the driver allows
        CoTaskFormat renFmt;
        {
            Com<IAudioClient> probe;
            if (FAILED(hr = outDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, probe.putVoid())))
            { code = VG_ERR_OUTPUT; return report(first, "output: Activate", hr); }
            if (FAILED(hr = probe->GetMixFormat(&renFmt.p))) { code = VG_ERR_OUTPUT; return report(first, "output: GetMixFormat", hr); }
        }
        vga::SampleFormat outFormat;
        if (!parseFormat(renFmt.p, outFormat)) { code = VG_ERR_FORMAT; return report(first, "output: unsupported sample format", E_FAIL); }
        UINT32 outPeriod = 0;
        bool outLow = false;
        if (FAILED(hr = openStream(outDev.p, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, renFmt.p, true, renClient, outPeriod, outLow)))
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
        const double srcPeriodMs = 1000.0 * capPeriod / srcRate, outPeriodMs = 1000.0 * outPeriod / outRate;
        logLine("start: source \"%s\" (%s, packets %.2f ms%s) -> output \"%s\" (%s, period %.2f ms%s, buffer %u frames)%s",
                narrow(deviceName(srcDev.p)).c_str(), describe(capFormat_, srcRate).c_str(), srcPeriodMs, srcLow ? ", low-latency" : "",
                narrow(deviceName(outDev.p)).c_str(), describe(outFormat, outRate).c_str(), outPeriodMs, outLow ? ", low-latency" : "",
                renFrames, keep_ ? "" : ", no keep-alive");

        // --- push mode: one thread takes each source packet, processes it and queues it in the output at once.
        // The only buffer is the output device's queue; OutputPacer keeps it just big enough (see Bridge.hpp).
        ring_.init((size_t)srcRate);                       // 1 s of room (normally holds less than a packet)
        vga::DriftResampler rs;
        rs.prepare(srcRate, outRate, 20.0);
        vga::OutputPacer pacer;
        const double packetOut = srcPeriodMs * outRate / 1000.0;
        pacer.prepare(outRate, outPeriod, packetOut, 2.0);
        vga::SourceWatch watch;   // stopped (feed silence) or only late in lumps (wait for it): see Bridge.hpp
        watch.prepare(srcPeriodMs / 1000.0);
        vga::Processor proc;
        proc.prepare(outRate);
        vga::Params params = currentParams();
        proc.setParams(params);
        std::vector<float> l(renFrames), r(renFrames), lr(4096 * 2);
        const float lookMs = 1000.0f * (float)proc.latencyFrames() / (float)outRate;
        sourceMs_.store((float)srcPeriodMs);
        lookaheadMs_.store(lookMs);
        const int blockAlign = capFormat_.blockAlign();

        // prime the output with the margin in silence, then start everything
        BYTE* data = nullptr;
        UINT32 prime = std::min(renFrames, (UINT32)pacer.targetFrames() + outPeriod / 2);
        if (SUCCEEDED(render->GetBuffer(prime, &data))) render->ReleaseBuffer(prime, AUDCLNT_BUFFERFLAGS_SILENT);
        if (keepClient_) keepClient_->Start();
        if (FAILED(hr = capClient->Start())) { cleanupCapture(); code = VG_ERR_SOURCE; return report(first, "source: Start", hr); }
        if (FAILED(hr = renClient->Start())) { capClient->Stop(); cleanupCapture(); code = VG_ERR_OUTPUT; return report(first, "output: Start", hr); }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        state_.store(VG_STATE_BUFFERING);
        if (first) { firstResult_ = VG_OK; SetEvent(readyEvent_); }

        const double t0 = nowSec();
        double scoreSum = 0.0, startedAt = -1.0;
        long scoreN = 0;
        const bool decided = srcChoice_.count(sourceId_) > 0;
        double lastPoll = t0, quietUntil = t0 + 1.0, lastGlitchLog = -1e9, waitSmooth = 0.0, lastLumpLog = -1e9;
        bool produced = false;
        bool silenced = false;   // silence was put into the output since the last audio: fade the next one in
        // joins in the source: a gap Windows reports (DATA_DISCONTINUITY) or sound coming back after silence
        float lastL = 0.0f, lastR = 0.0f;
        uint64_t packets = 0, gaps = 0, resumes = 0;
        double lastGapLog = -1e9, lastResumeLog = -1e9;
        const size_t declickFrames = (size_t)std::max(8.0, srcRate * 0.002);   // 2 ms
        uint64_t lastDips = 0;
        hr = S_OK;
        // wake up every ~1 ms, and right when the output device has taken its period (its event)
        HANDLE waits[2] = { stopEvent_, renEvent };
        for (;;)
        {
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 1);
            if (w == WAIT_OBJECT_0) break;
            const bool pulled = w == WAIT_OBJECT_0 + 1;
            const double now = nowSec();
            const double dt = std::max(0.0, std::min(0.1, now - lastPoll));
            lastPoll = now;

            // keep-alive: keep its buffer full of silence (its delay does not matter, it is silent)
            if (keep_)
            {
                UINT32 pad = 0;
                if (SUCCEEDED(keepClient_->GetCurrentPadding(&pad)) && pad < keepFrames_)
                {
                    const UINT32 n = keepFrames_ - pad;
                    BYTE* d = nullptr;
                    if (SUCCEEDED(keep_->GetBuffer(n, &d))) keep_->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }

            // 1. everything the source played since the last poll
            bool wasStalled = silenced;
            const double silentFor = now - watch.lastData();
            UINT32 polledFrames = 0;
            for (;;)
            {
                UINT32 next = 0;
                if (FAILED(hr = capture_->GetNextPacketSize(&next)) || next == 0) break;
                BYTE* cdata = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(hr = capture_->GetBuffer(&cdata, &frames, &flags, nullptr, nullptr))) break;
                if (frames > 0)
                {
                    if (lr.size() < (size_t)frames * 2) lr.resize((size_t)frames * 2);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !cdata || blockAlign <= 0)
                        std::fill(lr.begin(), lr.begin() + (size_t)frames * 2, 0.0f);
                    else
                        vga::toStereo(cdata, frames, capFormat_, lr.data());
                    const bool gap = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && packets > 0;
                    if (gap || wasStalled)
                    {
                        // after silence we played zeros, so fade in from 0; after a gap, from the last sample
                        if (wasStalled) { lastL = lastR = 0.0f; }
                        vga::declickStart(lr.data(), frames, declickFrames, lastL, lastR);
                        if (gap)
                        {
                            ++gaps;
                            if (now - lastGapLog > 10.0)
                            {
                                lastGapLog = now;
                                logLine("source: gap in the captured sound (%llu so far), smoothed over 2 ms", (unsigned long long)gaps);
                            }
                        }
                        if (wasStalled)
                        {
                            ++resumes;
                            if (now - lastResumeLog > 10.0)
                            {
                                lastResumeLog = now;
                                logLine("source: sound again after %.0f ms of nothing (%llu times so far), faded in",
                                        1000.0 * silentFor, (unsigned long long)resumes);
                            }
                        }
                        wasStalled = false;
                    }
                    lastL = lr[(size_t)frames * 2 - 2];
                    lastR = lr[(size_t)frames * 2 - 1];
                    ++packets;
                    ring_.write(lr.data(), frames, now);
                    polledFrames += frames;
                    silenced = false;
                }
                if (FAILED(hr = capture_->ReleaseBuffer(frames))) break;
            }
            if (FAILED(hr)) { logLine("source failed: 0x%08lX", (unsigned long)hr); break; }
            if (polledFrames > 0 && watch.onData(now, (double)polledFrames / srcRate))
            {
                // the source was only late: make the output queue big enough to cover such holes
                pacer.setPacketFrames(watch.holeSec() * outRate);
                if (now - lastLumpLog > 10.0)
                {
                    lastLumpLog = now;
                    logLine("source delivers in lumps (%llu so far): up to %.0f ms of nothing, then all of it; keeping %.1f ms queued",
                            (unsigned long long)watch.lumps(), 1000.0 * watch.holeSec(), pacer.targetMs());
                }
            }

            // 2. how much the output still has queued
            UINT32 padding = 0;
            if (FAILED(hr = renClient->GetCurrentPadding(&padding))) { logLine("output: GetCurrentPadding 0x%08lX", (unsigned long)hr); break; }

            const bool stalled = watch.stalled(now);

            // Right after the device took its period the queue is at its lowest: the safety check.
            if (pulled)
            {
                // dt > 8 ms: this thread itself did not run for a while (the system was busy)
                const bool glitch = pacer.onPull((double)padding, produced && !stalled && now > quietUntil, dt > 0.008);
                if (glitch) underrunsTotal_.fetch_add(1);
                if ((glitch || pacer.dips() != lastDips) && now - lastGlitchLog > 5.0)
                {
                    lastGlitchLog = now;
                    logLine("output queue ran low (%llu empty, %llu low), now keeping %.1f ms on average",
                            (unsigned long long)pacer.glitches(), (unsigned long long)pacer.dips(), pacer.targetMs());
                }
                lastDips = pacer.dips();
            }
            const double corr = pacer.update((double)padding, dt);

            // far too much queued (e.g. after the system hung for a moment): drop input not queued yet
            if (pacer.excess() > 0.020 * outRate)
            {
                const double drop = std::min(rs.pendingInput(ring_), pacer.excess());
                if (drop > 0) { rs.skipInput(ring_, (size_t)drop); pacer.settle(drop); }
            }

            // 3. turn everything that arrived into output, process, queue
            const vga::Params p = currentParams();
            if (p != params) { params = p; proc.setParams(p); }
            const UINT32 room = renFrames - std::min(renFrames, padding);
            int n = rs.produce(ring_, l.data(), r.data(), (int)std::min<UINT32>(room, (UINT32)l.size()), corr);
            if (n == 0 && produced && stalled)
            {
                // the source sends nothing (no keep-alive and the game is quiet): keep the output fed with
                // silence, and do not judge our own timing until sound flows again for a moment
                const UINT32 want = std::min(renFrames, (UINT32)pacer.floorFrames() + outPeriod);
                if (padding < want)
                {
                    n = (int)std::min<UINT32>(want - padding, (UINT32)l.size());
                    std::fill(l.begin(), l.begin() + n, 0.0f);
                    std::fill(r.begin(), r.begin() + n, 0.0f);
                    silenced = true;
                }
                quietUntil = now + 0.3;
                state_.store(VG_STATE_BUFFERING);
            }
            else if (n > 0)
            {
                if (!produced) { produced = true; quietUntil = now + 1.0; }
                state_.store(VG_STATE_RUNNING);
            }
            if (n > 0)
            {
                proc.process(l.data(), r.data(), n);
                if (FAILED(hr = render->GetBuffer((UINT32)n, &data))) { logLine("output: GetBuffer 0x%08lX", (unsigned long)hr); break; }
                vga::fromStereo(l.data(), r.data(), (size_t)n, outFormat, data);
                if (FAILED(hr = render->ReleaseBuffer((UINT32)n, 0))) { logLine("output: ReleaseBuffer 0x%08lX", (unsigned long)hr); break; }
                // the newest sample waits behind everything queued
                const double waitMs = 1000.0 * (double)(padding + (UINT32)n) / outRate;
                waitSmooth += (waitMs - waitSmooth) * 0.02;
            }

            // status for the UI
            raiseMax(inPeak_, proc.takeInputPeak());
            raiseMax(outPeak_, proc.takeOutputPeak());
            lowerMin(reduction_, proc.takeReductionDb());
            const float bufMs = (float)(1000.0 * (double)ring_.fill() / srcRate);
            bufferMs_.store(bufMs);
            correctionPpm_.store((float)(corr * 1e6));
            outputMs_.store((float)waitSmooth);
            latencyMs_.store((float)srcPeriodMs + bufMs + (float)waitSmooth + lookMs);

            // which source mode is faster here (see srcMode_): measured from 4 to 10 s after the sound started
            if (!decided && produced)
            {
                if (startedAt < 0) startedAt = now;
                const double age = now - startedAt;
                if (age > 4.0) { scoreSum += srcPeriodMs + bufMs + waitSmooth; ++scoreN; }
                if (age > 10.0 && scoreN > 0)
                {
                    const double score = scoreSum / scoreN;
                    if (srcMode_ == 0 && trialScore0_ < 0.0)
                    {
                        if (watch.lumps() >= 30 && watch.holeSec() > 2.0 * srcPeriodMs / 1000.0)
                        {
                            trialScore0_ = score;
                            srcMode_ = 1;
                            logLine("source mode: small packets come in lumps, %.1f ms; trying normal packets", score);
                            restartQuick_ = true;
                            break;
                        }
                        srcChoice_[sourceId_] = 0;   // steady small packets: nothing to try
                        logLine("source mode: small packets, steady (%.1f ms)", score);
                    }
                    else if (srcMode_ == 1 && trialScore0_ >= 0.0)
                    {
                        const int best = score < trialScore0_ - 1.0 ? 1 : 0;
                        srcChoice_[sourceId_] = best;
                        logLine("source mode: normal packets %.1f ms, small packets %.1f ms: keeping %s", score, trialScore0_,
                                best ? "normal packets" : "small packets");
                        if (best == 0) { srcMode_ = 0; restartQuick_ = true; break; }
                    }
                    startedAt = 1e18;   // decided for this session
                }
            }
        }
        if (hr == S_OK && !restartQuick_ && WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) hr = E_FAIL;

        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        renClient->Stop();
        capClient->Stop();
        cleanupCapture();
        if (FAILED(hr)) logLine("session ended: 0x%08lX, reconnecting", (unsigned long)hr);
        return true;
    }

    int measureOn(const std::wstring& srcId, const std::wstring& outId, float* ms, int* valid)
    {
        Com<IMMDeviceEnumerator> en;
        Com<IMMDevice> srcDev, outDev;
        HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, en.putVoid());
        if (FAILED(hr) || FAILED(en->GetDevice(srcId.c_str(), srcDev.put()))) return fail(VG_ERR_SOURCE, "measure: source device");
        if (FAILED(en->GetDevice(outId.c_str(), outDev.put()))) return fail(VG_ERR_OUTPUT, "measure: output device");

        // a beep player on the source device (where the game plays) and a recorder of what the headphones play
        CoTaskFormat pf, cf;
        Com<IAudioClient> player, recorder;
        Com<IAudioRenderClient> play;
        Com<IAudioCaptureClient> rec;
        Com<IAudioClock> clock;
        vga::SampleFormat pFmt, cFmt;
        UINT32 pPeriod = 0, cPeriod = 0, pFrames = 0;
        bool low = false;
        if (FAILED(srcDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, player.putVoid())) || FAILED(player->GetMixFormat(&pf.p))
            || !parseFormat(pf.p, pFmt)
            || FAILED(openStream(srcDev.p, 0, 2000000, pf.p, false, player, pPeriod, low))
            || FAILED(player->GetBufferSize(&pFrames))
            || FAILED(player->GetService(kIID_IAudioRenderClient, play.putVoid()))
            || FAILED(player->GetService(kIID_IAudioClock, clock.putVoid())))
            return fail(VG_ERR_SOURCE, "measure: could not play into the source device");
        if (FAILED(outDev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, recorder.putVoid())) || FAILED(recorder->GetMixFormat(&cf.p))
            || !parseFormat(cf.p, cFmt)
            || FAILED(openStream(outDev.p, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, cf.p, false, recorder, cPeriod, low))
            || FAILED(recorder->GetService(kIID_IAudioCaptureClient, rec.putVoid())))
            return fail(VG_ERR_OUTPUT, "measure: could not record the output device");

        const double pRate = pf.p->nSamplesPerSec, cRate = cf.p->nSamplesPerSec;
        UINT64 clockFreq = 0;
        clock->GetFrequency(&clockFreq);
        if (clockFreq == 0) return fail(VG_ERR_SYSTEM, "measure: no device clock");

        const int kBeeps = 5;
        const double kGap = 0.4;                                     // s between beeps
        const int beepFrames = (int)(pRate * 0.003);                 // 3 ms of 2 kHz at -10 dBFS
        std::vector<double> beepAt;                                  // when each beep starts playing (s, QPC)
        std::vector<float> lr, l(4096), r(4096);
        std::vector<float> recLevel;                                 // what the output played: |level| per frame
        std::vector<double> recTime;                                 // ... and when (s, QPC)
        recLevel.reserve((size_t)(cRate * 3.0));
        recTime.reserve((size_t)(cRate * 3.0));

        recorder->Start();
        player->Start();
        const double t0 = nowSec();
        UINT64 written = 0;
        int beepsSent = 0;
        const double qpcScale = 1e-7;                                // GetBuffer / GetPosition QPC values: 100 ns
        while (nowSec() - t0 < 0.3 + kBeeps * kGap + 0.4)
        {
            Sleep(2);
            // keep ~30 ms queued in the player; put a beep in at its time
            UINT32 pad = 0;
            if (SUCCEEDED(player->GetCurrentPadding(&pad)))
            {
                const UINT32 want = (UINT32)(pRate * 0.03);
                if (pad < want)
                {
                    const UINT32 n = std::min<UINT32>(std::min(want - pad, pFrames - pad), (UINT32)l.size());
                    std::fill(l.begin(), l.begin() + n, 0.0f);
                    std::fill(r.begin(), r.begin() + n, 0.0f);
                    const double el = nowSec() - t0;
                    if (beepsSent < kBeeps && el > 0.3 + beepsSent * kGap && (int)n >= beepFrames)
                    {
                        for (int i = 0; i < beepFrames; ++i)
                            l[i] = r[i] = 0.316f * (float)std::sin(2.0 * 3.14159265358979 * 2000.0 * i / pRate);
                        // the device position tells when frame `written` will play
                        UINT64 pos = 0, qpc = 0;
                        if (SUCCEEDED(clock->GetPosition(&pos, &qpc)))
                        {
                            const double posFrames = (double)pos * pRate / (double)clockFreq;
                            beepAt.push_back(qpc * qpcScale + ((double)written - posFrames) / pRate);
                        }
                        ++beepsSent;
                    }
                    BYTE* d = nullptr;
                    if (SUCCEEDED(play->GetBuffer(n, &d)))
                    {
                        vga::fromStereo(l.data(), r.data(), n, pFmt, d);
                        play->ReleaseBuffer(n, 0);
                        written += n;
                    }
                }
            }
            // everything the headphones played since the last look
            for (;;)
            {
                UINT32 next = 0;
                if (FAILED(rec->GetNextPacketSize(&next)) || next == 0) break;
                BYTE* d = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 qpc = 0;
                if (FAILED(rec->GetBuffer(&d, &frames, &flags, nullptr, &qpc))) break;
                if (lr.size() < (size_t)frames * 2) lr.resize((size_t)frames * 2);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !d) std::fill(lr.begin(), lr.begin() + (size_t)frames * 2, 0.0f);
                else vga::toStereo(d, frames, cFmt, lr.data());
                for (UINT32 i = 0; i < frames; ++i)
                {
                    recLevel.push_back(std::max(std::fabs(lr[2 * i]), std::fabs(lr[2 * i + 1])));
                    recTime.push_back(qpc * qpcScale + i / cRate);
                }
                rec->ReleaseBuffer(frames);
            }
        }
        player->Stop();
        recorder->Stop();

        // each beep: the first sample clearly above what played just before it, within 300 ms
        std::vector<double> found;
        size_t from = 0;
        for (double at : beepAt)
        {
            while (from < recTime.size() && recTime[from] < at - 0.15) ++from;
            double sum = 0.0;
            size_t i = from, n = 0;
            for (; i < recTime.size() && recTime[i] < at; ++i, ++n) sum += (double)recLevel[i] * recLevel[i];
            const double bg = n ? std::sqrt(sum / n) : 0.0;
            const float thr = (float)std::max(0.01, 6.0 * bg);    // -40 dBFS, or well above the game's sound
            for (; i < recTime.size() && recTime[i] < at + 0.3; ++i)
                if (recLevel[i] > thr) { found.push_back(1000.0 * (recTime[i] - at)); break; }
        }
        *valid = (int)found.size();
        if (found.size() < 3)
        {
            logLine("measure: heard %d of %d beeps", (int)found.size(), (int)beepAt.size());
            return fail(VG_ERR_TIMEOUT, "measure: the beeps did not come through (too loud around, or bypassed)");
        }
        std::sort(found.begin(), found.end());
        *ms = (float)found[found.size() / 2];
        logLine("measure: real delay %.1f ms (median of %d beeps, %.1f..%.1f)", *ms, (int)found.size(), found.front(), found.back());
        return VG_OK;
    }

    void cleanupCapture()
    {
        if (keepClient_) keepClient_->Stop();
        keepClient_ = nullptr;   // owned by runSession's Com<>
        keep_.reset();
        capture_.reset();
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
    HANDLE thread_ = nullptr, stopEvent_ = nullptr, readyEvent_ = nullptr;
    std::wstring sourceId_, outputId_, error_;
    int firstResult_ = VG_OK;

    // session objects
    vga::FrameRing ring_;
    vga::SampleFormat capFormat_;
    Com<IAudioCaptureClient> capture_;
    Com<IAudioRenderClient> keep_;
    IAudioClient* keepClient_ = nullptr;
    UINT32 keepFrames_ = 0;

    // Source mode: 0 = keep-alive with the smallest engine period (2.7 ms packets), 1 = normal period (10 ms).
    // Some systems deliver the small packets in lumps (~20 ms holes), and then the normal period may be
    // faster in total. When lumps show up, both are tried once (~10 s each) and the faster one is kept for
    // that source device while the DLL is loaded.
    int srcMode_ = 0;
    double trialScore0_ = -1.0;            // latency without lookahead in mode 0, while mode 1 is tried
    std::map<std::wstring, int> srcChoice_;
    bool restartQuick_ = false;            // end the session and start again at once (not a reconnect)

    // parameters (written by vg_set_params)
    std::atomic<int> pMode_{ 1 };
    std::atomic<float> pAmount_{ 50.0f }, pCeiling_{ -1.0f }, pGain_{ 0.0f }, pRelease_{ 100.0f };
    std::atomic<bool> pBypass_{ false };

    // status (read by vg_get_status)
    std::atomic<int> state_{ VG_STATE_STOPPED }, srcRate_{ 0 }, outRate_{ 0 }, restarts_{ 0 };
    std::atomic<uint64_t> underrunsTotal_{ 0 };
    std::atomic<float> inPeak_{ 0.0f }, outPeak_{ 0.0f }, reduction_{ 0.0f };
    std::atomic<float> latencyMs_{ 0.0f }, bufferMs_{ 0.0f }, correctionPpm_{ 0.0f };
    std::atomic<float> sourceMs_{ 0.0f }, outputMs_{ 0.0f }, lookaheadMs_{ 0.0f };
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

// ---------------------------------------------------------------------------------------------------------
// Which device an app plays to — what "Settings → System → Sound → Volume mixer → Output device" sets per app.
// Windows has no public API for it. The interface below is the one the Settings page itself uses (also used
// by the open-source EarTrumpet): the activation factory of "Windows.Media.Internal.AudioPolicyConfig".
// Its IID changed in Windows build 21390 (21H2); the method layout did not, so both IIDs are tried.
// Vtable: IUnknown (3) + IInspectable (3) + 19 other methods, then
//   [25] SetPersistedDefaultAudioEndpoint(UINT32 pid, EDataFlow, ERole, HSTRING deviceInterfacePath)
//   [26] GetPersistedDefaultAudioEndpoint(UINT32 pid, EDataFlow, ERole, HSTRING* deviceInterfacePath)
// The device is given as its interface path: \\?\SWD#MMDEVAPI#<endpoint id>#{e6327cad-…} (render endpoints).
// Windows remembers the choice for the app (by its exe), so it also holds the next time the game starts.

typedef void* HStr;
typedef HRESULT(WINAPI* RoGetActivationFactoryFn)(HStr, REFIID, void**);
typedef HRESULT(WINAPI* WindowsCreateStringFn)(const wchar_t*, UINT32, HStr*);
typedef HRESULT(WINAPI* WindowsDeleteStringFn)(HStr);
typedef const wchar_t*(WINAPI* WindowsGetStringRawBufferFn)(HStr, UINT32*);
typedef HRESULT(STDMETHODCALLTYPE* SetPersistedFn)(void*, UINT32, EDataFlow, ERole, HStr);
typedef HRESULT(STDMETHODCALLTYPE* GetPersistedFn)(void*, UINT32, EDataFlow, ERole, HStr*);

const wchar_t kMmdevPrefix[] = L"\\\\?\\SWD#MMDEVAPI#";
const wchar_t kRenderSuffix[] = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";

struct PolicyConfig
{
    RoGetActivationFactoryFn getFactory = nullptr;
    WindowsCreateStringFn createString = nullptr;
    WindowsDeleteStringFn deleteString = nullptr;
    WindowsGetStringRawBufferFn rawBuffer = nullptr;
    IUnknown* factory = nullptr;

    // in the caller's COM apartment; returns S_OK or why not
    HRESULT open()
    {
        HMODULE combase = LoadLibraryW(L"combase.dll");
        if (!combase) return HRESULT_FROM_WIN32(GetLastError());
        getFactory = (RoGetActivationFactoryFn)(void*)GetProcAddress(combase, "RoGetActivationFactory");
        createString = (WindowsCreateStringFn)(void*)GetProcAddress(combase, "WindowsCreateString");
        deleteString = (WindowsDeleteStringFn)(void*)GetProcAddress(combase, "WindowsDeleteString");
        rawBuffer = (WindowsGetStringRawBufferFn)(void*)GetProcAddress(combase, "WindowsGetStringRawBuffer");
        if (!getFactory || !createString || !deleteString || !rawBuffer) return E_NOTIMPL;   // Windows 7
        static const wchar_t cls[] = L"Windows.Media.Internal.AudioPolicyConfig";
        HStr name = nullptr;
        HRESULT hr = createString(cls, (UINT32)(sizeof cls / sizeof cls[0] - 1), &name);
        if (FAILED(hr)) return hr;
        static const IID iids[2] = {
            { 0xab3d4648, 0xe242, 0x459f, { 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24 } },   // build 21390 and later
            { 0x2a59116d, 0x6c4f, 0x45e0, { 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58 } },   // Windows 10 before 21H2
        };
        for (const IID& iid : iids)
        {
            hr = getFactory(name, iid, reinterpret_cast<void**>(&factory));
            if (SUCCEEDED(hr) && factory) break;
            factory = nullptr;
        }
        deleteString(name);
        return factory ? S_OK : (FAILED(hr) ? hr : E_NOINTERFACE);
    }

    ~PolicyConfig() { if (factory) factory->Release(); }

    void** vtable() const { return *reinterpret_cast<void***>(factory); }

    HRESULT set(UINT32 pid, const wchar_t* endpointId)
    {
        HStr path = nullptr;
        if (endpointId && *endpointId)
        {
            const std::wstring s = std::wstring(kMmdevPrefix) + endpointId + kRenderSuffix;
            const HRESULT hr = createString(s.c_str(), (UINT32)s.size(), &path);
            if (FAILED(hr)) return hr;
        }
        // null = back to the default device. Both roles, as the Settings page does.
        const SetPersistedFn fn = (SetPersistedFn)vtable()[25];
        HRESULT hr = fn(factory, pid, eRender, eMultimedia, path);
        if (SUCCEEDED(hr)) hr = fn(factory, pid, eRender, eConsole, path);
        if (path) deleteString(path);
        return hr;
    }

    std::wstring get(UINT32 pid)
    {
        HStr path = nullptr;
        const GetPersistedFn fn = (GetPersistedFn)vtable()[26];
        if (FAILED(fn(factory, pid, eRender, eMultimedia, &path)) || !path) return std::wstring();
        UINT32 len = 0;
        const wchar_t* raw = rawBuffer(path, &len);
        std::wstring s = raw ? std::wstring(raw, len) : std::wstring();
        deleteString(path);
        const std::wstring pre(kMmdevPrefix), suf(kRenderSuffix);
        if (s.compare(0, pre.size(), pre) == 0) s.erase(0, pre.size());
        if (s.size() >= suf.size() && _wcsicmp(s.c_str() + s.size() - suf.size(), suf.c_str()) == 0) s.erase(s.size() - suf.size());
        return s;
    }
};

int copyOut(const std::wstring& out, wchar_t* buffer, int capacity)
{
    const int need = (int)out.size() + 1;
    if (buffer && capacity >= need) wmemcpy(buffer, out.c_str(), (size_t)need);
    return need;
}

std::wstring processPath(DWORD pid)
{
    std::wstring path;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return path;
    wchar_t buf[MAX_PATH * 2];
    DWORD len = (DWORD)(sizeof buf / sizeof buf[0]);
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) path.assign(buf, len);
    CloseHandle(h);
    return path;
}

extern "C" {

VG_API int vg_version(void) { return VG_API_VERSION; }

VG_API int vg_list_apps(wchar_t* buffer, int capacity)
{
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring out;
    {
        const DWORD self = GetCurrentProcessId();
        std::vector<DWORD> seen;
        Com<IMMDeviceEnumerator> en;
        Com<IMMDeviceCollection> list;
        if (SUCCEEDED(CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, en.putVoid()))
            && SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, list.put())))
        {
            UINT count = 0;
            list->GetCount(&count);
            for (UINT i = 0; i < count; ++i)
            {
                Com<IMMDevice> dev;
                if (FAILED(list->Item(i, dev.put()))) continue;
                LPWSTR id = nullptr;
                if (FAILED(dev->GetId(&id)) || !id) continue;
                const std::wstring devId = id;
                CoTaskMemFree(id);
                Com<IAudioSessionManager2> mgr;
                Com<IAudioSessionEnumerator> sessions;
                if (FAILED(dev->Activate(kIID_IAudioSessionManager2, CLSCTX_ALL, nullptr, mgr.putVoid()))
                    || FAILED(mgr->GetSessionEnumerator(sessions.put())))
                    continue;
                int n = 0;
                sessions->GetCount(&n);
                for (int k = 0; k < n; ++k)
                {
                    Com<IAudioSessionControl> ctl;
                    Com<IAudioSessionControl2> ctl2;
                    if (FAILED(sessions->GetSession(k, ctl.put()))
                        || FAILED(ctl->QueryInterface(kIID_IAudioSessionControl2, ctl2.putVoid())))
                        continue;
                    AudioSessionState state = AudioSessionStateExpired;
                    DWORD pid = 0;
                    ctl2->GetState(&state);
                    if (state == AudioSessionStateExpired || FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self)
                        continue;   // gone, system sounds, or VolumeGuard's own output
                    bool dup = false;
                    for (DWORD s : seen) dup = dup || s == pid;
                    if (dup) continue;
                    seen.push_back(pid);
                    std::wstring path = processPath(pid);
                    if (path.empty()) continue;
                    for (wchar_t& c : path) if (c == L'\t' || c == L'\n') c = L' ';
                    wchar_t num[16];
                    swprintf(num, 16, L"%lu", (unsigned long)pid);
                    out += num;
                    out += L'\t';
                    out += path;
                    out += L'\t';
                    out += devId;
                    out += state == AudioSessionStateActive ? L"\t1\n" : L"\t0\n";
                }
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return copyOut(out, buffer, capacity);
}

VG_API int vg_route_app(unsigned int pid, const wchar_t* deviceId)
{
    if (pid == 0) return VG_ERR_ARGS;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr;
    {
        PolicyConfig pc;
        hr = pc.open();
        if (SUCCEEDED(hr)) hr = pc.set(pid, deviceId);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    logLine("route app pid %u to %s: hr 0x%08lX", pid, deviceId && *deviceId ? "a device" : "default", (unsigned long)hr);
    return SUCCEEDED(hr) ? VG_OK : (int)hr;
}

VG_API int vg_app_route(unsigned int pid, wchar_t* buffer, int capacity)
{
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring out;
    {
        PolicyConfig pc;
        if (SUCCEEDED(pc.open())) out = pc.get(pid);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return copyOut(out, buffer, capacity);
}

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

VG_API int vg_measure_latency(float* ms, int* beepsHeard)
{
    if (!ms || !beepsHeard) return VG_ERR_ARGS;
    *ms = 0.0f;
    *beepsHeard = 0;
    return gEngine->measure(ms, beepsHeard);
}

} // extern "C"
