// VolumeGuard audio engine — the bridge between two sound devices.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// The game plays into one device ("source"), we listen on another ("output"). The two devices run on
// their own clocks, which never match exactly (a difference of 0.01–0.1 % is normal). Without correction
// the buffer between them slowly runs dry or overflows, which is heard as clicks every few minutes.
//
//   capture thread ──► FrameRing (lock-free) ──► DriftResampler ──► DSP ──► output device
//
// DriftResampler reads the ring a tiny bit faster or slower (at most ±0.5 %, not audible) so that the
// amount of buffered audio stays around a target. It also converts between sample rates (e.g. 44.1 → 48 kHz).
// Everything here is plain C++ without Windows, so it is tested offline (tests/test_bridge.cpp).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vga {

/// The most channels the engine processes (7.1). More are folded down to stereo, as before 2.8.7.
constexpr int kMaxChannels = 8;

/// Single-producer / single-consumer ring of interleaved frames (stereo, or up to kMaxChannels).
class FrameRing
{
public:
    void init(size_t capacityFrames, int channels = 2)
    {
        ch_ = std::max(1, std::min(kMaxChannels, channels));
        cap_ = std::max<size_t>(capacityFrames, 16);
        buf_.assign(cap_ * (size_t)ch_, 0.0f);
        w_.store(0); r_.store(0);
    }

    size_t capacity() const { return cap_; }
    int channels() const { return ch_; }
    size_t fill() const { return (size_t)(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire)); }

    /// Producer: `frames` interleaved frames of channels() samples. Returns frames written; what does not fit is
    /// dropped. nowSec: when the packet arrived (any monotonic clock shared with the reader).
    size_t write(const float* x, size_t frames, double nowSec = 0.0)
    {
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const size_t room = cap_ - (size_t)(w - r_.load(std::memory_order_acquire));
        const size_t n = std::min(frames, room);
        for (size_t i = 0; i < n; ++i)
        {
            const size_t k = (size_t)((w + i) % cap_) * (size_t)ch_;
            for (int c = 0; c < ch_; ++c) buf_[k + (size_t)c] = x[i * (size_t)ch_ + (size_t)c];
        }
        lastWrite_.store(nowSec, std::memory_order_release);
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    double lastWriteSec() const { return lastWrite_.load(std::memory_order_acquire); }

    /// Consumer: frame at an absolute index in [readPos, writePos).
    inline float at(uint64_t frame, int ch) const { return buf_[(size_t)(frame % cap_) * (size_t)ch_ + (size_t)ch]; }
    inline const float* frame(uint64_t frame) const { return &buf_[(size_t)(frame % cap_) * (size_t)ch_]; }
    uint64_t readPos() const { return r_.load(std::memory_order_relaxed); }
    uint64_t writePos() const { return w_.load(std::memory_order_acquire); }
    void advance(size_t frames) { r_.store(r_.load(std::memory_order_relaxed) + frames, std::memory_order_release); }

private:
    std::vector<float> buf_;
    size_t cap_ = 0;
    int ch_ = 2;
    std::atomic<uint64_t> w_{ 0 }, r_{ 0 };
    std::atomic<double> lastWrite_{ 0.0 };
};

/// Reads the ring at outRate, following inRate × a small correction that keeps the fill near the target.
class DriftResampler
{
public:
    /// targetMs: starting buffer size. It grows by 5 ms after each underrun caused by uneven packets
    /// (up to maxMs) and slowly shrinks back towards minMs while everything runs smoothly.
    void prepare(double inRate, double outRate, double targetMs, double minMs = 10.0, double maxMs = 80.0)
    {
        inRate_ = inRate;
        outRate_ = outRate;
        nominal_ = inRate / outRate;
        buildSincTable(std::min(1.0, outRate / inRate) * 0.92);
        minMs_ = minMs;
        maxMs_ = maxMs;
        setTargetMs(targetMs);
        reset();
    }

    void setTargetMs(double ms)
    {
        targetMs_ = std::max(2.0, ms);
        target_ = targetMs_ * inRate_ / 1000.0;
    }

    void reset()
    {
        running_ = false;
        afterUnderrun_ = false;
        silentSec_ = stableSec_ = 0.0;
        minFill_ = 1e18;
        frac_ = kHalf;
        corr_ = integ_ = 0.0;
        fillSmooth_ = target_;
    }

