// Offline checks for the Game mode (GameProcessor + LookaheadLimiter), on a synthetic "shooter" soundtrack.
// Build: g++ -O2 -std=c++17 -I../src test_game.cpp
#include "GameDsp.hpp"
#include "LimiterDsp.hpp"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <vector>

static const double kPi = 3.14159265358979323846;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

// The same order as the plugin: input gain → game processor → limiter.
struct Chain
{
    vgl::GameProcessor game;
    vgl::LookaheadLimiter lim;
    void prepare(double sr, int mode, float amount, float ceilingDb)
    {
        game.prepare(sr); game.setMode(mode); game.setAmount(amount);
        lim.prepare(sr, 2); lim.setCeilingDb(ceilingDb); lim.setReleaseMs(100.0f);
    }
    void run(std::vector<float>& l, std::vector<float>& r)
    {
        const int block = 256;
        for (size_t pos = 0; pos < l.size(); pos += block)
        {
            int n = (int)std::min<size_t>(block, l.size() - pos);
            game.process(l.data() + pos, r.data() + pos, n);
            const float* in[2] = { l.data() + pos, r.data() + pos };
            float* out[2] = { l.data() + pos, r.data() + pos };
            lim.process(in, out, 2, n);
        }
    }
};

static double rmsDb(const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (double)x[i] * x[i];
    return 10 * std::log10(s / double(to - from) + 1e-30);
}

// power of one frequency (Goertzel), dB
static double toneDb(const std::vector<float>& x, size_t from, size_t to, double f, double sr)
{
    const double w = 2 * kPi * f / sr, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < to; ++i) { double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 10 * std::log10(p / double(to - from) / double(to - from) + 1e-30);
}

