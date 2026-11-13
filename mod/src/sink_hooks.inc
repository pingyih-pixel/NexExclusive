/* ---- IAudioClient hooks ------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE H_Initialize(void *self, AUDCLNT_SHAREMODE mode,
                                              DWORD flags, REFERENCE_TIME dur,
                                              REFERENCE_TIME period,
                                              const WAVEFORMATEX *fmt, LPCGUID session) {
    load_config();
    char d[160];
    describe("fmt", fmt, d, sizeof(d));
    log_line("Initialize mode=%d flags=0x%08lX dur=%lld %s",
             (int)mode, (unsigned long)flags, (long long)dur, d);

    if (g_enable && mode == AUDCLNT_SHAREMODE_SHARED && !InterlockedCompareExchange(&g_internal, 0, 0)) {
        int was_engaged = 0;
        HRESULT hs = sink_engage_controlled((IAudioClient *)self, fmt, dur, &was_engaged);
        if (SUCCEEDED(hs)) {
            log_line("Initialize -> intercepted, no real shared stream created");
            return S_OK;
        }
        /* Strict by default: the point of this module is exclusive output, and
           a silent downgrade to shared is exactly what the listener cannot
           hear happening. A second stream in the same process is different -
           it is not ours to fail, so it always gets a real stream. */
        if (!g_shared_fallback && !was_engaged) {
            log_line("sink failed 0x%08lX, exclusive output rejected; shared fallback is disabled",
                     (unsigned long)hs);
            return hs;
        }
        log_line("sink failed 0x%08lX, falling back to the real shared stream",
                 (unsigned long)hs);
    }

    HRESULT hr = O_INITIALIZE(self, mode, flags, dur, period, fmt, session);
    /* A stopped-but-open exclusive stream still owns the endpoint. If the client
       comes back with a plain shared stream and gets DEVICE_IN_USE, let go of
       ours and give it the device back: the alternative is a client that sits
       there with no stream at all. */
    if ((unsigned long)hr == 0x8889000Aul) {
        EnterCriticalSection(&g_control);
        int release_ours = g_sink.engaged && !g_sink.started;
        if (release_ours) {
            log_line("sink: passthrough blocked by our idle stream, releasing the device");
            sink_teardown();
        }
        LeaveCriticalSection(&g_control);
        if (release_ours) hr = O_INITIALIZE(self, mode, flags, dur, period, fmt, session);
    }
    log_line("Initialize(passthrough) returned 0x%08lX", (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_GetBufferSize(void *self, UINT32 *n) {
    EnterCriticalSection(&g_control);
    if (owns(self)) {
        if (n) *n = g_sink.reported_frames;
        LeaveCriticalSection(&g_control);
        return S_OK;
    }
    LeaveCriticalSection(&g_control);
    return O_GETBUFFERSIZE(self, n);
}

static HRESULT STDMETHODCALLTYPE H_GetCurrentPadding(void *self, UINT32 *p) {
    EnterCriticalSection(&g_control);
    if (owns(self)) {
        EnterCriticalSection(&g_lock);
        UINT32 used = g_sink.w - g_sink.r;
        /* Never report more than we told the client its buffer holds: the
           client computes space = GetBufferSize() - padding in unsigned math,
           and an overshoot there turns into a huge GetBuffer request. */
        if (p) *p = used < g_sink.reported_frames ? used : g_sink.reported_frames;
        LeaveCriticalSection(&g_lock);
        LeaveCriticalSection(&g_control);
        return S_OK;
    }
    LeaveCriticalSection(&g_control);
    return O_GETPADDING(self, p);
}

static HRESULT STDMETHODCALLTYPE H_GetStreamLatency(void *self, REFERENCE_TIME *l) {
    EnterCriticalSection(&g_control);
    if (owns(self)) {
        REFERENCE_TIME latency = g_sink.gen ? g_sink.gen->real_latency : 0;
        LeaveCriticalSection(&g_control);
        log_line("GetStreamLatency -> %.2f ms", latency / 10000.0);
        if (l) *l = latency;
        return S_OK;
    }
    LeaveCriticalSection(&g_control);
    return O_GETLATENCY(self, l);
}

static HRESULT STDMETHODCALLTYPE H_IsFormatSupported(void *self, AUDCLNT_SHAREMODE mode,
                                                     const WAVEFORMATEX *fmt,
                                                     WAVEFORMATEX **closest) {
    EnterCriticalSection(&g_control);
    int intercepted = owns(self) || (g_enable && mode == AUDCLNT_SHAREMODE_SHARED);
    if (intercepted && closest) *closest = NULL;
    LeaveCriticalSection(&g_control);
    if (intercepted) return S_OK;
    return O_ISFORMAT(self, mode, fmt, closest);
}

static HRESULT STDMETHODCALLTYPE H_SetEventHandle(void *self, HANDLE h) {
    EnterCriticalSection(&g_control);
    if (owns(self)) {
        g_sink.h_client = h;
        LeaveCriticalSection(&g_control);
        return S_OK;
    }
    LeaveCriticalSection(&g_control);
    return O_SETEVENT(self, h);
}

static HRESULT STDMETHODCALLTYPE H_Start(void *self) {
    EnterCriticalSection(&g_control);
    log_line("Start (owns=%d started=%d)", owns(self), (int)g_sink.started);
    if (!owns(self)) {
        LeaveCriticalSection(&g_control);
        return O_START(self);
    }

    if (g_sink.started) { LeaveCriticalSection(&g_control); log_line("Start -> S_FALSE (already started)"); return S_FALSE; }
    if (FAILED(g_sink.fault)) {
        HRESULT fault = g_sink.fault;
        LeaveCriticalSection(&g_control);
        return fault;
    }

    /* The device is started by drain_once() after the first real frames have
       been committed. Starting it here would leave the endpoint running on an
       empty buffer, which the driver reports as an immediate underrun. */
    SinkGen *g = g_sink.gen;
    if (!g) { LeaveCriticalSection(&g_control); log_line("Start -> no device generation"); return E_FAIL; }
    InterlockedExchange(&g->running, 1);
    g_sink.started = 1;
    g_sink.fade_total = (UINT32)((UINT64)g_sink.out_rate * (UINT64)g_fade_ms / 1000u);
    if (g_fade_ms > 0 && g_sink.fade_total < 32) g_sink.fade_total = 32;
    g_sink.fade_left = g_sink.fade_total;
    gen_retain(g);                      /* the thread's reference */
    g->h_thread = CreateThread(NULL, 0, drain_thread, g, 0, NULL);
    if (!g->h_thread) {
        log_line("sink: CreateThread failed %lu", GetLastError());
        InterlockedExchange(&g->running, 0);
        gen_release(g);
        g_sink.started = 0;
        LeaveCriticalSection(&g_control);
        return E_FAIL;
    }
    SetThreadPriority(g->h_thread, THREAD_PRIORITY_TIME_CRITICAL);
    /* Start means the ring is empty and the client may fill it. An event-driven
       client waits for exactly this before its first GetBuffer, so not setting
       it leaves the two sides waiting on each other. */
    if (g_sink.h_client) SetEvent(g_sink.h_client);
    log_line("sink: started");
    LeaveCriticalSection(&g_control);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE H_Stop(void *self) {
    EnterCriticalSection(&g_control);
    if (!owns(self)) {
        LeaveCriticalSection(&g_control);
        return O_STOP(self);
    }
    if (!g_sink.started) { LeaveCriticalSection(&g_control); log_line("Stop -> S_FALSE (not started)"); return S_FALSE; }
    HRESULT hr = sink_stop();
    log_line("sink: stopped 0x%08lX", (unsigned long)hr);
    LeaveCriticalSection(&g_control);
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_Reset(void *self) {
    EnterCriticalSection(&g_control);
    if (!owns(self)) {
        LeaveCriticalSection(&g_control);
        return O_RESET(self);
    }
    /* Do not erase history under the live drain thread. Reset is a stopped
       stream operation in WASAPI; propagate failures instead of pretending. */
    if (g_sink.started) { LeaveCriticalSection(&g_control); log_line("Reset -> NOT_STOPPED"); return AUDCLNT_E_NOT_STOPPED; }
    HRESULT hr = g_sink.gen && g_sink.gen->real ? O_RESET(g_sink.gen->real) : S_OK;
    if (FAILED(hr)) {
        log_line("sink: real Reset failed 0x%08lX", (unsigned long)hr);
        LeaveCriticalSection(&g_control);
        return hr;
    }
    EnterCriticalSection(&g_lock);
    g_sink.w = g_sink.r = 0;
    if (g_sink.ring)
        memset(g_sink.ring, 0, (size_t)g_sink.cap * g_sink.in_block);
    if (g_sink.resample) src_reset(g_sink.in_channels);
    LeaveCriticalSection(&g_lock);
    /* The device clock restarts at zero after Reset. fc_GetPosition() clamps to
       clock_last to keep the client from seeing a backwards jump, so leaving
       the old value here pins the reported position at the pre-seek point for
       the rest of the track - the client's progress bar freezes and it never
       asks for more data. A rebuilt stream starts its own sequence. */
    g_sink.clock_last = 0;
    g_sink.clock_base = 0;
    log_line("sink: reset (client ring and resampler history cleared)");
    LeaveCriticalSection(&g_control);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE H_GetService(void *self, const GUID *iid, void **out) {
    EnterCriticalSection(&g_control);
    if (owns(self) && iid && out) {
        if (iid->Data1 == IID_IAudioRenderClient_l.Data1) {
            FakeObj *o = g_sink.proxy_render;
            if (!o) { LeaveCriticalSection(&g_control); return E_NOINTERFACE; }
            InterlockedIncrement(&o->ref);
            *out = o;
            UINT64 generation = o->generation;
            /* 两个编号分开打：gen 是 SinkGen::id（drain 线程也用它），epoch 是
               g_sink.generation（代理对象有效性令牌）。两者共用 g_generation
               这一个计数器，必然错开一位，只打其中一个会让人对不上日志。 */
            UINT64 gen_id = g_sink.gen ? g_sink.gen->id : 0;
            LeaveCriticalSection(&g_control);
            log_line("GetService -> fake IAudioRenderClient (gen %llu, epoch %llu)",
                     (unsigned long long)gen_id, (unsigned long long)generation);
            return S_OK;
        }
        if (iid->Data1 == IID_IAudioClock_l.Data1) {
            FakeObj *o = g_sink.proxy_clock;
            if (!o) { LeaveCriticalSection(&g_control); return E_NOINTERFACE; }
            InterlockedIncrement(&o->ref);
            *out = o;
            UINT64 generation = o->generation;
            UINT64 gen_id = g_sink.gen ? g_sink.gen->id : 0;
            LeaveCriticalSection(&g_control);
            log_line("GetService -> fake IAudioClock (gen %llu, epoch %llu)",
                     (unsigned long long)gen_id, (unsigned long long)generation);
            return S_OK;
        }
    }
    LeaveCriticalSection(&g_control);
    /* Anything else goes to the real exclusive client, which refuses session
       volume and metering. Logged because what the client asks for here decides
       how it will try to control volume: 09:40 实测，它从来没在这里要过
       ISimpleAudioVolume（87CE5498），只出现过 IAudioClockAdjustment
       （F6E4C0A0）和 IAudioSessionControl（F4B1A599）。会话音量走的是
       IAudioSessionManager::GetSimpleAudioVolume，不经过这个函数。 */
    if (owns(self) && iid)
        log_line("GetService iid=%08lX -> real client", (unsigned long)iid->Data1);
    return O_GETSERVICE(self, iid, out);
}

/* IAudioClient3::InitializeSharedAudioStream(DWORD StreamFlags,
                                             UINT32 PeriodInFrames,
                                             const WAVEFORMATEX *pFormat,
                                             LPCGUID AudioSessionGuid)
   Four parameters, not six. Passing a six-argument prototype here made the
   hook read two arguments that the caller never pushed, so the format pointer
   was whatever happened to sit in the next register. */
static HRESULT STDMETHODCALLTYPE H_InitializeShared(void *self, DWORD flags,
                                                    UINT32 period,
                                                    const WAVEFORMATEX *fmt,
                                                    LPCGUID session) {
    load_config();
    char d[160];
    describe("fmt", fmt, d, sizeof(d));
    log_line("InitializeSharedAudioStream flags=0x%08lX period=%lu %s",
             (unsigned long)flags, (unsigned long)period, d);
    if (g_enable && !InterlockedCompareExchange(&g_internal, 0, 0)) {
        /* A shared-period stream asks for a smaller buffer than the 100 ms one
           the exclusive path opens; honour the caller's hint when it is sane. */
        REFERENCE_TIME dur = 0;
        if (period >= 32 && fmt && fmt->nSamplesPerSec)
            dur = (REFERENCE_TIME)((UINT64)period * 10000000ull / fmt->nSamplesPerSec);
        if (dur < 100000) dur = 100000;
        HRESULT hs = sink_engage_controlled((IAudioClient *)self, fmt, dur, NULL);
        if (SUCCEEDED(hs)) return S_OK;
    }
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, DWORD, UINT32,
                   const WAVEFORMATEX *, LPCGUID))
            ac_original(self, AC3_INITSHARED))(self, flags, period, fmt, session);
}

// IAudioClient itself has 15 slots (IUnknown + 12). IAudioClient2 adds 15..17 and
// IAudioClient3 adds 18..20. Writing slot 20 into a plain IAudioClient vtable
// would clobber whatever sits next in .rdata, so the caller has to say which one
// it is.
//
// The vtable is process-wide: the same table is handed to every IAudioClient,
// including the exclusive one we open ourselves. So the originals are recorded
// per vtable, not in one global array — a second, differently-shaped client
// (v1 vs v3) must not overwrite the pointers the first one needs.
static AudioPatch *find_patch(void **vt) {
    for (int i = 0; i < g_audio_patch_count; i++)
        if (g_audio_patches[i].vt == vt) return &g_audio_patches[i];
    return NULL;
}

static void patch_audio_client(void *iface, int is_v3) {
    if (!iface) return;
    void **vt = *(void ***)iface;
    int slots = is_v3 ? 21 : 15;

    EnterCriticalSection(&g_patch_lock);
    AudioPatch *rec = find_patch(vt);
    if (rec) {
        /* A v1 client may have been seen first and only 15 slots recorded.
           Extending it in place is safe: the extra slots exist on this vtable. */
        if (is_v3 && !rec->v3) {
            DWORD o = 0;
            if (VirtualProtect(vt, 21 * sizeof(void *), PAGE_READWRITE, &o)) {
                rec->orig[AC3_INITSHARED] = vt[AC3_INITSHARED];
                vt[AC3_INITSHARED] = (void *)H_InitializeShared;
                VirtualProtect(vt, 21 * sizeof(void *), o, &o);
                rec->v3 = 1;
                log_line("extended patch to IAudioClient3 slot %d (vt=%p)",
                         AC3_INITSHARED, (void *)vt);
            }
        }
        LeaveCriticalSection(&g_patch_lock);
        return;
    }
    if (g_audio_patch_count >= (int)(sizeof(g_audio_patches) / sizeof(g_audio_patches[0]))) {
        LeaveCriticalSection(&g_patch_lock);
        log_line("patch table full, vtable %p left alone", (void *)vt);
        return;
    }

    DWORD old = 0;
    if (!VirtualProtect(vt, (SIZE_T)slots * sizeof(void *), PAGE_READWRITE, &old)) {
        LeaveCriticalSection(&g_patch_lock);
        log_line("VirtualProtect(IAudioClient) failed %lu", GetLastError());
        return;
    }
    AudioPatch *slot = &g_audio_patches[g_audio_patch_count];
    memset(slot, 0, sizeof(*slot));
    slot->vt = vt;
    slot->v3 = is_v3;
    for (int i = 0; i < slots; i++) slot->orig[i] = vt[i];

    vt[AC_INITIALIZE]        = (void *)H_Initialize;
    vt[AC_GETBUFFERSIZE]     = (void *)H_GetBufferSize;
    vt[AC_GETSTREAMLATENCY]  = (void *)H_GetStreamLatency;
    vt[AC_GETCURRENTPADDING] = (void *)H_GetCurrentPadding;
    vt[AC_ISFORMATSUPPORTED] = (void *)H_IsFormatSupported;
    vt[AC_START]             = (void *)H_Start;
    vt[AC_STOP]              = (void *)H_Stop;
    vt[AC_RESET]             = (void *)H_Reset;
    vt[AC_SETEVENTHANDLE]    = (void *)H_SetEventHandle;
    vt[AC_GETSERVICE]        = (void *)H_GetService;
    if (is_v3) vt[AC3_INITSHARED] = (void *)H_InitializeShared;
    VirtualProtect(vt, (SIZE_T)slots * sizeof(void *), old, &old);

    g_audio_patch_count++;
    g_ac_vtbl = vt;
    memcpy(g_ac_orig, slot->orig, sizeof(g_ac_orig)); /* test fallback only */
    LeaveCriticalSection(&g_patch_lock);
    log_line("patched IAudioClient vtable=%p (%d slots%s)", (void *)vt, slots,
             is_v3 ? ", v3" : "");
}

/* ---- IMMDevice::Activate hook ------------------------------------------ */
static void **g_dev_vtbl = NULL;
static HRESULT (STDMETHODCALLTYPE *g_dev_orig_activate)(void *, const GUID *, DWORD, void *, void **);

/* Every activation we perform ourselves has to skip the hook: otherwise our own
   probe clients get registered as if the host had created them, and the
   client->device map fills up with streams that no longer exist. */
static HRESULT dev_activate(IMMDevice *dev, const GUID *iid, void **out) {
    if (!dev || !out) return E_POINTER;
    *out = NULL;
    void *fn = NULL;
    EnterCriticalSection(&g_patch_lock);
    if (g_dev_vtbl == *(void ***)dev) fn = (void *)g_dev_orig_activate;
    LeaveCriticalSection(&g_patch_lock);
    if (!fn) return dev->lpVtbl->Activate(dev, iid, CLSCTX_ALL, NULL, out);
    return ((HRESULT (STDMETHODCALLTYPE *)(void *, const GUID *, DWORD, void *, void **))fn)
           (dev, iid, CLSCTX_ALL, NULL, out);
}

/* ---- endpoint identity, for the settings panel -------------------------- */
/* The panel has to describe the endpoint we actually engaged, not whatever the
   system default happens to be when it asks: the client can be pinned to a
   specific device, and then a query against the default explains the wrong
   hardware. Called once per engage, from sink_engage, on the control path,
   while the device is still referenced.

   hw_bits is the QueryHardwareSupport mask (1 volume, 2 mute, 4 meter), or -1
   when the endpoint volume interface could not be activated - "unknown" and
   "no hardware volume" are different answers and the panel must not conflate
   them. */
static const GUID IID_IAudioEndpointVolume_l =
    {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

/* PKEY_Device_FriendlyName */
static const PROPERTYKEY PKEY_FriendlyName_l = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14
};

#define IAEV_HWSUPPORT 19

static void ep_describe(IMMDevice *dev, char *name, size_t n, int *hw_bits) {
    if (name && n) name[0] = 0;
    if (hw_bits) *hw_bits = -1;
    if (!dev) return;

    IPropertyStore *ps = NULL;
    if (SUCCEEDED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (name && n &&
            SUCCEEDED(ps->lpVtbl->GetValue(ps, &PKEY_FriendlyName_l, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal)
            WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, name, (int)n, NULL, NULL);
        PropVariantClear(&pv);
        ps->lpVtbl->Release(ps);
    }

    IUnknown *vol = NULL;
    if (hw_bits && SUCCEEDED(dev_activate(dev, &IID_IAudioEndpointVolume_l, (void **)&vol)) && vol) {
        void **vt = *(void ***)vol;
        DWORD bits = 0;
        if (SUCCEEDED(((HRESULT (STDMETHODCALLTYPE *)(void *, DWORD *))
                       vt[IAEV_HWSUPPORT])(vol, &bits)))
            *hw_bits = (int)bits;
        vol->lpVtbl->Release(vol);
    }
}

static void bind_endpoint(void *client, IMMDevice *dev) {
    if (!client || !dev) return;
    EnterCriticalSection(&g_patch_lock);
    int slot = -1;
    for (int i = 0; i < 128; i++) {
        if (g_bindings[i].client == client) { slot = i; break; }
        if (!g_bindings[i].client && slot < 0) slot = i;
    }
    if (slot < 0) {
        /* Full: evict the oldest entry instead of dropping the reference and
           leaking the device for the rest of the session. */
        slot = 0;
        if (g_bindings[0].dev) g_bindings[0].dev->lpVtbl->Release(g_bindings[0].dev);
        g_bindings[0].dev = NULL;
        g_bindings[0].client = NULL;
        memmove(&g_bindings[0], &g_bindings[1], sizeof(g_bindings[0]) * 127);
        slot = 127;
    }
    if (g_bindings[slot].client != client && g_bindings[slot].dev)
        g_bindings[slot].dev->lpVtbl->Release(g_bindings[slot].dev);
    g_bindings[slot].client = client;
    g_bindings[slot].dev = dev;
    dev->lpVtbl->AddRef(dev);
    LeaveCriticalSection(&g_patch_lock);
}

static HRESULT STDMETHODCALLTYPE H_Activate(void *self, const GUID *iid, DWORD clsctx,
                                            void *params, void **out) {
    g_activation_dev = (IMMDevice *)self; /* thread-local: no cross-thread bleed */
    /* Logged because "the client plays but nothing is intercepted" has exactly
       one cause worth ruling out: it never asked us for an audio client. */
    log_line("Activate iid=%08lX clsctx=0x%lX", (unsigned long)(iid ? iid->Data1 : 0),
             (unsigned long)clsctx);
    HRESULT hr = g_dev_orig_activate(self, iid, clsctx, params, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (iid && iid->Data1 == 0x1CB9AD4C) { patch_audio_client(*out, 0); bind_endpoint(*out, (IMMDevice *)self); }
        else if (iid && iid->Data1 == 0x7ED4EE07) { patch_audio_client(*out, 1); bind_endpoint(*out, (IMMDevice *)self); }
    }
    return hr;
}

static void patch_device(void *dev) {
    if (!dev) return;
    void **vt = *(void ***)dev;
    if (g_dev_vtbl == vt) return;
    DWORD old = 0;
    if (!VirtualProtect(vt, 8 * sizeof(void *), PAGE_READWRITE, &old)) return;
    g_dev_orig_activate = (HRESULT (STDMETHODCALLTYPE *)(void *, const GUID *, DWORD, void *, void **))
                          vt[DEV_ACTIVATE];
    vt[DEV_ACTIVATE] = (void *)H_Activate;
    VirtualProtect(vt, 8 * sizeof(void *), old, &old);
    g_dev_vtbl = vt;
    log_line("patched IMMDevice vtable=%p", (void *)vt);
}

/* ---- endpoint watchdog -------------------------------------------------- */
/* Two things invalidate the device side of a live stream: the user picks
   another default output, or the device is unplugged. Both are handled the
   same way - move the stream - so one poll covers both. The drain thread wakes
   it immediately on a loss; the 400 ms timeout only bounds the case where the
   default output moves while the old device is still alive. */
static UINT64 current_default_hash(void) {
    IMMDevice *dev = NULL;
    IMMDeviceEnumerator *e = NULL;
    UINT64 key = 0;
    InterlockedIncrement(&g_internal);
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &IID_IMMDeviceEnumerator, (void **)&e)) && e) {
        if (SUCCEEDED(e->lpVtbl->GetDefaultAudioEndpoint(e, eRender, eConsole, &dev)) && dev) {
            key = endpoint_hash(dev);
            dev->lpVtbl->Release(dev);
        }
        e->lpVtbl->Release(e);
    }
    InterlockedDecrement(&g_internal);
    return key;
}