    /// Fills l/r with `frames` output frames. Returns how many of them are real audio (the rest is silence
    /// while the buffer refills after a gap). nowSec: current time on the same clock the writer uses.
    int read(FrameRing& ring, float* l, float* r, int frames, double nowSec)
    {
        const size_t fill = ring.fill();

        const double dt = frames / outRate_;

        if (!running_)
        {
            if ((double)fill >= target_)
            {
                // A short refill means the buffer was simply too small for how unevenly packets arrive:
                // make it bigger. A long one was a pause in the game's sound: nothing to fix.
                if (afterUnderrun_ && silentSec_ < 0.25)
                    setTargetMs(std::min(maxMs_, targetMs_ + 5.0));
                afterUnderrun_ = false;
                running_ = true;
                frac_ = kHalf;               // keep kHalf older frames for the interpolation
                // start the measurement the same way it is taken later; keep the integral: after a pause
                // the two clocks still differ by the same amount
                // Audio arrives in 10 ms packets, so by the time the target is reached up to a packet more is
                // buffered. Start exactly at the target: skip the excess (these are the first milliseconds
                // after silence) instead of letting the controller drain it, which made it overshoot.
                const double since = std::max(0.0, std::min(0.05, nowSec - ring.lastWriteSec()));
                const double excess = (double)fill + since * inRate_ - target_;
                if (excess > 0 && (double)fill - excess > kHalf * 2)
                    ring.advance((size_t)excess);
                fillSmooth_ = target_;
                stableSec_ = 0.0;
                minFill_ = 1e18;
            }
            else
            {
                silentSec_ += dt;
                std::fill(l, l + frames, 0.0f);
                std::fill(r, r + frames, 0.0f);
                return 0;
            }
        }

        // Lower the latency only when the buffer clearly never needed all of it: if over 30 s the
        // fill never dropped below 8 ms, give back 1 ms. (Never probe until it breaks: that would click.)
        stableSec_ += dt;
        if (stableSec_ > 30.0)
        {
            const double minFillMs = minFill_ * 1000.0 / inRate_;
            if (minFillMs > 8.0 && targetMs_ > minMs_)
                setTargetMs(std::max(minMs_, targetMs_ - 1.0));
            stableSec_ = 0.0;
            minFill_ = 1e18;
        }

        // far too much buffered (e.g. the output device stalled): drop the oldest audio
        if ((double)fill > target_ * 2.0 + 0.025 * inRate_)
        {
            const size_t drop = fill - (size_t)target_;
            ring.advance(drop);
            ++drops_;
            fillSmooth_ = target_;
            integ_ = 0.0;
        }

        // PI controller on the buffer fill. Audio arrives in packets, so the raw fill is a saw-tooth; looked at
        // only when the output asks for audio it beats slowly between the two devices' packet clocks. Adding
        // what would have arrived since the last packet makes the measurement smooth.
        const double sinceWrite = std::max(0.0, std::min(0.05, nowSec - ring.lastWriteSec()));
        const double fillNow = (double)ring.fill() + sinceWrite * inRate_;
        fillSmooth_ += (std::min(fillNow, target_ * 4.0) - fillSmooth_) * std::min(1.0, dt / 2.0);
        const double err = (fillSmooth_ - target_) / inRate_;          // seconds of audio too much (+) / too little (-)
        // anti-windup: the integral only moves while the output is not pinned at its limit
        const double unclamped = kP * err + kI * (integ_ + err * dt);
        if (std::fabs(unclamped) < kMaxCorrection)
            integ_ = std::max(-kMaxCorrection / kI, std::min(kMaxCorrection / kI, integ_ + err * dt));
        corr_ = std::max(-kMaxCorrection, std::min(kMaxCorrection, kP * err + kI * integ_));
        const double step = nominal_ * (1.0 + corr_);

        const uint64_t base = ring.readPos();
        const uint64_t avail = ring.writePos();
        int i = 0;
        for (; i < frames; ++i)
        {
            const double p = frac_;
            const uint64_t i0 = base + (uint64_t)p;          // p ≥ kHalf, so i0 - (kHalf-1) ≥ base
            if (i0 + kHalf >= avail) break;                  // not enough audio: underrun
            const double t = p - std::floor(p);
            interpolate(ring, i0, t, l[i], r[i]);
            frac_ += step;
        }

        // consume what is behind us, keeping kHalf frames of history
        const double whole = std::floor(frac_) - (double)kHalf;
        if (whole > 0)
        {
            ring.advance((size_t)whole);
            frac_ -= whole;
        }
        minFill_ = std::min(minFill_, (double)ring.fill());

        if (i < frames)
        {
            std::fill(l + i, l + frames, 0.0f);
            std::fill(r + i, r + frames, 0.0f);
            ++underruns_;
            running_ = false;               // wait until the target is buffered again
            afterUnderrun_ = true;
            silentSec_ = 0.0;
        }
        return i;
    }

    /// Push mode (with OutputPacer): turns everything that has arrived into output frames, at most maxFrames,
    /// reading the input `corr` faster (+) or slower (-) than nominal. Keeps no buffer of its own beyond the
    /// kHalf frames the interpolation needs. Returns the number of frames written.
    int produce(FrameRing& ring, float* l, float* r, int maxFrames, double corr)
    {
        float* out[2] = { l, r };
        return produce(ring, out, maxFrames, corr);
    }

