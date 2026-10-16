/* VolumeGuard audio engine (vgaudio.dll) — C API used by VolumeGuard.exe.
 * Copyright (c) 2026 Nikita Bainov (qqkillua). MIT License.
 *
 * The game plays into a "source" device (a virtual cable or an unused output). The engine records what
 * that device plays (WASAPI loopback), runs it through input gain → Game mode → 5 ms lookahead limiter
 * and plays the result on the "output" device (headphones). All strings are UTF-16 (wchar_t). 64-bit only.
 */
#ifndef VGAUDIO_H
#define VGAUDIO_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef VGAUDIO_BUILD
#define VG_API __declspec(dllexport)
#else
#define VG_API __declspec(dllimport)
#endif

#define VG_API_VERSION 3

/* results */
#define VG_OK              0
#define VG_ERR_ARGS       -1   /* missing device id */
#define VG_ERR_SAME       -2   /* source and output are the same device: it would feed back into itself */
#define VG_ERR_SOURCE     -3   /* the source device could not be opened */
#define VG_ERR_OUTPUT     -4   /* the output device could not be opened */
#define VG_ERR_FORMAT     -5   /* a device uses a sample format the engine does not support */
#define VG_ERR_SYSTEM     -6   /* COM / thread / timer failure */
#define VG_ERR_TIMEOUT    -7   /* the engine did not start within 5 s */

/* states */
#define VG_STATE_STOPPED       0
#define VG_STATE_STARTING      1
#define VG_STATE_RUNNING       2
#define VG_STATE_BUFFERING     3   /* waiting for sound from the source (start, or after a gap) */
#define VG_STATE_RECONNECTING  4   /* a device disappeared or changed format; retrying every 2 s */

/* modes */
#define VG_MODE_LIMITER   0
#define VG_MODE_COMFORT   1
#define VG_MODE_TACTICAL  2

typedef struct vg_status
{
    int   state;
    int   sourceRate;       /* Hz */
    int   outputRate;       /* Hz */
    int   underruns;        /* times the output had to play silence because no sound came in time */
    int   restarts;         /* automatic reconnects */
    float inPeakDb;         /* loudest sample since the previous call, after input gain (dBFS, ≥ -100) */
    float outPeakDb;        /* the same at the output */
    float reductionDb;      /* strongest reduction since the previous call (≤ 0): Game mode + limiter */
    float latencyMs;        /* estimate of the whole path, source → output */
    float bufferMs;         /* audio waiting between the two devices */
    float correctionPpm;    /* clock-drift correction currently applied */
    /* API 2: where the latency comes from (latencyMs = sourceMs + bufferMs + outputMs + lookaheadMs) */
    float sourceMs;         /* packet size of the source device (how old the sound is when we get it) */
    float outputMs;         /* audio queued in the output device */
    float lookaheadMs;      /* limiter lookahead */
} vg_status;

VG_API int  vg_version(void);

/* List of active playback devices: one per line, "id\tname\tdefault(0/1)\n".
 * Returns the number of wchar_t needed including the terminating zero; writes only if it fits. */
VG_API int  vg_list_devices(wchar_t* buffer, int capacity);

/* Starts (or restarts) the engine. Blocks until audio flows or an error is known (at most 5 s). */
VG_API int  vg_start(const wchar_t* sourceId, const wchar_t* outputId);
VG_API void vg_stop(void);

/* Can be called at any time, from any thread. */
VG_API void vg_set_params(int mode, float amountPercent, float ceilingDb, float inputGainDb, float releaseMs, int bypass);

/* size = sizeof(vg_status), so newer DLLs can extend the struct. Returns VG_OK. */
VG_API int  vg_get_status(vg_status* status, int size);

/* API 3: per-app output device (the "Output device" of an app in Windows' volume mixer).
 * Apps that play sound now, one per line: "pid\texe path\tdevice id\tactive(0/1)\n". Same size rules as above. */
VG_API int  vg_list_apps(wchar_t* buffer, int capacity);
/* Send app pid to device id (as listed by vg_list_devices); NULL or "" = back to the Windows default.
 * Windows remembers it for that app. Returns VG_OK or a failure HRESULT (negative). Uses an undocumented
 * Windows interface (Windows 10 1803+); on older systems it fails and the app should open Settings instead. */
VG_API int  vg_route_app(unsigned int pid, const wchar_t* deviceId);
/* The device app pid is sent to ("" = the default). Returns the wchar_t count needed, like vg_list_devices. */
VG_API int  vg_app_route(unsigned int pid, wchar_t* buffer, int capacity);
/* The last error as text (English, for the log / tooltip). Returns its length. */
VG_API int  vg_last_error(wchar_t* buffer, int capacity);

#ifdef __cplusplus
}
#endif

#endif
