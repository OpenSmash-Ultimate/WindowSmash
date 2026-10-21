/* Audio output (audout:u), natively: WASAPI on Windows (waveOut where WASAPI cannot start), SDL
 * on Linux and macOS.
 *
 * The game mixes its own sound and streams 48 kHz stereo 16-bit PCM through the SDK's
 * AudioOut interface: it keeps a few short buffers appended and mixes the next one when one is
 * reported played. Reporting a buffer only once the sound card had really played it left too
 * little queued: whenever the report came late (waveOut reports late and in bunches, worse with
 * USB audio interfaces) the card ran dry, and players heard holes of ~17 ms several times a
 * second. So each buffer's sound is copied into a queue of our own, the output device takes from
 * it at its own pace (feed), and a buffer is reported played once the device is within the lead
 * of its end: the game stays that far ahead, its pace still set by the device's clock, as on the
 * console. When the queue runs dry anyway (the game's mixer fell behind), the device plays
 * silence until half the lead is back: one longer gap instead of a stutter.
 *
 * SSBU_AUDIO_LATENCY_MS sets the lead (default 20), SSBU_AUDIO=waveout picks the older Windows
 * output, SSBU_AUDIO_LOG=1 logs every buffer the game appends. */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipc.h"
#ifdef _WIN32
#define COBJMACROS
#include <mmsystem.h>   /* after windows.h */
#include <mmdeviceapi.h>
#include <audioclient.h>
#elif defined __APPLE__
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

#define RATE 48000
#define CHANNELS 2
#define FRAME (2 * CHANNELS)   /* bytes per sample frame */
#define MAX_BUFFERS 64
#define RING (1u << 20)        /* bytes: 5.4 s */
#define DEFAULT_LEAD_MS 20

static struct {
    u8 ring[RING];
    u64 wpos, rpos;                               /* bytes appended / taken by the device, ever */
    struct { u64 tag, end; } held[MAX_BUFFERS];   /* appended, not reported played yet; oldest first */
    u32 head, count;
    u64 released[MAX_BUFFERS * 2];
    u32 nreleased;
    u32 event;
    int started;
    int output;      /* a device takes the sound (else buffers go straight back) */
    int dry;         /* playing silence until the queue has half the lead again */
    u32 lead;        /* bytes */
    u64 dry_count;
    f32 volume;
    CRITICAL_SECTION lock;
} audio = {.volume = 1.0f};
static HANDLE played_event;   /* feed released buffers: the reaper tells the game */
static int audio_log;

/* Moves the held buffers that end within the lead of the device's position to the released list
 * (audio.lock held). Returns how many. */
static int release_due(void) {
    int n = 0;
    while (audio.count && audio.held[audio.head].end <= audio.rpos + audio.lead && audio.nreleased < MAX_BUFFERS * 2) {
        audio.released[audio.nreleased++] = audio.held[audio.head].tag;
        audio.head = (audio.head + 1) % MAX_BUFFERS;
        audio.count--;
        n++;
    }
    return n;
}

/* (the output's thread) the next `len` bytes the device plays */
static void feed(u8 *out, u32 len) {
    EnterCriticalSection(&audio.lock);
    u64 queued = audio.wpos - audio.rpos;
    if (audio.dry && queued >= audio.lead / 2) audio.dry = 0;
    u32 n = audio.dry ? 0 : (u32)(queued < len ? queued : len) & ~(u32)(FRAME - 1);
    for (u32 done = 0; done < n;) {
        u32 at = (u32)((audio.rpos + done) % RING), k = n - done;
        if (k > RING - at) k = RING - at;
        memcpy(out + done, audio.ring + at, k);
        done += k;
    }
    audio.rpos += n;
    u64 dry = 0;
    if (n < len) {
        memset(out + n, 0, len - n);
        if (!audio.dry && audio.wpos) { audio.dry = 1; dry = ++audio.dry_count; }
    }
    int any = release_due();
    LeaveCriticalSection(&audio.lock);
    if (any) SetEvent(played_event);
    if (dry && (dry <= 10 || dry % 100 == 0 || audio_log))
        fprintf(stderr, "[audio] the game's sound ran out at %.3f s (%llu times so far): waiting for %u ms of it\n",
                rt_now_ns() / 1e9, (unsigned long long)dry, audio.lead / (RATE / 1000 * FRAME) / 2);
}

