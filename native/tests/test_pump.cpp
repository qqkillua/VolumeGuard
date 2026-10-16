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
    double silenceMs = 0;
    uint64_t lumps = 0;
    double holeMs = 0;   // silence put into the output while the source was only late (cuts the sound)
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

// capMs: source packet; perMs: device period; ppm: source clock vs device clock; lateProb: chance that our
// thread wakes up lateMs late (a busy system); burst: the source delivers this many packets at once.
static Res run(double capMs, double perMs, double ppm, double seconds, unsigned seed, double lateProb = 0.002, double lateMs = 4.0,
               double stallFrom = -1, double stallTo = -1, int burst = 1, double holeEvery = 0, double holeMs = 0)
{
    const double rate = 48000;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    vga::FrameRing ring; ring.init(48000);
    vga::DriftResampler rs; rs.prepare(rate, rate, 20.0);
    vga::OutputPacer pacer;
    const int period = (int)std::lround(rate * perMs / 1000);
    const int capFrames = (int)std::lround(rate * capMs / 1000);
    pacer.prepare(rate, period, capFrames * burst, 2.0);
    vga::SourceWatch watch;
    watch.prepare(capMs / 1000.0);

    const double capReal = rate * (1 + ppm * 1e-6);
    std::deque<float> queue;                    // the device queue (left channel only, for the measurement)
    for (int i = 0; i < (int)pacer.targetFrames(); ++i) queue.push_back(0.0f);
    std::vector<float> lr((size_t)capFrames * 2), l(4096), r(4096), out;
    uint64_t capN = 0, pulls = 0, sample = 0;
    double tCap = burst * capFrames / capReal, tPull = 0.0013 + period / rate, tPoll = 0.001;
    double lastPoll = 0, waitSum = 0, quietUntil = 0;
    long waitN = 0;
    int held = 0;                               // packets the source holds back during a hole
    Res res;
    bool produced = false, eventPending = false;
    uint64_t written = 0, lastWritten = 0;   // frames the source has delivered
    auto wake = [&](double base) {
        const double late = u(rng) < lateProb ? lateMs / 1000.0 : u(rng) * 0.0004;
        return base + late;
    };
    while (tPull < seconds)
    {
        const double t = std::min(tCap, std::min(tPull, tPoll));
        if (t == tCap)
        {
            // holes: every holeEvery s the source sends nothing for holeMs, then everything it held at once
            const bool inHole = holeEvery > 0 && std::fmod(t, holeEvery) < holeMs / 1000.0;
            if (inHole) held += burst;
            for (int b = 0; b < (inHole ? 0 : burst + held); ++b)
            {
                if (!(t >= stallFrom && t < stallTo))
                {
                    for (int i = 0; i < capFrames; ++i, ++sample)
                    {
                        const float s = 0.5f * (float)std::sin(2 * kPi * 1000.0 * (double)sample / rate);
                        lr[2 * i] = s; lr[2 * i + 1] = s;
                    }
                    ring.write(lr.data(), (size_t)capFrames, t);
                    written += (uint64_t)capFrames;
                }
            }
            if (!inHole) held = 0;
            capN += burst;
            tCap = (capN + burst) * capFrames / capReal + (u(rng) - 0.5) * 0.001;   // ±0.5 ms jitter
        }
        else if (t == tPull)
        {
            const bool warm = t > 3.0 && !(t >= stallFrom && t < stallTo + 0.5);
            if ((int)queue.size() < period && warm) ++res.glitches;
            for (int i = 0; i < period; ++i)
            {
                float v = 0.0f;
                if (!queue.empty()) { v = queue.front(); queue.pop_front(); }
                if (t > seconds - 1.0) out.push_back(v);
            }
            ++pulls;
            tPull = 0.0013 + (pulls + 1) * period / rate + (u(rng) - 0.5) * 0.0002;
            // the device signals its event: our thread wakes up (usually within a fraction of a millisecond)
            eventPending = true;
            tPoll = std::min(tPoll, wake(t + 0.00005));
        }
        else
        {
            // our thread: what the engine does every poll
            const double q = (double)queue.size();
            // what arrived since the last poll (the engine reads it from the capture client here)
            if (written > lastWritten)
            {
                if (watch.onData(t, (double)(written - lastWritten) / rate))
                    pacer.setPacketFrames(watch.holeSec() * rate);
                lastWritten = written;
            }
            const bool stalled = watch.stalled(t);
            if (eventPending)
            {
                eventPending = false;
                if (pacer.onPull(q, produced && !stalled && t > quietUntil)) ++res.lates;
            }
            const double corr = pacer.update(q, t - lastPoll);
            lastPoll = t;
            if (pacer.excess() > 0.020 * rate)
            {
                const double drop = std::min(rs.pendingInput(ring), pacer.excess());
                if (drop > 0) { rs.skipInput(ring, (size_t)drop); pacer.settle(drop); }
            }
            const int n = rs.produce(ring, l.data(), r.data(), 4096, corr);
            for (int i = 0; i < n; ++i) queue.push_back(l[i]);
            if (stalled && produced)
            {
                const bool reallyStopped = t >= stallFrom && t < stallTo + 0.1;
                while (queue.size() < pacer.floorFrames() + period)
                {
                    queue.push_back(0.0f);
                    if (!reallyStopped && t > 3.0) res.silenceMs += 1000.0 / rate;
                }
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
            tPoll = wake(t + 0.001);
        }
    }
    res.avgWaitMs = waitSum / std::max(1L, waitN);
    res.lumps = watch.lumps();
    res.holeMs = watch.holeSec() * 1000.0;
    if (std::getenv("PUMP_DEBUG")) std::printf("   [lumps %llu, hole %.1f ms]\n", (unsigned long long)res.lumps, res.holeMs);
    res.marginMs = pacer.targetMs();
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
        std::printf("%-28s glitches %d (seen %d), queue wait avg %.1f / max %.1f ms, target %.1f ms, corr %+.0f ppm, SNR %.1f dB -> path ~%.0f ms\n",
                    c.name, r.glitches, r.lates, r.avgWaitMs, r.maxWaitMs, r.marginMs, r.corrPpm, r.snr, total);
        CHECK(r.glitches <= 3, "%s: %d glitches", c.name, r.glitches);
        CHECK(r.silenceMs < 20.0, "%s: %.0f ms of silence put in", c.name, r.silenceMs);
        CHECK(std::fabs(r.corrPpm - c.ppm) < 150, "%s: correction %.0f ppm for %.0f ppm drift", c.name, r.corrPpm, c.ppm);
        CHECK(r.snr > 80, "%s: SNR %.1f dB", c.name, r.snr);
        CHECK(r.avgWaitMs < 0.5 * (c.per + c.cap) + 12.0, "%s: queue on average %.1f ms", c.name, r.avgWaitMs);

    }
    // the source delivers its 2.67 ms packets in bursts of four (every ~10.7 ms): the margin must adapt
    {
        Res r = run(128 / 48.0, 10.0, 150, 120.0, 4, 0.002, 4.0, -1, -1, 4);
        std::printf("%-28s glitches %d (seen %d), queue wait avg %.1f ms, target %.1f ms, SNR %.1f dB\n", "bursts of 4 packets", r.glitches, r.lates, r.avgWaitMs, r.marginMs, r.snr);
        CHECK(r.glitches <= 4, "bursts: %d glitches", r.glitches);
        CHECK(r.snr > 80, "bursts: SNR %.1f", r.snr);
    }
    // what a laptop showed (2.7.1, Realtek 2.67 ms output): VB-CABLE packets of 2.67 ms, but every ~57 ms
    // nothing for ~20 ms, then all of it at once. The margin must grow to cover the hole, without silence.
    {
        Res r = run(128 / 48.0, 128 / 48.0, 100, 120.0, 7, 0.002, 4.0, -1, -1, 1, 0.057, 20.0);
        std::printf("%-28s glitches %d (seen %d), silence put in %.0f ms, queue wait avg %.1f ms, target %.1f ms, SNR %.1f dB\n", "20 ms holes every 57 ms", r.glitches, r.lates, r.silenceMs, r.avgWaitMs, r.marginMs, r.snr);
        CHECK(r.glitches <= 4, "holes: %d glitches", r.glitches);
        CHECK(r.silenceMs < 100.0, "holes: %.0f ms of silence put in while the source was only late", r.silenceMs);
        // the saw-tooth of the queue makes the speed correction move a little more: ~75 dB instead of ~88,
        // still far below hearing. Before 2.7.2 silence was put into every hole: ~14 dB.
        CHECK(r.snr > 70, "holes: SNR %.1f", r.snr);
        CHECK(r.avgWaitMs < 30.0, "holes: queue on average %.1f ms", r.avgWaitMs);
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
