// Soak test: one simulated hour of push mode (source → DriftResampler::produce → OutputPacer → device queue,
// with SourceWatch telling stops from late lumps), with everything that happens in a long evening of play:
//   - the clocks drift and the drift itself wanders (±400 ppm, warming up)
//   - two 10-minute stretches where the source comes in lumps (20 ms holes)
//   - the game pauses its sound for 5 s every 7 minutes
//   - the computer sleeps for 30 s at 30:00 (nothing runs, then everything continues)
//   - our thread is 40 ms late once a minute, and 4 ms late now and then
//   - three times the whole program stops for 0.3 s (the game loads a level and takes every core)
// The device queue is as small as a real one (1056 frames, ~22 ms): what does not fit waits in the ring.
// Checked: nothing grows without bound (queue, ring, margin), the sound recovers after every event,
// few glitches outside the events, no NaN, clean sound at the end. Same code paths as Engine.cpp.
// Build: g++ -O2 -std=c++17 -Isrc tests/test_soak.cpp
#include "Bridge.hpp"

#include <cstdio>
#include <deque>
#include <random>
#include <vector>

static const double kPi = 3.14159265358979323846;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

int main()
{
    const double rate = 48000, hour = 3600.0;
    const double capMs = 128 / 48.0, perMs = 10.0;
    const int period = (int)std::lround(rate * perMs / 1000), capFrames = (int)std::lround(rate * capMs / 1000);
    std::mt19937 rng(2026);
    std::uniform_real_distribution<double> u(0, 1);

    auto ppmAt = [&](double t) { return 150.0 + 250.0 * std::sin(2 * kPi * t / 1500.0); };
    auto lumpy = [&](double t) { return (t >= 600 && t < 1200) || (t >= 2400 && t < 3000); };
    auto paused = [&](double t) { return std::fmod(t, 420.0) >= 400.0 && std::fmod(t, 420.0) < 405.0; };
    const double sleepAt = 1800.0, sleepFor = 30.0;
    // an event happened recently: glitches right after it are expected, not counted
    auto nearEvent = [&](double t) {
        const double m = std::fmod(t, 420.0);
        if (m >= 400.0 && m < 406.0) return true;                                  // pause + 1 s
        if (t >= sleepAt - 0.01 && t < sleepAt + sleepFor + 2.0) return true;      // sleep + 2 s
        for (double e : { 600.0, 1200.0, 2400.0, 3000.0 }) if (t >= e && t < e + 2.0) return true;   // lumps start/stop
        for (double e : { 900.0, 2000.0, 3300.0 }) if (t >= e && t < e + 3.0) return true;         // 0.3 s stalls
        if (t > 60.0 && std::fmod(t, 60.0) < 0.3) return true;                                     // 40 ms hiccups
        return false;
    };

    vga::FrameRing ring; ring.init((size_t)rate);
    vga::DriftResampler rs; rs.prepare(rate, rate, 20.0);
    vga::OutputPacer pacer; pacer.prepare(rate, period, capFrames, 2.0);
    vga::SourceWatch watch; watch.prepare(capMs / 1000.0);

    std::deque<float> queue;
    for (int i = 0; i < (int)pacer.targetFrames(); ++i) queue.push_back(0.0f);
    std::vector<float> lr((size_t)capFrames * 2), l(4096), r(4096), tail;
    uint64_t sample = 0, written = 0, lastWritten = 0;
    double srcIdeal = capFrames / rate;       // when the next packet is due on the source's own (drifting) clock
    double tCap = capFrames / rate, tPull = 0.0013 + period / rate, tPoll = 0.001, lastPoll = 0, quietUntil = 0;
    int held = 0, glitches = 0, glitchesNearEvents = 0, lateHiccups = 0;
    bool produced = false, eventPending = false, nan = false;
    double maxWaitLate = 0, waitSumLast = 0, maxRing = 0, maxTarget = 0, silenceMs = 0;
    long waitNLast = 0;
    double nextHiccup = 60.0;
    const int bufFrames = 1056;               // the device buffer: what does not fit stays in the ring
    std::vector<double> stalls = { 900.0, 2000.0, 3300.0 };
    size_t nextStall = 0;
    int drops = 0;
    double blockedUntil = 0;
    bool slept = false;

    while (tPull < hour)
    {
        double t = std::min(tCap, std::min(tPull, tPoll));
        // the computer sleeps: nothing runs for 30 s, then every clock continues from where it was
        if (!slept && t >= sleepAt)
        {
            slept = true;
            tCap += sleepFor; srcIdeal += sleepFor; tPull += sleepFor; tPoll += sleepFor; lastPoll += sleepFor;
            continue;
        }
        if (t == tCap)
        {
            const bool inHole = lumpy(t) && std::fmod(t, 0.057) < 0.020;
            if (inHole) ++held;
            else
            {
                for (int b = 0; b < 1 + held; ++b)
                {
                    if (paused(t)) continue;   // the game is quiet and there is no keep-alive: nothing comes
                    for (int i = 0; i < capFrames; ++i, ++sample)
                    {
                        const float s = 0.5f * (float)std::sin(2 * kPi * 997.0 * (double)sample / rate);
                        lr[2 * i] = s; lr[2 * i + 1] = s;
                    }
                    ring.write(lr.data(), (size_t)capFrames, t);
                    written += (uint64_t)capFrames;
                }
                held = 0;
            }
            const double srcRate = rate * (1 + ppmAt(t) * 1e-6);
            // the clock drifts, the delivery jitters around it (±0.25 ms, not adding up)
            srcIdeal += capFrames / srcRate;
            tCap = srcIdeal + (u(rng) - 0.5) * 0.0005;
            if (tCap <= t) tCap = t + 1e-5;
        }
        else if (t == tPull)
        {
            if ((int)queue.size() < period && t > 3.0)
            {
                if (nearEvent(t)) ++glitchesNearEvents; else ++glitches;
            }
            for (int i = 0; i < period; ++i)
            {
                float v = 0.0f;
                if (!queue.empty()) { v = queue.front(); queue.pop_front(); }
                if (!std::isfinite(v)) nan = true;
                if (t > hour - 1.0) tail.push_back(v);
            }
            tPull += period / rate + (u(rng) - 0.5) * 0.0002;
            eventPending = true;
            tPoll = std::max(blockedUntil, std::min(tPoll, t + 0.00005 + u(rng) * 0.0004));   // a stalled thread does not wake
        }
        else
        {
            const double q = (double)queue.size();
            if (written > lastWritten)
            {
                if (watch.onData(t, (double)(written - lastWritten) / rate, t - lastPoll)) pacer.setPacketFrames(watch.holeSec() * rate);
                lastWritten = written;
            }
            if (watch.holeSec() == 0.0) pacer.setPacketFrames(0.0);
            const bool stalled = watch.stalled(t);
            if (eventPending) { eventPending = false; pacer.onPull(q, produced && !stalled && t > quietUntil, t - lastPoll > 0.008); }
            const double corr = pacer.update(q + rs.pendingInput(ring), std::min(0.1, t - lastPoll));   // whole wait
            lastPoll = t;
            // as Engine.cpp: drop late input from the device queue's excess and from the ring's backlog
            {
                const double pending = rs.pendingInput(ring);
                double drop = 0.0;
                if (pacer.excess() > 0.020 * rate) { drop = std::min(pending, pacer.excess()); pacer.settle(drop); }
                if (pending - drop > pacer.maxTargetFrames()) drop = pending - pacer.maxTargetFrames();
                if (drop >= 1.0) { rs.skipInput(ring, (size_t)drop); ++drops; }
            }
            const int room = std::max(0, bufFrames - (int)queue.size());
            const int n = rs.produce(ring, l.data(), r.data(), std::min(4096, room), corr);
            for (int i = 0; i < n; ++i) queue.push_back(l[i]);
            if (n == 0 && stalled && produced)
            {
                while (queue.size() < pacer.floorFrames() + period)
                {
                    queue.push_back(0.0f);
                    if (!paused(t) && !nearEvent(t)) silenceMs += 1000.0 / rate;
                }
                quietUntil = t + 0.3;
            }
            if (n > 0 && !produced) { produced = true; quietUntil = t + 1.0; }
            maxRing = std::max(maxRing, (double)ring.fill() / rate * 1000.0);
            maxTarget = std::max(maxTarget, pacer.targetMs());
            const double w = 1000.0 * (queue.size() + rs.pendingInput(ring)) / rate;   // device queue + ring backlog
            if (t > 60.0 && !nearEvent(t)) maxWaitLate = std::max(maxWaitLate, w);
            if (t > hour - 600.0 && n > 0) { waitSumLast += w; ++waitNLast; }
            // our thread: usually ~1 ms, sometimes 4 ms late, once a minute 40 ms late (a hiccup)
            double next = t + 0.001 + (u(rng) < 0.002 ? 0.004 : u(rng) * 0.0004);
            if (t >= nextHiccup) { next += 0.040; nextHiccup += 60.0; ++lateHiccups; }
            if (nextStall < stalls.size() && t >= stalls[nextStall]) { next += 0.300; ++nextStall; }
            tPoll = next;
            blockedUntil = next;
        }
    }

    // clean sound at the end: fit the 997 Hz tone (frequency found by golden-section search) in 2048-sample windows
    double sig = 0, noise = 0;
    for (size_t w0 = 0; w0 + 2048 <= tail.size(); w0 += 2048)
    {
        auto fitAt = [&](double f, double& s2, double& e) {
            double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
            for (int i = 0; i < 2048; ++i)
            {
                const double ph = 2 * kPi * f * i, s = std::sin(ph), c = std::cos(ph), y = tail[w0 + i];
                ss += s * s; sc += s * c; cc += c * c; ys += y * s; yc += y * c;
            }
            const double det = ss * cc - sc * sc, a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
            s2 = e = 0;
            for (int i = 0; i < 2048; ++i)
            {
                const double fit = a * std::sin(2 * kPi * f * i) + b * std::cos(2 * kPi * f * i);
                e += (tail[w0 + i] - fit) * (tail[w0 + i] - fit); s2 += fit * fit;
            }
        };
        double lo = 997.0 / rate * 0.995, hi = 997.0 / rate * 1.005, s2, e1, e2;
        for (int it = 0; it < 40; ++it)
        {
            const double m1 = hi - 0.618 * (hi - lo), m2 = lo + 0.618 * (hi - lo);
            fitAt(m1, s2, e1); fitAt(m2, s2, e2);
            if (e1 < e2) hi = m2; else lo = m1;
        }
        double e;
        fitAt(0.5 * (lo + hi), s2, e);
        sig += s2; noise += e;
    }
    const double snr = 10 * std::log10(sig / (noise + 1e-30));
    const double avgLast = waitSumLast / std::max(1L, waitNLast);

    std::printf("late input dropped %d times\n", drops);
    std::printf("1 h soak: glitches %d (+%d right after events), silence put in while only late %.0f ms, %d hiccups of 40 ms\n",
                glitches, glitchesNearEvents, silenceMs, lateHiccups);
    std::printf("          queue max %.1f ms (outside events), last 10 min avg %.1f ms, ring max %.1f ms, margin max %.1f ms, "
                "lumps %llu, SNR at the end %.1f dB\n", maxWaitLate, avgLast, maxRing, maxTarget,
                (unsigned long long)watch.lumps(), snr);
    CHECK(!nan, "NaN in the output");
    CHECK(glitches <= 60, "%d glitches outside the events in an hour", glitches);   // 40 ms hiccups may cost one each
    CHECK(silenceMs < 200.0, "%.0f ms of silence put in while the source was only late", silenceMs);
    CHECK(maxWaitLate < 80.0, "queue reached %.1f ms", maxWaitLate);   // in the lumpy stretches: a 20 ms hole on top of the margin
    CHECK(avgLast < 30.0, "queue at the end %.1f ms on average: it kept growing", avgLast);
    CHECK(maxRing < 200.0, "ring reached %.1f ms", maxRing);
    CHECK(maxTarget <= 60.0 + 1e-6, "margin reached %.1f ms", maxTarget);
    // at the end the margin is still being given back after the lumps (the speed is ~0.05 % off for a while):
    // the fit of one fixed frequency per 43 ms counts that gentle pitch glide as noise. ~69 dB here, 88+ steady.
    CHECK(snr > 65.0, "SNR at the end %.1f dB", snr);
    if (failures) { std::printf("%d check(s) failed\n", failures); return 1; }
    std::printf("soak test passed\n");
    return 0;
}