static DWORD WINAPI reaper(void *arg) {
    (void)arg;
    SetThreadDescription(GetCurrentThread(), L"audio reaper");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        WaitForSingleObject(played_event, INFINITE);
        kernel_signal_event(audio.event);
    }
}

#ifdef _WIN32
/* ---- WASAPI: shared mode, the device's event asks for more; Windows converts 48 kHz to the
 * device's own rate. Follows Windows' default output device; while there is none, the sound is
 * taken (and dropped) in real time so the game keeps going. */
static const GUID CLSID_MMDeviceEnumerator_ = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_IMMDeviceEnumerator_ = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_IAudioClient_ = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID IID_IAudioRenderClient_ = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

typedef struct {
    IMMDeviceEnumerator *devices;
    IMMDevice *dev;
    IAudioClient *client;
    IAudioRenderClient *render;
    WCHAR *id;
    HANDLE ready;
    UINT32 frames;
} Wasapi;

static void wasapi_close(Wasapi *w) {
    if (w->client) IAudioClient_Stop(w->client);
    if (w->render) IAudioRenderClient_Release(w->render);
    if (w->client) IAudioClient_Release(w->client);
    if (w->dev) IMMDevice_Release(w->dev);
    if (w->id) CoTaskMemFree(w->id);
    w->render = NULL; w->client = NULL; w->dev = NULL; w->id = NULL;
}

static int wasapi_open(Wasapi *w) {
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, CHANNELS, RATE, RATE * FRAME, FRAME, 16, 0}, *mix = NULL;
    HRESULT hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(w->devices, eRender, eConsole, &w->dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_GetId(w->dev, &w->id);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(w->dev, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **)&w->client);
    if (SUCCEEDED(hr)) hr = IAudioClient_Initialize(w->client, AUDCLNT_SHAREMODE_SHARED,
                                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 200000 /* 20 ms */, 0, &fmt, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_SetEventHandle(w->client, w->ready);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(w->client, &w->frames);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(w->client, &IID_IAudioRenderClient_, (void **)&w->render);
    if (SUCCEEDED(hr)) hr = IAudioClient_Start(w->client);
    if (FAILED(hr)) {
        fprintf(stderr, "[audio] WASAPI could not start (error %08lx)\n", (unsigned long)hr);
        wasapi_close(w);
        return 0;
    }
    if (FAILED(IAudioClient_GetMixFormat(w->client, &mix))) mix = NULL;
    fprintf(stderr, "[audio] WASAPI output: the device mixes at %lu Hz, %u-frame buffer, the game %u ms ahead\n",
            mix ? (unsigned long)mix->nSamplesPerSec : 0ul, w->frames, audio.lead / (RATE / 1000 * FRAME));
    CoTaskMemFree(mix);
    return 1;
}

/* whether Windows' default output is no longer the one playing */
static int wasapi_default_changed(Wasapi *w) {
    IMMDevice *d;
    WCHAR *id;
    int changed = 0;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(w->devices, eRender, eConsole, &d))) return 0;
    if (SUCCEEDED(IMMDevice_GetId(d, &id))) { changed = !w->id || wcscmp(id, w->id) != 0; CoTaskMemFree(id); }
    IMMDevice_Release(d);
    return changed;
}

static volatile LONG wasapi_ok;