    /// The same for any channel count: out[c] for each of ring.channels() channels.
    int produce(FrameRing& ring, float* const* out, int maxFrames, double corr)
    {
        corr_ = std::max(-kMaxCorrection, std::min(kMaxCorrection, corr));
        const double step = nominal_ * (1.0 + corr_);
        const uint64_t base = ring.readPos();
        const uint64_t avail = ring.writePos();
        const int nch = ring.channels();
        float v[kMaxChannels];
        int i = 0;
        for (; i < maxFrames; ++i)
        {
            const uint64_t i0 = base + (uint64_t)frac_;
            if (i0 + kHalf >= avail) break;
            interpolate(ring, i0, frac_ - std::floor(frac_), v);
            for (int c = 0; c < nch; ++c) out[c][i] = v[c];
            frac_ += step;
        }
        const double whole = std::floor(frac_) - (double)kHalf;
        if (whole > 0)
        {
            ring.advance((size_t)whole);
            frac_ -= whole;
        }
        return i;
    }

    /// Input frames that produce() could still turn into output right now.
    double pendingInput(const FrameRing& ring) const
    {
        return std::max(0.0, (double)(ring.writePos() - ring.readPos()) - frac_ - kHalf);
    }

    /// Push mode: throw away input that has piled up (after a stall of the output).
    void skipInput(FrameRing& ring, size_t frames)
    {
        const size_t keep = (size_t)kHalf * 2;
        const size_t have = ring.fill();
        if (have <= keep) return;
        ring.advance(std::min(frames, have - keep));
    }

    double correction() const { return corr_; }
    double targetMs() const { return targetMs_; }
    double fillMs() const { return fillSmooth_ * 1000.0 / inRate_; }
    uint64_t underruns() const { return underruns_; }
    uint64_t drops() const { return drops_; }
    bool running() const { return running_; }

    static constexpr double kMaxCorrection = 0.005;   // ±0.5 %: ~8.6 cents, not audible
    // The buffer error e (seconds) obeys e' = drift - correction, so the loop is s² + kP·s + kI = 0:
    // natural frequency 0.3 rad/s, damping 0.9 — settles in ~15 s without overshooting.
    static constexpr double kP = 0.54;
    static constexpr double kI = 0.09;

    static constexpr int kHalf = 16;                    // 32-tap windowed sinc
    static constexpr int kPhases = 512;

    /// The interpolated value at an absolute fractional frame position (for tests).
    void valueAt(const FrameRing& ring, double pos, float& l, float& r) const
    {
        const uint64_t i0 = (uint64_t)pos;
        interpolate(ring, i0, pos - (double)i0, l, r);
    }

private:
    // Kaiser-windowed sinc, kPhases fractional positions (+1 for interpolation between them).
    // cutoff < 1 also low-passes when converting down (e.g. 96 → 48 kHz) so nothing folds back.
    void buildSincTable(double cutoff)
    {
        const double beta = 8.6, pi = 3.14159265358979323846;
        auto bessel0 = [](double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; };
        const double i0beta = bessel0(beta);
        table_.assign((size_t)(kPhases + 1) * (2 * kHalf), 0.0f);
        for (int ph = 0; ph <= kPhases; ++ph)
        {
            const double t = (double)ph / kPhases;
            double sum = 0;
            for (int k = 0; k < 2 * kHalf; ++k)
            {
                const double x = (double)(k - (kHalf - 1)) - t;      // tap k sits at offset k-(kHalf-1)
                const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(pi * cutoff * x) / (pi * cutoff * x);
                const double u = x / kHalf;
                const double w = std::fabs(u) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - u * u)) / i0beta;
                const double v = cutoff * sinc * w;
                table_[(size_t)ph * 2 * kHalf + k] = (float)v;
                sum += v;
            }
            for (int k = 0; k < 2 * kHalf; ++k) table_[(size_t)ph * 2 * kHalf + k] /= (float)sum;   // unity gain
        }
    }

    inline void interpolate(const FrameRing& ring, uint64_t i0, double t, float& outL, float& outR) const
    {
        float v[kMaxChannels];
        interpolate(ring, i0, t, v);
        outL = v[0];
        outR = v[ring.channels() > 1 ? 1 : 0];
    }

    /// Every channel of the ring at once (the same sums, in the same order, as the stereo version had).
    inline void interpolate(const FrameRing& ring, uint64_t i0, double t, float* out) const
    {
        const double fp = t * kPhases;
        const int ph = std::min(kPhases - 1, (int)fp);
        const float a = (float)(fp - ph);
        const float* c0 = &table_[(size_t)ph * 2 * kHalf];
        const float* c1 = c0 + 2 * kHalf;
        const int nch = ring.channels();
        float s[kMaxChannels] = {};
        const uint64_t first = i0 - (kHalf - 1);
        for (int k = 0; k < 2 * kHalf; ++k)
        {
            const float c = c0[k] + (c1[k] - c0[k]) * a;
            const float* f = ring.frame(first + (uint64_t)k);
            for (int ch = 0; ch < nch; ++ch) s[ch] += c * f[ch];
        }
        for (int ch = 0; ch < nch; ++ch) out[ch] = s[ch];
    }

    std::vector<float> table_;

    double inRate_ = 48000, outRate_ = 48000, nominal_ = 1, targetMs_ = 12, target_ = 576;
    double frac_ = 1, corr_ = 0, integ_ = 0, fillSmooth_ = 0;
    double minMs_ = 10, maxMs_ = 80, silentSec_ = 0, stableSec_ = 0, minFill_ = 1e18;
    bool running_ = false, afterUnderrun_ = false;
    uint64_t underruns_ = 0, drops_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
