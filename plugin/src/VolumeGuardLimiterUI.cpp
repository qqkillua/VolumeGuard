// VolumeGuard Limiter — plugin window (DPF + NanoVG), in the same dark style as the VolumeGuard app.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#include "DistrhoUI.hpp"
#include "Presets.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

START_NAMESPACE_DISTRHO

namespace {

// Parameter indices — keep in sync with VolumeGuardLimiter.cpp
enum { kInputGain = 0, kCeiling, kRelease, kBypass, kGainReduction, kInputLevel, kOutputLevel, kMode, kAmount,
       kAutoRelease, kTruePeak, kLookahead, kCount };

constexpr uint kWidth = 520, kHeight = 600;
constexpr int kHistory = 200;          // gain reduction graph: 200 points × 50 ms = the last 10 seconds
constexpr double kHistoryStepSec = 0.05;
const char* const kPresetLabels[vgl::kPresetCount] = { "DEFAULT", "SHOOTER", "MOVIE", "NIGHT" };
constexpr int kSliderCount = 5;

const char* const kModeNames[3] = { "LIMITER", "COMFORT", "TACTICAL" };
const char* const kModeHints[3] = {
    "only keeps the output below the ceiling",
    "no sudden jumps: loud sounds pressed down gently",
    "quieter shots and rumble, louder steps",
};

// Colours of the VolumeGuard app
const Color kBg(24, 25, 28), kCard(35, 37, 42), kText(233, 234, 237), kSub(146, 150, 160), kTrack(58, 61, 68),
            kAccent(92, 141, 255), kGreen(67, 181, 129), kWarn(242, 176, 58), kRed(240, 71, 71), kOff(90, 94, 103);

struct Slider
{
    int param;
    const char* label;
    float min, max, def;
    bool logarithmic;
    float x, y, w;   // track geometry (1x scale)

    float toPos(float v) const
    {
        if (logarithmic) return (std::log(v / min)) / std::log(max / min);
        return (v - min) / (max - min);
    }
    float fromPos(float p) const
    {
        p = std::fmax(0.0f, std::fmin(1.0f, p));
        if (logarithmic) return min * std::pow(max / min, p);
        return min + p * (max - min);
    }
};

} // namespace

class VolumeGuardLimiterUI : public UI
{
public:
    VolumeGuardLimiterUI()
        : UI(kWidth, kHeight, true)
    {
        loadSharedResources();

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
        fValues[kLookahead] = 5.0f;

        const float x = 132, w = 290;
        fSliders[0] = { kInputGain, "Input Gain", -12.0f, 24.0f, 0.0f, false, x, 226, w };
        fSliders[1] = { kAmount, "Amount", 0.0f, 100.0f, 50.0f, false, x, 266, w };
        fSliders[2] = { kCeiling, "Ceiling", -30.0f, 0.0f, -1.0f, false, x, 306, w };
        fSliders[3] = { kRelease, "Release", 1.0f, 1000.0f, 100.0f, true, x, 346, w };
        fSliders[4] = { kLookahead, "Lookahead", 1.0f, 10.0f, 5.0f, false, x, 386, w };

        for (float& v : fHistory) v = 0.0f;
        fHistoryTime = std::chrono::steady_clock::now();
    }

protected:
    // ---------------------------------------------------------------------------------------------------
    // host → UI

    void parameterChanged(uint32_t index, float value) override
    {
        if (index >= kCount) return;
        if (index == kGainReduction) fGrMin = std::fmin(fGrMin, value);
        if (fValues[index] == value) return;
        fValues[index] = value;
        repaint();
    }

    void programLoaded(uint32_t index) override
    {
        if (index >= (uint32_t)vgl::kPresetCount) return;
        applyPresetValues(vgl::kPresets[index]);
        repaint();
    }

