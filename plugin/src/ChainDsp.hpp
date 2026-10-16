// VolumeGuard Limiter — the whole processing chain, shared by the plugin and the app's sound processing:
//   input gain → Game mode (Comfort / Tactical) → lookahead limiter, with a click-free bypass.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// Nothing here jumps: input gain glides (~20 ms), Game mode settings glide and switch on/off with a fade,
// the limiter's ceiling may change at any time (its safety clamp follows the delayed signal), and bypass
// cross-fades (~20 ms) between the processed signal and the dry one delayed by the same latency.
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
        lim_.prepare(sr_, 2, 5.0);
        lim_.setInputGainDb(0.0f);            // the input gain is applied here, before the Game mode
        game_.prepare(sr_);
        k_ = (float)(1.0 - std::exp(-1.0 / (sr_ * 0.020)));
        const int d = lim_.latencySamples();
        dry_.assign((size_t)(2 * std::max(1, d)), 0.0f);
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
    }

    void setInputGainDb(float db) { gainT_ = dbToGain(db); }
    void setCeilingDb(float db) { lim_.setCeilingDb(db); }
    void setReleaseMs(float ms) { lim_.setReleaseMs(ms); }
    void setAutoRelease(bool on) { lim_.setAutoRelease(on); }
    void setTruePeak(bool on) { lim_.setTruePeak(on); }
    void setMode(int mode) { game_.setMode(mode); }
    void setAmount(float amount01) { game_.setAmount(amount01); }
    void setBypass(bool on) { mixT_ = on ? 1.0f : 0.0f; }

    int latencySamples() const { return lim_.latencySamples(); }
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
        const int d = lim_.latencySamples();
        for (int pos = 0; pos < frames; pos += kChunk)
        {
            const int n = std::min(kChunk, frames - pos);
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
