// VolumeGuard Limiter — built-in presets, shared by the plugin (host preset list) and its window (buttons).
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#pragma once

namespace vgl {

struct Preset
{
    const char* name;
    float inputGainDb, ceilingDb, releaseMs, mode, amount, autoRelease;
};

constexpr int kPresetCount = 4;
constexpr Preset kPresets[kPresetCount] = {
    //  name        input  ceiling  release  mode  amount  auto release
    { "Default",     0.0f,  -1.0f,  100.0f,  0.0f,  50.0f, 1.0f },   // limiter only, the plugin's defaults
    { "Shooter",     0.0f,  -3.0f,   80.0f,  2.0f,  60.0f, 1.0f },   // Tactical: quieter shots, louder steps
    { "Movie",       0.0f,  -2.0f,  150.0f,  1.0f,  50.0f, 1.0f },   // Comfort: explosions tamed, dialogue kept
    { "Night",       0.0f, -12.0f,  200.0f,  1.0f, 100.0f, 1.0f },   // Comfort at full + a low ceiling: nothing loud
};

} // namespace vgl