    // the gain reduction graph moves on even when the host sends nothing (silence, a paused track)
    void uiIdle() override
    {
        const auto now = std::chrono::steady_clock::now();
        int steps = (int)(std::chrono::duration<double>(now - fHistoryTime).count() / kHistoryStepSec);
        if (steps <= 0) return;
        steps = std::min(steps, kHistory);
        fHistoryTime += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(steps * kHistoryStepSec));
        if (std::chrono::duration<double>(now - fHistoryTime).count() > 1.0) fHistoryTime = now;   // after a long stall
        const float v = fValues[kBypass] > 0.5f ? 0.0f : fGrMin;
        for (int i = 0; i < steps; ++i)
        {
            fHistory[fHistoryPos] = v;
            fHistoryPos = (fHistoryPos + 1) % kHistory;
        }
        fGrMin = fValues[kGainReduction];
        repaint();
    }

    // ---------------------------------------------------------------------------------------------------
    // drawing

    void onNanoDisplay() override
    {
        const bool bypass = fValues[kBypass] > 0.5f;

        // background and card
        rectFill(0, 0, kWidth, kHeight, kBg);
        roundFill(12, 56, kWidth - 24, 66, 10, kCard);
        roundFill(12, 130, kWidth - 24, 46, 10, kCard);
        roundFill(12, 184, kWidth - 24, 256, 10, kCard);
        roundFill(12, 448, kWidth - 24, 144, 10, kCard);

        // header
        fontFace(NANOVG_DEJAVU_SANS_TTF);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fontSize(20);
        fillColor(kText);
        text(18, 22, "VolumeGuard", nullptr);
        fillColor(kAccent);
        DGL_NAMESPACE::Rectangle<float> b;
        const float titleWidth = textBounds(18, 22, "VolumeGuard ", nullptr, b);   // returns the advance
        text(18 + titleWidth, 22, "Limiter", nullptr);
        fontSize(11.5f);
        fillColor(kSub);
        text(18, 42, "lookahead limiter with a game mode", nullptr);

        // bypass switch (top right)
        drawSwitch(kWidth - 120, 16, !bypass, bypass ? "BYPASS" : "ACTIVE");

        // section titles
        fontSize(10.5f);
        fillColor(kSub);
        text(26, 70, "MODE", nullptr);
        text(26, 153, "PRESET", nullptr);
        text(26, 198, "SETTINGS", nullptr);
        text(26, 462, "METERS", nullptr);

        // preset buttons: the one matching the current settings is lit
        const int preset = currentPreset();
        for (int i = 0; i < vgl::kPresetCount; ++i)
        {
            float bx, bw;
            presetButton(i, bx, bw);
            const bool on = i == preset;
            roundFill(bx, 141, bw, 24, 6, on ? (bypass ? kOff : kAccent) : kTrack);
            fontSize(11);
            textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
            fillColor(on ? Color(255, 255, 255) : kSub);
            text(bx + bw / 2, 153, kPresetLabels[i], nullptr);
        }
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);

        // mode buttons
        const int mode = modeIndex();
        for (int i = 0; i < 3; ++i)
        {
            float bx, bw;
            modeButton(i, bx, bw);
            const bool on = i == mode;
            roundFill(bx, 80, bw, 24, 6, on ? (bypass ? kOff : (i == 2 ? kRed : kAccent)) : kTrack);
            fontSize(11.5f);
            textAlign(ALIGN_CENTER | ALIGN_MIDDLE);
            fillColor(on ? Color(255, 255, 255) : kSub);
            text(bx + bw / 2, 92, kModeNames[i], nullptr);
        }
        fontSize(11);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(kSub);
        text(90, 70, kModeHints[mode], nullptr);

        // sliders
        for (const Slider& s : fSliders)
            drawSlider(s, bypass || (s.param == kAmount && mode == 0));

        // switches: auto release and true peak
        drawSmallSwitch(26, 132, 420, "Auto Release", fValues[kAutoRelease] > 0.5f, bypass);
        drawSmallSwitch(292, 398, 420, "True Peak", fValues[kTruePeak] > 0.5f, bypass);

        // meters
        const float ceiling = fValues[kCeiling];
        drawMeter(482, "IN", fValues[kInputLevel], -60.0f, 6.0f, kGreen, ceiling, bypass);
        drawMeter(504, "OUT", fValues[kOutputLevel], -60.0f, 6.0f, kGreen, ceiling, bypass);
        drawReduction(526, bypass ? 0.0f : fValues[kGainReduction]);
        drawHistory(540, 44, bypass);
    }

    // ---------------------------------------------------------------------------------------------------
    // mouse

    bool onMouse(const MouseEvent& ev) override
    {
        const float mx = ev.pos.getX(), my = ev.pos.getY();

        if (ev.button != 1) return false;

        if (!ev.press)
        {
            if (fDragging >= 0)
            {
                const int param = fSliders[fDragging].param;
                fDragging = -1;
                // the lookahead changes the latency: the host hears only where the drag ended
                if (param == kLookahead) setParameterValue(kLookahead, fValues[kLookahead]);
                editParameter((uint32_t)param, false);
                return true;
            }
            return false;
        }

        // bypass switch
        if (mx >= kWidth - 124 && mx <= kWidth - 12 && my >= 10 && my <= 40)
        {
            const float v = fValues[kBypass] > 0.5f ? 0.0f : 1.0f;
            editParameter(kBypass, true);
            setValue(kBypass, v);
            editParameter(kBypass, false);
            return true;
        }

        // auto release and true peak switches (the label is clickable too)
        if (my >= 406 && my <= 434)
        {
            int param = -1;
            if (mx >= 26 && mx <= 172) param = kAutoRelease;
            else if (mx >= 292 && mx <= 438) param = kTruePeak;
            if (param >= 0)
            {
                editParameter((uint32_t)param, true);
                setValue(param, fValues[param] > 0.5f ? 0.0f : 1.0f);
                editParameter((uint32_t)param, false);
                return true;
            }
        }

        // preset buttons
        if (my >= 139 && my <= 167)
        {
            for (int i = 0; i < vgl::kPresetCount; ++i)
            {
                float bx, bw;
                presetButton(i, bx, bw);
                if (mx < bx || mx > bx + bw) continue;
                const vgl::Preset& p = vgl::kPresets[i];
                const int params[6] = { kInputGain, kCeiling, kRelease, kMode, kAmount, kAutoRelease };
                const float values[6] = { p.inputGainDb, p.ceilingDb, p.releaseMs, p.mode, p.amount, p.autoRelease };
                for (int k = 0; k < 6; ++k)
                {
                    editParameter((uint32_t)params[k], true);
                    setValue(params[k], values[k]);
                    editParameter((uint32_t)params[k], false);
                }
                return true;
            }
        }

        // mode buttons
        if (my >= 78 && my <= 106)
        {
            for (int i = 0; i < 3; ++i)
            {
                float bx, bw;
                modeButton(i, bx, bw);
                if (mx < bx || mx > bx + bw || i == modeIndex()) continue;
                editParameter(kMode, true);
                setValue(kMode, (float)i);
                editParameter(kMode, false);
                return true;
            }
        }

        for (int i = 0; i < kSliderCount; ++i)
        {
            const Slider& s = fSliders[i];
            if (my < s.y - 16 || my > s.y + 16 || mx < s.x - 12 || mx > s.x + s.w + 12) continue;
            if (s.param == kAmount && modeIndex() == 0) return true;   // Amount only matters in a game mode

            // double click: back to default
            if (ev.time - fLastClickTime < 350 && fLastClickSlider == i)
            {
                editParameter((uint32_t)s.param, true);
                setValue(s.param, s.def);
                editParameter((uint32_t)s.param, false);
                fLastClickSlider = -1;
                return true;
            }
            fLastClickTime = ev.time;
            fLastClickSlider = i;

            fDragging = i;
            editParameter((uint32_t)s.param, true);
            setValue(s.param, s.fromPos((mx - s.x) / s.w));
            return true;
        }
        return false;
    }

    bool onMotion(const MotionEvent& ev) override
    {
        if (fDragging < 0) return false;
        const Slider& s = fSliders[fDragging];
        setValue(s.param, s.fromPos((ev.pos.getX() - s.x) / s.w));
        return true;
    }

    bool onScroll(const ScrollEvent& ev) override
    {
        const float mx = ev.pos.getX(), my = ev.pos.getY();
        for (const Slider& s : fSliders)
        {
            if (my < s.y - 16 || my > s.y + 16 || mx < s.x - 12 || mx > s.x + s.w + 30) continue;
            if (s.param == kAmount && modeIndex() == 0) return true;
            const float step = ev.delta.getY() > 0 ? 0.01f : -0.01f;
            editParameter((uint32_t)s.param, true);
            if (s.param == kLookahead)   // whole milliseconds: one notch = 1 ms
                setValue(s.param, std::fmax(s.min, std::fmin(s.max, fValues[kLookahead] + (step > 0 ? 1.0f : -1.0f))));
            else
                setValue(s.param, s.fromPos(s.toPos(fValues[s.param]) + step));
            editParameter((uint32_t)s.param, false);
            return true;
        }
        return false;
    }

