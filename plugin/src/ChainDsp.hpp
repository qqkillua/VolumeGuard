// VolumeGuard Limiter — the whole processing chain, shared by the plugin and the app's sound processing:
//   input gain → Game mode (Comfort / Tactical) → lookahead limiter, with a click-free bypass.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// Nothing here jumps: input gain glides (~20 ms), Game mode settings glide and switch on/off with a fade,
// the limiter's ceiling may change at any time (its safety clamp follows the delayed signal), and bypass
// cross-fades (~20 ms) between the processed signal and the dry one delayed by the same latency.
// Changing the lookahead changes the delay itself, so it cannot glide: the sound fades out over 2 ms, the
// limiter switches, and the sound fades back in once it comes out of the new delay (a short gap, no click).
#pragma once

#include "GameDsp.hpp"
#include "LimiterDsp.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vgl {

class Chain
{
public:
    void prepare(double sampleRate)
    {
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        lim_.prepare(sr_, 2, lookMs_);
        lim_.setInputGainDb(0.0f);            // the input gain is applied here, before the Game mode
        game_.prepare(sr_);
        k_ = (float)(1.0 - std::exp(-1.0 / (sr_ * 0.020)));
        rampStep_ = (float)(1.0 / std::max(1.0, sr_ * 0.002));
        // room for the longest lookahead, so changing it never allocates
        const int dmax = (int)std::lround(sr_ * LookaheadLimiter::kMaxLookaheadMs / 1000.0) + LookaheadLimiter::kTpHalf + 4;
        dry_.assign((size_t)(2 * std::max(1, dmax)), 0.0f);
        dryPos_ = 0;
        reset();
    }

    /// Clears the signal history; the next block starts directly with the current settings.
    void reset()
    {
        lim_.reset();
        game_.reset();
        std::fill(dry_.begin(), dry_.end(), 0.0f);
        dryPos_ = 0;
        snap_ = true;
        inPeak_ = 0.0f;
        gameGrDb_ = 0.0f;
        switch_ = kSteady;
        switchGain_ = 1.0f;
    }

    /// 1…10 ms (default 5). The new latency is reported at once; the sound switches over with a short fade.
    void setLookaheadMs(float ms)
    {
        ms = std::max((float)LookaheadLimiter::kMinLookaheadMs, std::min((float)LookaheadLimiter::kMaxLookaheadMs,
                                                                      std::isfinite(ms) ? ms : 5.0f));
        if (std::fabs(ms - lookMs_) < 1e-3f) return;
        lookMs_ = ms;
        if (snap_) { applyLookahead(); return; }   // nothing played yet: switch right away
        switch_ = kFadingOut;
    }
    float lookaheadMs() const { return lookMs_; }

    void setInputGainDb(float db) { gainT_ = dbToGain(db); }
    void setCeilingDb(float db) { lim_.setCeilingDb(db); }
    void setReleaseMs(float ms) { lim_.setReleaseMs(ms); }
    void setAutoRelease(bool on) { lim_.setAutoRelease(on); }
    void setTruePeak(bool on) { lim_.setTruePeak(on); }
    void setMode(int mode) { game_.setMode(mode); }
    void setAmount(float amount01) { game_.setAmount(amount01); }
    void setBypass(bool on) { mixT_ = on ? 1.0f : 0.0f; }

    /// For the lookahead asked for (during a switch the limiter may still run the old one for a few ms).
    int latencySamples() const
    {
        return std::max(2, (int)std::lround(sr_ * lookMs_ / 1000.0)) - 1 + LookaheadLimiter::kTpHalf;
    }
    uint64_t clampCount() const { return lim_.clampCount(); }

