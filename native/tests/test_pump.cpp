// Offline checks for push mode (DriftResampler::produce + OutputPacer): every source packet goes straight
// into the output device's queue; the device takes a whole period at a time.
// Simulated: source packets (2.67 or 10 ms) on a drifting clock with jitter, a device pulling its period
// on its own clock, and our polling thread waking every ~1 ms and sometimes late.
// Build: g++ -O2 -std=c++17 -Isrc tests/test_pump.cpp
#include "Bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

static const double kPi = 3.14159265358979323846;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

struct Res
{
    int glitches = 0, lates = 0;
    double avgWaitMs = 0, maxWaitMs = 0, marginMs = 0, corrPpm = 0, snr = 0;
};

static double snrOf(const std::vector<float>& out)
{
    double sig = 0, noise = 0;
    for (size_t w0 = 0; w0 + 2048 <= out.size(); w0 += 2048)
    {
        const float* x = out.data() + w0;
        const int n = 2048;
        auto fitAt = [&](double f, double& s2, double& n2)
        {
            double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
            for (int i = 0; i < n; ++i)
            {
                const double ph = 2 * kPi * f * i, s = std::sin(ph), c = std::cos(ph);
                ss += s * s; sc += s * c; cc += c * c; ys += x[i] * s; yc += x[i] * c;
            }
            const double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
            s2 = n2 = 0;
            for (int i = 0; i < n; ++i)
            {
                const double fit = a * std::sin(2 * kPi * f * i) + b * std::cos(2 * kPi * f * i);
                s2 += fit * fit; n2 += (x[i] - fit) * (x[i] - fit);
            }
        };
        double lo = 1000.0 / 48000 * 0.995, hi = 1000.0 / 48000 * 1.005, s2, a, b;
        for (int it = 0; it < 40; ++it)
        {
            const double m1 = hi - 0.618 * (hi - lo), m2 = lo + 0.618 * (hi - lo);
            fitAt(m1, s2, a); fitAt(m2, s2, b);
            if (a < b) hi = m2; else lo = m1;
        }
        double n2;
        fitAt(0.5 * (lo + hi), s2, n2);
        sig += s2; noise += n2;
    }
    return 10 * std::log10(sig / (noise + 1e-30));
}

// capMs: source packet; perMs: device period; ppm: source clock vs device clock; lateProb: chance that a poll
// wakes up lateMs late (a busy system).
static Res run(double capMs, double perMs, double ppm, double seconds, unsigned seed, double lateProb = 0.002, double lateMs = 4.0,
               double stallFrom = -1, double stallTo = -1)
{
    const double rate = 48000;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    vga::FrameRing ring; ring.init(48000);
    vga::DriftResampler rs; rs.prepare(rate, rate, 20.0);
    vga::OutputPacer pacer;
    const int period = (int)std::lround(rate * perMs / 1000);
    const int capFrames = (int)std::lround(rate * capMs / 1000);
    pacer.prepare(rate, period, capFrames, 2.0 + std::min(capMs, 4.0));
    const double capReal = rate * (1 + ppm * 1e-6);
    std::deque<float> queue;                    // the device queue (left channel only, for the measurement)
    for (int i = 0; i < (int)pacer.marginFrames(); ++i) queue.push_back(0.0f);
    std::vector<float> lr((size_t)capFrames * 2), l(4096), r(4096), out;
    uint64_t capN = 0, pulls = 0, polls = 0, sample = 0;
    double tCap = capFrames / capReal, tPull = 0.0013 + period / rate, tPoll = 0.001;
    double lastPoll = 0, lastData = 0, waitSum = 0;
    long waitN = 0;
    Res res;
    bool produced = false;
    double quietUntil = 0;
    while (tPull < seconds)
    {
        const double t = std::min(tCap, std::min(tPull, tPoll));
        if (t == tCap)
        {
            if (!(t >= stallFrom && t < stallTo))
            {
                for (int i = 0; i < capFrames; ++i, ++sample)
                {
                    const float s = 0.5f * (float)std::sin(2 * kPi * 1000.0 * (double)sample / rate);
                    lr[2 * i] = s; lr[2 * i + 1] = s;
                }
                ring.write(lr.data(), (size_t)capFrames, t);
                lastData = t;
            }
            ++capN;
            tCap = (capN + 1) * capFrames / capReal + (u(rng) - 0.5) * 0.001;   // ±0.5 ms jitter
        }
        else if (t == tPull)
        {
            const bool warm = t > 3.0;
            if ((int)queue.size() < period && warm && !(t >= stallFrom && t < stallTo + 0.5)) ++res.glitches;
            for (int i = 0; i < period; ++i)
            {
                float v = 0.0f;
                if (!queue.empty()) { v = queue.front(); queue.pop_front(); }
                if (t > seconds - 1.0) out.push_back(v);
            }
            ++pulls;
            tPull = 0.0013 + (pulls + 1) * period / rate + (u(rng) - 0.5) * 0.0002;
        }
        else
        {
            // our thread: what the engine does every poll
            const double q = (double)queue.size();
            const bool stalled = t - lastData > 3.0 * capMs / 1000.0 + 0.002;
            // late = the device ran dry while audio was already waiting for us (not: the source sent nothing)
            if (q == 0 && produced && t - quietUntil > 0 && rs.pendingInput(ring) >= 1.0) { pacer.onLate(); ++res.lates; if (getenv("VGDBG")) std::printf("  late at %.3f q=%g pend=%.1f lastData=%.3f\n", t, q, rs.pendingInput(ring), lastData); }
            const double corr = pacer.update(q, t - lastPoll);
            lastPoll = t;
            if (pacer.excess() > 0.020 * rate)
            {
                // far too much queued (after a stall): drop input we have not queued yet
                const double drop = std::min(rs.pendingInput(ring), pacer.excess());
                if (drop > 0) { rs.skipInput(ring, (size_t)drop); pacer.settle(drop); }
            }
            const int room = 4096;
            const int n = rs.produce(ring, l.data(), r.data(), room, corr);
            for (int i = 0; i < n; ++i) queue.push_back(l[i]);
            if (stalled && produced)
            {
                // the source is silent: keep the device fed with silence (a full period + margin), and do not
                // judge our own timing until audio flows again for a moment
                while (queue.size() < pacer.marginFrames() + period) queue.push_back(0.0f);
                quietUntil = t + 0.3;
            }
            if (n > 0 && !produced) quietUntil = t + 1.0;   // start-up
            if (n > 0) produced = true;
            if (n > 0 && t > 3.0)
            {
                const double w = 1000.0 * (double)queue.size() / rate;   // the newest sample waits this long
                waitSum += w; ++waitN;
                res.maxWaitMs = std::max(res.maxWaitMs, w);
            }
            ++polls;
            const double late = u(rng) < lateProb ? lateMs / 1000.0 : u(rng) * 0.0004;
            tPoll = t + 0.001 + late;
        }
    }
    res.avgWaitMs = waitSum / std::max(1L, waitN);
    res.marginMs = pacer.marginMs();
    res.corrPpm = pacer.correction() * 1e6;
    res.snr = snrOf(out);
    return res;
}

