/* nexsim - replay the way NetEase Cloud Music drives WASAPI, against the real
   nexsink.dll and the real default output device, and check what comes back.

   The unit tests drive the sink with mock endpoints; this tool is the other
   half (after Orion's playlist simulators): it loads nexsink.dll into its own
   process, so the DLL patches the IAudioClient vtables here exactly as it does
   inside cloudmusic.exe, and then plays scripted scenarios through the normal
   shared-mode, event-driven client contract:

     handoff   two tracks of the same format back to back (close, reopen)
     format    44.1 kHz -> 48 kHz -> 96 kHz -> 44.1 kHz (clock-family changes)
     pause     Stop for 1.5 s mid-track, then Start again
     seek      Stop, Reset, Start (what the client does on a seek)
     playlist  all of the above in one run
     release   idle_release_ms: pause long enough that the device is given
               back (another process must then be able to open it), resume
     share     auto_shared: another process is sounding when the track
               starts, so the sink opens shared; once it is quiet, a pause
               and resume go back to exclusive
     conflict  the host's rule end to end: while the track plays exclusively,
               start a video or audio in a browser; its media session turning
               Playing makes this tool post "close exclusive" to the module's
               control block, as the host does, and the stream must go shared
               at once and let another process open the device
   release, share and conflict write their own temporary hook.cfg unless
   --cfg is given.

   Each track plays a -20 dBFS sine. Checks per track: every call succeeds;
   the client's clock never goes backwards, holds still while paused, restarts
   after a seek, and stays within tolerance of the time actually played; the
   client's event never goes silent for longer than 500 ms. With --log the
   sink log is scanned afterwards for faults and empty device buffers.

   It needs a real output device and takes it exclusively while it runs.

   usage: nexsim [--dll nexsink.dll] [--cfg hook.cfg] [--log sink.log]
                 [--seconds N] [--quiet] [scenario ...]
   --quiet plays the sine at about -70 dBFS instead of -20 dBFS: still a
   signal for the peak meters, but nothing you would hear.
   exit code: 0 all checks passed, 1 a check failed, 2 setup failed. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <audiopolicy.h>
#include <math.h>
#include "nexsmtc.h"
#include "nexcontrol.h"
#include "nexconflict.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TrackFormat {
    DWORD rate;
    WORD  bits;        /* 16, 24, 32 */
    int   is_float;
    WORD  channels;
} TrackFormat;

typedef struct Track {
    IAudioClient       *client;
    IAudioRenderClient *render;
    IAudioClock        *clock;
    HANDLE              event;
    TrackFormat         fmt;
    UINT32              frames;     /* the client-visible buffer */
    UINT64              clock_freq;
    UINT64              written;    /* frames handed to the sink */
    double              phase;
    UINT64              last_pos;   /* 100 ns, from IAudioClock */
} Track;

