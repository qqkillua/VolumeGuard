// VolumeGuard audio engine — the processing chain: exactly the plugin's (plugin/src/ChainDsp.hpp),
//   input gain → Game mode (Comfort / Tactical) → 5 ms lookahead limiter, with click-free setting changes
// Plain C++, tested offline (tests/test_processor.cpp).
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#pragma once

#include "ChainDsp.hpp"

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
        chain_.prepare(sampleRate);
        apply(params_);
        chain_.reset();   // start directly with the current settings
    }

    void setParams(const Params& p)
    {
        const Params s = p.sanitized();
        if (s == params_) return;
        params_ = s;
        apply(s);           // the chain glides to the new values: no clicks
    }

    const Params& params() const { return params_; }
    int latencyFrames() const { return chain_.latencySamples(); }
    uint64_t clampCount() const { return chain_.clampCount(); }

    /// In place, any number of frames.
    void process(float* l, float* r, int frames) { chain_.process(l, r, l, r, frames); }

    /// Meter values since the previous call.
    float takeInputPeak() { return chain_.takeInputPeak(); }
    float takeOutputPeak() { return chain_.takeOutputPeak(); }
    float takeReductionDb() { return chain_.takeReductionDb(); }

private:
    void apply(const Params& p)
    {
        chain_.setInputGainDb(p.inputGainDb);
        chain_.setCeilingDb(p.ceilingDb);
        chain_.setReleaseMs(p.releaseMs);
        chain_.setMode(p.mode);
        chain_.setAmount(p.amount / 100.0f);
        chain_.setBypass(p.bypass);
    }

    Params params_;
    vgl::Chain chain_;
};

} // namespace vga