static DWORD WINAPI wasapi_thread(void *arg) {
    HANDLE opened = arg;
    SetThreadDescription(GetCurrentThread(), L"audio output");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    Wasapi w = {0};
    w.ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    int ok = SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **)&w.devices)) &&
             wasapi_open(&w);
    InterlockedExchange(&wasapi_ok, ok);
    SetEvent(opened);
    if (!ok) {
        if (w.devices) IMMDeviceEnumerator_Release(w.devices);
        CloseHandle(w.ready);
        CoUninitialize();
        return 0;
    }
    static u8 scratch[RATE / 50 * FRAME];   /* 20 ms */
    u64 checked = rt_now_ns(), last = checked;
    for (;;) {
        u64 now = rt_now_ns();
        if (!w.client) {   /* no device: take the sound in real time; look for one twice a second */
            Sleep(5);
            now = rt_now_ns();
            u64 frames = (now - last) * RATE / 1000000000ull;
            if (frames > RATE / 50) frames = RATE / 50;
            if (frames) { feed(scratch, (u32)frames * FRAME); last += frames * 1000000000ull / RATE; }
            if (now - checked >= 500000000ull) {
                checked = now;
                if (wasapi_open(&w)) last = now;
            }
            continue;
        }
        WaitForSingleObject(w.ready, 100);
        UINT32 padding, n;
        BYTE *data;
        HRESULT hr = IAudioClient_GetCurrentPadding(w.client, &padding);
        if (SUCCEEDED(hr) && (n = w.frames - padding) > 0) {
            hr = IAudioRenderClient_GetBuffer(w.render, n, &data);
            if (SUCCEEDED(hr)) {
                feed(data, n * FRAME);
                hr = IAudioRenderClient_ReleaseBuffer(w.render, n, 0);
            }
        }
        now = rt_now_ns();
        if (FAILED(hr)) {   /* unplugged, or taken over */
            fprintf(stderr, "[audio] the output device stopped (error %08lx)\n", (unsigned long)hr);
            wasapi_close(&w);
            checked = last = now;
        } else if (now - checked >= 1000000000ull) {   /* headphones plugged in, another output chosen */
            checked = now;
            if (wasapi_default_changed(&w)) {
                fprintf(stderr, "[audio] Windows' default output changed\n");
                wasapi_close(&w);
                if (!wasapi_open(&w)) last = now;
            }
        }
    }
}

/* ---- waveOut: four 10 ms buffers kept queued, each refilled from feed when it has played */
static DWORD WINAPI waveout_thread(void *arg) {
    HANDLE opened = arg;
    SetThreadDescription(GetCurrentThread(), L"audio output");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE done = CreateEventA(NULL, FALSE, FALSE, NULL);
    HWAVEOUT out;
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, CHANNELS, RATE, RATE * FRAME, FRAME, 16, 0};
    MMRESULT r = waveOutOpen(&out, WAVE_MAPPER, &fmt, (DWORD_PTR)done, 0, CALLBACK_EVENT);
    InterlockedExchange(&wasapi_ok, r == MMSYSERR_NOERROR);
    SetEvent(opened);
    if (r != MMSYSERR_NOERROR) { CloseHandle(done); return 0; }
    fprintf(stderr, "[audio] waveOut output, the game %u ms ahead\n", audio.lead / (RATE / 1000 * FRAME));
    static WAVEHDR hdr[4];
    static u8 data[4][RATE / 100 * FRAME];
    for (int i = 0; i < 4; i++) {
        hdr[i].lpData = (LPSTR)data[i];
        hdr[i].dwBufferLength = sizeof data[i];
        waveOutPrepareHeader(out, &hdr[i], sizeof hdr[i]);
        feed(data[i], sizeof data[i]);
        waveOutWrite(out, &hdr[i], sizeof hdr[i]);
    }
    for (;;) {
        WaitForSingleObject(done, 100);
        for (int i = 0; i < 4; i++)
            if (hdr[i].dwFlags & WHDR_DONE) {
                feed(data[i], sizeof data[i]);
                waveOutWrite(out, &hdr[i], sizeof hdr[i]);
            }
    }
}