static int g_fail;
static int g_checks;
static double g_amp = 0.1;
static double g_seconds = 3.0;
static IMMDevice *g_dev;

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; \
    printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void fill_wfx(WAVEFORMATEXTENSIBLE *w, const TrackFormat *f) {
    memset(w, 0, sizeof(*w));
    w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels = f->channels;
    w->Format.nSamplesPerSec = f->rate;
    w->Format.wBitsPerSample = f->bits == 24 ? 32 : f->bits;
    w->Format.nBlockAlign = (WORD)(f->channels * w->Format.wBitsPerSample / 8);
    w->Format.nAvgBytesPerSec = f->rate * w->Format.nBlockAlign;
    w->Format.cbSize = sizeof(*w) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = f->bits;
    w->dwChannelMask = f->channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : SPEAKER_FRONT_CENTER;
    w->SubFormat = f->is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

static const char *fmt_name(const TrackFormat *f, char *out, size_t n) {
    snprintf(out, n, "%lu Hz/%u%s/%uch", (unsigned long)f->rate, f->bits, f->is_float ? "f" : "",
             f->channels);
    return out;
}

static int track_open(Track *t, TrackFormat fmt) {
    memset(t, 0, sizeof(*t));
    t->fmt = fmt;
    HRESULT hr = IMMDevice_Activate(g_dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&t->client);
    CHECK(SUCCEEDED(hr) && t->client, "Activate 0x%08lX", (unsigned long)hr);
    if (FAILED(hr) || !t->client) return 0;
    WAVEFORMATEXTENSIBLE w;
    fill_wfx(&w, &fmt);
    /* What the client does: shared mode, event driven, ~30 ms buffer. */
    hr = IAudioClient_Initialize(t->client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 300000, 0,
                                 (WAVEFORMATEX *)&w, NULL);
    CHECK(SUCCEEDED(hr), "Initialize 0x%08lX", (unsigned long)hr);
    if (FAILED(hr)) return 0;
    t->event = CreateEventA(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_SetEventHandle(t->client, t->event);
    CHECK(SUCCEEDED(hr), "SetEventHandle 0x%08lX", (unsigned long)hr);
    hr = IAudioClient_GetBufferSize(t->client, &t->frames);
    CHECK(SUCCEEDED(hr) && t->frames, "GetBufferSize 0x%08lX frames=%u", (unsigned long)hr, t->frames);
    hr = IAudioClient_GetService(t->client, &IID_IAudioRenderClient, (void **)&t->render);
    CHECK(SUCCEEDED(hr) && t->render, "GetService(render) 0x%08lX", (unsigned long)hr);
    hr = IAudioClient_GetService(t->client, &IID_IAudioClock, (void **)&t->clock);
    CHECK(SUCCEEDED(hr) && t->clock, "GetService(clock) 0x%08lX", (unsigned long)hr);
    if (t->clock) IAudioClock_GetFrequency(t->clock, &t->clock_freq);
    return SUCCEEDED(hr) && t->render && t->clock && t->frames;
}

static void track_close(Track *t) {
    if (t->clock) IAudioClock_Release(t->clock);
    if (t->render) IAudioRenderClient_Release(t->render);
    if (t->client) IAudioClient_Release(t->client);
    if (t->event) CloseHandle(t->event);
    memset(t, 0, sizeof(*t));
}

static void synth(Track *t, BYTE *dst, UINT32 n) {
    const double amp = g_amp, step = 2.0 * 3.14159265358979 * 440.0 / t->fmt.rate;
    for (UINT32 i = 0; i < n; i++) {
        double v = amp * sin(t->phase);
        t->phase += step;
        if (t->phase > 6.283185307179586) t->phase -= 6.283185307179586;
        for (WORD c = 0; c < t->fmt.channels; c++) {
            if (t->fmt.is_float) { ((float *)dst)[0] = (float)v; dst += 4; }
            else if (t->fmt.bits == 16) { ((INT16 *)dst)[0] = (INT16)(v * 32767.0); dst += 2; }
            else { ((INT32 *)dst)[0] = (INT32)(v * 2147483647.0); dst += 4; }
        }
    }
}

/* 100 ns from the client's IAudioClock. */
static UINT64 track_position(Track *t) {
    UINT64 p = 0, q = 0;
    HRESULT hr = IAudioClock_GetPosition(t->clock, &p, &q);
    CHECK(SUCCEEDED(hr), "GetPosition 0x%08lX", (unsigned long)hr);
    if (FAILED(hr) || !t->clock_freq) return t->last_pos;
    UINT64 ticks = (UINT64)((double)p * 10000000.0 / (double)t->clock_freq);
    return ticks;
}

/* Feed the client buffer for `seconds` like the client's render thread. */
static void track_play(Track *t, double seconds, int *late) {
    DWORD start = GetTickCount();
    while ((double)(GetTickCount() - start) < seconds * 1000.0) {
        DWORD w = WaitForSingleObject(t->event, 500);
        if (w != WAIT_OBJECT_0) { (*late)++; continue; }
        UINT32 pad = 0;
        HRESULT hr = IAudioClient_GetCurrentPadding(t->client, &pad);
        if (FAILED(hr)) { CHECK(0, "GetCurrentPadding 0x%08lX", (unsigned long)hr); return; }
        UINT32 n = t->frames - pad;
        if (!n) continue;
        BYTE *dst = NULL;
        hr = IAudioRenderClient_GetBuffer(t->render, n, &dst);
        if (FAILED(hr)) { CHECK(0, "GetBuffer(%u) 0x%08lX", n, (unsigned long)hr); return; }
        synth(t, dst, n);
        hr = IAudioRenderClient_ReleaseBuffer(t->render, n, 0);
        if (FAILED(hr)) { CHECK(0, "ReleaseBuffer 0x%08lX", (unsigned long)hr); return; }
        t->written += n;
        UINT64 pos = track_position(t);
        CHECK(pos >= t->last_pos, "clock went backwards %.3f -> %.3f s",
              t->last_pos / 1e7, pos / 1e7);
        t->last_pos = pos;
    }
}

/* Prefill one buffer and Start, as the client does. */
static void track_start(Track *t) {
    UINT32 pad = 0;
    IAudioClient_GetCurrentPadding(t->client, &pad);
    UINT32 n = t->frames - pad;
    BYTE *dst = NULL;
    if (n && SUCCEEDED(IAudioRenderClient_GetBuffer(t->render, n, &dst))) {
        synth(t, dst, n);
        IAudioRenderClient_ReleaseBuffer(t->render, n, 0);
        t->written += n;
    }
    HRESULT hr = IAudioClient_Start(t->client);
    CHECK(SUCCEEDED(hr), "Start 0x%08lX", (unsigned long)hr);
}

/* The clock must cover the time played, minus start-up latency, prime and
   resync silence (up to ~0.45 s), and never run ahead of what was written. */
static void check_progress(Track *t, double played_s, const char *what) {
    double pos = t->last_pos / 1e7;
    double written = (double)t->written / t->fmt.rate;
    CHECK(pos >= played_s - 0.6, "%s: clock %.3f s behind %.3f s played", what, pos, played_s);
    CHECK(pos <= written + 0.01, "%s: clock %.3f s ahead of %.3f s written", what, pos, written);
}

static void scenario_track(TrackFormat f, double seconds, int *late) {
    Track t;
    char name[64];
    printf("  track %s, %.1f s\n", fmt_name(&f, name, sizeof(name)), seconds);
    if (!track_open(&t, f)) { track_close(&t); return; }
    track_start(&t);
    track_play(&t, seconds, late);
    check_progress(&t, seconds, name);
    IAudioClient_Stop(t.client);
    track_close(&t);
}

static void scenario_handoff(int *late) {
    TrackFormat f = {44100, 32, 1, 2};
    printf("handoff: same format, close and reopen\n");
    scenario_track(f, g_seconds, late);
    scenario_track(f, g_seconds, late);
}

static void scenario_format(int *late) {
    TrackFormat a = {44100, 32, 1, 2}, b = {48000, 32, 1, 2}, c = {96000, 24, 0, 2};
    printf("format: clock-family changes\n");
    scenario_track(a, g_seconds, late);
    scenario_track(b, g_seconds, late);
    scenario_track(c, g_seconds, late);
    scenario_track(a, g_seconds, late);
}

static void scenario_pause(int *late) {
    TrackFormat f = {44100, 32, 1, 2};
    Track t;
    printf("pause: Stop 1.5 s mid-track, Start again\n");
    if (!track_open(&t, f)) { track_close(&t); return; }
    track_start(&t);
    track_play(&t, g_seconds, late);
    HRESULT hr = IAudioClient_Stop(t.client);
    CHECK(SUCCEEDED(hr), "Stop 0x%08lX", (unsigned long)hr);
    UINT64 at_stop = track_position(&t);
    Sleep(1500);
    UINT64 after = track_position(&t);
    CHECK(after <= at_stop + 200000, "clock ran %.3f s while paused", (after - at_stop) / 1e7);
    t.last_pos = after;
    hr = IAudioClient_Start(t.client);
    CHECK(SUCCEEDED(hr), "Start after pause 0x%08lX", (unsigned long)hr);
    track_play(&t, g_seconds, late);
    check_progress(&t, 2 * g_seconds, "after resume");
    IAudioClient_Stop(t.client);
    track_close(&t);
}

static void scenario_seek(int *late) {
    TrackFormat f = {48000, 32, 1, 2};
    Track t;
    printf("seek: Stop, Reset, Start\n");
    if (!track_open(&t, f)) { track_close(&t); return; }
    track_start(&t);
    track_play(&t, g_seconds, late);
    IAudioClient_Stop(t.client);
    HRESULT hr = IAudioClient_Reset(t.client);
    CHECK(SUCCEEDED(hr), "Reset 0x%08lX", (unsigned long)hr);
    UINT64 pos = track_position(&t);
    CHECK(pos < 1000000, "clock did not restart after Reset (%.3f s)", pos / 1e7);
    t.last_pos = pos;
    t.written = 0;
    track_start(&t);
    track_play(&t, g_seconds, late);
    check_progress(&t, g_seconds, "after seek");
    IAudioClient_Stop(t.client);
    track_close(&t);
}

/* ---- helper processes for release/share --------------------------------
   They run without nexsink.dll, so their streams are plain shared WASAPI
   clients, like a browser. */
static int child_shared(double seconds) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&en)) || !en ||
        FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) || !dev)
        return 4;
    IAudioClient *ac = NULL;
    if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) || !ac)
        return 4;
    WAVEFORMATEX *mix = NULL;
    IAudioClient_GetMixFormat(ac, &mix);
    HRESULT hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, NULL);
    if ((unsigned long)hr == 0x8889000Aul) return 3;
    if (FAILED(hr)) return 4;
    if (seconds <= 0) return 0;          /* probe only: the device can be opened */
    IAudioRenderClient *rc = NULL;
    UINT32 frames = 0;
    IAudioClient_GetBufferSize(ac, &frames);
    IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    double phase = 0, step = 2.0 * 3.14159265358979 * 440.0 / mix->nSamplesPerSec;
    IAudioClient_Start(ac);
    DWORD start = GetTickCount();
    while ((double)(GetTickCount() - start) < seconds * 1000.0) {
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(ac, &pad))) break;
        UINT32 n = frames - pad;
        BYTE *dst = NULL;
        if (n && rc && SUCCEEDED(IAudioRenderClient_GetBuffer(rc, n, &dst))) {
            float *f = (float *)dst;      /* the shared mix format is float32 */
            for (UINT32 i = 0; i < n; i++) {
                float v = (float)(g_amp * sin(phase));
                phase += step;
                for (WORD c = 0; c < mix->nChannels; c++) *f++ = v;
            }
            IAudioRenderClient_ReleaseBuffer(rc, n, 0);
        }
        Sleep(10);
    }
    IAudioClient_Stop(ac);
    return 0;
}

