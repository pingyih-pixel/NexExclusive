/* ---- other applications on the endpoint, idle release, shared output ------
   What the session layer can and cannot tell us (measured with sessprobe on a
   USB DAC, Windows 11, both exclusive options at their defaults):
     - while we hold the endpoint exclusively, another process's shared
       Initialize fails with AUDCLNT_E_DEVICE_IN_USE. A process that never had
       a session on the endpoint still creates one first (OnSessionCreated
       fires); a process that already has one - every browser or player that
       made a sound before - fails without any notification at all. So "some
       application wants to play" is NOT reliably observable while exclusive.
     - a Stop()ped but still initialized exclusive stream blocks the same way:
       a pause keeps the device.
     - once the endpoint is shared, Active sessions and their peak meters show
       exactly who is sounding.
     - taking the device exclusively while another application plays shared
       succeeds ("give exclusive mode applications priority" is on by
       default) and that application's stream is invalidated with
       DisconnectReasonExclusiveModeOverride.
   Hence the design: give the device back during a long pause
   (idle_release_ms), and when a device side is built again, look at who is
   sounding before deciding how to open it (auto_shared). */

static const GUID IID_IAudioSessionManager2_l =
    {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const GUID IID_IAudioSessionControl2_l =
    {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
static const GUID IID_IAudioMeterInformation_l =
    {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};

/* mingw only forward-declares IAudioMeterInformation; GetPeakValue is the
   first method after IUnknown. */
typedef struct PeakMeter PeakMeter;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(PeakMeter *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(PeakMeter *);
    ULONG   (STDMETHODCALLTYPE *Release)(PeakMeter *);
    HRESULT (STDMETHODCALLTYPE *GetPeakValue)(PeakMeter *, float *);
} PeakMeterVtbl;
struct PeakMeter { PeakMeterVtbl *lpVtbl; };

typedef struct {
    int   known;        /* the scan itself worked */
    int   sounding;     /* someone else is audible right now */
    DWORD pid;
    char  name[64];
} OthersScan;

/* Last time another application was heard on the endpoint we play on. Both
   the watch thread and the control path update it; a torn name is cosmetic. */
static volatile LONG g_others_heard;
static volatile LONG g_others_last_tick;
static char g_others_name[64];
static DWORD g_others_pid;

static unsigned long others_quiet_for(DWORD now) {
    if (!InterlockedCompareExchange(&g_others_heard, 0, 0)) return NEX_AUDIO_NEVER_HEARD;
    return (unsigned long)(now - (DWORD)InterlockedCompareExchange(&g_others_last_tick, 0, 0));
}

static void image_base_name(DWORD pid, char *out, size_t cap) {
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    WCHAR w[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, w, &n)) {
        const WCHAR *b = wcsrchr(w, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : w, -1, out, (int)cap, NULL, NULL);
    }
    CloseHandle(h);
}

/* Sessions that are not "another application": our own process (any of its
   streams, including a shared one we opened), other processes of the same
   client executable, the separate audio host, and the system-sounds session
   (pid 0; notification blips are what the quiet window is for anyway). */
static int others_session_is_ours(DWORD pid, const char *name) {
    static char self_name[64];
    if (!self_name[0]) image_base_name(GetCurrentProcessId(), self_name, sizeof(self_name));
    if (pid == 0 || pid == GetCurrentProcessId()) return 1;
    if (g_remote_process.dwProcessId && pid == g_remote_process.dwProcessId) return 1;
    if (name[0] && self_name[0] && _stricmp(name, self_name) == 0) return 1;
    if (_stricmp(name, "NexAudioHost.exe") == 0) return 1;
    return 0;
}

static void sink_scan_others(IMMDevice *dev, OthersScan *out) {
    memset(out, 0, sizeof(*out));
    if (!dev) return;
    IAudioSessionManager2 *sm = NULL;
    if (FAILED(dev_activate(dev, &IID_IAudioSessionManager2_l, (void **)&sm)) || !sm) return;
    IAudioSessionEnumerator *en = NULL;
    if (FAILED(sm->lpVtbl->GetSessionEnumerator(sm, &en)) || !en) {
        sm->lpVtbl->Release(sm);
        return;
    }
    out->known = 1;
    int n = 0;
    en->lpVtbl->GetCount(en, &n);
    for (int i = 0; i < n && !out->sounding; i++) {
        IAudioSessionControl *c = NULL;
        if (FAILED(en->lpVtbl->GetSession(en, i, &c)) || !c) continue;
        AudioSessionState st = AudioSessionStateInactive;
        c->lpVtbl->GetState(c, &st);
        if (st == AudioSessionStateActive) {
            DWORD pid = 0;
            IAudioSessionControl2 *c2 = NULL;
            if (SUCCEEDED(c->lpVtbl->QueryInterface(c, &IID_IAudioSessionControl2_l, (void **)&c2)) && c2) {
                c2->lpVtbl->GetProcessId(c2, &pid);
                c2->lpVtbl->Release(c2);
            }
            char name[64];
            image_base_name(pid, name, sizeof(name));
            float peak = 0;
            PeakMeter *m = NULL;
            if (SUCCEEDED(c->lpVtbl->QueryInterface(c, &IID_IAudioMeterInformation_l, (void **)&m)) && m) {
                m->lpVtbl->GetPeakValue(m, &peak);
                m->lpVtbl->Release(m);
            }
            if (!others_session_is_ours(pid, name) &&
                nex_audio_policy_session_sounding(1, peak)) {
                out->sounding = 1;
                out->pid = pid;
                snprintf(out->name, sizeof(out->name), "%s", name[0] ? name : "?");
            }
        }
        c->lpVtbl->Release(c);
    }
    en->lpVtbl->Release(en);
    sm->lpVtbl->Release(sm);
    if (out->sounding) {
        snprintf(g_others_name, sizeof(g_others_name), "%s", out->name);
        g_others_pid = out->pid;
        InterlockedExchange(&g_others_last_tick, (LONG)GetTickCount());
        InterlockedExchange(&g_others_heard, 1);
    }
}

/* The endpoint the sink plays on, or would play on: the panel's choice, then
   the device the client bound, then the default. AddRef'd; NULL if none. */
static IMMDevice *sink_pinned_device_q(int quiet);
static IMMDevice *sink_resolve_device(int keep_bound, int quiet) {
    IMMDevice *dev = sink_pinned_device_q(quiet);
    if (!dev && keep_bound) {
        EnterCriticalSection(&g_patch_lock);
        for (int i = 0; i < 128; i++) if (g_bindings[i].client == g_sink.owner && g_bindings[i].dev) {
            dev = g_bindings[i].dev; dev->lpVtbl->AddRef(dev); break;
        }
        LeaveCriticalSection(&g_patch_lock);
    }
    if (!dev) {
        InterlockedIncrement(&g_internal);
        IMMDeviceEnumerator *e = NULL;
        if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                       &IID_IMMDeviceEnumerator, (void **)&e)) && e) {
            e->lpVtbl->GetDefaultAudioEndpoint(e, eRender, eConsole, &dev);
            e->lpVtbl->Release(e);
        }
        InterlockedDecrement(&g_internal);
    }
    return dev;
}