static int start_output(void) {
    const char *want = getenv("SSBU_AUDIO");
    HANDLE opened = CreateEventA(NULL, FALSE, FALSE, NULL);
    int ok = 0;
    for (int waveout = want && !_stricmp(want, "waveout"); waveout < 2 && !ok; waveout++) {
        CloseHandle(CreateThread(NULL, 0, waveout ? waveout_thread : wasapi_thread, opened, 0, NULL));
        WaitForSingleObject(opened, INFINITE);
        ok = (int)wasapi_ok;
    }
    CloseHandle(opened);
    return ok;
}

#else
/* ---- SDL: its audio thread asks for what the device is about to play */
static void SDLCALL sdl_feed(void *user, Uint8 *out, int len) {
    (void)user;
    feed(out, (u32)len);
}

#ifdef __APPLE__
static void SDLCALL pull(void *user, SDL_AudioStream *stream, int wanted, int total) {
    (void)user; (void)total;
    Uint8 buf[4096];
    for (wanted = (wanted + 3) & ~3; wanted > 0; wanted -= (int)sizeof buf) {   /* (whole frames) */
        int n = wanted < (int)sizeof buf ? wanted : (int)sizeof buf;
        feed(buf, (u32)n);
        SDL_PutAudioStreamData(stream, buf, n);
    }
}
#endif

static int start_output(void) {
#ifdef __APPLE__
    /* (SDL's audio started with the window, on the main thread) */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want = {SDL_AUDIO_S16, CHANNELS, RATE}, got;
    int frames = 0;
    SDL_AudioStream *out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, pull, NULL);
    if (!out) { fprintf(stderr, "[audio] no audio device (%s); sound is dropped\n", SDL_GetError()); return 0; }
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(out), &got, &frames)) got.freq = 0;
    fprintf(stderr, "[audio] SDL %s output, %d Hz, %d-sample periods\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", got.freq, frames);
    SDL_ResumeAudioStreamDevice(out);
#else
    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof want);
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = CHANNELS;
    want.samples = 256;
    want.callback = sdl_feed;
    if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioDeviceID out = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!out) { fprintf(stderr, "[audio] no audio device (%s); sound is dropped\n", SDL_GetError()); return 0; }
    fprintf(stderr, "[audio] SDL %s output, %d Hz, %d-sample periods\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", got.freq, got.samples);
    SDL_PauseAudioDevice(out, 0);
#endif
    return 1;
}
#endif

static void open_device(void) {
    static int opened;
    if (opened++) return;
    InitializeCriticalSection(&audio.lock);
    const char *v = getenv("SSBU_AUDIO_LATENCY_MS");
    int ms = v ? atoi(v) : DEFAULT_LEAD_MS;
    if (ms < 5) ms = 5;
    if (ms > 1000) ms = 1000;
    audio.lead = (u32)ms * (RATE / 1000) * FRAME;
    audio.dry = 1;   /* (the device starts once half the lead is there) */
    audio_log = (v = getenv("SSBU_AUDIO_LOG")) && *v && *v != '0';
    audio.event = kernel_create_event();
    played_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, reaper, NULL, 0, NULL));
    audio.output = start_output();
    if (!audio.output) fprintf(stderr, "[audio] no audio device; sound is dropped\n");
}

