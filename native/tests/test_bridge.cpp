// Offline checks for the device bridge: clock drift, different sample rates, jitter, gaps, formats.
// Build: g++ -O2 -std=c++17 -I../src test_bridge.cpp
#include "Bridge.hpp"

#include <cstdio>
#include <random>
#include <vector>

static const double kPi = 3.14159265358979323846;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

struct Result
{
    uint64_t underrunsAfterWarmup = 0, drops = 0;
    double minFillMs = 1e9, maxFillMs = 0, snrDb = 0, finalCorrection = 0;
};

// Simulates `seconds` of a source device (nominal capRate, real clock off by driftPpm, sending packets of
// ~10 ms with jitter) and an output device asking for `renderFrames` every period. The source plays a 1 kHz sine.
static Result simulate(double capRate, double renRate, double driftPpm, double seconds, double targetMs,
                       double gapFrom = -1, double gapTo = -1, unsigned seed = 1, double toneHz = 1000.0)
{
    vga::FrameRing ring;
    ring.init((size_t)(capRate * 0.5));
    vga::DriftResampler rs;
    rs.prepare(capRate, renRate, targetMs);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(-0.003, 0.003);

    const double capReal = capRate * (1.0 + driftPpm * 1e-6);   // samples per real second
    const int capChunk = (int)(capRate * 0.01);
    const int renFrames = (int)(renRate * 0.01);
    // event times: an exact clock plus independent jitter per event (jitter does not accumulate)
    uint64_t capPackets = 0, renPackets = 0;
    double jitterCap = jitter(rng) * 0.3, jitterRen = jitter(rng) * 0.2;
    auto capTime = [&]() { return (capPackets + 1) * (capChunk / capReal) + jitterCap; };
    auto renTime = [&]() { return 0.002 + (renPackets + 1) * (renFrames / renRate) + jitterRen; };
    double tCap = capTime(), tRen = renTime();
    uint64_t capSamples = 0;
    std::vector<float> lr((size_t)capChunk * 2), l(renFrames), r(renFrames), out;
    out.reserve((size_t)(renRate * 2));

    Result res;
    const double warmup = 5.0, measureFrom = seconds - 1.0;
    uint64_t underrunsAtWarmup = 0;
    bool warm = false;

    while (tRen < seconds)
    {
        if (tCap <= tRen)
        {
            const bool silentGap = tCap >= gapFrom && tCap < gapTo;   // loopback sends nothing while idle
            if (!silentGap)
            {
                for (int i = 0; i < capChunk; ++i, ++capSamples)
                {
                    const float s = 0.5f * (float)std::sin(2 * kPi * toneHz * (double)capSamples / capRate);
                    lr[2 * i] = s; lr[2 * i + 1] = -s;
                }
                ring.write(lr.data(), (size_t)capChunk, tCap);
            }
            ++capPackets; jitterCap = jitter(rng) * 0.3; tCap = capTime();
        }
        else
        {
            rs.read(ring, l.data(), r.data(), renFrames, tRen);
            if (tRen >= measureFrom) out.insert(out.end(), l.begin(), l.end());
            if (!warm && tRen >= warmup) { warm = true; underrunsAtWarmup = rs.underruns(); }
            if (warm && (gapFrom < 0 || tRen < gapFrom || tRen > gapTo + 1.0))
            {
                res.minFillMs = std::min(res.minFillMs, rs.fillMs());
                res.maxFillMs = std::max(res.maxFillMs, rs.fillMs());
            }
            ++renPackets; jitterRen = jitter(rng) * 0.2; tRen = renTime();
        }
    }
    res.underrunsAfterWarmup = rs.underruns() - underrunsAtWarmup;
    res.drops = rs.drops();
    res.finalCorrection = rs.correction();

    // Quality of the last second, in 2048-sample windows: in each window fit a sine (frequency from zero
    // crossings); what is left is noise, distortion and clicks. Slow drift of pitch by a few hundredths of
    // a percent (the clock correction) is fitted away per window — it is far below what anyone can hear.
    double sig = 0, noise = 0;
    for (size_t w0 = 0; w0 + 2048 <= out.size(); w0 += 2048)
    {
        const float* x = out.data() + w0;
        const size_t n = 2048;
        double firstZ = -1, lastZ = -1; int zc = 0;
        for (size_t i = 1; i < n; ++i)
            if (x[i - 1] < 0 && x[i] >= 0)
            {
                const double z = (double)(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                if (firstZ < 0) firstZ = z;
                lastZ = z; ++zc;
            }
        if (zc < 3) { for (size_t i = 0; i < n; ++i) noise += 0.125; continue; }   // silence/click: count as noise
        // least-squares sine at frequency f; residual energy
        auto fitAt = [&](double f, double& s2, double& n2)
        {
            double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double ph = 2 * kPi * f * (double)i, s = std::sin(ph), c = std::cos(ph);
                ss += s * s; sc += s * c; cc += c * c; ys += x[i] * s; yc += x[i] * c;
            }
            const double det = ss * cc - sc * sc;
            const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
            s2 = n2 = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double ph = 2 * kPi * f * (double)i;
                const double fit = a * std::sin(ph) + b * std::cos(ph);
                s2 += fit * fit; n2 += (x[i] - fit) * (x[i] - fit);
            }
        };
        // refine the frequency from the zero crossings with a golden-section search
        double lo = (zc - 1) / (lastZ - firstZ) * 0.999, hi = (zc - 1) / (lastZ - firstZ) * 1.001;
        const double g = 0.6180339887;
        double s2, n2a, n2b;
        for (int it = 0; it < 40; ++it)
        {
            const double m1 = hi - g * (hi - lo), m2 = lo + g * (hi - lo);
            fitAt(m1, s2, n2a); fitAt(m2, s2, n2b);
            if (n2a < n2b) hi = m2; else lo = m1;
        }
        double n2;
        fitAt(0.5 * (lo + hi), s2, n2);
        sig += s2; noise += n2;
    }
    res.snrDb = 10 * std::log10(sig / (noise + 1e-30));
    return res;
}