static DWORD WINAPI watch_thread(LPVOID unused) {
    (void)unused;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    log_line("watch: endpoint monitor up");
    DWORD last_attempt = 0, moved_since = 0;
    int   failures = 0;
    while (g_watch_running) {
        if (g_h_wake) WaitForSingleObject(g_h_wake, 400); else Sleep(400);
        if (!g_watch_running) break;
        if (!g_sink.engaged) { failures = 0; moved_since = 0; continue; }

        int lost = (int)g_sink.device_lost;
        UINT64 key = lost ? 0 : current_default_hash();
        int moved = key && key != g_sink.endpoint_key;
        if (!moved) moved_since = 0;
        if (!lost && !moved) continue;
        if (failures >= 5) continue;            /* the endpoint keeps refusing us */

        DWORD now = GetTickCount();
        /* The client watches the same notification and usually rebuilds its own
           stream within a few hundred milliseconds. Migrating at the same
           instant makes the two fight over an endpoint the engine is still
           reconfiguring, and our drain thread ends up stalled inside a WASAPI
           call. Give it the grace period; if it rebuilds, the endpoint key it
           engages on makes "moved" false and nothing happens here. A device
           that disappeared is different: there is nothing left to fight over. */
        if (!lost) {
            if (!moved_since) { moved_since = now; continue; }
            if (now - moved_since < 600) continue;
        }
        if (now - last_attempt < 1000) continue; /* one attempt per second */
        last_attempt = now;

        UINT64 gen = g_sink.generation;
        if (g_h_migrating) SetEvent(g_h_migrating);
        EnterCriticalSection(&g_control);
        HRESULT hr = E_FAIL;
        if (g_sink.engaged && g_sink.generation == gen) {
            /* Only the drain thread is stopped: the client never asked for a
               stop, so its own view of the stream has to survive the swap. */
            LONG was_started = g_sink.started;
            sink_stop();
            g_sink.started = was_started;
            hr = sink_retarget(lost ? "device removed" : "default output changed");
        }
        LeaveCriticalSection(&g_control);
        if (g_h_migrating) ResetEvent(g_h_migrating);

        if (SUCCEEDED(hr)) { failures = 0; log_line("watch: migration complete"); }
        else if (hr != E_FAIL || g_sink.engaged) {
            failures++;
            log_line("watch: migration failed 0x%08lX (attempt %d)", (unsigned long)hr, failures);
        }
    }
    log_line("watch: endpoint monitor exit");
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

/* ---- init --------------------------------------------------------------- */
static DWORD WINAPI setup_thread(LPVOID unused) {
    (void)unused;
    log_line("=== nexsink attached ===");
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator *e = NULL;
    InterlockedIncrement(&g_internal);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&e);
    if (FAILED(hr)) { InterlockedDecrement(&g_internal); log_line("enumerator failed"); return 0; }

    IMMDevice *dev = NULL;
    if (SUCCEEDED(e->lpVtbl->GetDefaultAudioEndpoint(e, eRender, eConsole, &dev)) && dev) {
        patch_device(dev);
        IAudioClient *ac = NULL;
        if (SUCCEEDED(dev_activate(dev, &IID_IAudioClient, (void **)&ac)) && ac) {
            patch_audio_client(ac, 0);
            ac->lpVtbl->Release(ac);
        }
        IUnknown *v3 = NULL;
        static const GUID IID_IAudioClient3_l =
            {0x7ED4EE07, 0x8E67, 0x4CD4, {0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42}};
        if (SUCCEEDED(dev_activate(dev, &IID_IAudioClient3_l, (void **)&v3)) && v3) {
            patch_audio_client(v3, 1);
            v3->lpVtbl->Release(v3);
        }
        dev->lpVtbl->Release(dev);
    }
    e->lpVtbl->Release(e);
    InterlockedDecrement(&g_internal);
    log_line("=== setup done ===");

    g_watch_running = 1;
    HANDLE w = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    if (w) CloseHandle(w); else log_line("watch: CreateThread failed %lu", GetLastError());
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_control);
        InitializeCriticalSection(&g_patch_lock);
        DisableThreadLibraryCalls(inst);

        const char *env = getenv("NEX_LOG");
        if (env && *env) { strncpy(g_log, env, sizeof(g_log) - 1); g_log[sizeof(g_log) - 1] = 0; }
        else {
            if (!module_dir(g_log, sizeof(g_log))) g_log[0] = 0;
            else {
                size_t used = strlen(g_log);
                if (used + strlen("sink.log") + 1 <= sizeof(g_log))
                    memcpy(g_log + used, "sink.log", strlen("sink.log") + 1);
                else g_log[0] = 0;
            }
        }

        g_fake_render.vtbl = g_fake_render_vtbl;
        g_fake_render.ref  = 1;
        g_fake_clock.vtbl  = g_fake_clock_vtbl;
        g_fake_clock.ref   = 1;

        /* Created here, not with the sink: they describe the migration window
           and have to outlive every teardown. */
        g_h_migrating = CreateEventA(NULL, TRUE,  FALSE, NULL);
        g_h_wake      = CreateEventA(NULL, FALSE, FALSE, NULL);

        HANDLE t = CreateThread(NULL, 0, setup_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
