// Offline checks for LookaheadLimiter. Build: g++ -O2 -std=c++17 -I../src test_limiter.cpp
#include "LimiterDsp.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <vector>

using vgl::LookaheadLimiter;

static const double kPi = 3.14159265358979323846;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

struct Stereo { std::vector<float> l, r; };

// Runs the limiter over a whole signal in random block sizes (1..1024), optionally in place.
static Stereo run(LookaheadLimiter& lim, const Stereo& in, unsigned seed, bool inPlace)
{
    Stereo out = in;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> blk(1, 1024);
    size_t pos = 0, n = in.l.size();
    while (pos < n)
    {
        int f = (int)std::min<size_t>((size_t)blk(rng), n - pos);
        const float* ins[2];
        float* outs[2] = { out.l.data() + pos, out.r.data() + pos };
        if (inPlace) { ins[0] = outs[0]; ins[1] = outs[1]; }
        else { ins[0] = in.l.data() + pos; ins[1] = in.r.data() + pos; }
        lim.process(ins, outs, 2, f);
        pos += (size_t)f;
    }
    return out;
}

static float peakOf(const Stereo& s)
{
    float p = 0;
    for (size_t i = 0; i < s.l.size(); ++i) p = std::max(p, std::max(std::fabs(s.l[i]), std::fabs(s.r[i])));
    return p;
}

int main()
{
    const double rates[] = { 44100.0, 48000.0, 96000.0 };
    for (double sr : rates)
    {
        // 1. Ceiling is never exceeded: noise with +12 dB spikes, sines, clicks, silence, in random blocks.
        for (int inPlace = 0; inPlace < 2; ++inPlace)
        for (float ceilDb : { -1.0f, -6.0f, -0.1f })
        for (float relMs : { 1.0f, 50.0f, 500.0f })
        {
            LookaheadLimiter lim;
            lim.prepare(sr, 2);
            lim.setCeilingDb(ceilDb);
            lim.setInputGainDb(6.0f);
            lim.setReleaseMs(relMs);

            std::mt19937 rng(1234);
            std::normal_distribution<float> noise(0.0f, 0.3f);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            const size_t n = (size_t)(sr * 4);
            Stereo in; in.l.resize(n); in.r.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                double t = i / sr;
                float s = 0.5f * (float)std::sin(2 * kPi * 220 * t) + noise(rng);
                if (u(rng) < 0.0005f) s *= 4.0f;                 // random spikes
                if (t > 1.0 && t < 1.2) s = 0.0f;               // silence
                if (t > 2.0 && t < 2.001) s = (i % 2) ? 4 : -4;  // click burst
                in.l[i] = s;
                in.r[i] = (t > 3.0) ? -s : 0.5f * s;            // one channel louder at times
            }
            Stereo out = run(lim, in, 99, inPlace != 0);
            const float ceil = vgl::dbToGain(ceilDb);
            float pk = peakOf(out);
            CHECK(pk <= ceil * 1.000001f, "sr=%.0f ceil=%.1f rel=%.0f inplace=%d: peak %.6f > ceiling %.6f",
                  sr, ceilDb, relMs, inPlace, pk, ceil);
        }

        // 2. Quiet audio passes unchanged, only delayed by the reported latency.
        {
            LookaheadLimiter lim;
            lim.prepare(sr, 2);
            lim.setCeilingDb(-1.0f);
            const int lat = lim.latencySamples();
            CHECK(lat == (int)std::lround(sr * 0.005) - 1, "sr=%.0f: latency %d", sr, lat);
            const size_t n = (size_t)sr;
            Stereo in; in.l.resize(n); in.r.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                in.l[i] = 0.1f * (float)std::sin(2 * kPi * 1000 * i / sr);
                in.r[i] = 0.1f * (float)std::cos(2 * kPi * 300 * i / sr);
            }
            Stereo out = run(lim, in, 7, false);
            float maxErr = 0;
            for (size_t i = (size_t)lat; i < n; ++i)
                maxErr = std::max(maxErr, std::max(std::fabs(out.l[i] - in.l[i - lat]), std::fabs(out.r[i] - in.r[i - lat])));
            CHECK(maxErr < 1e-6f, "sr=%.0f: quiet signal changed, max error %g", sr, maxErr);
            CHECK(lim.takeGainReductionDb() == 0.0f, "sr=%.0f: reduction reported on quiet signal", sr);
        }

        // 3. After a loud burst the gain comes back (release 100 ms → within 0.1 dB after 1 s).
        {
            LookaheadLimiter lim;
            lim.prepare(sr, 2);
            lim.setCeilingDb(-1.0f);
            lim.setReleaseMs(100.0f);
            const size_t n = (size_t)(sr * 2);
            Stereo in; in.l.assign(n, 0.0f); in.r.assign(n, 0.0f);
            for (size_t i = 0; i < n; ++i)
            {
                float a = (i < (size_t)(sr * 0.5)) ? 2.0f : 0.2f;   // +6 dBFS for 0.5 s, then -14 dBFS
                in.l[i] = in.r[i] = a * (float)std::sin(2 * kPi * 440 * i / sr);
            }
            Stereo out = run(lim, in, 3, false);
            const int lat = lim.latencySamples();
            size_t from = (size_t)(sr * 1.5) + (size_t)lat;
            float ratio = 1.0f;
            for (size_t i = from; i < n; ++i)
                if (std::fabs(in.l[i - lat]) > 0.1f) ratio = std::min(ratio, out.l[i] / in.l[i - lat]);
            CHECK(vgl::gainToDb(ratio) > -0.1f, "sr=%.0f: gain not recovered, %.2f dB", sr, vgl::gainToDb(ratio));
        }

        // 4. Bypass: dry signal, same latency.
        {
            LookaheadLimiter lim;
            lim.prepare(sr, 2);
            lim.setCeilingDb(-12.0f);
            lim.setInputGainDb(12.0f);
            const int lat = lim.latencySamples();
            std::vector<float> l(4096), r(4096), ol(4096), orr(4096);
            for (int i = 0; i < 4096; ++i) l[i] = r[i] = 0.9f * (float)std::sin(i * 0.05);
            const float* ins[2] = { l.data(), r.data() };
            float* outs[2] = { ol.data(), orr.data() };
            lim.process(ins, outs, 2, 4096, true);
            float maxErr = 0;
            for (int i = lat; i < 4096; ++i) maxErr = std::max(maxErr, std::fabs(ol[i] - l[i - lat]));
            CHECK(maxErr < 1e-5f, "sr=%.0f: bypass changed the signal, max error %g", sr, maxErr);
        }
    }

    // 5. Speed: 60 s of stereo at 48 kHz.
    {
        LookaheadLimiter lim;
        lim.prepare(48000, 2);
        std::vector<float> l(512), r(512);
        for (int i = 0; i < 512; ++i) l[i] = r[i] = 1.5f * (float)std::sin(i * 0.1);
        const float* ins[2] = { l.data(), r.data() };
        float* outs[2] = { l.data(), r.data() };
        auto t0 = std::clock();
        for (int b = 0; b < 48000 * 60 / 512; ++b) lim.process(ins, outs, 2, 512);
        double sec = double(std::clock() - t0) / CLOCKS_PER_SEC;
        std::printf("speed: 60 s of 48 kHz stereo in %.3f s of CPU (%.2f%% of one core)\n", sec, sec / 60 * 100);
    }

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all limiter checks passed\n");
    return 0;
}