/* Append { u64 next; u64 address; u64 capacity; u64 size; u64 offset } with a 64-bit tag. */
static void append(u64 desc, u64 tag) {
    u64 addr = ld64(desc + 8), size = ld64(desc + 24) & ~(u64)(FRAME - 1), offset = ld64(desc + 32);
    static DWORD raised;   /* (the game's sound thread keeps pace with the device: before other work) */
    if (raised != GetCurrentThreadId()) { raised = GetCurrentThreadId(); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }
    EnterCriticalSection(&audio.lock);
    int any;
    if (!audio.output || audio.count == MAX_BUFFERS || audio.wpos + size - audio.rpos > RING) {
        /* no device (or saturated): hand the buffer straight back */
        if (audio.nreleased < MAX_BUFFERS * 2) audio.released[audio.nreleased++] = tag;
        any = 1;
    } else {
        const u8 *src = P(u8, addr + offset);
        for (u64 done = 0; done < size;) {
            u32 at = (u32)((audio.wpos + done) % RING), k = (u32)(size - done);
            if (k > RING - at) k = RING - at;
            memcpy(audio.ring + at, src + done, k);
            if (audio.volume != 1.0f) {
                i16 *s = (i16 *)(audio.ring + at);
                for (u32 i = 0; i < k / 2; i++) s[i] = (i16)(s[i] * audio.volume);
            }
            done += k;
        }
        audio.wpos += size;
        audio.held[(audio.head + audio.count) % MAX_BUFFERS].tag = tag;
        audio.held[(audio.head + audio.count) % MAX_BUFFERS].end = audio.wpos;
        audio.count++;
        any = release_due();
    }
    u64 queued = audio.wpos - audio.rpos;
    u32 held = audio.count;
    LeaveCriticalSection(&audio.lock);
    if (any) kernel_signal_event(audio.event);
    if (audio_log)
        fprintf(stderr, "[audio] %.4f s: appended %llu bytes (%.2f ms); queued %.2f ms, %u held\n", rt_now_ns() / 1e9,
                (unsigned long long)size, size / (double)(RATE / 1000 * FRAME), queued / (double)(RATE / 1000 * FRAME), held);
}

static void audio_out(IpcObject *self, Ipc *ipc) {
    u64 size, b;
    switch (ipc->command) {
    case 0: IPC_PUSH(ipc, (u32)(audio.started ? 0 : 1)); return;   /* GetAudioOutState */
    case 1: audio.started = 1; return;                             /* Start */
    case 2: audio.started = 0; return;                             /* Stop */
    case 3: case 7:                                                /* AppendAudioOutBuffer(Auto)(tag) */
        b = ipc_in_buffer(ipc, 0, &size);
        if (rt_trace) {
            fprintf(stderr, "[audio] append: %u raw bytes:", ipc->in_size);
            for (u32 i = 0; i < ipc->in_size && i < 32; i += 4) fprintf(stderr, " %08x", ipc_u32(ipc, i));
            fprintf(stderr, "; buffer %#llx (%llu bytes):", b, size);
            for (u64 i = 0; b && i < size && i < 40; i += 8) fprintf(stderr, " %llx", ld64(b + i));
            fprintf(stderr, " (a %u, x %u)\n", ipc->na, ipc->nx);
        }
        if (b && size >= 40) append(b, ipc_u64(ipc, 0));
        return;
    case 4: ipc_copy_handle(ipc, audio.event); return;            /* RegisterBufferEvent */
    case 5: case 8: {                                              /* GetReleasedAudioOutBuffers(Auto) */
        b = ipc_out_buffer(ipc, 0, &size);
        if (b) memset(P(void, b), 0, size);   /* the client reads the first entry whatever the count */
        EnterCriticalSection(&audio.lock);
        u32 n = 0;
        while (n < audio.nreleased && (n + 1) * 8 <= size) { st64(b + 8ull * n, audio.released[n]); n++; }
        memmove(audio.released, audio.released + n, (audio.nreleased - n) * 8);
        audio.nreleased -= n;
        LeaveCriticalSection(&audio.lock);
        if (rt_trace)
            fprintf(stderr, "[audio] released %u (buffer %#llx, %llu bytes; b %u (%#llx), c %u (%#llx))\n", n, b, size,
                    ipc->nb, ipc->nb ? ipc->b[0].addr : 0, ipc->nc, ipc->nc ? ipc->c[0].addr : 0);
        ipc_wrote(ipc, 0, size);
        IPC_PUSH(ipc, n);
        return;
    }
    case 6: IPC_PUSH(ipc, (u32)0); return;                         /* ContainsAudioOutBuffer */
    case 9: {                                                      /* GetAudioOutBufferCount */
        EnterCriticalSection(&audio.lock);
        u32 n = audio.count;
        LeaveCriticalSection(&audio.lock);
        IPC_PUSH(ipc, n);
        return;
    }
    case 10: IPC_PUSH(ipc, audio.rpos / FRAME); return;            /* GetAudioOutPlayedSampleCount */
    case 11: IPC_PUSH(ipc, (u8)0); return;                         /* FlushAudioOutBuffers */
    case 12: memcpy(&audio.volume, ipc->in, 4); return;            /* SetAudioOutVolume */
    case 13: ipc_push(ipc, &audio.volume, 4); return;              /* GetAudioOutVolume */
    }
    ipc_unknown(ipc, self);
}