/* How to open the device side about to be built on dev. reclaim is set when
   the device was given back during a pause: with auto_shared off the stream
   still takes it exclusively (the listener pressed play), but the log says
   whose sound that cuts off so the panel can explain it. */
static int sink_pick_output(IMMDevice *dev, const char *reason, int reclaim) {
    if (!g_auto_shared && !reclaim) return NEX_AUDIO_OUTPUT_EXCLUSIVE;
    OthersScan s;
    sink_scan_others(dev, &s);
    DWORD now = GetTickCount();
    unsigned long quiet = others_quiet_for(now);
    int mode = nex_audio_policy_output_mode(g_auto_shared, s.known, s.sounding, quiet,
                                            g_share_quiet_ms);
    if (mode == NEX_AUDIO_OUTPUT_SHARED) {
        if (s.sounding)
            log_line("output(%s): shared next to %s pid=%lu, which is playing",
                     reason, s.name, (unsigned long)s.pid);
        else
            log_line("output(%s): shared, %s was heard %lu ms ago (quiet window %d ms)",
                     reason, g_others_name[0] ? g_others_name : "another application",
                     quiet, g_share_quiet_ms);
    } else if (s.sounding) {
        log_line("output(%s): exclusive takes the device from %s pid=%lu, which is playing; "
                 "auto_shared=1 would play next to it instead",
                 reason, s.name, (unsigned long)s.pid);
    } else if (g_auto_shared) {
        log_line("output(%s): exclusive, no other application sounding%s", reason,
                 s.known ? "" : " (session scan unavailable)");
    }
    return mode;
}