/// Push mode pacing. Every sound packet from the source is converted and queued in the output device at once,
/// so there is only ONE buffer (the device queue) instead of a ring + a device queue. The device takes a whole
/// period (e.g. 10 ms) at a time and packets arrive whole, so the queue is a saw-tooth. Two loops:
///  - fast and smooth: the time-average of the queue is held at `target` by reading the input a tiny bit
///    faster or slower (clock drift), like before;
///  - safety: right after each pull of the device (its event) the queue is at its lowest. That low point must
///    stay above `floor`. If it dips below, or the queue is found empty (a glitch: the device got less than a
///    period), the target goes up; if for 20 s it never came near the floor, the target comes down by 0.5 ms.
///    So the margin fits however unevenly the packets really arrive.
class OutputPacer
{
public:
    void prepare(double outRate, double periodFrames, double packetFrames, double floorMs, double maxTargetMs = 60.0)
    {
        rate_ = outRate;
        period_ = std::max(1.0, periodFrames);
        packet_ = basePacket_ = std::max(0.0, packetFrames);
        floor_ = floorMs * outRate / 1000.0;
        target_ = floor_ + 0.5 * (period_ + packet_) + 0.001 * outRate;
        // never below what the worst moment needs: a whole packet arriving just after the device took its period
        minTarget_ = floor_ + 0.5 * (period_ + packet_);
        maxTarget_ = maxTargetMs * outRate / 1000.0;
        window_ = 0.1;
        reset();
    }

    void reset()
    {
        integ_ = corr_ = want_ = ramp_ = giveBack_ = 0.0;
        sumQ_ = sumT_ = 0.0;
        smooth_ = target_;
        primed_ = false;
        lowSince_ = 1e18;
        lowSec_ = 0.0;
        lastRaise_ = -1e9;
        clock_ = 0.0;
        glitches_ = dips_ = 0;
    }

    /// Every poll: queueFrames = the device queue BEFORE new audio is added; dt = seconds since the last poll.
    /// Returns the correction for DriftResampler::produce.
    double update(double queueFrames, double dt)
    {
        clock_ += dt;
        sumQ_ += queueFrames * dt;
        sumT_ += dt;
        if (sumT_ >= window_)
        {
            const double d = sumT_, avg = sumQ_ / sumT_;
            sumQ_ = sumT_ = 0.0;
            if (!primed_) { primed_ = true; smooth_ = avg; }
            smooth_ += (avg - smooth_) * std::min(1.0, d / 2.0);
            const double err = (smooth_ - target_) / rate_;       // seconds of output too much (+) / too little (-)
            const double unclamped = kP * err + kI * (integ_ + err * d);
            if (std::fabs(unclamped) < kMax)
                integ_ = std::max(-kMax / kI, std::min(kMax / kI, integ_ + err * d));
            want_ = std::max(-kMax, std::min(kMax, kP * err + kI * integ_));
            ramp_ = std::fabs(want_ - corr_) / window_;   // get there within one window, in a straight line

            lowSec_ += d;
            if (lowSec_ > 20.0)
            {
                // 20 s in which the queue never came near the floor: the margin is more than needed. Give back
                // a quarter of what was never used, 0.5…1 ms at a time. Slowly on purpose: with two clocks
                // drifting apart, the worst moment (a packet just missing a period) comes round only every few
                // minutes, so 20 calm seconds do not prove much (2.8.0 gave back up to 2 ms and then ran dry).
                // The margin glides down (kGiveBackPerSec), it does not step: a step every 20 s, about the
                // period of the speed control itself, kept it swinging by up to ±0.4 % (heard as a slight warble).
                const double unused = lowSince_ - floor_ - 0.002 * rate_;
                if (unused > 0.0 && target_ > minTarget_)
                    giveBack_ += std::min(0.001 * rate_, std::max(0.0005 * rate_, 0.25 * unused));
                lowSince_ = 1e18;
                lowSec_ = 0.0;
            }
        }
        if (giveBack_ > 0.0)
        {
            const double g = std::min(giveBack_, kGiveBackPerSec * rate_ * dt);
            target_ = std::max(minTarget_, target_ - g);
            giveBack_ = target_ > minTarget_ ? giveBack_ - g : 0.0;
        }
        // the speed glides to the wanted one within a window (every poll) instead of jumping once per window:
        // the pitch bends smoothly. (2.8.0 limited it to 0.3 %/s: that lag made the control swing to ±0.5 %.)
        const double step = ramp_ * dt;
        corr_ += std::max(-step, std::min(step, want_ - corr_));
        return corr_;
    }

