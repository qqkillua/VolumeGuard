// VolumeGuard Limiter — "Game" mode: two-band compressor that tames gunshots, explosions and engine rumble
// and lifts quiet sounds such as footsteps. The lookahead limiter after it keeps the ceiling.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// Signal path (both channels always get the same gain, so the direction of sounds is not changed):
//   input ─┬─ low  (< 150 Hz: engines, explosions, rumble)     ── compressor ─────────────────┐
//          ├─ mid  (150 Hz–2 kHz: body of shots, voices)       ── compressor ─────────────────┤
//          └─ high (> 2 kHz: footsteps, reloads, doors, crack) ── compressor × quiet lift ────┴─ sum × make-up → limiter
// The split is 4th-order Linkwitz-Riley at 150 Hz and 2 kHz: the bands add back up to a flat response.
#pragma once

#include <algorithm>
#include <cmath>

namespace vgl {

/// Channels the Game mode can process at once (the plugin uses 2; the app up to 7.1).
constexpr int kGameMaxChannels = 8;

class Biquad
{
public:
    void lowpass(double fs, double f, double q) { set(fs, f, q, true); }
    void highpass(double fs, double f, double q) { set(fs, f, q, false); }
    void reset() { for (int c = 0; c < kGameMaxChannels; ++c) z1_[c] = z2_[c] = 0.0f; }

    inline float process(float x, int ch)
    {
        // transposed direct form II
        const float y = b0_ * x + z1_[ch];
        z1_[ch] = b1_ * x - a1_ * y + z2_[ch];
        z2_[ch] = b2_ * x - a2_ * y;
        if (std::fabs(z1_[ch]) < 1e-25f) z1_[ch] = 0.0f;   // no denormals in long silence
        if (std::fabs(z2_[ch]) < 1e-25f) z2_[ch] = 0.0f;
        return y;
    }

private:
    void set(double fs, double f, double q, bool lp)
    {
        const double w = 2.0 * 3.14159265358979323846 * f / fs;
        const double c = std::cos(w), s = std::sin(w), alpha = s / (2.0 * q);
        const double a0 = 1.0 + alpha;
        const double b0 = lp ? (1.0 - c) / 2.0 : (1.0 + c) / 2.0;
        const double b1 = lp ? (1.0 - c) : -(1.0 + c);
        b0_ = (float)(b0 / a0);
        b1_ = (float)(b1 / a0);
        b2_ = (float)(b0 / a0);
        a1_ = (float)(-2.0 * c / a0);
        a2_ = (float)((1.0 - alpha) / a0);
    }

    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_[kGameMaxChannels] = {}, z2_[kGameMaxChannels] = {};
};

/// Stereo-linked feed-forward compressor working on a peak envelope, soft knee.
class BandCompressor
{
public:
    /// Threshold and ratio glide to new values (see glide()), so switching mode or amount does not jump.
    void setup(double fs, float thresholdDb, float ratio, float attackMs, float releaseMs, float kneeDb = 6.0f)
    {
        thrT_ = thresholdDb;
        ratioT_ = std::max(1.0f, ratio);
        knee_ = std::max(0.01f, kneeDb);
        att_ = (float)std::exp(-1.0 / (fs * attackMs / 1000.0));
        rel_ = (float)std::exp(-1.0 / (fs * releaseMs / 1000.0));
    }
    void reset() { env_ = 0.0f; }
    /// Jump to the targets (after prepare, before any sound).
    void snap() { thr_ = thrT_; ratio_ = ratioT_; }
    /// One sample of gliding towards the targets; k = 1 - e^(-1 / (fs * 20 ms)).
    inline void glide(float k) { thr_ += (thrT_ - thr_) * k; ratio_ += (ratioT_ - ratio_) * k; }

    /// Gain for this sample (linear, ≤ 1) from the louder of the two channels.
    inline float gain(float l, float r) { return gainFor(std::max(std::fabs(l), std::fabs(r))); }