/* Watch-thread duties, every poll (~400 ms):
   - keep the "last heard" time current while auto_shared can use it, so the
     quiet window measures real silence and not the time since the last
     Start;
   - give the device back when a pause has lasted idle_release_ms. */
static void sink_watch_share_tick(void) {
    static DWORD last_scan;
    DWORD now = GetTickCount();
    if (g_auto_shared && g_sink.engaged && now - last_scan >= 1000) {
        last_scan = now;
        IMMDevice *dev = sink_resolve_device(1, 1);
        if (dev) {
            OthersScan s;
            sink_scan_others(dev, &s);
            dev->lpVtbl->Release(dev);
        }
    }
    if (!g_idle_release_ms || !g_sink.engaged || g_sink.started) return;

    EnterCriticalSection(&g_control);
    now = GetTickCount();
    if (nex_audio_policy_idle_release_due(g_idle_release_ms, g_sink.engaged, g_sink.started,
                                          g_sink.gen != NULL, g_sink.pause_token,
                                          g_sink.generation, now - g_sink.pause_tick)) {
        SinkGen *g = g_sink.gen;
        int was_remote = g->remote;
        UINT64 id = g->id;
        /* Read the device clock one last time so the timeline the client sees
           after the resume continues from where the device actually stopped. */
        if (g_sink.proxy_clock) {
            UINT64 pos = 0;
            fc_GetPosition(g_sink.proxy_clock, &pos, NULL);
        }
        g_sink.gen = NULL;
        gen_release(g);                 /* last reference: stream, render client, gate */
        if (was_remote) remote_close();
        g_sink.idle_released = 1;
        g_sink.pause_token = 0;
        log_line("idle release: paused %lu ms, device given back (generation %llu%s); "
                 "the next Start reopens it, clock held at %.3f s",
                 (unsigned long)(now - g_sink.pause_tick), (unsigned long long)id,
                 was_remote ? ", audiohost closed" : "",
                 (double)g_sink.clock_last / 10000000.0);
    }
    LeaveCriticalSection(&g_control);
}

/* A shared stream that auto_shared opened goes back to exclusive at the next
   point where the client is stopped anyway (a resume, a seek before it), once
   nobody else has been heard for the quiet window. Mid-track it stays: a
   switch there would be a gap in the music. Returns 1 when the shared device
   side was dropped and the caller must rebuild it. */
static int sink_end_shared_if_quiet(const char *reason) {
    if (!g_sink.gen || !g_sink.gen->shared || g_sink.started) return 0;
    /* Only a resume: the first Start of a stream that was just opened shared
       has nothing new to decide. */
    if (!g_sink.pause_token) return 0;
    /* auto_shared switched off while a shared stream exists: back to
       exclusive without asking anyone. */
    int mode = NEX_AUDIO_OUTPUT_EXCLUSIVE;
    if (g_auto_shared) {
        IMMDevice *dev = sink_resolve_device(1, 1);
        if (!dev) return 0;
        mode = sink_pick_output(dev, reason, 0);
        dev->lpVtbl->Release(dev);
    }
    if (mode == NEX_AUDIO_OUTPUT_SHARED) return 0;
    if (g_sink.proxy_clock) {
        UINT64 pos = 0;
        fc_GetPosition(g_sink.proxy_clock, &pos, NULL);
    }
    gen_release(g_sink.gen);
    g_sink.gen = NULL;
    log_line("output(%s): leaving shared output, rebuilding exclusive", reason);
    return 1;
}
