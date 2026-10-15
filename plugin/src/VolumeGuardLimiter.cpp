// VolumeGuard Limiter — DPF plugin wrapper around LookaheadLimiter.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#include "DistrhoPlugin.hpp"
#include "LimiterDsp.hpp"

START_NAMESPACE_DISTRHO

class VolumeGuardLimiter : public Plugin
{
public:
    enum Parameters
    {
        kInputGain = 0,
        kCeiling,
        kRelease,
        kBypass,
        kGainReduction,   // output: meter
        kParameterCount
    };

    VolumeGuardLimiter()
        : Plugin(kParameterCount, 0, 0)
    {
        fValues[kInputGain] = 0.0f;
        fValues[kCeiling] = -1.0f;
        fValues[kRelease] = 100.0f;
        fValues[kBypass] = 0.0f;
        fValues[kGainReduction] = 0.0f;
        prepare();
    }

protected:
    const char* getLabel() const override { return "VolumeGuardLimiter"; }
    const char* getDescription() const override
    {
        return "Lookahead brickwall limiter: keeps the output below the ceiling without clipping.";
    }
    const char* getMaker() const override { return "qqkillua"; }
    const char* getHomePage() const override { return "https://github.com/qqkillua/VolumeGuard"; }
    const char* getLicense() const override { return "MIT"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }
    int64_t getUniqueId() const override { return d_cconst('V', 'G', 'L', 'm'); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
    }

    void initParameter(uint32_t index, Parameter& p) override
    {
        switch (index)
        {
        case kInputGain:
            p.hints = kParameterIsAutomatable;
            p.name = "Input Gain";
            p.shortName = "Input";
            p.symbol = "input_gain";
            p.unit = "dB";
            p.ranges.min = -12.0f;
            p.ranges.max = 24.0f;
            p.ranges.def = 0.0f;
            break;
        case kCeiling:
            p.hints = kParameterIsAutomatable;
            p.name = "Ceiling";
            p.symbol = "ceiling";
            p.unit = "dB";
            p.ranges.min = -30.0f;
            p.ranges.max = 0.0f;
            p.ranges.def = -1.0f;
            break;
        case kRelease:
            p.hints = kParameterIsAutomatable | kParameterIsLogarithmic;
            p.name = "Release";
            p.symbol = "release";
            p.unit = "ms";
            p.ranges.min = 1.0f;
            p.ranges.max = 1000.0f;
            p.ranges.def = 100.0f;
            break;
        case kBypass:
            p.initDesignation(kParameterDesignationBypass);
            break;
        case kGainReduction:
            p.hints = kParameterIsOutput;
            p.name = "Gain Reduction";
            p.shortName = "GR";
            p.symbol = "gain_reduction";
            p.unit = "dB";
            p.ranges.min = -40.0f;
            p.ranges.max = 0.0f;
            p.ranges.def = 0.0f;
            break;
        }
    }

    float getParameterValue(uint32_t index) const override
    {
        return index < kParameterCount ? fValues[index] : 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        if (index >= kParameterCount || index == kGainReduction)
            return;
        fValues[index] = value;
        switch (index)
        {
        case kInputGain: fLimiter.setInputGainDb(value); break;
        case kCeiling:   fLimiter.setCeilingDb(value); break;
        case kRelease:   fLimiter.setReleaseMs(value); break;
        }
    }

    void activate() override { fLimiter.reset(); }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        fLimiter.process(inputs, outputs, 2, (int)frames, fValues[kBypass] > 0.5f);
        fValues[kGainReduction] = std::max(-40.0f, fLimiter.takeGainReductionDb());
    }

    void sampleRateChanged(double) override { prepare(); }

private:
    void prepare()
    {
        fLimiter.prepare(getSampleRate(), 2, 5.0);
        fLimiter.setInputGainDb(fValues[kInputGain]);
        fLimiter.setCeilingDb(fValues[kCeiling]);
        fLimiter.setReleaseMs(fValues[kRelease]);
        setLatency((uint32_t)fLimiter.latencySamples());
    }

    vgl::LookaheadLimiter fLimiter;
    float fValues[kParameterCount];

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VolumeGuardLimiter)
};

Plugin* createPlugin()
{
    return new VolumeGuardLimiter();
}

END_NAMESPACE_DISTRHO
