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
        kInputLevel,      // output: meter
        kOutputLevel,     // output: meter
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
        fValues[kInputLevel] = -60.0f;
        fValues[kOutputLevel] = -60.0f;
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
    uint32_t getVersion() const override { return d_version(1, 1, 1); }
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
            // A plain on/off parameter of the plugin. It is deliberately NOT the host's special VST3 bypass
            // parameter: some hosts disable the plugin and close its window when the plugin itself flips that
            // one, which crashed the host when the button in our window was clicked.
            p.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger;
            p.name = "Bypass";
            p.symbol = "bypass";
            p.ranges.min = 0.0f;
            p.ranges.max = 1.0f;
            p.ranges.def = 0.0f;
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
        case kInputLevel:
        case kOutputLevel:
            p.hints = kParameterIsOutput;
            p.name = index == kInputLevel ? "Input Level" : "Output Level";
            p.shortName = index == kInputLevel ? "In" : "Out";
            p.symbol = index == kInputLevel ? "input_level" : "output_level";
            p.unit = "dB";
            p.ranges.min = -60.0f;
            p.ranges.max = 6.0f;
            p.ranges.def = -60.0f;
            break;
        }
    }

    float getParameterValue(uint32_t index) const override
    {
        return index < kParameterCount ? fValues[index] : 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        if (index >= kParameterCount || index >= kGainReduction)
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
        if (frames == 0 || inputs == nullptr || outputs == nullptr
            || inputs[0] == nullptr || inputs[1] == nullptr || outputs[0] == nullptr || outputs[1] == nullptr)
            return;
        fLimiter.process(inputs, outputs, 2, (int)frames, fValues[kBypass] > 0.5f);

        // Meters: jump up at once, fall back at 20 dB per second so they are readable.
        const float fall = 20.0f * (float)frames / (float)getSampleRate();
        const float gr = std::max(-40.0f, fLimiter.takeGainReductionDb());
        const float in = std::max(-60.0f, vgl::gainToDb(fLimiter.takeInputPeak()));
        const float out = std::max(-60.0f, vgl::gainToDb(fLimiter.takeOutputPeak()));
        fValues[kGainReduction] = std::min(gr, std::min(0.0f, fValues[kGainReduction] + fall));
        fValues[kInputLevel] = std::min(6.0f, std::max(in, fValues[kInputLevel] - fall));
        fValues[kOutputLevel] = std::min(6.0f, std::max(out, fValues[kOutputLevel] - fall));
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
