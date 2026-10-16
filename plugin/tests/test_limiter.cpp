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
            CHECK(lat == (int)std::lround(sr * 0.005) - 1 + vgl::LookaheadLimiter::kTpHalf, "sr=%.0f: latency %d", sr, lat);
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

    // 6. Auto release: one short shot is released faster than the Release setting; a run of shots is
    //    released slower (less pumping between them); the ceiling holds either way.
    {
        const double sr = 48000;
        // gain (dB) the limiter applies to every sample, Release 100 ms
        auto gains = [&](bool autoRel, const std::vector<float>& x) {
            vgl::LookaheadLimiter lim;
            lim.prepare(sr, 1); lim.setCeilingDb(-1.0f); lim.setReleaseMs(100.0f); lim.setAutoRelease(autoRel);
            std::vector<float> g(x.size());
            float pk = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                float v = x[i], o = 0;
                const float* in[1] = { &v };
                float* out[1] = { &o };
                lim.process(in, out, 1, 1);
                g[i] = lim.takeGainReductionDb();
                pk = std::max(pk, std::fabs(o));
            }
            CHECK(pk <= vgl::dbToGain(-1.0f) * 1.000001f, "auto release %d: peak %.5f over the ceiling", (int)autoRel, pk);
            CHECK(lim.clampCount() == 0, "auto release %d: safety clamp acted", (int)autoRel);
            return g;
        };
        auto quietTone = [&](size_t i) { return 0.1f * (float)std::sin(2 * 3.14159265358979 * 300 * i / sr); };

        // a) one 10 ms shot (+12 dB over the ceiling), then quiet: time until the gain is back above -1 dB
        const size_t n = (size_t)(sr * 1.5), shotEnd = (size_t)(sr * 0.51);
        std::vector<float> x(n);
        for (size_t i = 0; i < n; ++i) x[i] = (i >= (size_t)(sr * 0.5) && i < shotEnd) ? 4.0f * ((i / 24) % 2 ? 1.0f : -1.0f) : quietTone(i);
        double backMs[2] = { 0, 0 };
        for (int a = 0; a < 2; ++a)
        {
            const std::vector<float> g = gains(a == 1, x);
            size_t k = shotEnd;
            while (k < n && g[k] < -1.0f) ++k;
            backMs[a] = (k - shotEnd) * 1000.0 / sr;
        }

        // b) a burst of fire: 20 ms shots every 60 ms for 3 s; how far the gain swings between the shots
        //    in the last 1.5 s, and how long it takes to come back fully after the burst
        const size_t m = (size_t)(sr * 6), fireEnd = (size_t)(sr * 3);
        std::vector<float> y(m);
        for (size_t i = 0; i < m; ++i)
        {
            const double t = i / sr, ph = std::fmod(t, 0.06);
            y[i] = (i < fireEnd && ph < 0.02) ? 4.0f * (float)std::sin(2 * 3.14159265358979 * 150 * t) : quietTone(i);
        }
        double swing[2] = { 0, 0 }, recoverS[2] = { 0, 0 };
        for (int a = 0; a < 2; ++a)
        {
            const std::vector<float> g = gains(a == 1, y);
            float lo = 0, hi = -200;
            for (size_t i = (size_t)(sr * 1.5); i < fireEnd; ++i) { lo = std::min(lo, g[i]); hi = std::max(hi, g[i]); }
            swing[a] = hi - lo;
            size_t k = m - 1;
            while (k > fireEnd && g[k] > -0.1f) --k;
            recoverS[a] = (k - fireEnd) / sr;
        }
        std::printf("auto release: one shot back in %.0f ms (fixed %.0f ms); during fire the gain swings %.1f dB (fixed %.1f dB); "
                    "after fire back to 0 dB in %.2f s (fixed %.2f s)\n",
                    backMs[1], backMs[0], swing[1], swing[0], recoverS[1], recoverS[0]);
        CHECK(backMs[1] < backMs[0] * 0.7, "auto release is not faster after one shot (%.0f vs %.0f ms)", backMs[1], backMs[0]);
        CHECK(swing[1] < swing[0] * 0.6, "auto release does not reduce pumping (%.1f vs %.1f dB)", swing[1], swing[0]);
        CHECK(recoverS[1] < 2.0, "auto release takes %.2f s to come back after the fire", recoverS[1]);
    }

    // 7. True peak: sounds whose samples stay under the ceiling but whose wave goes over it between the
    //    samples (what a DAC or a resampler would play). Measured with a long 16× reconstruction.
    {
        const double sr = 48000, pi = 3.14159265358979;
        // the loudest point of the rebuilt wave: 16 points between every two samples, 64-tap windowed sinc
        auto truePeak = [&](const std::vector<float>& x, size_t from) {
            const int half = 32, over = 16;
            double pk = 0;
            for (size_t n = from + half; n + half < x.size(); ++n)
                for (int k = 0; k < over; ++k)
                {
                    const double f = k / double(over);
                    double v = 0;
                    for (int j = -half + 1; j <= half; ++j)
                    {
                        const double d = f - j;
                        const double s = std::fabs(d) < 1e-12 ? 1.0 : std::sin(pi * d) / (pi * d);
                        const double w = 0.5 + 0.5 * std::cos(pi * d / half);
                        v += x[n + j] * s * w;
                    }
                    pk = std::max(pk, std::fabs(v));
                }
            return pk;
        };
        struct Case { const char* name; int type; };
        const Case cases[] = {
            { "12 kHz sine, samples at 45 deg", 0 },        // samples ±0.707, wave ±1.0 (+3 dB)
            { "hard-clipped 1 kHz sine at -1.5 dB", 1 },    // clipped game audio: overshoot between samples
            { "noise up to 18 kHz, peaks at +3 dB", 2 },
        };
        // noise like real audio: nothing above 18 kHz (game sound is filtered when it is made or resampled),
        // scaled so its sample peak is exactly 1
        std::vector<float> noise((size_t)(sr * 0.3));
        {
            std::mt19937 rng(11);
            std::uniform_real_distribution<double> u(-1.0, 1.0);
            const int half = 64;
            std::vector<double> w(noise.size() + 2 * half);
            for (double& v : w) v = u(rng);
            double pk = 0;
            for (size_t i = 0; i < noise.size(); ++i)
            {
                double v = 0;
                for (int j = -half; j <= half; ++j)
                {
                    const double fc = 18000.0 / sr, d = j;
                    const double s = j == 0 ? 2 * fc : std::sin(2 * pi * fc * d) / (pi * d);
                    v += w[i + half + j] * s * (0.42 + 0.5 * std::cos(pi * d / half) + 0.08 * std::cos(2 * pi * d / half));
                }
                noise[i] = (float)v;
                pk = std::max(pk, std::fabs(v));
            }
            for (float& v : noise) v = (float)(v / pk);
        }
        auto make = [&](int type, size_t i, std::mt19937& rng) {
            const float clip = vgl::dbToGain(-1.5f);
            if (type == 0) return (float)std::sin(2 * pi * 12000 * i / sr + pi / 4);
            if (type == 1) return std::max(-clip, std::min(clip, (float)(3.0 * std::sin(2 * pi * 1000 * i / sr + 0.2))));
            (void)rng;
            return noise[i] * vgl::dbToGain(3.0f);
        };
        const float ceilingDb = -1.0f, ceil = vgl::dbToGain(ceilingDb);
        for (const Case& cs : cases)
        {
            double tpOn = 0, tpOff = 0, inTp = 0, inSample = 0;
            for (int on = 0; on < 2; ++on)
            {
                const size_t n = (size_t)(sr * 0.3);
                std::vector<float> l(n), r(n);
                std::mt19937 rng(11);
                for (size_t i = 0; i < n; ++i) l[i] = r[i] = make(cs.type, i, rng);
                if (on) { inTp = truePeak(l, 0); for (float v : l) inSample = std::max(inSample, (double)std::fabs(v)); }
                vgl::LookaheadLimiter lim;
                lim.prepare(sr, 2); lim.setCeilingDb(ceilingDb); lim.setTruePeak(on == 1);
                const float* ins[2] = { l.data(), r.data() };
                float* outs[2] = { l.data(), r.data() };
                lim.process(ins, outs, 2, (int)n);
                (on ? tpOn : tpOff) = truePeak(l, (size_t)(sr * 0.1));
            }
            std::printf("true peak, %s: input samples %.2f dB, wave %.2f dB; output wave %.2f dB (True peak off: %.2f dB), ceiling %.1f\n",
                        cs.name, vgl::gainToDb((float)inSample), vgl::gainToDb((float)inTp),
                        vgl::gainToDb((float)tpOn), vgl::gainToDb((float)tpOff), ceilingDb);
            CHECK(tpOn <= ceil * vgl::dbToGain(0.1f), "%s: true peak %.2f dB over the ceiling", cs.name, vgl::gainToDb((float)tpOn));
        }
        // the first case must actually show the problem with True peak off, or the test proves nothing
        {
            const size_t n = (size_t)(sr * 0.3);
            std::vector<float> l(n), r(n);
            for (size_t i = 0; i < n; ++i) l[i] = r[i] = (float)std::sin(2 * pi * 12000 * i / sr + pi / 4);
            vgl::LookaheadLimiter lim;
            lim.prepare(sr, 2); lim.setCeilingDb(ceilingDb); lim.setTruePeak(false);
            const float* ins[2] = { l.data(), r.data() };
            float* outs[2] = { l.data(), r.data() };
            lim.process(ins, outs, 2, (int)n);
            CHECK(truePeak(l, (size_t)(sr * 0.1)) > ceil * vgl::dbToGain(1.0f), "test signal has no intersample peak");
        }
    }

    // 8. Every lookahead from 1 to 10 ms keeps the ceiling (algorithm alone in the no-clamp build), with the
    //    reported latency, at 44.1 / 48 / 96 kHz — also when it is changed between blocks.
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        std::mt19937 rng(17);
        std::normal_distribution<float> nd(0.0f, 0.4f);
        vgl::LookaheadLimiter lim;
        lim.prepare(sr, 2, 5.0);
        lim.setCeilingDb(-3.0f);
        float pk = 0;
        std::vector<float> l(512), r(512);
        for (int b = 0; b < 400; ++b)
        {
            if (b % 40 == 0)
            {
                const double ms = 1.0 + (b / 40) % 10;
                lim.setLookaheadMs(ms);
                CHECK(lim.latencySamples() == (int)std::lround(sr * ms / 1000.0) - 1 + vgl::LookaheadLimiter::kTpHalf,
                      "sr %.0f, %.0f ms: latency %d", sr, ms, lim.latencySamples());
            }
            for (int i = 0; i < 512; ++i) { l[i] = nd(rng) * (b % 7 == 0 ? 4.0f : 1.0f); r[i] = -l[i]; }
            const float* in[2] = { l.data(), r.data() };
            float* out[2] = { l.data(), r.data() };
            lim.process(in, out, 2, 512);
            for (int i = 0; i < 512; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        }
        CHECK(pk <= vgl::dbToGain(-3.0f) * 1.000001f, "sr %.0f: peak %.4f over the ceiling with changing lookahead", sr, pk);
        CHECK(lim.clampCount() == 0, "sr %.0f: safety clamp acted %llu times", sr, (unsigned long long)lim.clampCount());
    }
    std::printf("lookahead 1..10 ms: ceiling and latency right at 44.1 / 48 / 96 kHz\n");

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
