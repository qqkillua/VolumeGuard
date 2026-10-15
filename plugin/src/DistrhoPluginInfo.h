// VolumeGuard Limiter — plugin description for DPF.
// Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "qqkillua"
#define DISTRHO_PLUGIN_NAME    "VolumeGuard Limiter"
#define DISTRHO_PLUGIN_URI     "https://github.com/qqkillua/VolumeGuard#volumeguard-limiter"
#define DISTRHO_PLUGIN_CLAP_ID "io.github.qqkillua.volumeguard.limiter"

#define DISTRHO_PLUGIN_BRAND_ID  Qqkl
#define DISTRHO_PLUGIN_UNIQUE_ID VGLm

#define DISTRHO_PLUGIN_HAS_UI       1
#define DISTRHO_UI_USE_NANOVG       1
#define DISTRHO_UI_USER_RESIZABLE   0
#define DISTRHO_UI_DEFAULT_WIDTH    520
#define DISTRHO_UI_DEFAULT_HEIGHT   440
#define DISTRHO_PLUGIN_IS_RT_SAFE   1
#define DISTRHO_PLUGIN_NUM_INPUTS   2
#define DISTRHO_PLUGIN_NUM_OUTPUTS  2
#define DISTRHO_PLUGIN_WANT_LATENCY 1

#define DISTRHO_PLUGIN_VST3_CATEGORIES "Fx|Dynamics|Stereo"
#define DISTRHO_PLUGIN_CLAP_FEATURES   "audio-effect", "limiter", "stereo"
#define DISTRHO_PLUGIN_LV2_CATEGORY    "lv2:LimiterPlugin"

#endif
