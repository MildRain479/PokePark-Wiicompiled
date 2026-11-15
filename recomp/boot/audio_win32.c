/* Sound output: WASAPI in shared mode, with waveOut if WASAPI can't start.
 * The DSP HLE hands over 3 ms frames as AX mixes them.
 *
 * WASAPI: the frames go into a ring that a playback thread empties into
 * the default output device, which Windows converts to the device's own
 * format (rate, channels, float). The thread follows the default device:
 * when it changes (headphones plugged in, another device chosen) or the
 * device goes away, it opens the new default; with no device at all it
 * waits for one and the sound is dropped meanwhile. (waveOut, which this
 * used before, stayed on the device it opened and was silent for some:
 * GitHub issue 5.)
 *
 * The game's audio timer runs on host time, so on average it keeps pace
 * with the device, but its ticks arrive in bursts whenever the game holds
 * off interrupts. Playback therefore starts only once PRIME_FRAMES are
 * queued, which absorbs that much jitter; if the queue still runs dry (a
 * long stall), playback holds until it has primed again, so a stall costs
 * one gap instead of a run of crackles. Frames that arrive with the queue
 * full are dropped, which bounds the latency.
 *
 * RECOMP_AUDIO=0 turns sound off; RECOMP_AUDIO=waveout uses waveOut. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_audio.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

/* Defined here so no GUID library is needed. */
static const CLSID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* ---- WASAPI ---- */

#define RING_FRAMES 8192         /* a power of two */
#define MAX_QUEUED_FRAMES 3200   /* 100 ms in the ring at most */
#define PRIME_FRAMES 2048        /* 64 ms, besides the device's own 20 */
#define DEVICE_BUFFER 200000     /* 20 ms, in 100 ns units */

static uint32_t sRing[RING_FRAMES]; /* left/right pairs */
static volatile LONG sWritten, sRead; /* frames, wrapping */

static int queued_frames(void) { return (int)((uint32_t)sWritten - (uint32_t)sRead); }

static void ring_push(const int16_t* lr, int frames) {
    uint32_t w = (uint32_t)sWritten;
    int i, room = MAX_QUEUED_FRAMES - queued_frames();
    if (frames > room) frames = room; /* full: drop */
    for (i = 0; i < frames; i++) memcpy(&sRing[(w + (uint32_t)i) & (RING_FRAMES - 1)], lr + i * 2, 4);
    MemoryBarrier();
    sWritten = (LONG)(w + (uint32_t)(frames > 0 ? frames : 0));
}

typedef struct {
    IMMDevice* device;
    IAudioClient* client;
    IAudioRenderClient* render;
    UINT32 bufferFrames;
    WCHAR* id;
} Output;

static void output_close(Output* o) {
    if (o->client) IAudioClient_Stop(o->client);
    if (o->render) IAudioRenderClient_Release(o->render);
    if (o->client) IAudioClient_Release(o->client);
    if (o->device) IMMDevice_Release(o->device);
    if (o->id) CoTaskMemFree(o->id);
    memset(o, 0, sizeof(*o));
}

/* The default device's ID, or NULL with none (freed with CoTaskMemFree). */
static WCHAR* default_id(IMMDeviceEnumerator* en) {
    IMMDevice* d = NULL;
    WCHAR* id = NULL;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &d))) return NULL;
    if (FAILED(IMMDevice_GetId(d, &id))) id = NULL;
    IMMDevice_Release(d);
    return id;
}

static int output_open(Output* o, IMMDeviceEnumerator* en, HANDLE event) {
    WAVEFORMATEX fmt;
    HRESULT hr;
    memset(o, 0, sizeof(*o));
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = HOST_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = HOST_AUDIO_RATE * 4;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &o->device);
    if (SUCCEEDED(hr)) hr = IMMDevice_GetId(o->device, &o->id);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(o->device, &kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&o->client);
    if (SUCCEEDED(hr))
        hr = IAudioClient_Initialize(o->client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     DEVICE_BUFFER, 0, &fmt, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(o->client, event);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(o->client, &o->bufferFrames);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(o->client, &kIID_IAudioRenderClient, (void**)&o->render);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(o->client);
    if (FAILED(hr)) {
        fprintf(stderr, "audio: the default output device didn't open (0x%08lX)\n", (unsigned long)hr);
        output_close(o);
        return 0;
    }
    fprintf(stderr, "audio: WASAPI, %d Hz stereo, %u frames on the device\n", HOST_AUDIO_RATE, (unsigned)o->bufferFrames);
    return 1;
}

/* Fills what the device has room for: queued frames, or silence while
 * priming. 0 if the device has gone. */
static int output_fill(Output* o, int* priming) {
    UINT32 padding, room, n, i;
    BYTE* buf;
    uint32_t r;
    int have;
    if (FAILED(IAudioClient_GetCurrentPadding(o->client, &padding))) return 0;
    room = o->bufferFrames - padding;
    if (!room) return 1;
    have = queued_frames();
    if (*priming && have >= PRIME_FRAMES) *priming = 0;
    if (!*priming && have == 0) *priming = 1; /* ran dry: prime again */
    n = *priming ? 0 : (UINT32)have < room ? (UINT32)have : room;
    if (*priming && padding) return 1; /* let what's queued play out */
    if (FAILED(IAudioRenderClient_GetBuffer(o->render, n ? n : room, &buf))) return 0;
    if (!n) return SUCCEEDED(IAudioRenderClient_ReleaseBuffer(o->render, room, AUDCLNT_BUFFERFLAGS_SILENT));
    r = (uint32_t)sRead;
    MemoryBarrier();
    for (i = 0; i < n; i++) memcpy(buf + i * 4, &sRing[(r + i) & (RING_FRAMES - 1)], 4);
    MemoryBarrier();
    sRead = (LONG)(r + n);
    return SUCCEEDED(IAudioRenderClient_ReleaseBuffer(o->render, n, 0));
}

