// VolumeGuard Limiter — lookahead brickwall peak limiter.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
//
// How it works, per sample:
//   1. peak    = loudest channel after the input gain; with True peak (on by default) also the loudest
//                point BETWEEN the samples, found by 4× interpolation (a DAC or a later resampler
//                rebuilds the wave there, and it can be up to ~3 dB above the samples)
//   2. need    = gain that would bring this peak down to the ceiling (1 if it is already below)
//   3. hold    = smallest "need" over the last L samples (sliding minimum)
//   4. release = hold, but allowed to rise only slowly (one-pole in decibels, release time)
//   5. gain    = "release" averaged twice: over A samples, and that over B samples, A + B = L + 1
//                (two box filters in a row: an S-shaped fade-in with no corners)
//   6. output  = input delayed by L-1 samples × gain
// The two averages together reach back A + B - 2 = L - 1 samples, exactly the delay, so every value they
// take in at the moment a peak leaves the delay line already "knew" that peak (step 3): the gain applied
// to it is at most the gain it needs, and the ceiling is never exceeded. A single box over L samples
// (VolumeGuard 2.8.4 and earlier) fades in along a straight line whose corners add a little distortion,
// most audible on bass; two boxes round those corners off at the same latency.
// L = lookahead (5 ms by default); latency = L-1 samples, plus kTpHalf samples for the true peak
// interpolator (always, so switching True peak does not change it).
//
// Auto release (on by default): the release time adapts to the sound. A single short peak (a shot) is
// released twice as fast as the Release setting, so the quiet sounds after it come back quickly; when the
// limiter has been working for a while (a long explosion, a loud scene) the release gets up to 3× slower,
// so the level does not pump up and down between the peaks. Release time only shapes how the gain comes
// back up, so the ceiling guarantee above is not affected.
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

    static constexpr double kMinLookaheadMs = 1.0, kMaxLookaheadMs = 10.0;

    /// Allocates for the longest lookahead (kMaxLookaheadMs), so setLookaheadMs never allocates.
    void prepare(double sampleRate, int channels, double lookaheadMs = 5.0)
    {
        sr_ = sampleRate > 0 ? sampleRate : 48000.0;
        channels_ = std::max(1, std::min(channels, kMaxChannels));
        Lmax_ = std::max(2, (int)std::lround(sr_ * kMaxLookaheadMs / 1000.0));
        Dmax_ = Lmax_ - 1;
        if (!tpReady_) buildTruePeakFilter();
        delay_.assign((size_t)channels_ * (size_t)Dmax_, 0.0f);
        ceilDelay_.assign((size_t)Dmax_, ceiling_);
        box_.assign((size_t)Lmax_, 1.0f);
        box2_.assign((size_t)Lmax_, 1.0);
        dqVal_.assign((size_t)Lmax_ + 1, 1.0f);
        dqIdx_.assign((size_t)Lmax_ + 1, 0);
        setReleaseMs(releaseMs_);
        applyLookahead(lookaheadMs);
        reset();
    }

    /// How far ahead the limiter looks, 1…10 ms. Longer: the gain comes down more gently (cleaner, less
    /// distortion on bass); shorter: less delay. Changing it clears the limiter's history (call it between
    /// sounds, or fade around it — the Chain does), and changes latencySamples(). Never allocates.
    void setLookaheadMs(double ms)
    {
        const int before = L_;
        applyLookahead(ms);
        if (L_ != before) reset();
    }
    double lookaheadMs() const { return 1000.0 * L_ / sr_; }

    void reset()
    {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        for (auto& h : tpHist_) std::fill(std::begin(h), std::end(h), 0.0f);
        tpPos_ = 0;
        prevInterPeak_ = 0.0f;
        std::fill(ceilDelay_.begin(), ceilDelay_.end(), ceiling_);
        clamps_ = 0;
        std::fill(box_.begin(), box_.end(), 1.0f);
        std::fill(box2_.begin(), box2_.end(), 1.0);
        boxSum_ = (double)A_;   // only the first A_ / B_ entries are in use
        box2Sum_ = (double)B_;
        boxPos_ = box2Pos_ = 0;
        sinceResum_ = 0;
        dPos_ = 0;
        dqHead_ = dqCount_ = 0;
        n_ = 0;
        rel_ = 1.0f;
        busy_ = 0.0f;
        sinceBusy_ = busyGap_;
        minGain_ = 1.0f;
        inPeak_ = outPeak_ = 0.0f;
    }

    void setCeilingDb(float db) { ceiling_ = dbToGain(std::min(0.0f, db)); }
    void setInputGainDb(float db) { inGain_ = dbToGain(db); }
    void setReleaseMs(float ms)
    {
        releaseMs_ = std::max(0.1f, ms);
        relCoef_ = coefMs(releaseMs_ * kDbReleaseScale);
        relFastS_ = (float)(sr_ * releaseMs_ * kDbReleaseScale * kFastFactor / 1000.0);   // in samples
        relSlowS_ = (float)(sr_ * releaseMs_ * kDbReleaseScale * kSlowFactor / 1000.0);
        busyUp_ = 1.0f - coefMs(400.0f);     // ~0.4 s of limiting → counted as "busy"
        busyDown_ = 1.0f - coefMs(1500.0f);  // forgotten over ~1.5 s
        busyGap_ = (int)(sr_ * 0.1);
    }

    /// Release adapts to the sound (see the top of the file). On by default.
    void setAutoRelease(bool on) { autoRelease_ = on; }
    bool autoRelease() const { return autoRelease_; }
    /// How busy the limiter has been lately, 0…1 (what auto release goes by). For tests.
    float busy() const { return busy_; }

    static constexpr float kFastFactor = 0.5f, kSlowFactor = 3.0f;
    /// The release works in decibels (step 4). With the same time constant that reaches -1 dB later than the old
    /// release in linear gain did (the last decibel takes longest); this scale makes the time back to -1 dB after
    /// a typical 6–20 dB reduction about the same as before, so the Release setting keeps its meaning.
    static constexpr float kDbReleaseScale = 0.78f;

    int latencySamples() const { return D_ + kTpHalf; }

    /// Also catch the peaks between samples (see the top of the file). On by default.
    void setTruePeak(bool on) { truePeak_ = on; }
    bool truePeak() const { return truePeak_; }

    static constexpr int kTpHalf = 8;            // interpolator: 16 taps, 8 samples of delay
    static constexpr int kTpTaps = 2 * kTpHalf;
    static constexpr int kTpPhases = 3;          // points at 1/4, 2/4 and 3/4 between two samples
    static constexpr float kTpMargin = 1.0116f;  // +0.1 dB on the points between samples

    /// Lowest gain applied since the last call, in dB (0 = no reduction). For the meter.
    float takeGainReductionDb()
    {
        const float g = minGain_;
        minGain_ = 1.0f;
        return std::min(0.0f, gainToDb(g));
    }

    /// Loudest input (after Input Gain) and output sample since the last call, linear. For the meters.
    float takeInputPeak() { const float v = inPeak_; inPeak_ = 0.0f; return v; }
    /// How many samples the final safety clamp had to change. Should stay 0: the algorithm itself keeps the
    /// ceiling; the clamp is only a guard against float rounding.
    uint64_t clampCount() const { return clamps_; }
    float takeOutputPeak() { const float v = outPeak_; outPeak_ = 0.0f; return v; }

    /// in and out may point to the same buffers. bypass keeps the latency but applies no gain.
    void process(const float* const* in, float* const* out, int channels, int frames, bool bypass = false)
    {
        const int nch = std::min(channels, channels_);
        float x[kMaxChannels];

        for (int i = 0; i < frames; ++i)
        {
            // 1-2. loudest channel and the gain it needs. The interpolator needs kTpHalf samples of the
            //      future, so everything from here on works on the sample from kTpHalf steps ago.
            float peak = 0.0f, inter = 0.0f;
            for (int c = 0; c < nch; ++c)
            {
                float* h = tpHist_[c];
                h[tpPos_] = h[tpPos_ + kTpTaps] = in[c][i] * inGain_;   // doubled ring: always contiguous
                const float* w = h + tpPos_ + 1;                         // the last kTpTaps samples, oldest first
                x[c] = w[kTpHalf - 1];
                peak = std::max(peak, std::fabs(x[c]));
                if (truePeak_)
                    for (int ph = 0; ph < kTpPhases; ++ph)
                    {
                        const float* k = tpCoef_[ph];
                        float v = 0.0f;
                        for (int j = 0; j < kTpTaps; ++j) v += k[j] * w[j];
                        inter = std::max(inter, std::fabs(v));
                    }
            }
            if (++tpPos_ == kTpTaps) tpPos_ = 0;
            inter *= kTpMargin;   // the gain changes from sample to sample while limiting, which can add a little
            // this sample, and the wave just before it and just after it
            const float tp = std::max(peak, std::max(inter, prevInterPeak_));
            prevInterPeak_ = inter;
            peak = tp;
            const float need = peak > ceiling_ ? ceiling_ / peak : 1.0f;
            inPeak_ = std::max(inPeak_, peak);

            // 3. sliding minimum over the last L samples (monotonic queue)
            while (dqCount_ > 0 && dqVal_[back()] >= need) --dqCount_;
            const int slot = (dqHead_ + dqCount_) % (L_ + 1);
            dqVal_[(size_t)slot] = need;
            dqIdx_[(size_t)slot] = n_;
            ++dqCount_;
            while (dqIdx_[(size_t)dqHead_] <= n_ - L_) { dqHead_ = (dqHead_ + 1) % (L_ + 1); --dqCount_; }
            const float hold = dqVal_[(size_t)dqHead_];

            // 4. instant down, slow up (auto release: between fast and slow by how busy the limiter has been)
            if (autoRelease_)
            {
                // grows while reducing; short pauses (< 100 ms, the gaps of automatic fire) keep it; longer quiet forgets it
                if (hold < kBusyBelow) { sinceBusy_ = 0; busy_ += (1.0f - busy_) * busyUp_; }
                else if (sinceBusy_ < busyGap_) ++sinceBusy_;
                else if ((busy_ -= busy_ * busyDown_) < 1e-20f) busy_ = 0.0f;   // no denormals in long quiet
            }
            if (hold <= rel_) rel_ = hold;
            else
            {
                // the release TIME goes from fast to slow (interpolating the coefficients would lean to fast)
                const float coef = autoRelease_ ? std::exp(-1.0f / std::max(1.0f, relFastS_ + (relSlowS_ - relFastS_) * busy_))
                                                : relCoef_;
                // Back up in decibels (2.8.7 / Limiter 1.4.2): the gain in dB approaches the target exponentially,
                // so it comes back evenly. Before, it approached in linear gain: from -20 dB it jumped to about -5 dB
                // at once and then crawled the rest of the way, heard as a "breath" right after a shot.
                // Still never above hold (the ceiling guarantee needs rel_ <= hold).
                const float lh = std::log(hold);
                rel_ = std::min(hold, std::exp(lh + (std::log(rel_) - lh) * coef));
            }

            // 5. two box filters (running sums, recomputed now and then so rounding errors cannot build up)
            boxSum_ += (double)rel_ - (double)box_[(size_t)boxPos_];
            box_[(size_t)boxPos_] = rel_;
            if (++boxPos_ == A_) boxPos_ = 0;
            const double avg = boxSum_ / (double)A_;
            box2Sum_ += avg - box2_[(size_t)box2Pos_];
            box2_[(size_t)box2Pos_] = avg;
            if (++box2Pos_ == B_) box2Pos_ = 0;
            if (++sinceResum_ >= 65536)
            {
                sinceResum_ = 0;
                double s = 0.0, s2 = 0.0;
                for (int k = 0; k < A_; ++k) s += box_[(size_t)k];
                for (int k = 0; k < B_; ++k) s2 += box2_[(size_t)k];
                boxSum_ = s;
                box2Sum_ = s2;
            }
            const float gain = std::min(1.0f, (float)(box2Sum_ / (double)B_));
            minGain_ = std::min(minGain_, gain);

            // 6. delay and apply. The clamp uses the ceiling that was in force when this sample's gain was
            //    worked out (it travels through the delay too): when the ceiling is lowered, samples already
            //    in the delay line keep their old one for 5 ms instead of being hard-clipped (a click).
            float& cd = ceilDelay_[(size_t)dPos_];
            const float ceil = cd;
            cd = ceiling_;
            for (int c = 0; c < nch; ++c)
            {
                float& d = delay_[(size_t)c * (size_t)Dmax_ + (size_t)dPos_];
                const float delayed = d;
                d = x[c];
                const float y = bypass ? delayed / inGain_ : delayed * gain;
                if (!bypass && std::fabs(y) > ceil * 1.000001f) ++clamps_;
#ifdef VGL_TEST_NO_CLAMP
                out[c][i] = y;   // tests: prove the algorithm alone keeps the ceiling
#else
                out[c][i] = bypass ? y : std::max(-ceil, std::min(ceil, y));
#endif
                outPeak_ = std::max(outPeak_, std::fabs(out[c][i]));
            }
            for (int c = nch; c < channels; ++c) out[c][i] = 0.0f;
            if (++dPos_ == D_) dPos_ = 0;
            ++n_;
        }
    }

