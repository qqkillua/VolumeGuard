// VolumeGuard Limiter — DPF plugin: input gain → Game mode (optional) → lookahead limiter.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#include "DistrhoPlugin.hpp"
#include "LimiterDsp.hpp"
#include "GameDsp.hpp"

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
        fValues[kMode] = 0.0f;
        fValues[kAmount] = 50.0f;
        fLimiter.setInputGainDb(0.0f);   // input gain is applied before the Game mode, see run()
        prepare();
    }

protected:
    const char* getLabel() const override { return "VolumeGuardLimiter"; }
    const char* getDescription() const override
    {
        return "Lookahead brickwall limiter with a Game mode that tames gunshots and rumble and lifts quiet sounds.";
    }
    const char* getMaker() const override { return "qqkillua"; }
    const char* getHomePage() const override { return "https://github.com/qqkillua/VolumeGuard"; }
    const char* getLicense() const override { return "MIT"; }
    uint32_t getVersion() const override { return d_version(1, 2, 0); }
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
        }
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
        case kInputGain: fInputGain = vgl::dbToGain(value); break;
        case kCeiling:   fLimiter.setCeilingDb(value); break;
        case kRelease:   fLimiter.setReleaseMs(value); break;
        case kMode:      fGame.setMode((int)std::lround(value)); break;
        case kAmount:    fGame.setAmount(value / 100.0f); break;
        }
    }

    void activate() override { fLimiter.reset(); fGame.reset(); }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        if (frames == 0 || inputs == nullptr || outputs == nullptr
            || inputs[0] == nullptr || inputs[1] == nullptr || outputs[0] == nullptr || outputs[1] == nullptr)
            return;
        float inPeak = 0.0f;
        float gameGrDb = 0.0f;
        if (fValues[kBypass] > 0.5f)
        {
            // dry signal, same latency
            fLimiter.process(inputs, outputs, 2, (int)frames, true);
            inPeak = fLimiter.takeInputPeak();
        }
        else
        {
            // in chunks through a scratch buffer: input gain → Game mode → limiter
            for (uint32_t pos = 0; pos < frames; pos += kChunk)
            {
                const uint32_t n = std::min<uint32_t>(kChunk, frames - pos);
                for (uint32_t i = 0; i < n; ++i)
                {
                    fScratch[0][i] = inputs[0][pos + i] * fInputGain;
                    fScratch[1][i] = inputs[1][pos + i] * fInputGain;
                    inPeak = std::max(inPeak, std::max(std::fabs(fScratch[0][i]), std::fabs(fScratch[1][i])));
                }
                fGame.process(fScratch[0], fScratch[1], (int)n);
                const float* in[2] = { fScratch[0], fScratch[1] };
                float* out[2] = { outputs[0] + pos, outputs[1] + pos };
                fLimiter.process(in, out, 2, (int)n);
            }
            fLimiter.takeInputPeak();
            if (fGame.mode() != vgl::GameProcessor::kOff) gameGrDb = fGame.takeReductionDb();
        }

        // Meters: jump up at once, fall back at 20 dB per second so they are readable.
        const float fall = 20.0f * (float)frames / (float)getSampleRate();
        const float gr = std::max(-40.0f, fLimiter.takeGainReductionDb() + gameGrDb);
        const float in = std::max(-60.0f, vgl::gainToDb(inPeak));
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
        fLimiter.setInputGainDb(0.0f);
        fLimiter.setCeilingDb(fValues[kCeiling]);
        fInputGain = vgl::dbToGain(fValues[kInputGain]);
        fGame.prepare(getSampleRate());
        fGame.setMode((int)std::lround(fValues[kMode]));
        fGame.setAmount(fValues[kAmount] / 100.0f);
        fLimiter.setReleaseMs(fValues[kRelease]);
        setLatency((uint32_t)fLimiter.latencySamples());
    }

    static constexpr uint32_t kChunk = 256;

    vgl::LookaheadLimiter fLimiter;
    vgl::GameProcessor fGame;
    float fInputGain = 1.0f;
    float fScratch[2][kChunk];
    float fValues[kParameterCount];

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VolumeGuardLimiter)
};

Plugin* createPlugin()
{
    return new VolumeGuardLimiter();
}

END_NAMESPACE_DISTRHO
