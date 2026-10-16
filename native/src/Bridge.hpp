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
#include <vector>

namespace vga {

/// Single-producer / single-consumer ring of interleaved stereo frames.
class FrameRing
{
public:
    void init(size_t capacityFrames)
    {
        cap_ = std::max<size_t>(capacityFrames, 16);
        buf_.assign(cap_ * 2, 0.0f);
        w_.store(0); r_.store(0);
    }

    size_t capacity() const { return cap_; }
    size_t fill() const { return (size_t)(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire)); }

    /// Producer. Returns frames written; what does not fit is dropped.
    /// nowSec: when the packet arrived (any monotonic clock shared with the reader).
    size_t write(const float* lr, size_t frames, double nowSec = 0.0)
    {
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const size_t room = cap_ - (size_t)(w - r_.load(std::memory_order_acquire));
        const size_t n = std::min(frames, room);
        for (size_t i = 0; i < n; ++i)
        {
            const size_t k = (size_t)((w + i) % cap_) * 2;
            buf_[k] = lr[2 * i];
            buf_[k + 1] = lr[2 * i + 1];
        }
        lastWrite_.store(nowSec, std::memory_order_release);
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    double lastWriteSec() const { return lastWrite_.load(std::memory_order_acquire); }

    /// Consumer: frame at an absolute index in [readPos, writePos).
    inline float at(uint64_t frame, int ch) const { return buf_[(size_t)(frame % cap_) * 2 + (size_t)ch]; }
    uint64_t readPos() const { return r_.load(std::memory_order_relaxed); }
    uint64_t writePos() const { return w_.load(std::memory_order_acquire); }
    void advance(size_t frames) { r_.store(r_.load(std::memory_order_relaxed) + frames, std::memory_order_release); }

private:
    std::vector<float> buf_;
    size_t cap_ = 0;
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
        corr_ = std::max(-kMaxCorrection, std::min(kMaxCorrection, corr));
        const double step = nominal_ * (1.0 + corr_);
        const uint64_t base = ring.readPos();
        const uint64_t avail = ring.writePos();
        int i = 0;
        for (; i < maxFrames; ++i)
        {
            const uint64_t i0 = base + (uint64_t)frac_;
            if (i0 + kHalf >= avail) break;
            interpolate(ring, i0, frac_ - std::floor(frac_), l[i], r[i]);
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
        const double fp = t * kPhases;
        const int ph = std::min(kPhases - 1, (int)fp);
        const float a = (float)(fp - ph);
        const float* c0 = &table_[(size_t)ph * 2 * kHalf];
        const float* c1 = c0 + 2 * kHalf;
        float sl = 0, sr = 0;
        const uint64_t first = i0 - (kHalf - 1);
        for (int k = 0; k < 2 * kHalf; ++k)
        {
            const float c = c0[k] + (c1[k] - c0[k]) * a;
            sl += c * ring.at(first + (uint64_t)k, 0);
            sr += c * ring.at(first + (uint64_t)k, 1);
        }
        outL = sl;
        outR = sr;
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
        packet_ = std::max(0.0, packetFrames);
        floor_ = floorMs * outRate / 1000.0;
        target_ = floor_ + 0.5 * (period_ + packet_) + 0.001 * outRate;
        minTarget_ = floor_ + 0.5 * period_;
        maxTarget_ = maxTargetMs * outRate / 1000.0;
        window_ = 0.1;
        reset();
    }

    void reset()
    {
        integ_ = corr_ = 0.0;
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
            corr_ = std::max(-kMax, std::min(kMax, kP * err + kI * integ_));

            lowSec_ += d;
            if (lowSec_ > 20.0)
            {
                if (lowSince_ > floor_ + 0.002 * rate_ && target_ > minTarget_)
                    target_ = std::max(minTarget_, target_ - 0.0005 * rate_);
                lowSince_ = 1e18;
                lowSec_ = 0.0;
            }
        }
        return corr_;
    }

    /// Right after the device took its period (its event): the queue is at its low point. judge = false while
    /// the source is silent or just started (then a low queue is not our fault).
    /// Returns true if this was a glitch (the device found the queue empty).
    bool onPull(double queueFrames, bool judge)
    {
        if (!judge || !primed_) return false;
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

    /// How far the average queue is above what it should be (frames).
    double excess() const { return primed_ ? smooth_ - target_ : 0.0; }
    double targetFrames() const { return target_; }
    double targetMs() const { return target_ * 1000.0 / rate_; }
    double floorFrames() const { return floor_; }
    double correction() const { return corr_; }
    uint64_t glitches() const { return glitches_; }
    uint64_t dips() const { return dips_; }
    void settle(double removedFrames) { smooth_ -= removedFrames; integ_ = 0.0; }

    static constexpr double kMax = 0.005;
    static constexpr double kP = 0.54, kI = 0.09;

private:
    void raise(double sec)
    {
        target_ = std::min(maxTarget_, target_ + sec * rate_);
        lastRaise_ = clock_;
        lowSince_ = 1e18;
        lowSec_ = 0.0;
    }

    double rate_ = 48000, period_ = 480, packet_ = 128, floor_ = 96, target_ = 500, minTarget_ = 336, maxTarget_ = 2880;
    double window_ = 0.1, integ_ = 0, corr_ = 0, sumQ_ = 0, sumT_ = 0, smooth_ = 0;
    double lowSince_ = 1e18, lowSec_ = 0, lastRaise_ = -1e9, clock_ = 0;
    bool primed_ = false;
    uint64_t glitches_ = 0, dips_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Sample format conversion (WASAPI shared-mode mix formats: float32, or PCM 16 / 24 / 32-bit, any channel count)

struct SampleFormat
{
    int channels = 2;
    int bitsPerSample = 32;     // container size
    int validBits = 32;
    bool isFloat = true;
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

} // namespace vga
