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

   Each track plays a -20 dBFS sine. Checks per track: every call succeeds;
   the client's clock never goes backwards, holds still while paused, restarts
   after a seek, and stays within tolerance of the time actually played; the
   client's event never goes silent for longer than 500 ms. With --log the
   sink log is scanned afterwards for faults and empty device buffers.

   It needs a real output device and takes it exclusively while it runs.

   usage: nexsim [--dll nexsink.dll] [--cfg hook.cfg] [--log sink.log]
                 [--seconds N] [scenario ...]
   exit code: 0 all checks passed, 1 a check failed, 2 setup failed. */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <math.h>
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
    const double amp = 0.1, step = 2.0 * 3.14159265358979 * 440.0 / t->fmt.rate;
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
    int count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) dll = argv[++i];
        else if (!strcmp(argv[i], "--cfg") && i + 1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) log = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_seconds = atof(argv[++i]);
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
        if (!all && strcmp(s, "handoff") && strcmp(s, "format") && strcmp(s, "pause") &&
            strcmp(s, "seek"))
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