    /// Right after the device took its period (its event): the queue is at its low point. judge = false while
    /// the source is silent or just started (then a low queue is not our fault).
    /// Returns true if this was a glitch (the device found the queue empty).
    /// ourThreadLate: our thread did not run for a while before this (the system was busy): a glitch then is
    /// counted but does not make the margin bigger — a bigger queue would not have helped against that.
    bool onPull(double queueFrames, bool judge, bool ourThreadLate = false)
    {
        if (!judge || !primed_) return false;
        if (ourThreadLate)
        {
            if (queueFrames <= 0.0) { ++glitches_; return true; }
            return false;
        }
        lowSince_ = std::min(lowSince_, queueFrames);
        if (queueFrames <= 0.0)
        {
            ++glitches_;
            raise(0.002);
            return true;
        }
        if (queueFrames < floor_ && clock_ - lastRaise_ > 0.5)
        {
            ++dips_;
            raise(0.0005);
        }
        return false;
    }

    /// The source delivers in bigger lumps than its packet size (see SourceWatch): the queue must cover a whole
    /// lump. Grows the target at once; when the lumps are over (0 or less than the packet), only the lowest
    /// allowed target comes back down, and the 20 s check lets the margin glide down.
    void setPacketFrames(double packetFrames)
    {
        packetFrames = std::max(basePacket_, packetFrames);
        if (packetFrames < packet_)
        {
            // the lumps are over: the floor of the margin comes down again (the margin itself glides down)
            packet_ = packetFrames;
            minTarget_ = floor_ + 0.5 * (period_ + packet_);
            return;
        }
        if (packetFrames == packet_) return;
        packet_ = packetFrames;
        minTarget_ = floor_ + 0.5 * (period_ + packet_);
        target_ = std::min(maxTarget_, std::max(target_, floor_ + 0.5 * (period_ + packet_) + 0.001 * rate_));
        lowSince_ = 1e18;
        lowSec_ = 0.0;
    }
    double packetFrames() const { return packet_; }

    /// How far the average queue is above what it should be (frames).
    double excess() const { return primed_ ? smooth_ - target_ : 0.0; }
    double targetFrames() const { return target_; }
    double targetMs() const { return target_ * 1000.0 / rate_; }
    double maxTargetFrames() const { return maxTarget_; }
    double floorFrames() const { return floor_; }
    double correction() const { return corr_; }
    uint64_t glitches() const { return glitches_; }
    uint64_t dips() const { return dips_; }
    void settle(double removedFrames) { smooth_ -= removedFrames; integ_ = 0.0; }

    static constexpr double kMax = 0.005;
    static constexpr double kP = 0.54, kI = 0.09;
    static constexpr double kGiveBackPerSec = 0.00005;   // the margin shrinks by at most 0.05 ms per second

private:
    void raise(double sec)
    {
        giveBack_ = 0.0;
        target_ = std::min(maxTarget_, target_ + sec * rate_);
        lastRaise_ = clock_;
        lowSince_ = 1e18;
        lowSec_ = 0.0;
    }

    double basePacket_ = 128;
    double rate_ = 48000, period_ = 480, packet_ = 128, floor_ = 96, target_ = 500, minTarget_ = 336, maxTarget_ = 2880;
    double window_ = 0.1, integ_ = 0, corr_ = 0, want_ = 0, ramp_ = 0, giveBack_ = 0, sumQ_ = 0, sumT_ = 0, smooth_ = 0;
    double lowSince_ = 1e18, lowSec_ = 0, lastRaise_ = -1e9, clock_ = 0;
    bool primed_ = false;
    uint64_t glitches_ = 0, dips_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
// SourceWatch: tells a source that STOPPED (nothing plays into it and there is no keep-alive: the output
// should get silence) from a source that is only LATE (some drivers/systems deliver the loopback in lumps:
// nothing for 20 ms, then all 20 ms at once). A late source must not get silence inserted — that cut the
// sound ~17 times a second on one laptop — but a queue big enough to cover its holes.
//   - every poll: stalled(now) → may the output be fed silence now?
//   - when audio arrives: onData(now, seconds of audio that arrived in this poll)
//     returns true if the hole before it was a lump (late audio caught up); holeSec() then grows

class SourceWatch
{
public:
    void prepare(double packetSec)
    {
        packet_ = std::max(0.0005, packetSec);
        hole_ = 0.0;
        lastData_ = -1e9;
        lastLump_ = -1e9;
        lumps_ = 0;
        recentPos_ = 0;
        recent_[0] = recent_[1] = recent_[2] = -1e9;
    }

    /// Longest lump hole seen recently (seconds), 0 if the source is steady.
    double holeSec() const { return hole_; }
    uint64_t lumps() const { return lumps_; }
    double lastData() const { return lastData_; }

    /// No audio for longer than the source's usual holes: it has stopped, feed silence.
    bool stalled(double now) const
    {
        return lastData_ > -1e8 && now - lastData_ > 3.0 * packet_ + 0.003 + 1.5 * hole_;
    }

