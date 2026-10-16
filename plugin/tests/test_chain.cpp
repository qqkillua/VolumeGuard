// Offline checks that nothing in the processing chain clicks when settings change while sound plays:
// ceiling, input gain, Game mode on/off/switch, amount, bypass. Plus: the safety clamp never acts.
// Build: g++ -O2 -std=c++17 -I../src test_chain.cpp
#include "ChainDsp.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

static const double kPi = 3.14159265358979323846;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

// Runs `seconds` of a 200 Hz sine (amplitude amp) through the chain in 128-frame blocks; `change(t)` may
// change settings between blocks. Returns the largest sample-to-sample jump, relative to what a clean
// sine of the output's level can do (2π·f/fs · amplitude): 1.0 = perfectly smooth, a click is far above.
static double worstJump(vgl::Chain& ch, double sr, double seconds, float amp, const std::function<void(double)>& change,
                        double settle = 0.2)
{
    const int block = 128;
    std::vector<float> l(block), r(block);
    double prev = 0, worst = 0, ph = 0;
    const double w = 2 * kPi * 200.0 / sr;
    long n = 0;
    for (double t = 0; t < seconds; t += block / sr)
    {
        change(t);
        for (int i = 0; i < block; ++i, ++n) { l[i] = r[i] = amp * (float)std::sin(ph); ph += w; }
        ch.process(l.data(), r.data(), l.data(), r.data(), block);
        for (int i = 0; i < block; ++i)
        {
            const double y = l[i];
            if (t > settle)
            {
                // the sine's own largest step at the current level: estimate the level from |y| over time
                worst = std::max(worst, std::fabs(y - prev));
            }
            prev = y;
        }
    }
    return worst / w;   // in units of "amplitude of a clean sine with this step"
}

int main()
{
    const double sr = 48000;

    // 1. Ceiling lowered from -1 to -13 dB on a loud sine: no hard clipping by the safety clamp.
    {
        vgl::Chain ch; ch.prepare(sr); ch.setCeilingDb(-1.0f); ch.reset();
        const double jump = worstJump(ch, sr, 1.0, 0.9f, [&](double t) { if (t >= 0.5) ch.setCeilingDb(-13.0f); });
        std::printf("ceiling -1 -> -13 dB: safety clamp acted %llu times, worst step %.3f\n", (unsigned long long)ch.clampCount(), jump);
        CHECK(ch.clampCount() == 0, "lowering the ceiling hard-clipped %llu samples", (unsigned long long)ch.clampCount());
        CHECK(jump < 0.95, "lowering the ceiling made a jump of %.3f (sine of 0.9 = 0.9)", jump);
    }

    // 2. Input gain 0 -> +12 dB on a quiet sine (no limiting): the level glides, no step.
    {
        vgl::Chain ch; ch.prepare(sr); ch.reset();
        const double jump = worstJump(ch, sr, 1.0, 0.05f, [&](double t) { if (t >= 0.5) ch.setInputGainDb(12.0f); });
        std::printf("input gain 0 -> +12 dB: worst step %.3f (clean sine after: 0.199)\n", jump);
        CHECK(jump < 0.21, "input gain change jumped: %.3f", jump);
    }

    // 3. Game mode switched on, between modes, amount changed, switched off — on a loud sine.
    {
        vgl::Chain ch; ch.prepare(sr); ch.reset();
        const double jump = worstJump(ch, sr, 2.0, 0.5f, [&](double t)
        {
            if (t >= 0.4 && t < 0.41) ch.setMode(2);
            if (t >= 0.8 && t < 0.81) ch.setMode(1);
            if (t >= 1.2 && t < 1.21) ch.setAmount(1.0f);
            if (t >= 1.6 && t < 1.61) ch.setMode(0);
        });
        std::printf("Game mode off -> Tactical -> Comfort -> 100%% -> off: worst step %.3f (input sine 0.5)\n", jump);
        CHECK(jump < 0.6, "switching the Game mode jumped: %.3f", jump);
    }

    // 4. Bypass on and off while the limiter works hard: the processed sine is 0.25, the dry one 0.5.
    {
        vgl::Chain ch; ch.prepare(sr); ch.setInputGainDb(6.0f); ch.setCeilingDb(-12.0f); ch.reset();
        const double jump = worstJump(ch, sr, 1.5, 0.5f, [&](double t)
        {
            if (t >= 0.5 && t < 0.51) ch.setBypass(true);
            if (t >= 1.0 && t < 1.01) ch.setBypass(false);
        });
        std::printf("bypass on/off (processed 0.25, dry 0.5): worst step %.3f\n", jump);
        CHECK(jump < 0.55, "bypass jumped: %.3f", jump);
    }

    // 5. Bypass set before the first sound: exactly the dry signal, delayed by the latency.
    {
        vgl::Chain ch; ch.prepare(sr); ch.setInputGainDb(12.0f); ch.setMode(2); ch.setBypass(true); ch.reset();
        const int n = 4800, lat = ch.latencySamples();
        std::vector<float> l(n), r(n), l0(n);
        for (int i = 0; i < n; ++i) l0[i] = l[i] = r[i] = 0.9f * (float)std::sin(i * 0.05);
        ch.process(l.data(), r.data(), l.data(), r.data(), n);
        double err = 0;
        for (int i = lat; i < n; ++i) err = std::max(err, (double)std::fabs(l[i] - l0[i - lat]));
        CHECK(err < 1e-6, "bypass is not the dry signal: %g", err);
    }

    // 6. The ceiling holds through all of it, at several rates (the clamp never has to act).
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        vgl::Chain ch; ch.prepare(rate); ch.setInputGainDb(18.0f); ch.setCeilingDb(-1.0f); ch.reset();
        std::vector<float> l(333), r(333);
        float pk = 0;
        unsigned s = 1;
        for (int b = 0; b < 2000; ++b)
        {
            if (b == 400) ch.setMode(2);
            if (b == 800) ch.setCeilingDb(-9.0f);
            if (b == 1200) ch.setInputGainDb(24.0f);
            if (b == 1600) ch.setMode(1);
            for (int i = 0; i < 333; ++i) { s = s * 1664525u + 1013904223u; l[i] = ((s >> 8) / 8388608.0f - 1.0f) * 0.8f; r[i] = -l[i]; }
            ch.process(l.data(), r.data(), l.data(), r.data(), 333);
            for (int i = 0; i < 333; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        }
        CHECK(pk <= vgl::dbToGain(-1.0f) * 1.000001f, "@%.0f: peak %.4f over the ceiling", rate, pk);
        CHECK(ch.clampCount() == 0, "@%.0f: safety clamp acted %llu times", rate, (unsigned long long)ch.clampCount());
    }

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all chain checks passed\n");
    return 0;
}