int main()
{
    struct C { const char* name; double cap, per, ppm; };
    const C cases[] = {
        { "VB-CABLE 2.67 ms -> 10 ms", 128 / 48.0, 10.0, 0 },
        { "2.67 -> 10 ms, +400 ppm", 128 / 48.0, 10.0, 400 },
        { "2.67 -> 10 ms, -400 ppm", 128 / 48.0, 10.0, -400 },
        { "10 -> 10 ms, +200 ppm", 10.0, 10.0, 200 },
        { "10 -> 10 ms, -200 ppm", 10.0, 10.0, -200 },
        { "2.67 -> 2.67 ms, +300 ppm", 128 / 48.0, 128 / 48.0, 300 },
    };
    for (const C& c : cases)
    {
        Res r = run(c.cap, c.per, c.ppm, 300.0, 11);
        // latency of the path: source packet + wait in the device queue + 5 ms lookahead
        const double total = c.cap + r.avgWaitMs + 5.0;
        std::printf("%-28s glitches %d (late %d), queue wait avg %.1f / max %.1f ms, margin %.1f ms, corr %+.0f ppm, SNR %.1f dB -> path ~%.0f ms\n",
                    c.name, r.glitches, r.lates, r.avgWaitMs, r.maxWaitMs, r.marginMs, r.corrPpm, r.snr, total);
        CHECK(r.glitches <= 3, "%s: %d glitches", c.name, r.glitches);
        CHECK(std::fabs(r.corrPpm - c.ppm) < 150, "%s: correction %.0f ppm for %.0f ppm drift", c.name, r.corrPpm, c.ppm);
        CHECK(r.snr > 80, "%s: SNR %.1f dB", c.name, r.snr);
        CHECK(r.avgWaitMs < 0.5 * (c.per + c.cap) + 12.0, "%s: queue on average %.1f ms", c.name, r.avgWaitMs);
        CHECK(r.marginMs <= 8.0, "%s: margin grew to %.1f ms", c.name, r.marginMs);
    }
    // a busy system: 2 % of polls wake 6 ms late
    {
        Res r = run(128 / 48.0, 10.0, 250, 120.0, 5, 0.02, 6.0);
        std::printf("%-28s glitches %d (late %d), queue wait avg %.1f ms, margin %.1f ms\n", "busy system", r.glitches, r.lates, r.avgWaitMs, r.marginMs);
        CHECK(r.glitches <= 8, "busy: %d glitches", r.glitches);
    }
    // the source stops for 2 s (game paused, no keep-alive) and comes back
    {
        Res r = run(128 / 48.0, 10.0, 300, 60.0, 9, 0.002, 4.0, 20.0, 22.0);
        std::printf("%-28s glitches %d (late %d), queue wait avg %.1f ms, margin %.1f ms, SNR %.1f dB\n", "source stall 2 s", r.glitches, r.lates, r.avgWaitMs, r.marginMs, r.snr);
        CHECK(r.lates <= 2, "stall: %d late polls", r.lates);
        CHECK(r.avgWaitMs < 20.0, "queue after the stall %.1f ms", r.avgWaitMs);
        CHECK(r.snr > 80, "stall: SNR %.1f", r.snr);
    }
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("all push-mode checks passed\n");
    return 0;
}