/* Run a helper. as_other copies this executable under another name first,
   because the sink ignores sessions of its own executable name. */
static HANDLE spawn_child(const char *args, int as_other) {
    char exe[MAX_PATH], cmd[MAX_PATH * 2];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (as_other) {
        char other[MAX_PATH];
        GetTempPathA(MAX_PATH, other);
        strncat(other, "nexsim-other.exe", MAX_PATH - strlen(other) - 1);
        CopyFileA(exe, other, FALSE);
        snprintf(exe, sizeof(exe), "%s", other);
    }
    snprintf(cmd, sizeof(cmd), "\"%s\" %s%s", exe, args, g_amp < 0.01 ? " --quiet" : "");
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return NULL;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* 0 = another process could open the device in shared mode, 3 = busy. */
static int probe_shared(void) {
    HANDLE h = spawn_child("--child-probe", 0);
    if (!h) return 4;
    DWORD code = 4;
    WaitForSingleObject(h, 5000);
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return (int)code;
}

/* Another application already audible on the device (a video, a game)?
   release and share assume a quiet device; with one playing, the sink rightly
   stays shared next to it, so those checks are skipped instead of failed. */
typedef struct PeakMeterSim PeakMeterSim;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(PeakMeterSim *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(PeakMeterSim *);
    ULONG   (STDMETHODCALLTYPE *Release)(PeakMeterSim *);
    HRESULT (STDMETHODCALLTYPE *GetPeakValue)(PeakMeterSim *, float *);
} PeakMeterSimVtbl;
struct PeakMeterSim { PeakMeterSimVtbl *lpVtbl; };
static const GUID IID_ASM2_sim = {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const GUID IID_ASC2_sim = {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
static const GUID IID_AMI_sim  = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
static int other_app_sounding(char *name, size_t cap) {
    IAudioSessionManager2 *sm = NULL;
    IAudioSessionEnumerator *en = NULL;
    int found = 0, n = 0;
    name[0] = 0;
    if (FAILED(IMMDevice_Activate(g_dev, &IID_ASM2_sim, CLSCTX_ALL, NULL, (void **)&sm)) || !sm) return 0;
    if (SUCCEEDED(sm->lpVtbl->GetSessionEnumerator(sm, &en)) && en) {
        en->lpVtbl->GetCount(en, &n);
        for (int i = 0; i < n && !found; i++) {
            IAudioSessionControl *c = NULL;
            if (FAILED(en->lpVtbl->GetSession(en, i, &c)) || !c) continue;
            AudioSessionState st = AudioSessionStateInactive;
            c->lpVtbl->GetState(c, &st);
            DWORD pid = 0;
            IAudioSessionControl2 *c2 = NULL;
            if (SUCCEEDED(c->lpVtbl->QueryInterface(c, &IID_ASC2_sim, (void **)&c2)) && c2) {
                c2->lpVtbl->GetProcessId(c2, &pid);
                c2->lpVtbl->Release(c2);
            }
            float peak = 0;
            PeakMeterSim *m = NULL;
            if (SUCCEEDED(c->lpVtbl->QueryInterface(c, &IID_AMI_sim, (void **)&m)) && m) {
                m->lpVtbl->GetPeakValue(m, &peak);
                m->lpVtbl->Release(m);
            }
            if (st == AudioSessionStateActive && peak > 0.0001f && pid && pid != GetCurrentProcessId()) {
                found = 1;
                snprintf(name, cap, "pid %lu", (unsigned long)pid);
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (h) {
                    char path[MAX_PATH];
                    DWORD len = MAX_PATH;
                    if (QueryFullProcessImageNameA(h, 0, path, &len)) {
                        const char *b = strrchr(path, '\\');
                        snprintf(name, cap, "%s", b ? b + 1 : path);
                    }
                    CloseHandle(h);
                }
            }
            c->lpVtbl->Release(c);
        }
        en->lpVtbl->Release(en);
    }
    sm->lpVtbl->Release(sm);
    return found;
}

static int log_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int hit = 0;
    while (!hit && fgets(line, sizeof(line), f)) hit = strstr(line, needle) != NULL;
    fclose(f);
    return hit;
}
static const char *g_log_path;

static void scenario_release(int *late) {
    TrackFormat f = {44100, 32, 1, 2};
    Track t;
    printf("release: pause past idle_release_ms, another process opens the device, resume\n");
    char busy_app[MAX_PATH];
    if (other_app_sounding(busy_app, sizeof(busy_app))) {
        printf("  SKIP: %s is playing on this device; release needs a quiet device "
               "(the sink would rightly open shared next to it)\n", busy_app);
        return;
    }
    if (!track_open(&t, f)) { track_close(&t); return; }
    track_start(&t);
    track_play(&t, g_seconds, late);
    HRESULT hr = IAudioClient_Stop(t.client);
    CHECK(SUCCEEDED(hr), "Stop 0x%08lX", (unsigned long)hr);
    UINT64 at_stop = track_position(&t);
    int busy = probe_shared();
    printf("  right after Stop: another process %s\n",
           busy == 0 ? "opened the device" : busy == 3 ? "got DEVICE_IN_USE" : "failed");
    CHECK(busy == 3, "a fresh pause should still hold the device (probe %d)", busy);
    Sleep(2000);                         /* idle_release_ms=1000 + watch poll */
    Sleep(300);
    CHECK(log_contains(g_log_path, "idle release: paused"), "no idle release line in the log");
    int freed = probe_shared();
    printf("  after the idle release: another process %s\n",
           freed == 0 ? "opened the device" : freed == 3 ? "got DEVICE_IN_USE" : "failed");
    CHECK(freed == 0, "the device was not given back (probe %d)", freed);
    UINT64 held = track_position(&t);
    CHECK(held + 200000 >= at_stop && held <= at_stop + 200000,
          "clock moved while released: %.3f -> %.3f s", at_stop / 1e7, held / 1e7);
    t.last_pos = held;
    hr = IAudioClient_Start(t.client);
    CHECK(SUCCEEDED(hr), "Start after the release 0x%08lX", (unsigned long)hr);
    track_play(&t, g_seconds, late);
    CHECK(log_contains(g_log_path, "retarget(resume after idle release)"),
          "resume did not go through the rebuild path");
    check_progress(&t, 2 * g_seconds, "after the release");
    IAudioClient_Stop(t.client);
    track_close(&t);
}

static void scenario_share(int *late) {
    TrackFormat f = {48000, 32, 1, 2};
    Track t;
    printf("share: another process is sounding when the track starts\n");
    char busy_app[MAX_PATH];
    int busy_before = other_app_sounding(busy_app, sizeof(busy_app));
    /* A previous scenario may have left its stream paused, and a paused
       exclusive stream still owns the device until idle_release_ms gives it
       back; the helper would get DEVICE_IN_USE and exit. Start it once the
       device is free, and wait until it is actually audible. */
    HANDLE other = NULL;
    char heard[MAX_PATH];
    for (int attempt = 0; attempt < 4 && !busy_before; attempt++) {
        if (other) CloseHandle(other);
        other = spawn_child("--child-play 4", 1);
        for (int i = 0; i < 15 && !other_app_sounding(heard, sizeof(heard)); i++) Sleep(100);
        if (other_app_sounding(heard, sizeof(heard))) break;
        Sleep(700);                      /* not yet free: try again */
    }
    if (busy_before && !other) other = spawn_child("--child-play 4", 1);
    CHECK(other != NULL, "could not start the other process");
    CHECK(busy_before || other_app_sounding(heard, sizeof(heard)),
          "the other process never became audible");
    if (!track_open(&t, f)) { track_close(&t); if (other) CloseHandle(other); return; }
    track_start(&t);
    track_play(&t, g_seconds, late);
    Sleep(300);
    CHECK(log_contains(g_log_path, busy_before ? "shared next to " : "shared next to nexsim-other.exe"),
          "the sink did not open shared next to the other process");
    CHECK(log_contains(g_log_path, "output=shared"), "ENGAGED line does not say output=shared");
    if (other) { WaitForSingleObject(other, 8000); CloseHandle(other); }
    track_play(&t, 1.0, late);           /* share_quiet_ms=500 passes */
    if (busy_before) {
        printf("  SKIP return to exclusive: %s keeps playing, so the stream stays shared "
               "(checking only that the shared stream resumes)\n", busy_app);
        IAudioClient_Stop(t.client);
        Sleep(200);
        t.last_pos = track_position(&t);
        HRESULT h2 = IAudioClient_Start(t.client);
        CHECK(SUCCEEDED(h2), "Start of the paused shared stream 0x%08lX", (unsigned long)h2);
        track_play(&t, g_seconds, late);
        IAudioClient_Stop(t.client);
        track_close(&t);
        return;
    }
    HRESULT hr = IAudioClient_Stop(t.client);
    CHECK(SUCCEEDED(hr), "Stop 0x%08lX", (unsigned long)hr);
    Sleep(200);
    t.last_pos = track_position(&t);
    hr = IAudioClient_Start(t.client);
    CHECK(SUCCEEDED(hr), "Start after the other process went quiet 0x%08lX", (unsigned long)hr);
    track_play(&t, g_seconds, late);
    Sleep(300);
    CHECK(log_contains(g_log_path, "leaving shared output"), "resume did not leave shared output");
    CHECK(log_contains(g_log_path, "output=exclusive"), "no exclusive rebuild after the resume");
    IAudioClient_Stop(t.client);
    track_close(&t);
}

static NexControl *open_own_control(void) {
    WCHAR name[64];
    nex_control_name(name, 64, GetCurrentProcessId());
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) return NULL;
    NexControl *c = (NexControl *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(NexControl));
    return c && nex_control_valid(c) ? c : NULL;
}

static void scenario_conflict(int *late) {
    TrackFormat f = {48000, 32, 1, 2};
    Track t;
    printf("conflict: start a browser video now (waiting up to 30 s)\n");
    char busy_app[MAX_PATH];
    if (other_app_sounding(busy_app, sizeof(busy_app))) {
        printf("  SKIP: %s is already playing; conflict needs a quiet device\n", busy_app);
        return;
    }
    NexControl *c = open_own_control();
    CHECK(c != NULL, "the module did not create its control block");
    NexSmtc smtc;
    int smtc_ok = nex_smtc_open(&smtc);
    CHECK(smtc_ok, "SMTC unavailable: %s", smtc.error);
    if (!c || !smtc_ok) return;
    if (!track_open(&t, f)) { track_close(&t); return; }
    track_start(&t);
    track_play(&t, 1.0, late);
    Sleep(500);
    CHECK(c->state == NEX_CONTROL_STATE_EXCLUSIVE && c->playing,
          "control block does not say exclusive/playing (state %ld playing %ld)",
          (long)c->state, (long)c->playing);
    int posted = 0;
    WCHAR who[160] = L"";
    for (int i = 0; i < 120 && !posted; i++) {
        track_play(&t, 0.25, late);
        NexSmtcEntry e[16];
        int n = nex_smtc_poll(&smtc, e, 16);
        for (int k = 0; k < n && !posted; k++) {
            int kind = nex_app_kind(e[k].app);
            if (e[k].status != NEX_SMTC_PLAYING) continue;
            /* the host would ask here; this tool answers yes */
            if (nex_conflict_action(kind, NEX_REMEMBER_NONE) != NEX_CONFLICT_ASK) continue;
            nex_control_post(c, NEX_CONTROL_CMD_SHARE);
            wcsncpy(who, e[k].app, 159);
            posted = 1;
        }
    }
    CHECK(posted, "no other application reported Playing within 30 s");
    if (posted) {
        printf("  %ls reported Playing; posted close-exclusive\n", who);
        track_play(&t, 1.5, late);
        CHECK(c->ack_seq == c->cmd_seq && c->ack_result == 0,
              "command not applied (ack %ld/%ld result 0x%08lX)",
              (long)c->ack_seq, (long)c->cmd_seq, (unsigned long)c->ack_result);
        CHECK(c->state == NEX_CONTROL_STATE_SHARED, "still not shared (state %ld)", (long)c->state);
        CHECK(log_contains(g_log_path, "listener closed exclusive"), "no switch in the log");
        int freed = probe_shared();
        printf("  after the switch: another process %s\n",
               freed == 0 ? "opened the device" : freed == 3 ? "got DEVICE_IN_USE" : "failed");
        CHECK(freed == 0, "the device is still taken (probe %d)", freed);
        track_play(&t, 1.0, late);
        nex_control_post(c, NEX_CONTROL_CMD_EXCLUSIVE);
        track_play(&t, 1.0, late);
        CHECK(c->share_forced == 0, "exclusive was not allowed again");
    }
    IAudioClient_Stop(t.client);
    track_close(&t);
    nex_smtc_close(&smtc);
}

/* Lines in the sink log that mean something went wrong. */
static void scan_log(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("log: %s not readable\n", path); return; }
    char line[1024];
    int faults = 0, empty = 0, engaged = 0, primed = 0, event_mode = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "sink fault") || strstr(line, "EOS wait timed out")) {
            faults++;
            printf("  log: %s", line);
        }
        const char *e = strstr(line, "empty-buffer=");
        if (e && atoi(e + 13) > 0) { empty += atoi(e + 13); printf("  log: %s", line); }
        if (strstr(line, "sink ENGAGED")) engaged++;
        if (strstr(line, "start-up silence armed")) primed++;
        if (strstr(line, "event-driven exclusive")) event_mode++;
    }
    fclose(f);
    printf("log: engaged=%d primed=%d event-mode=%d faults=%d empty-buffers=%d\n",
           engaged, primed, event_mode, faults, empty);
    CHECK(engaged > 0, "the sink never engaged (enable=0, or the DLL did not hook)");
    CHECK(faults == 0, "%d fault line(s) in the sink log", faults);
    CHECK(empty == 0, "%d empty device buffer(s) in the sink log", empty);
}