static void manager(IpcObject *self, Ipc *ipc) {
    u64 size, b;
    switch (ipc->command) {
    case 0: case 2:   /* ListAudioOuts(Auto): one device */
        b = ipc_out_buffer(ipc, 0, &size);
        if (b && size >= 0x100) { memset(P(void, b), 0, 0x100); memcpy(P(void, b), "DeviceOut", 10); ipc_wrote(ipc, 0, 0x100); }
        IPC_PUSH(ipc, (u32)1);
        return;
    case 1: case 3: { /* OpenAudioOut(Auto)(parameter, applet) -> rate, channels, format, state; IAudioOut */
        open_device();
        b = ipc_out_buffer(ipc, 0, &size);
        if (b && size >= 0x100) { memset(P(void, b), 0, 0x100); memcpy(P(void, b), "DeviceOut", 10); ipc_wrote(ipc, 0, 0x100); }
        u32 out[4] = {RATE, CHANNELS, 2 /* PCM int16 */, 1 /* stopped */};
        ipc_push(ipc, out, sizeof out);
        ipc_object(ipc, ipc_new_object("audout:IAudioOut", audio_out, NULL));
        return;
    }
    }
    ipc_unknown(ipc, self);
}

/* ---- audren:u: only its audio-device interface is used (device name, volume, events) -- */
#define DEVICE "AudioTvOutput"

static void put_name(Ipc *ipc, u32 index) {
    u64 size, b = ipc_out_buffer(ipc, index, &size);
    if (b && size >= 0x100) {
        memset(P(void, b), 0, 0x100);
        memcpy(P(void, b), DEVICE, sizeof DEVICE);
        ipc_wrote(ipc, index, 0x100);
    }
}

static void audio_device(IpcObject *self, Ipc *ipc) {
    static u32 event;
    if (!event) event = kernel_create_event();
    switch (ipc->command) {
    case 0: case 6: put_name(ipc, 0); IPC_PUSH(ipc, (u32)1); return;   /* ListAudioDeviceName(Auto) */
    case 1: case 7: return;                                            /* SetAudioDeviceOutputVolume(Auto) */
    case 2: case 8: { f32 v = 1.0f; ipc_push(ipc, &v, 4); return; }   /* GetAudioDeviceOutputVolume(Auto) */
    case 3: case 10: case 13: put_name(ipc, 0); return;                /* GetActiveAudioDeviceName(Auto) */
    case 4: case 11: case 12: ipc_copy_handle(ipc, event); return;     /* device change / input / output events */
    case 5: IPC_PUSH(ipc, (u32)CHANNELS); return;                      /* GetActiveChannelCount */
    }
    ipc_unknown(ipc, self);
}

static void renderer_manager(IpcObject *self, Ipc *ipc) {
    switch (ipc->command) {
    case 1: IPC_PUSH(ipc, (u64)0x10000); return;                       /* GetWorkBufferSize */
    case 2: case 4:                                                    /* GetAudioDeviceService(WithRevisionInfo) */
        ipc_object(ipc, ipc_new_object("audren:IAudioDevice", audio_device, NULL));
        return;
    }
    if (ipc->command == 0) rt_fatal("the game opened an audio renderer, which is not implemented");
    ipc_unknown(ipc, self);
}

IpcObject *service_audio(const char *name) {
    if (!strcmp(name, "audout:u")) return ipc_new_object("audout:u", manager, NULL);
    if (!strcmp(name, "audren:u")) return ipc_new_object("audren:u", renderer_manager, NULL);
    return NULL;
}
