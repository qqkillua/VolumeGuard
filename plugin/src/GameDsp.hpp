// VolumeGuard Limiter — "Game" mode: two-band compressor that tames gunshots, explosions and engine rumble
// and lifts quiet sounds such as footsteps. The lookahead limiter after it keeps the ceiling.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// Signal path (both channels always get the same gain, so the direction of sounds is not changed):
//   input ─┬─ low band  (< 150 Hz: engines, explosions, rumble) ── compressor ─┐
//          └─ high band (≥ 150 Hz: shots, steps, voices)       ── compressor ─┴─ sum × make-up gain → limiter
// The split is a 4th-order Linkwitz-Riley crossover: both bands add back up to a flat response.
#pragma once

#include <algorithm>
#include <cmath>

namespace vgl {

class Biquad
{
public:
    void lowpass(double fs, double f, double q) { set(fs, f, q, true); }
    void highpass(double fs, double f, double q) { set(fs, f, q, false); }
    void reset() { z1_[0] = z1_[1] = z2_[0] = z2_[1] = 0.0f; }

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
    float z1_[2] = { 0, 0 }, z2_[2] = { 0, 0 };
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
    inline float gain(float l, float r)
    {
        const float d = std::max(std::fabs(l), std::fabs(r));
        env_ = d > env_ ? d + (env_ - d) * att_ : d + (env_ - d) * rel_;
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

private:
    float thr_ = 0, ratio_ = 1, thrT_ = 0, ratioT_ = 1, knee_ = 6, att_ = 0, rel_ = 0, env_ = 0, minGrDb_ = 0;
};

class GameProcessor
{
public:
    enum Mode { kOff = 0, kComfort = 1, kTactical = 2 };

    static constexpr double kCrossoverHz = 150.0;

    void prepare(double sampleRate)
    {
        fs_ = sampleRate > 0 ? sampleRate : 48000.0;
        const double q = 0.70710678118654752;   // Butterworth; two in a row = Linkwitz-Riley 4th order
        lp1_.lowpass(fs_, kCrossoverHz, q);
        lp2_.lowpass(fs_, kCrossoverHz, q);
        hp1_.highpass(fs_, kCrossoverHz, q);
        hp2_.highpass(fs_, kCrossoverHz, q);
        glideK_ = (float)(1.0 - std::exp(-1.0 / (fs_ * 0.020)));
        configure();
        reset();
    }

    /// Clears the filters and jumps straight to the current settings (no glide): call before sound starts.
    void reset()
    {
        lp1_.reset(); lp2_.reset(); hp1_.reset(); hp2_.reset();
        low_.reset(); high_.reset();
        snap();
    }

    /// Jump to the current settings without gliding (the next sound starts with them).
    void snap()
    {
        low_.snap(); high_.snap();
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
        const float wantMix = mode_ != kOff ? 1.0f : 0.0f;
        if (mode_ == kOff && mix_ <= 1e-4f)
        {
            if (mix_ != 0.0f)
            {
                mix_ = 0.0f;
                lp1_.reset(); lp2_.reset(); hp1_.reset(); hp2_.reset();
                low_.reset(); high_.reset();
            }
            return;   // off: the signal is untouched
        }
        const float k = glideK_;
        for (int i = 0; i < frames; ++i)
        {
            low_.glide(k);
            high_.glide(k);
            makeup_ += (makeupT_ - makeup_) * k;
            mix_ += (wantMix - mix_) * k;
            if (std::fabs(mix_ - wantMix) < 1e-5f) mix_ = wantMix;
            const float lowL = lp2_.process(lp1_.process(l[i], 0), 0);
            const float lowR = lp2_.process(lp1_.process(r[i], 1), 1);
            const float highL = hp2_.process(hp1_.process(l[i], 0), 0);
            const float highR = hp2_.process(hp1_.process(r[i], 1), 1);
            const float gLow = low_.gain(lowL, lowR);
            const float gHigh = high_.gain(highL, highR);
            // 4th-order Linkwitz-Riley bands are in phase and add up flat (checked: ±0.00 dB, 50 Hz…5 kHz)
            const float yl = (lowL * gLow + highL * gHigh) * makeup_;
            const float yr = (lowR * gLow + highR * gHigh) * makeup_;
            if (mix_ >= 1.0f) { l[i] = yl; r[i] = yr; }
            else { l[i] += (yl - l[i]) * mix_; r[i] += (yr - r[i]) * mix_; }
        }
    }

    /// Strongest reduction (dB, ≤ 0) of the main band since the last call, for the meter.
    float takeReductionDb()
    {
        const float lo = low_.takeReductionDb(), hi = high_.takeReductionDb();
        return std::min(hi, lo * 0.5f);   // rumble counts half: it is heard less than the shots
    }

private:
    void configure()
    {
        const float a = amount_;
        if (mode_ == kComfort)
        {
            low_.setup(fs_, -16.0f - 10.0f * a, 3.0f, 10.0f, 200.0f);
            high_.setup(fs_, -20.0f - 10.0f * a, 2.5f, 8.0f, 250.0f);
            makeupT_ = dbToGainLocal(6.0f * a);
        }
        else if (mode_ == kTactical)
        {
            low_.setup(fs_, -26.0f - 10.0f * a, 8.0f, 3.0f, 150.0f);
            high_.setup(fs_, -22.0f - 14.0f * a, 6.0f, 1.5f, 120.0f);
            makeupT_ = dbToGainLocal(4.0f + 10.0f * a);
        }
        else makeupT_ = 1.0f;
    }

    static float dbToGainLocal(float db) { return std::pow(10.0f, db / 20.0f); }

    double fs_ = 48000.0;
    int mode_ = kOff;
    float amount_ = 0.5f, makeup_ = 1.0f, makeupT_ = 1.0f, mix_ = 0.0f, glideK_ = 0.001f;
    Biquad lp1_, lp2_, hp1_, hp2_;
    BandCompressor low_, high_;
};

} // namespace vgl