int main()
{
    struct Case { const char* name; double cap, ren, ppm; };
    const Case cases[] = {
        { "48k -> 48k, same clock", 48000, 48000, 0 },
        { "48k -> 48k, source +500 ppm", 48000, 48000, 500 },
        { "48k -> 48k, source -500 ppm", 48000, 48000, -500 },
        { "44.1k -> 48k, +200 ppm", 44100, 48000, 200 },
        { "48k -> 44.1k, -300 ppm", 48000, 44100, -300 },
        { "96k -> 48k, +1000 ppm", 96000, 48000, 1000 },
    };
    for (const Case& c : cases)
    {
        Result r = simulate(c.cap, c.ren, c.ppm, 600.0, 25.0);   // 10 minutes
        std::printf("%-28s underruns %llu, drops %llu, buffer %.1f..%.1f ms, correction %+.0f ppm, SNR %.1f dB\n",
                    c.name, (unsigned long long)r.underrunsAfterWarmup, (unsigned long long)r.drops,
                    r.minFillMs, r.maxFillMs, r.finalCorrection * 1e6, r.snrDb);
        CHECK(r.underrunsAfterWarmup == 0, "%s: %llu underruns", c.name, (unsigned long long)r.underrunsAfterWarmup);
        CHECK(r.drops == 0, "%s: dropped audio", c.name);
        CHECK(r.maxFillMs < 45.0 && r.minFillMs > 3.0, "%s: buffer drifted to %.1f..%.1f ms", c.name, r.minFillMs, r.maxFillMs);
        CHECK(std::fabs(r.finalCorrection * 1e6 - c.ppm) < 150.0, "%s: correction %.0f ppm, drift is %.0f ppm", c.name, r.finalCorrection * 1e6, c.ppm);
        CHECK(r.snrDb > 80.0, "%s: SNR only %.1f dB", c.name, r.snrDb);
    }

    // High frequencies (10 kHz) through 44.1 → 48 kHz conversion with drift: the resampler's quality.
    {
        Result r = simulate(44100, 48000, 300, 60.0, 25.0, -1, -1, 3, 10000.0);
        std::printf("%-28s underruns %llu, SNR %.1f dB\n", "10 kHz tone, 44.1k -> 48k", (unsigned long long)r.underrunsAfterWarmup, r.snrDb);
        CHECK(r.snrDb > 60.0, "10 kHz: SNR only %.1f dB", r.snrDb);
        CHECK(r.underrunsAfterWarmup == 0, "10 kHz: %llu underruns", (unsigned long long)r.underrunsAfterWarmup);
    }

    // A 3-second pause in the game (loopback sends nothing): silence, then clean recovery.
    {
        Result r = simulate(48000, 48000, 300, 60.0, 25.0, 20.0, 23.0);
        std::printf("%-28s underruns %llu (1 expected: the pause), buffer %.1f..%.1f ms, SNR %.1f dB\n", "pause of 3 s",
                    (unsigned long long)r.underrunsAfterWarmup, r.minFillMs, r.maxFillMs, r.snrDb);
        CHECK(r.underrunsAfterWarmup == 1, "pause: %llu underruns", (unsigned long long)r.underrunsAfterWarmup);
        CHECK(r.snrDb > 80.0, "pause: SNR after recovery %.1f dB", r.snrDb);
    }

    // Format conversion round trips.
    {
        float lr[8] = { 0.5f, -0.25f, -1.0f, 0.999f, 0.0f, 0.1f, 0.75f, -0.75f };
        float l[4], r[4];
        for (int i = 0; i < 4; ++i) { l[i] = lr[2 * i]; r[i] = lr[2 * i + 1]; }
        struct F { int ch, bits, valid; bool fl; double tol; };
        for (F f : { F{ 2, 32, 32, true, 0 }, F{ 2, 16, 16, false, 1.0 / 32768 }, F{ 2, 24, 24, false, 1.0 / 8388608 },
                     F{ 2, 32, 24, false, 1.0 / 8388608 }, F{ 8, 32, 32, true, 0 }, F{ 6, 16, 16, false, 1.0 / 32768 } })
        {
            vga::SampleFormat sf; sf.channels = f.ch; sf.bitsPerSample = f.bits; sf.validBits = f.valid; sf.isFloat = f.fl;
            std::vector<uint8_t> buf((size_t)(4 * sf.blockAlign()));
            vga::fromStereo(l, r, 4, sf, buf.data());
            float back[8];
            vga::toStereo(buf.data(), 4, sf, back);
            double err = 0;
            for (int i = 0; i < 8; ++i) err = std::max(err, (double)std::fabs(back[i] - lr[i]));
            CHECK(err <= f.tol + 1e-7, "format %dch %d-bit %s: round trip error %g", f.ch, f.bits, f.fl ? "float" : "int", err);
        }
        std::printf("format conversions: float32, 16, 24, 32-bit, 2/6/8 channels round-trip OK\n");
    }

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all bridge checks passed\n");
    return 0;
}