static HANDLE sStarted;
static volatile LONG sWasapiOk;

static DWORD WINAPI playback(LPVOID arg) {
    IMMDeviceEnumerator* en = NULL;
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    Output out;
    int priming = 1, open;
    DWORD lastCheck = GetTickCount();
    (void)arg;
    memset(&out, 0, sizeof(out));
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)) || !event ||
        FAILED(CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &kIID_IMMDeviceEnumerator, (void**)&en))) {
        SetEvent(sStarted); /* sWasapiOk stays 0: waveOut instead */
        return 0;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    open = output_open(&out, en, event);
    sWasapiOk = 1;
    SetEvent(sStarted);
    for (;;) {
        DWORD now;
        WaitForSingleObject(event, open ? 100 : 500);
        if (open && !output_fill(&out, &priming)) {
            fprintf(stderr, "audio: the output device went away\n");
            output_close(&out);
            open = 0;
        }
        if (!open) sRead = sWritten; /* nowhere to play: drop */
        now = GetTickCount();
        if (now - lastCheck >= 1000) {
            /* Follow the default device. */
            WCHAR* id = default_id(en);
            lastCheck = now;
            if (open && (!id || wcscmp(id, out.id) != 0)) {
                fprintf(stderr, "audio: the default output device changed\n");
                output_close(&out);
                open = 0;
            }
            if (!open && id) {
                open = output_open(&out, en, event);
                sRead = sWritten;
                priming = 1;
            }
            if (id) CoTaskMemFree(id);
        }
    }
}

static int wasapi_start(void) {
    HANDLE t;
    sStarted = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!sStarted) return 0;
    t = CreateThread(NULL, 0, playback, NULL, 0, NULL);
    if (!t) return 0;
    CloseHandle(t);
    WaitForSingleObject(sStarted, 5000);
    return sWasapiOk != 0;
}

/* ---- waveOut ---- */

#define BLOCK_FRAMES 384 /* 12 ms */
#define BLOCKS 10 /* at most 120 ms queued */
#define PRIME_BLOCKS 7 /* 84 ms */

static HWAVEOUT sOut;
static WAVEHDR sHdr[BLOCKS];
static int16_t sData[BLOCKS][BLOCK_FRAMES * 2];
static int sFill;   /* block being filled */
static int sFrames; /* frames in it */
static int sPriming = 1;

static int busy(const WAVEHDR* h) { return (h->dwFlags & WHDR_INQUEUE) && !(h->dwFlags & WHDR_DONE); }

static int queued(void) {
    int i, n = 0;
    for (i = 0; i < BLOCKS; i++) n += busy(&sHdr[i]);
    return n;
}

static void wave_push(const int16_t* lr, int frames) {
    while (frames > 0) {
        WAVEHDR* h = &sHdr[sFill];
        int n;
        if (sFrames == 0) {
            if (busy(h)) return; /* queue full: drop */
            if (!sPriming && queued() == 0) {
                /* Ran dry: hold the device until the queue is primed. */
                waveOutPause(sOut);
                sPriming = 1;
            }
        }
        n = BLOCK_FRAMES - sFrames;
        if (n > frames) n = frames;
        memcpy(&sData[sFill][sFrames * 2], lr, (size_t)n * 4);
        sFrames += n;
        lr += n * 2;
        frames -= n;
        if (sFrames == BLOCK_FRAMES) {
            h->dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(sOut, h, sizeof(*h)) != MMSYSERR_NOERROR) h->dwFlags |= WHDR_DONE;
            sFill = (sFill + 1) % BLOCKS;
            sFrames = 0;
            if (sPriming && (queued() >= PRIME_BLOCKS || sFill == 0)) {
                sPriming = 0;
                waveOutRestart(sOut);
            }
        }
    }
}

static int waveout_start(void) {
    WAVEFORMATEX fmt;
    int i;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = HOST_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = HOST_AUDIO_RATE * 4;
    if (waveOutOpen(&sOut, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return 0;
    fprintf(stderr, "audio: waveOut, %d Hz stereo, %d ms buffered\n", HOST_AUDIO_RATE,
            PRIME_BLOCKS * BLOCK_FRAMES * 1000 / HOST_AUDIO_RATE);
    waveOutPause(sOut);
    for (i = 0; i < BLOCKS; i++) {
        sHdr[i].lpData = (LPSTR)sData[i];
        sHdr[i].dwBufferLength = sizeof(sData[i]);
        waveOutPrepareHeader(sOut, &sHdr[i], sizeof(sHdr[i]));
        sHdr[i].dwFlags |= WHDR_DONE;
    }
    return 1;
}

void audio_init(void) {
    const char* e = getenv("RECOMP_AUDIO");
    if (e && *e == '0') return;
    if (!(e && _stricmp(e, "waveout") == 0) && wasapi_start()) {
        g_host_audio_push = ring_push;
        return;
    }
    if (waveout_start()) {
        g_host_audio_push = wave_push;
        return;
    }
    fprintf(stderr, "audio: no output device; running without sound\n");
}