    /// Audio arrived at `now`: `seconds` of it in this poll. ourGap: how long our own thread had not looked
    /// before this poll — if WE were late (the system was busy), the hole is ours, not the source's.
    /// Returns true if it ended a lump hole and the source is known to come in lumps (holeSec() then grows).
    bool onData(double now, double seconds, double ourGap = 0.0)
    {
        bool lump = false;
        if (lastData_ > -1e8)
        {
            const double gap = now - lastData_;
            // a hole long enough to count as a stop, followed by at least half of it in one go: late, not absent.
            // Only a pattern counts (3 such holes within a second): one long hole is a hiccup, not the source's way.
            if (gap > 3.0 * packet_ + 0.003 && seconds >= 0.5 * gap && ourGap < 0.008)
            {
                ++lumps_;
                recent_[recentPos_++ % 3] = now;
                const bool pattern = recentPos_ >= 3 && now - std::min(recent_[0], std::min(recent_[1], recent_[2])) < 1.0;
                if (pattern)
                {
                    lump = true;
                    hole_ = std::min(kMaxHole, std::max(hole_, gap));
                    lastLump_ = now;
                }
            }
        }
        // a steady source again for 30 s: forget the holes slowly
        if (hole_ > 0.0 && now - lastLump_ > 30.0) { hole_ = 0.0; lastLump_ = now; }
        lastData_ = now;
        return lump;
    }

    static constexpr double kMaxHole = 0.060;

private:
    double packet_ = 0.00267, hole_ = 0.0, lastData_ = -1e9, lastLump_ = -1e9;
    double recent_[3] = { -1e9, -1e9, -1e9 };
    unsigned recentPos_ = 0;
    uint64_t lumps_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Sample format conversion (WASAPI shared-mode mix formats: float32, or PCM 16 / 24 / 32-bit, any channel count)

struct SampleFormat
{
    int channels = 2;
    int bitsPerSample = 32;     // container size
    int validBits = 32;
    bool isFloat = true;
    uint32_t channelMask = 0;   // WAVEFORMATEXTENSIBLE::dwChannelMask; 0 = not given (the usual layout for the count)
    int blockAlign() const { return channels * bitsPerSample / 8; }
};

inline float readSample(const uint8_t* p, const SampleFormat& f)
{
    if (f.isFloat)
    {
        if (f.bitsPerSample == 64) { double d; std::memcpy(&d, p, 8); return (float)d; }
        float v; std::memcpy(&v, p, 4); return v;
    }
    switch (f.bitsPerSample)
    {
    case 16: { int16_t v; std::memcpy(&v, p, 2); return v / 32768.0f; }
    case 24: { int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24); return (float)(v / 2147483648.0); }
    case 32: { int32_t v; std::memcpy(&v, p, 4); return (float)(v / 2147483648.0); }
    default: return 0.0f;
    }
}

inline void writeSample(uint8_t* p, const SampleFormat& f, float x)
{
    if (f.isFloat)
    {
        if (f.bitsPerSample == 64) { const double d = x; std::memcpy(p, &d, 8); return; }
        std::memcpy(p, &x, 4);
        return;
    }
    const double c = std::max(-1.0, std::min(1.0, (double)x));
    switch (f.bitsPerSample)
    {
    case 16: { const int16_t v = (int16_t)std::lround(std::min(32767.0, c * 32768.0)); std::memcpy(p, &v, 2); break; }
    case 24: { const int32_t v = (int32_t)std::lround(std::min(8388607.0, c * 8388608.0)); p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); p[2] = (uint8_t)((v >> 16) & 0xFF); break; }
    case 32: { const int32_t v = (int32_t)std::llround(std::min(2147483647.0, c * 2147483648.0)); std::memcpy(p, &v, 4); break; }
    default: break;
    }
}

/// Smooths a jump at the start of a packet (after a gap in the source, or after silence we filled in):
/// the first `ramp` frames blend from the last value that was played (fromL / fromR) into the packet with a
/// raised-cosine curve, so the join is a short fade instead of a step (a click).
inline void declickStart(float* lr, size_t frames, size_t ramp, float fromL, float fromR)
{
    const size_t n = std::min(frames, ramp);
    for (size_t i = 0; i < n; ++i)
    {
        const float w = 0.5f - 0.5f * (float)std::cos(3.14159265358979323846 * (double)(i + 1) / (double)(n + 1));
        lr[2 * i] = fromL + (lr[2 * i] - fromL) * w;
        lr[2 * i + 1] = fromR + (lr[2 * i + 1] - fromR) * w;
    }
}

/// Device frames → interleaved stereo. Surround (5.1 / 7.1 in the usual Windows order
/// FL FR FC LFE BL BR [SL SR]) is folded down; LFE is left out.
inline void toStereo(const uint8_t* data, size_t frames, const SampleFormat& f, float* lr)
{
    const int ch = f.channels, bytes = f.bitsPerSample / 8, ba = f.blockAlign();
    for (size_t i = 0; i < frames; ++i)
    {
        const uint8_t* fr = data + i * (size_t)ba;
        auto s = [&](int c) { return readSample(fr + c * bytes, f); };
        float L, R;
        if (ch == 1) L = R = s(0);
        else if (ch >= 6)
        {
            const float c = 0.7071f * s(2);
            L = s(0) + c + 0.7071f * s(4);
            R = s(1) + c + 0.7071f * s(5);
            if (ch >= 8) { L += 0.7071f * s(6); R += 0.7071f * s(7); }
        }
        else { L = s(0); R = s(1); }
        lr[2 * i] = L;
        lr[2 * i + 1] = R;
    }
}