    /// Gain for this sample from the loudest channel's level d (≥ 0): links any number of channels.
    inline float gainFor(float d)
    {
        env_ = d > env_ ? d + (env_ - d) * att_ : d + (env_ - d) * rel_;
        if (env_ < 1e-20f) env_ = 0.0f;                      // no denormals in long silence
        if (env_ < 1e-6f) return 1.0f;                       // below -120 dBFS: nothing to do
        const float x = 20.0f * std::log10(env_);
        const float over = x - thr_;
        float y;
        if (2.0f * over < -knee_) y = x;
        else if (2.0f * std::fabs(over) <= knee_)
        {
            const float t = over + knee_ * 0.5f;
            y = x + (1.0f / ratio_ - 1.0f) * t * t / (2.0f * knee_);
        }
        else y = thr_ + over / ratio_;
        const float grDb = std::min(0.0f, y - x);
        minGrDb_ = std::min(minGrDb_, grDb);
        return std::pow(10.0f, grDb / 20.0f);
    }

    float takeReductionDb() { const float v = minGrDb_; minGrDb_ = 0.0f; return v; }
    float envelope() const { return env_; }

private:
    float thr_ = 0, ratio_ = 1, thrT_ = 0, ratioT_ = 1, knee_ = 6, att_ = 0, rel_ = 0, env_ = 0, minGrDb_ = 0;
};

/// Splits stereo sound into three bands that add back up flat (4th-order Linkwitz-Riley at 150 Hz and 2 kHz):
///   low  < 150 Hz       engines, explosions, rumble
///   mid  150 Hz–2 kHz   body of shots, voices
///   high > 2 kHz        footsteps, reloads, doors, the crack of a shot
/// The low band also goes through the 2 kHz all-pass, so all three stay in phase.
class ThreeBandSplit
{
public:
    static constexpr double kLowHz = 150.0, kHighHz = 2000.0;

    void prepare(double fs)
    {
        const double q = 0.70710678118654752;   // Butterworth; two in a row = Linkwitz-Riley 4th order
        lpA1_.lowpass(fs, kLowHz, q); lpA2_.lowpass(fs, kLowHz, q);
        hpA1_.highpass(fs, kLowHz, q); hpA2_.highpass(fs, kLowHz, q);
        lpB1_.lowpass(fs, kHighHz, q); lpB2_.lowpass(fs, kHighHz, q);
        hpB1_.highpass(fs, kHighHz, q); hpB2_.highpass(fs, kHighHz, q);
        apL1_.lowpass(fs, kHighHz, q); apL2_.lowpass(fs, kHighHz, q);
        apH1_.highpass(fs, kHighHz, q); apH2_.highpass(fs, kHighHz, q);
        reset();
    }

    void reset()
    {
        for (Biquad* b : { &lpA1_, &lpA2_, &hpA1_, &hpA2_, &lpB1_, &lpB2_, &hpB1_, &hpB2_, &apL1_, &apL2_, &apH1_, &apH2_ })
            b->reset();
    }

    /// One sample of channel ch (0 or 1).
    inline void split(float x, int ch, float& low, float& mid, float& high)
    {
        const float lo = lpA2_.process(lpA1_.process(x, ch), ch);
        const float rest = hpA2_.process(hpA1_.process(x, ch), ch);
        mid = lpB2_.process(lpB1_.process(rest, ch), ch);
        high = hpB2_.process(hpB1_.process(rest, ch), ch);
        // the same phase turn the 2 kHz split gives to mid + high
        low = apL2_.process(apL1_.process(lo, ch), ch) + apH2_.process(apH1_.process(lo, ch), ch);
    }

private:
    Biquad lpA1_, lpA2_, hpA1_, hpA2_, lpB1_, lpB2_, hpB1_, hpB2_, apL1_, apL2_, apH1_, apH2_;
};

/// Lifts QUIET sounds in a band (upward compression): below `liftBelowDb` the band gets louder, by 0.6 dB per dB
/// it is below, up to `maxDb`. Very quiet sound (under `gateDb`, the noise floor) is not lifted,
/// and the lift fades in over the first 10 dB above the gate, so hiss is never pumped up.
class QuietLift
{
public:
    void setup(double fs, float liftBelowDb, float maxDb, float gateDb = -72.0f)
    {
        belowT_ = liftBelowDb;
        maxT_ = std::max(0.0f, maxDb);
        gate_ = gateDb;
        att_ = (float)std::exp(-1.0 / (fs * 0.003));    // 3 ms: a loud sound stops the lift at once
        rel_ = (float)std::exp(-1.0 / (fs * 0.150));
    }
    void reset() { env_ = 0.0f; }
    void snap() { below_ = belowT_; max_ = maxT_; }
    inline void glide(float k) { below_ += (belowT_ - below_) * k; max_ += (maxT_ - max_) * k; }

