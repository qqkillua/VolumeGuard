// VolumeGuard Limiter — plugin window (DPF + NanoVG), in the same dark style as the VolumeGuard app.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#include "DistrhoUI.hpp"

#include <cmath>
#include <cstdio>

START_NAMESPACE_DISTRHO

namespace {

// Parameter indices — keep in sync with VolumeGuardLimiter.cpp
enum { kInputGain = 0, kCeiling, kRelease, kBypass, kGainReduction, kInputLevel, kOutputLevel, kCount };

constexpr uint kWidth = 520, kHeight = 352;

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

        const float x = 132, w = 290;
        fSliders[0] = { kInputGain, "Input Gain", -12.0f, 24.0f, 0.0f, false, x, 92, w };
        fSliders[1] = { kCeiling, "Ceiling", -30.0f, 0.0f, -1.0f, false, x, 140, w };
        fSliders[2] = { kRelease, "Release", 1.0f, 1000.0f, 100.0f, true, x, 188, w };
    }

protected:
    // ---------------------------------------------------------------------------------------------------
    // host → UI

    void parameterChanged(uint32_t index, float value) override
    {
        if (index >= kCount) return;
        if (fValues[index] == value) return;
        fValues[index] = value;
        repaint();
    }

    // ---------------------------------------------------------------------------------------------------
    // drawing

    void onNanoDisplay() override
    {
        const bool bypass = fValues[kBypass] > 0.5f;

        // background and card
        rectFill(0, 0, kWidth, kHeight, kBg);
        roundFill(12, 56, kWidth - 24, 172, 10, kCard);
        roundFill(12, 238, kWidth - 24, 102, 10, kCard);

        // header
        fontFace(NANOVG_DEJAVU_SANS_TTF);
        textAlign(ALIGN_LEFT | ALIGN_MIDDLE);
        fontSize(20);
        fillColor(kText);
        text(18, 22, "VolumeGuard", nullptr);
        fillColor(kAccent);
        Rectangle<float> b;
        const float titleWidth = textBounds(18, 22, "VolumeGuard ", nullptr, b);   // returns the advance
        text(18 + titleWidth, 22, "Limiter", nullptr);
        fontSize(11.5f);
        fillColor(kSub);
        text(18, 42, "lookahead brickwall limiter", nullptr);

        // bypass switch (top right)
        drawSwitch(kWidth - 120, 16, !bypass, bypass ? "BYPASS" : "ACTIVE");

        // section titles
        fontSize(10.5f);
        fillColor(kSub);
        text(26, 70, "SETTINGS", nullptr);
        text(26, 252, "METERS", nullptr);

        // sliders
        for (const Slider& s : fSliders)
            drawSlider(s, bypass);

        // meters
        const float ceiling = fValues[kCeiling];
        drawMeter(274, "IN", fValues[kInputLevel], -60.0f, 6.0f, kGreen, ceiling, bypass);
        drawMeter(300, "OUT", fValues[kOutputLevel], -60.0f, 6.0f, kGreen, ceiling, bypass);
        drawReduction(326, bypass ? 0.0f : fValues[kGainReduction]);
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
                editParameter((uint32_t)fSliders[fDragging].param, false);
                fDragging = -1;
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

        for (int i = 0; i < 3; ++i)
        {
            const Slider& s = fSliders[i];
            if (my < s.y - 16 || my > s.y + 16 || mx < s.x - 12 || mx > s.x + s.w + 12) continue;

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
            const float step = ev.delta.getY() > 0 ? 0.01f : -0.01f;
            editParameter((uint32_t)s.param, true);
            setValue(s.param, s.fromPos(s.toPos(fValues[s.param]) + step));
            editParameter((uint32_t)s.param, false);
            return true;
        }
        return false;
    }

private:

    void setValue(int param, float v)
    {
        if (param == kRelease) v = std::round(v);
        else if (param != kBypass) v = std::round(v * 10.0f) / 10.0f;
        fValues[param] = v;
        setParameterValue((uint32_t)param, v);
        repaint();
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
        if (s.param == kRelease) std::snprintf(buf, sizeof buf, "%.0f ms", v);
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

    float fValues[kCount];
    Slider fSliders[3];
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