/// Stereo → device frames: left and right go to the first two channels, the others stay silent.
inline void fromStereo(const float* l, const float* r, size_t frames, const SampleFormat& f, uint8_t* data)
{
    const int ch = f.channels, bytes = f.bitsPerSample / 8, ba = f.blockAlign();
    std::memset(data, 0, frames * (size_t)ba);
    for (size_t i = 0; i < frames; ++i)
    {
        uint8_t* fr = data + i * (size_t)ba;
        if (ch == 1) writeSample(fr, f, 0.5f * (l[i] + r[i]));
        else
        {
            writeSample(fr, f, l[i]);
            writeSample(fr + bytes, f, r[i]);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Surround (2.8.7). Before, everything was folded down to stereo and played on the output's front left and
// right only: a 7.1 game into a 7.1 headset lost where the sounds came from. When both devices have more than
// two channels, the engine now processes each speaker position on its own (with one gain for all, so the
// balance between them is kept) and plays it on the same position of the output.

/// Speaker positions: the bits of WAVEFORMATEXTENSIBLE::dwChannelMask, in the order the channels sit in a frame.
constexpr uint32_t kSpFL = 0x1, kSpFR = 0x2, kSpFC = 0x4, kSpLFE = 0x8, kSpBL = 0x10, kSpBR = 0x20,
                   kSpFLC = 0x40, kSpFRC = 0x80, kSpBC = 0x100, kSpSL = 0x200, kSpSR = 0x400;

/// The layout Windows assumes for a channel count when a format gives no mask.
inline uint32_t defaultChannelMask(int channels)
{
    switch (channels)
    {
    case 1: return kSpFC;
    case 2: return kSpFL | kSpFR;
    case 3: return kSpFL | kSpFR | kSpLFE;
    case 4: return kSpFL | kSpFR | kSpBL | kSpBR;
    case 5: return kSpFL | kSpFR | kSpFC | kSpBL | kSpBR;
    case 6: return kSpFL | kSpFR | kSpFC | kSpLFE | kSpBL | kSpBR;
    case 7: return kSpFL | kSpFR | kSpFC | kSpLFE | kSpBC | kSpSL | kSpSR;
    case 8: return kSpFL | kSpFR | kSpFC | kSpLFE | kSpBL | kSpBR | kSpSL | kSpSR;
    default: return 0;
    }
}

/// The speaker of each channel of a format (0 for a channel the mask does not name).
inline void speakersOf(const SampleFormat& f, uint32_t* sp, int maxCh)
{
    uint32_t mask = f.channelMask ? f.channelMask : defaultChannelMask(f.channels);
    for (int c = 0; c < f.channels && c < maxCh; ++c)
    {
        const uint32_t bit = mask & (~mask + 1);   // lowest set bit
        sp[c] = bit;
        mask &= ~bit;
    }
}

inline const char* speakerName(uint32_t s)
{
    switch (s)
    {
    case kSpFL: return "FL"; case kSpFR: return "FR"; case kSpFC: return "FC"; case kSpLFE: return "LFE";
    case kSpBL: return "BL"; case kSpBR: return "BR"; case kSpFLC: return "FLC"; case kSpFRC: return "FRC";
    case kSpBC: return "BC"; case kSpSL: return "SL"; case kSpSR: return "SR"; default: return "?";
    }
}

/// How the source's channels become the processed channels, and where those go in the output.
/// Folding (adding channels together) happens only BEFORE the processing; afterwards every processed channel is
/// copied to exactly one output channel, so the output can never be louder than the processing's ceiling.
struct ChannelPlan
{
    static constexpr int kMaxSource = 32;
    bool surround = false;                  // false: the stereo path (toStereo / fromStereo), exactly as before
    int channels = 2;                       // channels processed
    int sourceChannels = 2;
    int route[kMaxChannels] = { 0, 1 };     // output channel of each processed channel
    uint32_t speaker[kMaxChannels] = { kSpFL, kSpFR };
    float fold[kMaxChannels][kMaxSource] = {};   // [processed][source] weight

    std::string describe() const
    {
        std::string s;
        for (int c = 0; c < channels; ++c) { if (c) s += ' '; s += speakerName(speaker[c]); }
        return s;
    }
};

/// Surround only when both devices have more than two channels (and at most 8 positions end up processed);
/// otherwise the stereo plan: the same sound as before 2.8.7.
inline ChannelPlan planChannels(const SampleFormat& src, const SampleFormat& out)
{
    ChannelPlan p;
    p.sourceChannels = src.channels;
    if (src.channels <= 2 || out.channels <= 2 || src.channels > ChannelPlan::kMaxSource || out.channels > 32) return p;

    uint32_t sIn[ChannelPlan::kMaxSource] = {}, sOut[32] = {};
    speakersOf(src, sIn, ChannelPlan::kMaxSource);
    speakersOf(out, sOut, 32);
    uint32_t outMask = 0;
    for (int c = 0; c < out.channels; ++c) outMask |= sOut[c];
    if ((outMask & (kSpFL | kSpFR)) != (kSpFL | kSpFR)) return p;   // no front pair to fall back on
    auto has = [&](uint32_t s) { return (outMask & s) != 0; };
    auto outIndex = [&](uint32_t s) { for (int c = 0; c < out.channels; ++c) if (sOut[c] == s) return c; return -1; };

    // where a source speaker plays: the same position, a close one (side ↔ back, front-centre pairs → front),
    // or nowhere (0: folded into the front pair instead)
    auto place = [&](uint32_t s) -> uint32_t {
        if (has(s)) return s;
        switch (s)
        {
        case kSpSL: return has(kSpBL) ? kSpBL : 0;
        case kSpSR: return has(kSpBR) ? kSpBR : 0;
        case kSpBL: return has(kSpSL) ? kSpSL : 0;
        case kSpBR: return has(kSpSR) ? kSpSR : 0;
        case kSpFLC: return kSpFL;
        case kSpFRC: return kSpFR;
        default: return 0;
        }
    };
    auto addTo = [&](uint32_t target, int srcCh, float w) -> bool {
        int k = 0;
        for (; k < p.channels; ++k) if (p.speaker[k] == target) break;
        if (k == p.channels)
        {
            if (p.channels == kMaxChannels) return false;
            p.speaker[k] = target;
            p.route[k] = outIndex(target);
            ++p.channels;
        }
        p.fold[k][srcCh] += w;
        return true;
    };

    p.channels = 0;
    const float h = 0.7071f;   // folded into the front pair at -3 dB, as toStereo does
    for (int c = 0; c < src.channels; ++c)
    {
        const uint32_t s = sIn[c];
        if (s == 0) continue;                                   // a channel the layout does not name
        if (s == kSpLFE && !has(kSpLFE)) continue;              // no subwoofer channel: left out, as in stereo
        const uint32_t t = s == kSpLFE ? kSpLFE : place(s);
        bool ok;
        if (t) ok = addTo(t, c, 1.0f);
        else if (s == kSpFC || s == kSpBC) ok = addTo(kSpFL, c, h) && addTo(kSpFR, c, h);
        else if (s == kSpBL || s == kSpSL) ok = addTo(kSpFL, c, h);
        else if (s == kSpBR || s == kSpSR) ok = addTo(kSpFR, c, h);
        else continue;
        if (!ok) return ChannelPlan{ false, 2, src.channels };   // more than kMaxChannels positions
    }
    if (p.channels < 2) return ChannelPlan{ false, 2, src.channels };
    // processed channels in the output's channel order (a stable, readable layout)
    for (int a = 0; a < p.channels; ++a)
        for (int b = a + 1; b < p.channels; ++b)
            if (p.route[b] < p.route[a])
            {
                std::swap(p.route[a], p.route[b]);
                std::swap(p.speaker[a], p.speaker[b]);
                for (int c = 0; c < ChannelPlan::kMaxSource; ++c) std::swap(p.fold[a][c], p.fold[b][c]);
            }
    p.surround = true;
    return p;
}

/// Device frames → interleaved processed channels, by the plan's fold weights (surround plans only).
inline void foldFrames(const uint8_t* data, size_t frames, const SampleFormat& f, const ChannelPlan& p, float* x)
{
    const int ch = std::min(f.channels, ChannelPlan::kMaxSource), bytes = f.bitsPerSample / 8, ba = f.blockAlign();
    float s[ChannelPlan::kMaxSource];
    for (size_t i = 0; i < frames; ++i)
    {
        const uint8_t* fr = data + i * (size_t)ba;
        for (int c = 0; c < ch; ++c) s[c] = readSample(fr + c * bytes, f);
        float* o = x + i * (size_t)p.channels;
        for (int k = 0; k < p.channels; ++k)
        {
            float v = 0.0f;
            for (int c = 0; c < ch; ++c) v += p.fold[k][c] * s[c];
            o[k] = v;
        }
    }
}

/// Processed channels → device frames: each to its own output channel (no adding), the rest silent.
inline void fromChannels(const float* const* x, const ChannelPlan& p, size_t frames, const SampleFormat& f, uint8_t* data)
{
    const int bytes = f.bitsPerSample / 8, ba = f.blockAlign();
    std::memset(data, 0, frames * (size_t)ba);
    for (size_t i = 0; i < frames; ++i)
    {
        uint8_t* fr = data + i * (size_t)ba;
        for (int k = 0; k < p.channels; ++k) writeSample(fr + p.route[k] * bytes, f, x[k][i]);
    }
}

/// declickStart for any channel count: x interleaved, from[c] the last value played on each channel.
inline void declickStartN(float* x, size_t frames, int nch, size_t ramp, const float* from)
{
    const size_t n = std::min(frames, ramp);
    for (size_t i = 0; i < n; ++i)
    {
        const float w = 0.5f - 0.5f * (float)std::cos(3.14159265358979323846 * (double)(i + 1) / (double)(n + 1));
        for (int c = 0; c < nch; ++c) x[i * (size_t)nch + (size_t)c] = from[c] + (x[i * (size_t)nch + (size_t)c] - from[c]) * w;
    }
}

} // namespace vga
