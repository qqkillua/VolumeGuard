// VolumeGuard Limiter — lookahead brickwall peak limiter.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// How it works, per sample:
//   1. peak    = loudest channel after the input gain
//   2. need    = gain that would bring this peak down to the ceiling (1 if it is already below)
//   3. hold    = smallest "need" over the last L samples (sliding minimum)
//   4. release = hold, but allowed to rise only slowly (one-pole, release time)
//   5. gain    = average of "release" over the last L samples (box filter, a smooth fade-in)
//   6. output  = input delayed by L-1 samples × gain
// Every value averaged in step 5 at the moment a peak leaves the delay line already "knew" that
// peak (step 3), so the gain applied to it is at most the gain it needs: the ceiling is never exceeded.
// The fade-in over L samples avoids clicks. L = lookahead (5 ms by default); latency = L-1 samples.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vgl {

inline float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
inline float gainToDb(float g) { return g <= 1e-9f ? -180.0f : 20.0f * std::log10(g); }

class LookaheadLimiter
{
public:
    static constexpr int kMaxChannels = 8;

    void prepare(double sampleRate, int channels, double lookaheadMs = 5.0)
    {
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        channels_ = std::max(1, std::min(channels, kMaxChannels));
        L_ = std::max(2, (int)std::lround(sr_ * lookaheadMs / 1000.0));
        D_ = L_ - 1;
        delay_.assign((size_t)channels_ * (size_t)D_, 0.0f);
        box_.assign((size_t)L_, 1.0f);
        dqVal_.assign((size_t)L_ + 1, 1.0f);
        dqIdx_.assign((size_t)L_ + 1, 0);
        setReleaseMs(releaseMs_);
        reset();
    }

    void reset()
    {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        std::fill(box_.begin(), box_.end(), 1.0f);
        boxSum_ = (double)L_;
        boxPos_ = 0;
        sinceResum_ = 0;
        dPos_ = 0;
        dqHead_ = dqCount_ = 0;
        n_ = 0;
        rel_ = 1.0f;
        minGain_ = 1.0f;
    }

    void setCeilingDb(float db) { ceiling_ = dbToGain(std::min(0.0f, db)); }
    void setInputGainDb(float db) { inGain_ = dbToGain(db); }
    void setReleaseMs(float ms)
    {
        releaseMs_ = std::max(0.1f, ms);
        relCoef_ = (float)std::exp(-1.0 / (sr_ * releaseMs_ / 1000.0));
    }

    int latencySamples() const { return D_; }

    /// Lowest gain applied since the last call, in dB (0 = no reduction). For the meter.
    float takeGainReductionDb()
    {
        const float g = minGain_;
        minGain_ = 1.0f;
        return std::min(0.0f, gainToDb(g));
    }

    /// in and out may point to the same buffers. bypass keeps the latency but applies no gain.
    void process(const float* const* in, float* const* out, int channels, int frames, bool bypass = false)
    {
        const int nch = std::min(channels, channels_);
        float x[kMaxChannels];

        for (int i = 0; i < frames; ++i)
        {
            // 1-2. loudest channel and the gain it needs
            float peak = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                x[c] = in[c][i] * inGain_;
                peak = std::max(peak, std::fabs(x[c]));
            }
            const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;

            // 3. sliding minimum over the last L samples (monotonic queue)
            while (dqCount_ > 0 && dqVal_[back()] >= need) --dqCount_;
            const int slot = (dqHead_ + dqCount_) % (L_ + 1);
            dqVal_[(size_t)slot] = need;
            dqIdx_[(size_t)slot] = n_;
            ++dqCount_;
            while (dqIdx_[(size_t)dqHead_] <= n_ - L_) { dqHead_ = (dqHead_ + 1) % (L_ + 1); --dqCount_; }
            const float hold = dqVal_[(size_t)dqHead_];

            // 4. instant down, slow up
            rel_ = hold < rel_ ? hold : hold + (rel_ - hold) * relCoef_;

            // 5. box filter (running sum, recomputed now and then so rounding errors cannot build up)
            boxSum_ += (double)rel_ - (double)box_[(size_t)boxPos_];
            box_[(size_t)boxPos_] = rel_;
            if (++boxPos_ == L_) boxPos_ = 0;
            if (++sinceResum_ >= 65536)
            {
                sinceResum_ = 0;
                double s = 0.0;
                for (float v : box_) s += v;
                boxSum_ = s;
            }
            const float gain = std::min(1.0f, (float)(boxSum_ / (double)L_));
            minGain_ = std::min(minGain_, gain);

            // 6. delay and apply; the final clamp only guards against float rounding
            for (int c = 0; c < nch; ++c)
            {
                float& d = delay_[(size_t)c * (size_t)D_ + (size_t)dPos_];
                const float delayed = d;
                d = x[c];
                const float y = bypass ? delayed / inGain_ : delayed * gain;
#ifdef VGL_TEST_NO_CLAMP
                out[c][i] = y;   // tests: prove the algorithm alone keeps the ceiling
#else
                out[c][i] = bypass ? y : std::max(-ceiling_, std::min(ceiling_, y));
#endif
            }
            for (int c = nch; c < channels; ++c) out[c][i] = 0.0f;
            if (++dPos_ == D_) dPos_ = 0;
            ++n_;
        }
    }

private:
    int back() const { return (dqHead_ + dqCount_ - 1) % (L_ + 1); }

    double sr_ = 48000.0;
    int channels_ = 2, L_ = 240, D_ = 239;
    float ceiling_ = dbToGain(-1.0f), inGain_ = 1.0f, releaseMs_ = 100.0f, relCoef_ = 0.0f;

    std::vector<float> delay_;
    int dPos_ = 0;

    std::vector<float> dqVal_;
    std::vector<int64_t> dqIdx_;
    int dqHead_ = 0, dqCount_ = 0;
    int64_t n_ = 0;

    float rel_ = 1.0f;
    std::vector<float> box_;
    double boxSum_ = 0.0;
    int boxPos_ = 0, sinceResum_ = 0;

    float minGain_ = 1.0f;
};

} // namespace vgl