private:
    // windowed sinc (Kaiser, beta 6), each phase normalised to unity gain at DC
    void buildTruePeakFilter()
    {
        auto bessel0 = [](double v) {
            double s = 1.0, term = 1.0;
            for (int k = 1; k < 30; ++k) { term *= (v / (2.0 * k)) * (v / (2.0 * k)); s += term; }
            return s;
        };
        const double beta = 6.0, pi = 3.14159265358979323846;
        for (int ph = 0; ph < kTpPhases; ++ph)
        {
            const double f = (ph + 1) / 4.0;
            double sum = 0.0, k[kTpTaps];
            for (int j = 0; j < kTpTaps; ++j)
            {
                const double d = kTpHalf - 1 + f - j;   // distance from the point to sample j
                const double sinc = std::sin(pi * d) / (pi * d);
                const double r = d / kTpHalf;
                k[j] = sinc * bessel0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(beta);
                sum += k[j];
            }
            for (int j = 0; j < kTpTaps; ++j) tpCoef_[ph][j] = (float)(k[j] / sum);
        }
        tpReady_ = true;
    }

    static constexpr float kBusyBelow = 0.97f;   // more than ~0.25 dB of reduction counts as "working"
    float coefMs(float ms) const { return (float)std::exp(-1.0 / (sr_ * std::max(0.1f, ms) / 1000.0)); }
    int back() const { return (dqHead_ + dqCount_ - 1) % (L_ + 1); }

    void applyLookahead(double ms)
    {
        ms = std::max(kMinLookaheadMs, std::min(kMaxLookaheadMs, std::isfinite(ms) ? ms : 5.0));
        L_ = std::max(2, std::min(Lmax_, (int)std::lround(sr_ * ms / 1000.0)));
        D_ = L_ - 1;
        A_ = (L_ + 1) / 2;   // the two boxes: A + B = L + 1, both ≥ 1 and ≤ L
        B_ = L_ + 1 - A_;
    }

    double sr_ = 48000.0;
    int channels_ = 2, L_ = 240, D_ = 239, A_ = 120, B_ = 121, Lmax_ = 480, Dmax_ = 479;
    float ceiling_ = dbToGain(-1.0f), inGain_ = 1.0f, releaseMs_ = 100.0f, relCoef_ = 0.0f;

    std::vector<float> delay_, ceilDelay_;
    int dPos_ = 0;
    uint64_t clamps_ = 0;

    std::vector<float> dqVal_;
    std::vector<int64_t> dqIdx_;
    int dqHead_ = 0, dqCount_ = 0;
    int64_t n_ = 0;

    float rel_ = 1.0f;
    bool autoRelease_ = true;
    bool truePeak_ = true, tpReady_ = false;
    float tpCoef_[kTpPhases][kTpTaps] = {};
    float tpHist_[kMaxChannels][2 * kTpTaps] = {};
    int tpPos_ = 0;
    float prevInterPeak_ = 0.0f;
    float relFastS_ = 1.0f, relSlowS_ = 1.0f, busy_ = 0.0f, busyUp_ = 0.0f, busyDown_ = 0.0f;
    int sinceBusy_ = 1 << 30, busyGap_ = 4800;
    std::vector<float> box_;
    std::vector<double> box2_;
    double boxSum_ = 0.0, box2Sum_ = 0.0;
    int boxPos_ = 0, box2Pos_ = 0, sinceResum_ = 0;

    float minGain_ = 1.0f;
    float inPeak_ = 0.0f, outPeak_ = 0.0f;
};

} // namespace vgl
