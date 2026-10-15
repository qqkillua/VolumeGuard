// VolumeGuard audio engine — the processing chain, the same as in the VolumeGuard Limiter plugin:
//   input gain → Game mode (Comfort / Tactical) → 5 ms lookahead limiter
// Plain C++, tested offline (tests/test_processor.cpp).
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#pragma once

#include "GameDsp.hpp"
#include "LimiterDsp.hpp"

#include <algorithm>
#include <cmath>

namespace vga {

struct Params
{
    int mode = 1;              // 0 limiter only, 1 Comfort, 2 Tactical
    float amount = 50.0f;      // %
    float ceilingDb = -1.0f;
    float inputGainDb = 0.0f;
    float releaseMs = 100.0f;
    bool bypass = false;

    bool operator==(const Params& o) const
    {
        return mode == o.mode && amount == o.amount && ceilingDb == o.ceilingDb && inputGainDb == o.inputGainDb
            && releaseMs == o.releaseMs && bypass == o.bypass;
    }
    bool operator!=(const Params& o) const { return !(*this == o); }

    Params sanitized() const
    {
        Params p = *this;
        p.mode = std::max(0, std::min(2, p.mode));
        p.amount = std::max(0.0f, std::min(100.0f, std::isfinite(p.amount) ? p.amount : 50.0f));
        p.ceilingDb = std::max(-24.0f, std::min(0.0f, std::isfinite(p.ceilingDb) ? p.ceilingDb : -1.0f));
        p.inputGainDb = std::max(-24.0f, std::min(24.0f, std::isfinite(p.inputGainDb) ? p.inputGainDb : 0.0f));
        p.releaseMs = std::max(10.0f, std::min(1000.0f, std::isfinite(p.releaseMs) ? p.releaseMs : 100.0f));
        return p;
    }
};

class Processor
{
public:
    void prepare(double sampleRate)
    {
        lim_.prepare(sampleRate, 2, 5.0);
        lim_.setInputGainDb(0.0f);          // the gain is applied before the Game mode, below
        game_.prepare(sampleRate);
        apply(params_);
    }

    void setParams(const Params& p)
    {
        const Params s = p.sanitized();
        if (s == params_) return;
        params_ = s;
        apply(s);
    }

    const Params& params() const { return params_; }
    int latencyFrames() const { return lim_.latencySamples(); }

    /// In place, any number of frames.
    void process(float* l, float* r, int frames)
    {
        if (params_.bypass)
        {
            for (int i = 0; i < frames; ++i) inPeak_ = std::max(inPeak_, std::max(std::fabs(l[i]), std::fabs(r[i])));
            const float* in[2] = { l, r };
            float* out[2] = { l, r };
            lim_.process(in, out, 2, frames, true);   // dry, but with the same delay, so switching does not jump
            lim_.takeInputPeak();
            lim_.takeGainReductionDb();
            return;
        }
        for (int i = 0; i < frames; ++i)
        {
            l[i] *= gain_;
            r[i] *= gain_;
            inPeak_ = std::max(inPeak_, std::max(std::fabs(l[i]), std::fabs(r[i])));
        }
        game_.process(l, r, frames);
        if (game_.mode() != vgl::GameProcessor::kOff) gameGrDb_ = std::min(gameGrDb_, game_.takeReductionDb());
        const float* in[2] = { l, r };
        float* out[2] = { l, r };
        lim_.process(in, out, 2, frames);
        lim_.takeInputPeak();
    }

    /// Meter values since the previous call.
    float takeInputPeak() { const float v = inPeak_; inPeak_ = 0.0f; return v; }
    float takeOutputPeak() { return lim_.takeOutputPeak(); }
    float takeReductionDb()
    {
        const float v = std::max(-40.0f, lim_.takeGainReductionDb() + gameGrDb_);
        gameGrDb_ = 0.0f;
        return std::min(0.0f, v);
    }

private:
    void apply(const Params& p)
    {
        gain_ = vgl::dbToGain(p.inputGainDb);
        lim_.setCeilingDb(p.ceilingDb);
        lim_.setReleaseMs(p.releaseMs);
        game_.setMode(p.mode);
        game_.setAmount(p.amount / 100.0f);
    }

    Params params_;
    vgl::LookaheadLimiter lim_;
    vgl::GameProcessor game_;
    float gain_ = 1.0f, inPeak_ = 0.0f, gameGrDb_ = 0.0f;
};

} // namespace vga
