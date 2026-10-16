// VolumeGuard Limiter — DPF plugin: input gain → Game mode (optional) → lookahead limiter.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#include "DistrhoPlugin.hpp"
#include "ChainDsp.hpp"
#include "Presets.hpp"

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
        kMode,            // 0 Limiter, 1 Comfort, 2 Tactical (added in 1.2.0, so it comes after the meters)
        kAmount,          // Game mode strength, %
        kAutoRelease,     // release adapts to the sound (added in 1.3.0, so new ones come last)
        kTruePeak,        // also limit the peaks between samples (1.3.0)
        kParameterCount
    };

    VolumeGuardLimiter()
        : Plugin(kParameterCount, vgl::kPresetCount, 0)
    {
        fValues[kInputGain] = 0.0f;
        fValues[kCeiling] = -1.0f;
        fValues[kRelease] = 100.0f;
        fValues[kBypass] = 0.0f;
        fValues[kGainReduction] = 0.0f;
        fValues[kInputLevel] = -60.0f;
        fValues[kOutputLevel] = -60.0f;
        fValues[kMode] = 0.0f;
        fValues[kAmount] = 50.0f;
        fValues[kAutoRelease] = 1.0f;
        fValues[kTruePeak] = 1.0f;
        prepare();
    }

protected:
    const char* getLabel() const override { return "VolumeGuardLimiter"; }
    const char* getDescription() const override
    {
        return "Lookahead true-peak limiter with a Game mode that tames gunshots and rumble and lifts quiet footsteps.";
    }
    const char* getMaker() const override { return "qqkillua"; }
    const char* getHomePage() const override { return "https://github.com/qqkillua/VolumeGuard"; }
    const char* getLicense() const override { return "MIT"; }
    uint32_t getVersion() const override { return d_version(1, 3, 0); }
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
        case kMode:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.name = "Mode";
            p.symbol = "mode";
            p.ranges.min = 0.0f;
            p.ranges.max = 2.0f;
            p.ranges.def = 0.0f;
            p.enumValues.count = 3;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[3];
            p.enumValues.values[0] = ParameterEnumerationValue(0.0f, "Limiter");
            p.enumValues.values[1] = ParameterEnumerationValue(1.0f, "Comfort");
            p.enumValues.values[2] = ParameterEnumerationValue(2.0f, "Tactical");
            break;
        case kAmount:
            p.hints = kParameterIsAutomatable;
            p.name = "Amount";
            p.symbol = "amount";
            p.unit = "%";
            p.ranges.min = 0.0f;
            p.ranges.max = 100.0f;
            p.ranges.def = 50.0f;
            break;
        case kAutoRelease:
            p.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger;
            p.name = "Auto Release";
            p.shortName = "Auto Rel";
            p.symbol = "auto_release";
            p.ranges.min = 0.0f;
            p.ranges.max = 1.0f;
            p.ranges.def = 1.0f;
            break;
        case kTruePeak:
            p.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger;
            p.name = "True Peak";
            p.shortName = "TP";
            p.symbol = "true_peak";
            p.ranges.min = 0.0f;
            p.ranges.max = 1.0f;
            p.ranges.def = 1.0f;
            break;
        }
    }

    void initProgramName(uint32_t index, String& name) override
    {
        if (index < (uint32_t)vgl::kPresetCount) name = vgl::kPresets[index].name;
    }

    void loadProgram(uint32_t index) override
    {
        if (index >= (uint32_t)vgl::kPresetCount) return;
        const vgl::Preset& p = vgl::kPresets[index];
        setParameterValue(kInputGain, p.inputGainDb);
        setParameterValue(kCeiling, p.ceilingDb);
        setParameterValue(kRelease, p.releaseMs);
        setParameterValue(kMode, p.mode);
        setParameterValue(kAmount, p.amount);
        setParameterValue(kAutoRelease, p.autoRelease);
    }

    float getParameterValue(uint32_t index) const override
    {
        return index < kParameterCount ? fValues[index] : 0.0f;
    }

    void setParameterValue(uint32_t index, float value) override
    {
        if (index >= kParameterCount || (index >= kGainReduction && index <= kOutputLevel))
            return;
        fValues[index] = value;
        switch (index)
        {
        case kInputGain: fChain.setInputGainDb(value); break;
        case kCeiling:   fChain.setCeilingDb(value); break;
        case kRelease:   fChain.setReleaseMs(value); break;
        case kBypass:    fChain.setBypass(value > 0.5f); break;
        case kMode:      fChain.setMode((int)std::lround(value)); break;
        case kAmount:    fChain.setAmount(value / 100.0f); break;
        case kAutoRelease: fChain.setAutoRelease(value > 0.5f); break;
        case kTruePeak:    fChain.setTruePeak(value > 0.5f); break;
        }
    }

    void activate() override { fChain.reset(); }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        if (frames == 0 || inputs == nullptr || outputs == nullptr
            || inputs[0] == nullptr || inputs[1] == nullptr || outputs[0] == nullptr || outputs[1] == nullptr)
            return;
        // input gain → Game mode → limiter; every setting change glides and bypass cross-fades (ChainDsp.hpp)
        fChain.process(inputs[0], inputs[1], outputs[0], outputs[1], (int)frames);

        // Meters: jump up at once, fall back at 20 dB per second so they are readable.
        const float fall = 20.0f * (float)frames / (float)getSampleRate();
        const float gr = fChain.takeReductionDb();
        const float in = std::max(-60.0f, vgl::gainToDb(fChain.takeInputPeak()));
        const float out = std::max(-60.0f, vgl::gainToDb(fChain.takeOutputPeak()));
        fValues[kGainReduction] = std::min(gr, std::min(0.0f, fValues[kGainReduction] + fall));
        fValues[kInputLevel] = std::min(6.0f, std::max(in, fValues[kInputLevel] - fall));
        fValues[kOutputLevel] = std::min(6.0f, std::max(out, fValues[kOutputLevel] - fall));
    }

    void sampleRateChanged(double) override { prepare(); }

private:
    void prepare()
    {
        fChain.prepare(getSampleRate());
        fChain.setInputGainDb(fValues[kInputGain]);
        fChain.setCeilingDb(fValues[kCeiling]);
        fChain.setReleaseMs(fValues[kRelease]);
        fChain.setBypass(fValues[kBypass] > 0.5f);
        fChain.setMode((int)std::lround(fValues[kMode]));
        fChain.setAmount(fValues[kAmount] / 100.0f);
        fChain.setAutoRelease(fValues[kAutoRelease] > 0.5f);
        fChain.setTruePeak(fValues[kTruePeak] > 0.5f);
        setLatency((uint32_t)fChain.latencySamples());
    }

    vgl::Chain fChain;
    float fValues[kParameterCount];

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VolumeGuardLimiter)
};

Plugin* createPlugin()
{
    return new VolumeGuardLimiter();
}

END_NAMESPACE_DISTRHO