private:

    int modeIndex() const { return std::max(0, std::min(2, (int)std::lround(fValues[kMode]))); }

    void modeButton(int i, float& x, float& w) const
    {
        const float left = 26, right = kWidth - 26, gap = 8;
        w = (right - left - 2 * gap) / 3;
        x = left + i * (w + gap);
    }

    void presetButton(int i, float& x, float& w) const
    {
        const float left = 100, right = kWidth - 26, gap = 8;
        w = (right - left - (vgl::kPresetCount - 1) * gap) / vgl::kPresetCount;
        x = left + i * (w + gap);
    }

    /// The preset whose values the settings match right now, or -1.
    int currentPreset() const
    {
        for (int i = 0; i < vgl::kPresetCount; ++i)
        {
            const vgl::Preset& p = vgl::kPresets[i];
            const bool gameMode = p.mode > 0.5f;
            if (std::fabs(fValues[kInputGain] - p.inputGainDb) < 0.05f && std::fabs(fValues[kCeiling] - p.ceilingDb) < 0.05f
                && std::fabs(fValues[kRelease] - p.releaseMs) < 0.5f && modeIndex() == (int)p.mode
                && (!gameMode || std::fabs(fValues[kAmount] - p.amount) < 0.5f)
                && (fValues[kAutoRelease] > 0.5f) == (p.autoRelease > 0.5f))
                return i;
        }
        return -1;
    }

    void applyPresetValues(const vgl::Preset& p)
    {
        fValues[kInputGain] = p.inputGainDb;
        fValues[kCeiling] = p.ceilingDb;
        fValues[kRelease] = p.releaseMs;
        fValues[kMode] = p.mode;
        fValues[kAmount] = p.amount;
        fValues[kAutoRelease] = p.autoRelease;
    }

    void setValue(int param, float v)
    {
        if (param == kRelease || param == kAmount || param == kMode || param == kLookahead) v = std::round(v);
        else if (param != kBypass && param != kAutoRelease && param != kTruePeak) v = std::round(v * 10.0f) / 10.0f;
        if (fValues[param] == v && param == kLookahead) return;
        fValues[param] = v;
        repaint();
        if (param == kLookahead && fDragging >= 0 && fSliders[fDragging].param == kLookahead) return;   // sent on release
        // tell the host last: nothing in this window is touched after the host has reacted
        setParameterValue((uint32_t)param, v);
    }

    // ---------------------------------------------------------------------------------------------------
    // drawing helpers

    void rectFill(float x, float y, float w, float h, const Color& c)
    {
        beginPath(); rect(x, y, w, h); fillColor(c); fill();
    }

    void roundFill(float x, float y, float w, float h, float r, const Color& c)
    {
        if (w <= 0 || h <= 0) return;
        beginPath(); roundedRect(x, y, w, h, std::fmin(r, std::fmin(w, h) / 2)); fillColor(c); fill();
    }

    void drawSwitch(float x, float y, bool on, const char* label)
    {
        roundFill(x, y, 34, 18, 9, on ? kAccent : kTrack);
        beginPath();
        circle(on ? x + 25 : x + 9, y + 9, 6);
        fillColor(Color(255, 255, 255));
        fill();
        fontSize(12);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(on ? kText : kWarn);
        text(x + 42, y + 9, label, nullptr);
    }

    void drawSmallSwitch(float labelX, float x, float y, const char* label, bool on, bool bypass)
    {
        fontSize(13);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(kSub);
        text(labelX, y, label, nullptr);
        roundFill(x, y - 9, 34, 18, 9, on ? (bypass ? kOff : kAccent) : kTrack);
        beginPath(); circle(on ? x + 25 : x + 9, y, 6); fillColor(Color(255, 255, 255)); fill();
    }

    void drawSlider(const Slider& s, bool bypass)
    {
        const float v = fValues[s.param];
        const float pos = std::fmax(0.0f, std::fmin(1.0f, s.toPos(v)));
        const float kx = s.x + pos * s.w;
        const bool isCeiling = s.param == kCeiling;
        const Color accent = bypass ? kOff : (isCeiling ? kRed : kAccent);

        fontSize(13);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(kSub);
        text(26, s.y, s.label, nullptr);

        roundFill(s.x, s.y - 3, s.w, 6, 3, kTrack);
        if (!isCeiling) roundFill(s.x, s.y - 3, kx - s.x, 6, 3, accent);
        else            roundFill(kx, s.y - 3, s.x + s.w - kx, 6, 3, Color(accent, kTrack, 0.55f));

        beginPath();
        circle(kx, s.y, 7.5f);
        fillColor(isCeiling ? accent : (bypass ? kSub : Color(255, 255, 255)));
        fill();
        if (fDragging >= 0 && fSliders[fDragging].param == s.param)
        {
            beginPath(); circle(kx, s.y, 11); strokeColor(Color(accent, kCard, 0.45f)); strokeWidth(2); stroke();
        }

        char buf[32];
        if (s.param == kRelease || s.param == kLookahead) std::snprintf(buf, sizeof buf, "%.0f ms", v);
        else if (s.param == kAmount) std::snprintf(buf, sizeof buf, "%.0f %%", v);
        else if (std::fabs(v) < 0.05f) std::snprintf(buf, sizeof buf, "0.0 dB");
        else std::snprintf(buf, sizeof buf, "%+.1f dB", v);
        textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
        fillColor(kText);
        text(kWidth - 26, s.y, buf, nullptr);
    }

    // level meter: -60..+6 dB, ceiling marked in red
    void drawMeter(float y, const char* label, float db, float lo, float hi, const Color& c, float ceilingDb, bool bypass)
    {
        const float x = 70, w = 352, h = 8;
        auto px = [&](float d) { return x + w * std::fmax(0.0f, std::fmin(1.0f, (d - lo) / (hi - lo))); };

        fontSize(11.5f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(kSub);
        text(26, y, label, nullptr);

        roundFill(x, y - h / 2, w, h, h / 2, kTrack);
        const float lx = px(db), cx = px(ceilingDb);
        roundFill(x, y - h / 2, std::fmin(lx, cx) - x, h, h / 2, bypass ? kOff : c);
        if (lx > cx) roundFill(cx, y - h / 2, lx - cx, h, h / 2, bypass ? kOff : kRed);
        rectFill(cx - 1, y - 8, 2, 16, bypass ? kOff : kRed);

        char buf[24];
        if (db <= lo + 0.05f) std::snprintf(buf, sizeof buf, "-inf");
        else std::snprintf(buf, sizeof buf, "%.1f dB", db);
        textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
        fillColor(kText);
        text(kWidth - 26, y, buf, nullptr);
    }

    // gain reduction: grows from the right, 0..-24 dB
    void drawReduction(float y, float db)
    {
        const float x = 70, w = 352, h = 8;
        const float amount = std::fmax(0.0f, std::fmin(1.0f, -db / 24.0f));

        fontSize(11.5f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(kSub);
        text(26, y, "GR", nullptr);

        roundFill(x, y - h / 2, w, h, h / 2, kTrack);
        roundFill(x + w * (1.0f - amount), y - h / 2, w * amount, h, h / 2, kWarn);

        char buf[24];
        if (db > -0.05f) std::snprintf(buf, sizeof buf, "0.0 dB");
        else std::snprintf(buf, sizeof buf, "%.1f dB", db);
        textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
        fillColor(db < -0.05f ? kWarn : kText);
        text(kWidth - 26, y, buf, nullptr);
    }

    // gain reduction over the last 10 s: 0 dB at the top, -24 dB at the bottom; the newest point on the right
    void drawHistory(float y, float h, bool bypass)
    {
        const float x = 70, w = 352;
        fontSize(10.5f);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fillColor(kSub);
        text(26, y + 8, "10 s", nullptr);
        roundFill(x, y, w, h, 4, Color(kTrack, kCard, 0.5f));
        for (float db : { -6.0f, -12.0f, -18.0f })
            rectFill(x, y + h * (-db / 24.0f), w, 1, Color(kTrack, kCard, 0.25f));

        beginPath();
        moveTo(x, y);
        float worst = 0.0f;
        for (int i = 0; i < kHistory; ++i)
        {
            const float v = fHistory[(fHistoryPos + i) % kHistory];
            worst = std::fmin(worst, v);
            lineTo(x + w * (float)i / (kHistory - 1), y + h * std::fmin(1.0f, -v / 24.0f));
        }
        lineTo(x + w, y);
        closePath();
        fillColor(bypass ? kOff : Color(kWarn, kCard, 0.65f));
        fill();

        char buf[32];
        if (worst > -0.05f) std::snprintf(buf, sizeof buf, "max 0.0");
        else std::snprintf(buf, sizeof buf, "max %.1f", worst);
        textAlign(ALIGN_RIGHT | ALIGN_MIDDLE);
        fillColor(kSub);
        text(kWidth - 26, y + 8, buf, nullptr);
    }

    float fValues[kCount];
    Slider fSliders[kSliderCount];
    float fHistory[kHistory];
    int fHistoryPos = 0;
    float fGrMin = 0.0f;
    std::chrono::steady_clock::time_point fHistoryTime;
    int fDragging = -1;
    int fLastClickSlider = -1;
    uint fLastClickTime = 0;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VolumeGuardLimiterUI)
};

UI* createUI()
{
    return new VolumeGuardLimiterUI();
}

END_NAMESPACE_DISTRHO