    inline float gain(float l, float r) { return gainFor(std::max(std::fabs(l), std::fabs(r))); }

    inline float gainFor(float d)
    {
        env_ = d > env_ ? d + (env_ - d) * att_ : d + (env_ - d) * rel_;
        if (env_ < 1e-20f) env_ = 0.0f;   // no denormals in long silence
        if (max_ <= 0.01f || env_ < 1e-6f) return 1.0f;
        const float x = 20.0f * std::log10(env_);
        if (x <= gate_) return 1.0f;
        float lift = std::min(max_, std::max(0.0f, (below_ - x) * 0.6f));
        lift *= std::min(1.0f, (x - gate_) / 10.0f);
        maxLiftDb_ = std::max(maxLiftDb_, lift);
        return std::pow(10.0f, lift / 20.0f);
    }

    float takeLiftDb() { const float v = maxLiftDb_; maxLiftDb_ = 0.0f; return v; }
    float envelope() const { return env_; }

private:
    float below_ = -40, max_ = 0, belowT_ = -40, maxT_ = 0, gate_ = -72, att_ = 0, rel_ = 0, env_ = 0, maxLiftDb_ = 0;
};

class GameProcessor
{
public:
    enum Mode { kOff = 0, kComfort = 1, kTactical = 2 };

    static constexpr double kCrossoverHz = ThreeBandSplit::kLowHz;

    void prepare(double sampleRate)
    {
        fs_ = sampleRate > 0 ? sampleRate : 48000.0;
        split_.prepare(fs_);
        glideK_ = (float)(1.0 - std::exp(-1.0 / (fs_ * 0.020)));
        configure();
        reset();
    }

    /// Clears the filters and jumps straight to the current settings (no glide): call before sound starts.
    void reset()
    {
        split_.reset();
        low_.reset(); mid_.reset(); high_.reset(); lift_.reset();
        snap();
    }

    /// Jump to the current settings without gliding (the next sound starts with them).
    void snap()
    {
        low_.snap(); mid_.snap(); high_.snap(); lift_.snap();
        makeup_ = makeupT_;
        mix_ = mode_ != kOff ? 1.0f : 0.0f;
    }

    void setMode(int mode)
    {
        mode_ = std::max(0, std::min(2, mode));
        configure();
    }

    /// 0…1: how strongly loud sounds are pressed down and quiet ones lifted.
    void setAmount(float amount01)
    {
        amount_ = std::max(0.0f, std::min(1.0f, amount01));
        configure();
    }

    int mode() const { return mode_; }

    /// In-place stereo processing. Switching the mode on or off fades over ~20 ms, and changing the mode or
    /// the amount glides the settings over ~20 ms, so neither clicks.
    void process(float* l, float* r, int frames)
    {
        float* ch[2] = { l, r };
        process(ch, 2, frames);
    }