int main()
{
    const double sr = 48000;
    const char* names[] = { "Off", "Comfort", "Tactical" };

    // 1. Footsteps vs. gunfire: 1.5 s of steps (-40 dBFS), then 1.5 s of automatic fire (-2 dBFS).
    //    Measure the last second of each part, after the compressors have settled.
    double inGap = 0, outGap[3] = { 0, 0, 0 };
    for (int mode = 0; mode < 3; ++mode)
    {
        const size_t n = (size_t)(sr * 3);
        std::vector<float> l(n), r(n);
        std::mt19937 rng(5);
        std::normal_distribution<float> noise(0.0f, 1.0f);
        for (size_t i = 0; i < n; ++i)
        {
            double t = i / sr;
            float s = 0;
            if (t < 1.5)
            {   // a step: 40 ms of filtered thump + scuff every 350 ms
                double ph = std::fmod(t, 0.35);
                if (ph < 0.04) s = 0.01f * (float)(std::sin(2 * kPi * 900 * t) + 0.5 * std::sin(2 * kPi * 2300 * t)) * (float)std::exp(-ph * 60);
            }
            else
            {   // a shot: 25 ms noise burst every 100 ms
                double ph = std::fmod(t - 1.5, 0.10);
                if (ph < 0.025) s = 0.79f * std::max(-1.0f, std::min(1.0f, noise(rng) * 0.5f)) * (float)std::exp(-ph * 80);
            }
            l[i] = s; r[i] = 0.8f * s;
        }
        const size_t a0 = (size_t)(sr * 0.5), a1 = (size_t)(sr * 1.5), b0 = (size_t)(sr * 2.0), b1 = n;
        if (mode == 0) inGap = rmsDb(l, b0, b1) - rmsDb(l, a0, a1);
        Chain ch; ch.prepare(sr, mode, 0.5f, -1.0f);
        ch.run(l, r);
        const int lat = ch.lim.latencySamples();
        outGap[mode] = rmsDb(l, b0 + lat, b1) - rmsDb(l, a0 + lat, a1 + lat);
        std::printf("%-8s: gunfire is %.1f dB louder than steps (input: %.1f dB)\n", names[mode], outGap[mode], inGap);
    }
    CHECK(inGap - outGap[1] >= 6.0, "Comfort narrows the gap by only %.1f dB", inGap - outGap[1]);
    CHECK(inGap - outGap[2] >= 14.0, "Tactical narrows the gap by only %.1f dB", inGap - outGap[2]);
    CHECK(outGap[2] < outGap[1], "Tactical should narrow the gap more than Comfort");

    // 2. Engine rumble (50 Hz, -6 dBFS) together with steps (2 kHz, -36 dBFS): Tactical should bring the steps forward.
    {
        double inRatio = 0, outRatio = 0;
        const size_t n = (size_t)(sr * 2);
        std::vector<float> l(n), r(n);
        for (size_t i = 0; i < n; ++i)
        {
            l[i] = r[i] = 0.5f * (float)std::sin(2 * kPi * 50 * i / sr) + 0.0158f * (float)std::sin(2 * kPi * 2000 * i / sr);
        }
        inRatio = toneDb(l, sr, n, 50, sr) - toneDb(l, sr, n, 2000, sr);
        Chain ch; ch.prepare(sr, 2, 0.5f, -1.0f);
        ch.run(l, r);
        outRatio = toneDb(l, sr, n, 50, sr) - toneDb(l, sr, n, 2000, sr);
        std::printf("rumble vs steps: %.1f dB in, %.1f dB out (Tactical)\n", inRatio, outRatio);
        CHECK(inRatio - outRatio >= 6.0, "rumble not reduced relative to steps: %.1f dB", inRatio - outRatio);
    }

    // 3. Direction is kept: a sound only on the left stays only on the left; L/R balance is unchanged.
    for (int mode = 1; mode < 3; ++mode)
    {
        const size_t n = (size_t)sr;
        std::vector<float> l(n), r(n, 0.0f);
        for (size_t i = 0; i < n; ++i) l[i] = 0.7f * (float)std::sin(2 * kPi * 440 * i / sr);
        std::vector<float> l2 = l, r2(n);
        for (size_t i = 0; i < n; ++i) r2[i] = 0.25f * l2[i];          // panned: right at -12 dB
        Chain a; a.prepare(sr, mode, 1.0f, -1.0f); a.run(l, r);
        Chain b; b.prepare(sr, mode, 1.0f, -1.0f); b.run(l2, r2);
        float maxR = 0;
        for (float v : r) maxR = std::max(maxR, std::fabs(v));
        double bal = rmsDb(l2, n / 2, n) - rmsDb(r2, n / 2, n);
        CHECK(maxR == 0.0f, "%s: left-only sound leaked to the right (%g)", names[mode], maxR);
        CHECK(std::fabs(bal - 12.04) < 0.05, "%s: L/R balance changed to %.2f dB", names[mode], bal);
    }

    // 4. The ceiling still holds in every mode and at every amount, including make-up gain.
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    for (int mode = 0; mode < 3; ++mode)
    for (float amount : { 0.0f, 0.5f, 1.0f })
    {
        const size_t n = (size_t)(rate * 2);
        std::vector<float> l(n), r(n);
        std::mt19937 rng(9);
        std::normal_distribution<float> noise(0.0f, 0.4f);
        for (size_t i = 0; i < n; ++i) { l[i] = noise(rng) * (i % 9000 < 300 ? 4.0f : 0.3f); r[i] = -l[i] * 0.7f; }
        Chain ch; ch.prepare(rate, mode, amount, -1.0f);
        ch.run(l, r);
        float pk = 0;
        for (size_t i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        CHECK(pk <= vgl::dbToGain(-1.0f) * 1.000001f, "%s %.0f%% @%.0f: peak %.5f over ceiling", names[mode], amount * 100, rate, pk);
        CHECK(std::isfinite(pk), "%s: not finite", names[mode]);
    }

    // 5. Off really is off.
    {
        vgl::GameProcessor g; g.prepare(sr); g.setMode(0);
        std::vector<float> l(1000), r(1000);
        for (int i = 0; i < 1000; ++i) l[i] = r[i] = (float)std::sin(i * 0.1);
        std::vector<float> l0 = l;
        g.process(l.data(), r.data(), 1000);
        CHECK(l == l0, "Off mode changed the signal");
    }

    // 6. Speed of the whole chain in Tactical.
    {
        Chain ch; ch.prepare(48000, 2, 0.5f, -1.0f);
        std::vector<float> l(512), r(512);
        for (int i = 0; i < 512; ++i) l[i] = r[i] = 0.5f * (float)std::sin(i * 0.1);
        auto t0 = std::clock();
        for (int b = 0; b < 48000 * 60 / 512; ++b) ch.run(l, r);
        double sec = double(std::clock() - t0) / CLOCKS_PER_SEC;
        std::printf("speed: Tactical chain, 60 s of 48 kHz stereo in %.3f s (%.2f%% of one core)\n", sec, sec / 60 * 100);
    }

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all game mode checks passed\n");
    return 0;
}
