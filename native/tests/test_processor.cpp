// Offline checks for the engine's processing chain and the whole path without Windows:
// device bytes (5.1, 16-bit) → stereo → ring → drift resampler → Processor → device bytes (float stereo).
// Build: g++ -O2 -std=c++17 -Isrc -I../plugin/src tests/test_processor.cpp
#include "Bridge.hpp"
#include "Processor.hpp"

#include <cstdio>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

int main()
{
    const double sr = 48000;

    // 1. Bypass: the output is the input, delayed by exactly the reported latency.
    {
        vga::Processor p;
        p.prepare(sr);
        vga::Params prm; prm.bypass = true; prm.inputGainDb = 12.0f;   // gain must not apply in bypass
        p.setParams(prm);
        const int n = 4800, lat = p.latencyFrames();
        std::vector<float> l(n), r(n), l0(n);
        for (int i = 0; i < n; ++i) l0[i] = l[i] = r[i] = 0.9f * (float)std::sin(i * 0.05);
        p.process(l.data(), r.data(), n);
        double err = 0;
        for (int i = lat; i < n; ++i) err = std::max(err, (double)std::fabs(l[i] - l0[i - lat]));
        std::printf("bypass: latency %d frames (%.2f ms), max error %.2g\n", lat, lat * 1000.0 / sr, err);
        CHECK(err < 1e-6, "bypass changed the signal (%g)", err);
        CHECK(lat > 0 && lat <= (int)(sr * 0.0055), "unexpected latency %d", lat);
    }

    // 2. The ceiling holds in every mode with +24 dB input gain and odd block sizes.
    for (int mode = 0; mode < 3; ++mode)
    {
        vga::Processor p;
        p.prepare(sr);
        vga::Params prm; prm.mode = mode; prm.amount = 100; prm.inputGainDb = 24; prm.ceilingDb = -3;
        p.setParams(prm);
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0.0f, 0.5f);
        float pk = 0;
        std::vector<float> l(997), r(997);
        for (int b = 0; b < 200; ++b)
        {
            const int n = 1 + (int)(rng() % 997);
            for (int i = 0; i < n; ++i) { l[i] = nd(rng); r[i] = -l[i]; }
            p.process(l.data(), r.data(), n);
            for (int i = 0; i < n; ++i) pk = std::max(pk, std::max(std::fabs(l[i]), std::fabs(r[i])));
        }
        CHECK(pk <= vgl::dbToGain(-3.0f) * 1.000001f, "mode %d: peak %.4f over the -3 dB ceiling", mode, pk);
        const float gr = p.takeReductionDb();
        CHECK(gr < -6.0f, "mode %d: reduction meter shows only %.1f dB", mode, gr);
    }
    std::printf("ceiling holds in all modes at +24 dB input\n");

    // 3. Out-of-range parameters are clamped, NaN does not get through.
    {
        vga::Params prm; prm.mode = 7; prm.amount = 1e9f; prm.ceilingDb = NAN; prm.inputGainDb = -1e9f; prm.releaseMs = 0;
        const vga::Params s = prm.sanitized();
        CHECK(s.mode == 2 && s.amount == 100 && s.ceilingDb == -1 && s.inputGainDb == -24 && s.releaseMs == 10, "sanitize");
        vga::Processor p; p.prepare(sr); p.setParams(prm);
        std::vector<float> l(256, 0.5f), r(256, 0.5f);
        p.process(l.data(), r.data(), 256);
        bool finite = true;
        for (float v : l) finite = finite && std::isfinite(v);
        CHECK(finite, "NaN parameters produced non-finite output");
    }

    // 4. The whole path: a 5.1 16-bit 44.1 kHz source (a game in surround) → stereo float 48 kHz output.
    {
        vga::SampleFormat src; src.channels = 6; src.bitsPerSample = 16; src.validBits = 16; src.isFloat = false;
        vga::SampleFormat dst; dst.channels = 2; dst.bitsPerSample = 32; dst.validBits = 32; dst.isFloat = true;
        const double inRate = 44100, outRate = 48000;
        vga::FrameRing ring; ring.init((size_t)inRate);
        vga::DriftResampler rs; rs.prepare(inRate, outRate, 25.0);
        vga::Processor p; p.prepare(outRate);
        vga::Params prm; prm.mode = 0; p.setParams(prm);   // limiter only: the level must come through

        const int inChunk = 441, outChunk = 480;
        std::vector<uint8_t> dev((size_t)inChunk * src.blockAlign()), out((size_t)outChunk * dst.blockAlign());
        std::vector<float> lr((size_t)inChunk * 2), l(outChunk), r(outChunk);
        uint64_t n = 0;
        double t = 0, peak = 0;
        int real = 0, calls = 0;
        for (int step = 0; step < 1000; ++step, t += 0.01)
        {
            // front centre only (-4.3 dBFS): must end up equally in L and R, at -3 dB each (≈0.43)
            for (int i = 0; i < inChunk; ++i, ++n)
            {
                int16_t s[6] = { 0, 0, (int16_t)(20000 * std::sin(2 * 3.14159265358979 * 440 * n / inRate)), 0, 0, 0 };
                std::memcpy(dev.data() + (size_t)i * src.blockAlign(), s, sizeof s);
            }
            vga::toStereo(dev.data(), inChunk, src, lr.data());
            ring.write(lr.data(), inChunk, t);
            real += rs.read(ring, l.data(), r.data(), outChunk, t + 0.005);
            ++calls;
            p.process(l.data(), r.data(), outChunk);
            vga::fromStereo(l.data(), r.data(), outChunk, dst, out.data());
            if (step > 500)
                for (int i = 0; i < outChunk; ++i)
                {
                    float a, b;
                    std::memcpy(&a, out.data() + (size_t)i * 8, 4);
                    std::memcpy(&b, out.data() + (size_t)i * 8 + 4, 4);
                    CHECK(std::fabs(a - b) < 1e-6f, "centre channel not centred: %g vs %g", a, b);
                    if (std::fabs(a - b) >= 1e-6f) { step = 1000; break; }
                    peak = std::max(peak, (double)std::fabs(a));
                }
        }
        std::printf("5.1 int16 44.1k -> stereo float 48k: %d of %d frames real audio, output peak %.3f, underruns %llu\n",
                    real, calls * outChunk, peak, (unsigned long long)rs.underruns());
        CHECK(rs.underruns() == 0, "underruns on the full path");
        CHECK(peak > 0.3 && peak <= vgl::dbToGain(-1.0f) * 1.000001, "output peak %.3f", peak);
    }

    // 5. A gap in the source (last sample 0.8, the next packet starts at -0.8): smoothed over 2 ms, no step.
    {
        const size_t n = 480, ramp = 96;
        std::vector<float> lr(n * 2);
        for (size_t i = 0; i < n; ++i) lr[2 * i] = lr[2 * i + 1] = -0.8f * (float)std::cos(2 * 3.14159265358979 * 300 * i / 48000.0);
        vga::declickStart(lr.data(), n, ramp, 0.8f, 0.8f);
        double worst = std::fabs(lr[0] - 0.8f);
        for (size_t i = 1; i < n; ++i) worst = std::max(worst, (double)std::fabs(lr[2 * i] - lr[2 * i - 2]));
        std::printf("gap 0.8 -> -0.8 smoothed: largest step %.3f (a clean 300 Hz sine of 0.8 steps up to 0.031)\n", worst);
        CHECK(worst < 0.04, "declick left a step of %.3f", worst);
    }

    // 6. Surround (2.8.7): an 8-channel processor whose front pair carries a stereo sound and the rest silence gives
    //    exactly the stereo processor's output on the front pair, and silence elsewhere — in every mode.
    for (int mode = 0; mode < 3; ++mode)
    {
        vga::Processor p2, p8;
        p2.prepare(sr, 2);
        p8.prepare(sr, 8);
        vga::Params prm; prm.mode = mode; prm.amount = 70; prm.inputGainDb = 12; prm.ceilingDb = -2;
        p2.setParams(prm);
        p8.setParams(prm);
        std::mt19937 rng(5);
        std::normal_distribution<float> nd(0.0f, 0.3f);
        const int block = 480, blocks = 200;
        std::vector<float> l(block), r(block), ch8(8 * block);
        float* c8[8];
        for (int c = 0; c < 8; ++c) c8[c] = ch8.data() + (size_t)c * block;
        double diff = 0, rest = 0;
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                l[i] = nd(rng) * (b % 9 == 0 ? 3.0f : 1.0f);
                r[i] = 0.5f * nd(rng);
                for (int c = 0; c < 8; ++c) c8[c][i] = c == 0 ? l[i] : c == 1 ? r[i] : 0.0f;
            }
            p2.process(l.data(), r.data(), block);
            p8.process(c8, block);
            for (int i = 0; i < block; ++i)
            {
                diff = std::max(diff, (double)std::max(std::fabs(l[i] - c8[0][i]), std::fabs(r[i] - c8[1][i])));
                for (int c = 2; c < 8; ++c) rest = std::max(rest, (double)std::fabs(c8[c][i]));
            }
        }
        CHECK(diff == 0.0, "mode %d: 8-channel front pair differs from stereo by %g", mode, diff);
        CHECK(rest == 0.0, "mode %d: silent channels became %g", mode, rest);
    }

    // 7. Surround: loud sound on all 8 channels, +24 dB, every mode: no channel goes over the ceiling, and all
    //    channels get the same gain (a quiet rear channel is lowered exactly as much as the loud front).
    for (int mode = 0; mode < 3; ++mode)
    {
        vga::Processor p;
        p.prepare(sr, 8);
        vga::Params prm; prm.mode = mode; prm.amount = 100; prm.inputGainDb = 24; prm.ceilingDb = -3;
        p.setParams(prm);
        const int block = 333, blocks = 300;
        std::vector<float> buf(8 * block);
        float* c8[8];
        for (int c = 0; c < 8; ++c) c8[c] = buf.data() + (size_t)c * block;
        double peak = 0, ratioErr = 0;
        const float ceil = vgl::dbToGain(-3.0f);
        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                const double t = (double)(b * block + i) / sr;
                const float front = 0.8f * (float)std::sin(2 * 3.14159265358979 * 220 * t);
                for (int c = 0; c < 8; ++c) c8[c][i] = c == 7 ? 0.1f * front : front;   // rear right: same sound, 18 dB quieter
            }
            p.process(c8, block);
            for (int i = 0; i < block; ++i)
            {
                for (int c = 0; c < 8; ++c) peak = std::max(peak, (double)std::fabs(c8[c][i]));
                // away from zero crossings: there the Game mode's band filters round both channels a little
                // differently (up to ~3e-4 of the ratio, the same in the stereo code before 2.8.7)
                if (b > 20 && std::fabs(c8[0][i]) > 0.3f) ratioErr = std::max(ratioErr, std::fabs(c8[7][i] / c8[0][i] - 0.1));
            }
        }
        CHECK(peak <= ceil * 1.000001, "8 channels, mode %d: peak %.5f over the ceiling %.5f", mode, peak, ceil);
        CHECK(ratioErr < 1e-4, "8 channels, mode %d: the quiet channel got a different gain (ratio off by %g)", mode, ratioErr);
        CHECK(p.clampCount() == 0, "8 channels, mode %d: safety clamp acted %llu times", mode, (unsigned long long)p.clampCount());
    }
    std::printf("surround: 8-channel front pair bit-identical to stereo in all modes; ceiling and linked gain hold on 8 channels\n");

    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all processor checks passed\n");
    return 0;
}