    /// The same for 1…kGameMaxChannels channels: every channel gets the same gains (from the loudest one).
    void process(float* const* ch, int nch, int frames)
    {
        nch = std::max(1, std::min(kGameMaxChannels, nch));
        const float wantMix = mode_ != kOff ? 1.0f : 0.0f;
        if (mode_ == kOff && mix_ <= 1e-4f)
        {
            if (mix_ != 0.0f)
            {
                mix_ = 0.0f;
                split_.reset();
                low_.reset(); mid_.reset(); high_.reset(); lift_.reset();
            }
            return;   // off: the signal is untouched
        }
        const float k = glideK_;
        float lo[kGameMaxChannels], mi[kGameMaxChannels], hi[kGameMaxChannels];
        for (int i = 0; i < frames; ++i)
        {
            low_.glide(k); mid_.glide(k); high_.glide(k); lift_.glide(k);
            makeup_ += (makeupT_ - makeup_) * k;
            mix_ += (wantMix - mix_) * k;
            if (std::fabs(mix_ - wantMix) < 1e-5f) mix_ = wantMix;
            float dLo = 0.0f, dMi = 0.0f, dHi = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                split_.split(ch[c][i], c, lo[c], mi[c], hi[c]);
                dLo = std::max(dLo, std::fabs(lo[c]));
                dMi = std::max(dMi, std::fabs(mi[c]));
                dHi = std::max(dHi, std::fabs(hi[c]));
            }
            const float gLow = low_.gainFor(dLo);
            const float gMid = mid_.gainFor(dMi);
            // the footsteps band: loud sounds pressed down, quiet ones lifted (all channels the same)
            const float gHigh = high_.gainFor(dHi) * lift_.gainFor(dHi);
            for (int c = 0; c < nch; ++c)
            {
                const float y = (lo[c] * gLow + mi[c] * gMid + hi[c] * gHigh) * makeup_;
                if (mix_ >= 1.0f) ch[c][i] = y;
                else ch[c][i] += (y - ch[c][i]) * mix_;
            }
        }
    }

    /// Strongest reduction (dB, ≤ 0) since the last call, for the meter.
    float takeReductionDb()
    {
        const float lo = low_.takeReductionDb(), mi = mid_.takeReductionDb(), hi = high_.takeReductionDb();
        lift_.takeLiftDb();
        return std::min(std::min(mi, hi), lo * 0.5f);   // rumble counts half: it is heard less than the shots
    }

    /// Largest lift of quiet high sounds (dB, ≥ 0) since the last call.
    float takeLiftDb() { return lift_.takeLiftDb(); }

private:
    void configure()
    {
        const float a = amount_;
        if (mode_ == kComfort)
        {
            low_.setup(fs_, -16.0f - 10.0f * a, 3.0f, 10.0f, 200.0f);
            mid_.setup(fs_, -20.0f - 10.0f * a, 2.5f, 8.0f, 250.0f);
            high_.setup(fs_, -20.0f - 10.0f * a, 2.5f, 8.0f, 250.0f);
            lift_.setup(fs_, -36.0f, 4.0f * a);             // a little more detail in quiet scenes
            makeupT_ = dbToGainLocal(6.0f * a);
        }
        else if (mode_ == kTactical)
        {
            low_.setup(fs_, -26.0f - 10.0f * a, 8.0f, 3.0f, 150.0f);
            mid_.setup(fs_, -22.0f - 14.0f * a, 6.0f, 1.5f, 120.0f);
            high_.setup(fs_, -22.0f - 14.0f * a, 6.0f, 1.5f, 120.0f);
            lift_.setup(fs_, -30.0f, 4.0f + 8.0f * a);      // footsteps: up to +8 dB at 50 %, +12 dB at 100 %
            makeupT_ = dbToGainLocal(4.0f + 10.0f * a);
        }
        else
        {
            lift_.setup(fs_, -40.0f, 0.0f);
            makeupT_ = 1.0f;
        }
    }

    static float dbToGainLocal(float db) { return std::pow(10.0f, db / 20.0f); }

    double fs_ = 48000.0;
    int mode_ = kOff;
    float amount_ = 0.5f, makeup_ = 1.0f, makeupT_ = 1.0f, mix_ = 0.0f, glideK_ = 0.001f;
    ThreeBandSplit split_;
    BandCompressor low_, mid_, high_;
    QuietLift lift_;
};

} // namespace vgl