int main(int argc, char **argv) {
    const char *dll = "nexsink.dll", *cfg = NULL, *log = NULL;
    const char *scenarios[16];
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--quiet")) g_amp = 0.0003;
    if (argc >= 2 && !strcmp(argv[1], "--child-probe")) return child_shared(0);
    if (argc >= 3 && !strcmp(argv[1], "--child-play")) return child_shared(atof(argv[2]));
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--cfg") && i + 1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--quiet")) continue;
        else if (argv[i][0] == '-') { printf("unknown option %s\n", argv[i]); return 2; }
        else if (count < 16) scenarios[count++] = argv[i];
    }
    if (!count) scenarios[count++] = "playlist";
    if (g_seconds < 1.0) g_seconds = 1.0;
    char log_path[MAX_PATH];
    if (!log) {
        GetTempPathA(MAX_PATH, log_path);
        snprintf(log_path + strlen(log_path), MAX_PATH - strlen(log_path),
                 "nexsim-%lu.log", (unsigned long)GetCurrentProcessId());
        log = log_path;
    }
    DeleteFileA(log);
    g_log_path = log;
    char own_cfg[MAX_PATH];
    if (!cfg) {
        int needs = 0;
        for (int i = 0; i < count; i++)
            needs |= !strcmp(scenarios[i], "release") || !strcmp(scenarios[i], "share") ||
                     !strcmp(scenarios[i], "conflict");
        if (needs) {
            GetTempPathA(MAX_PATH, own_cfg);
            snprintf(own_cfg + strlen(own_cfg), MAX_PATH - strlen(own_cfg),
                     "nexsim-%lu.cfg", (unsigned long)GetCurrentProcessId());
            FILE *cf = fopen(own_cfg, "w");
            if (cf) {
                fputs("enable=1\nidle_release_ms=1000\nauto_shared=1\nshare_quiet_ms=500\n", cf);
                fclose(cf);
                cfg = own_cfg;
            }
        }
    }
    SetEnvironmentVariableA("NEX_LOG", log);
    _putenv_s("NEX_LOG", log);
    if (cfg) { SetEnvironmentVariableA("NEX_CFG", cfg); _putenv_s("NEX_CFG", cfg); }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HMODULE sink = LoadLibraryA(dll);
    if (!sink) { printf("cannot load %s (%lu)\n", dll, GetLastError()); return 2; }
    /* nexsink patches the vtables on its own setup thread. */
    Sleep(800);

    IMMDeviceEnumerator *en = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&en)) || !en ||
        FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &g_dev)) || !g_dev) {
        printf("no default render endpoint\n");
        return 2;
    }

    int late = 0;
    for (int i = 0; i < count; i++) {
        const char *s = scenarios[i];
        int all = !strcmp(s, "playlist");
        if (all || !strcmp(s, "handoff")) scenario_handoff(&late);
        if (all || !strcmp(s, "format")) scenario_format(&late);
        if (all || !strcmp(s, "pause")) scenario_pause(&late);
        if (all || !strcmp(s, "seek")) scenario_seek(&late);
        if (!strcmp(s, "release")) scenario_release(&late);
        if (!strcmp(s, "share")) scenario_share(&late);
        if (!strcmp(s, "conflict")) scenario_conflict(&late);
        if (late) printf("  (client event silent for 500 ms: %d time(s) so far)\n", late);
        if (!all && strcmp(s, "handoff") && strcmp(s, "format") && strcmp(s, "pause") &&
            strcmp(s, "seek") && strcmp(s, "release") && strcmp(s, "share") &&
            strcmp(s, "conflict"))
            printf("unknown scenario %s\n", s);
    }
    CHECK(late == 0, "the client event stayed silent for 500 ms %d time(s)", late);
    IMMDevice_Release(g_dev);
    IMMDeviceEnumerator_Release(en);
    Sleep(300);                          /* let the async log writer flush */
    scan_log(log);
    printf("%s: %d check(s), %d failure(s); log %s\n", g_fail ? "FAILED" : "PASSED",
           g_checks, g_fail, log);
    return g_fail ? 1 : 0;
}