    /// Stereo; in and out may be the same buffers. Any number of frames.
    void process(const float* inL, const float* inR, float* outL, float* outR, int frames)
    {
        if (snap_)
        {
            gain_ = gainT_;
            mix_ = mixT_;
            game_.snap();
            snap_ = false;
        }
        for (int pos = 0; pos < frames; pos += kChunk)
        {
            const int n = std::min(kChunk, frames - pos);
            if (switch_ == kFadingOut && switchGain_ <= 0.0f)
            {
                applyLookahead();
                switch_ = kWaiting;
                waitLeft_ = lim_.latencySamples();   // the new delay line starts empty
            }
            const int d = lim_.latencySamples();
            // dry copy (for bypass) and input gain
            for (int i = 0; i < n; ++i)
            {
                const float l = inL[pos + i], r = inR[pos + i];
                dryIn_[0][i] = l;
                dryIn_[1][i] = r;
                gain_ += (gainT_ - gain_) * k_;
                wet_[0][i] = l * gain_;
                wet_[1][i] = r * gain_;
                inPeak_ = std::max(inPeak_, std::max(std::fabs(wet_[0][i]), std::fabs(wet_[1][i])));
            }
            game_.process(wet_[0], wet_[1], n);
            if (game_.mode() != GameProcessor::kOff) gameGrDb_ = std::min(gameGrDb_, game_.takeReductionDb());
            const float* in[2] = { wet_[0], wet_[1] };
            float* out[2] = { wet_[0], wet_[1] };
            lim_.process(in, out, 2, n);
            lim_.takeInputPeak();
            // bypass cross-fade against the dry signal delayed by the limiter's latency
            for (int i = 0; i < n; ++i)
            {
                float dl = dryIn_[0][i], dr = dryIn_[1][i];
                if (d > 0)
                {
                    float& sl = dry_[(size_t)dryPos_ * 2];
                    float& sr = dry_[(size_t)dryPos_ * 2 + 1];
                    std::swap(dl, sl);
                    std::swap(dr, sr);
                    if (++dryPos_ == d) dryPos_ = 0;
                }
                mix_ += (mixT_ - mix_) * k_;
                if (std::fabs(mix_ - mixT_) < 1e-5f) mix_ = mixT_;
                float yl, yr;
                if (mix_ <= 0.0f) { yl = wet_[0][i]; yr = wet_[1][i]; }
                else if (mix_ >= 1.0f) { yl = dl; yr = dr; }
                else { yl = wet_[0][i] + (dl - wet_[0][i]) * mix_; yr = wet_[1][i] + (dr - wet_[1][i]) * mix_; }
                if (switch_ != kSteady)
                {
                    if (switch_ == kFadingOut) switchGain_ = std::max(0.0f, switchGain_ - rampStep_);
                    else if (switch_ == kWaiting) { switchGain_ = 0.0f; if (--waitLeft_ <= 0) switch_ = kFadingIn; }
                    else if ((switchGain_ += rampStep_) >= 1.0f) { switchGain_ = 1.0f; switch_ = kSteady; }
                    const float w = 0.5f - 0.5f * std::cos(3.14159265f * switchGain_);   // raised cosine
                    yl *= w;
                    yr *= w;
                }
                outL[pos + i] = yl;
                outR[pos + i] = yr;
                outPeak_ = std::max(outPeak_, std::max(std::fabs(yl), std::fabs(yr)));
            }
        }
    }

    /// Meters since the previous call.
    float takeInputPeak() { const float v = inPeak_; inPeak_ = 0.0f; return v; }
    float takeOutputPeak() { lim_.takeOutputPeak(); const float v = outPeak_; outPeak_ = 0.0f; return v; }
    /// Strongest reduction (dB, ≤ 0): limiter + Game mode. 0 while bypassed.
    float takeReductionDb()
    {
        const float v = std::max(-40.0f, lim_.takeGainReductionDb() + gameGrDb_);
        gameGrDb_ = 0.0f;
        return mixT_ >= 1.0f ? 0.0f : std::min(0.0f, v);
    }
    int mode() const { return game_.mode(); }

private:
    static constexpr int kChunk = 256;
    enum Switch { kSteady, kFadingOut, kWaiting, kFadingIn };

    void applyLookahead()
    {
        lim_.setLookaheadMs(lookMs_);
        std::fill(dry_.begin(), dry_.end(), 0.0f);
        dryPos_ = 0;
    }

    float lookMs_ = 5.0f, switchGain_ = 1.0f, rampStep_ = 0.01f;
    Switch switch_ = kSteady;
    int waitLeft_ = 0;

    double sr_ = 48000.0;
    LookaheadLimiter lim_;
    GameProcessor game_;
    float k_ = 0.001f, gain_ = 1.0f, gainT_ = 1.0f, mix_ = 0.0f, mixT_ = 0.0f;
    float inPeak_ = 0.0f, outPeak_ = 0.0f, gameGrDb_ = 0.0f;
    bool snap_ = true;
    std::vector<float> dry_;
    int dryPos_ = 0;
    float dryIn_[2][kChunk];
    float wet_[2][kChunk];
};

} // namespace vgl
