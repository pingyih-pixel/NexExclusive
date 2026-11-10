/* ---- drain thread, fault handling and the fake client objects ----------- */
static void sink_fault(SinkGen *g, HRESULT hr, const char *operation) {
    EnterCriticalSection(&g_lock);
    if (SUCCEEDED(g_sink.fault)) g_sink.fault = FAILED(hr) ? hr : E_FAIL;
    if (g) InterlockedExchange(&g->running, 0);
    if (g_sink.h_data) SetEvent(g_sink.h_data);
    if (g_sink.h_space) SetEvent(g_sink.h_space);
    if (g_sink.h_client) SetEvent(g_sink.h_client);
    LeaveCriticalSection(&g_lock);
    log_line("sink fault: %s 0x%08lX; client will receive failure",operation,(unsigned long)hr);
}

/* A device that disappears is not a stream failure: the audio that is already
   in the ring is still valid, and the same stream can continue on whatever
   endpoint becomes the default. Mark it and wake the watchdog; the client is
   held at the door by g_h_migrating instead of being handed an error it would
   treat as fatal. */
static void sink_device_lost(SinkGen *g, const char *where) {
    EnterCriticalSection(&g_lock);
    g_sink.device_lost = 1;
    if (g) InterlockedExchange(&g->running, 0);
    if (g_sink.h_data) SetEvent(g_sink.h_data);
    if (g_sink.h_space) SetEvent(g_sink.h_space);
    LeaveCriticalSection(&g_lock);
    if (g_h_wake) SetEvent(g_h_wake);
    log_line("sink: device lost during %s; looking for a new default endpoint", where);
}

/* Where the drain thread is, purely for diagnosis. A join that times out has
   exactly two possible causes - a stalled WASAPI call or a lock - and the stage
   tells them apart without a debugger. */
#define STAGE_IDLE 0
#define STAGE_PADDING 1
#define STAGE_GETBUF 2
#define STAGE_RELEASE 3
#define STAGE_START 4

/* ---- device-frame dump (diagnostics only) ------------------------------- */
/* Set NEX_DUMP=<file> in the environment before the client starts to capture
   exactly what is handed to the device, in the output format. Comparing this
   against what the client wrote is what separates "our conversion or timing is
   broken" from "the device or its driver is". Off unless the variable is set. */
static HANDLE g_dump;
static int    g_dump_state;   /* 0 = not tried, 1 = open, -1 = off */

static void dump_frames(const BYTE *p, UINT32 frames) {
    if (!g_dump_state) {
        g_dump_state = -1;
        const char *path = getenv("NEX_DUMP");
        if (path && *path) {
            g_dump = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (g_dump != INVALID_HANDLE_VALUE) {
                g_dump_state = 1;
                log_line("dump: device frames -> %s (%d bit, %d ch)",
                         path, g_sink.out_bits, g_sink.in_channels);
            } else {
                log_line("dump: cannot open %s (%lu)", path, GetLastError());
            }
        }
    }
    if (g_dump_state == 1) {
        DWORD written = 0;
        WriteFile(g_dump, p, (DWORD)frames * (DWORD)g_sink.out_block, &written, NULL);
    }
}

/* Underrun accounting. In exclusive mode the device buffer running dry is
   audible - clicks, a buzzing at the submission period - but it is invisible in
   the data we submit, so it has to be counted at the submission point. An empty
   buffer seen here means the drain thread arrived after the device had already
   run out. */
static UINT32 g_writes, g_starve, g_max_gap;
static DWORD  g_last_write;

/* One timer-driven exclusive submission. GetBuffer and ReleaseBuffer agree on
   size, no fabricated silence, and data already buffered in SRC is not skipped.
   Every blocking call is followed by a retired check: once the control path has
   let go of this generation, the thread must not touch the ring, the resampler
   or the negotiated format again - they may already belong to the next stream. */
static HRESULT drain_once(SinkGen *g) {
    UINT32 pad=0;
    g->stage=STAGE_PADDING;
    HRESULT hr=O_GETPADDING(g->real,&pad);
    if(g->retired)return E_ABORT;
    if (FAILED(hr)) return hr;
    if (g->device_started && pad==0 && g->submitted) g_starve++;
    if (pad>g->real_frames) return E_UNEXPECTED;
    UINT32 space=g->real_frames-pad;
    if (!space) return S_FALSE;
    UINT32 n=0;
    if (g_sink.resample) {
        src_compact(g_sink.in_channels);
        int ready=src_ready(g_sink.in_channels);
        double missing=(double)space*g_src.step+g_src.half+2.0 -
                       ((double)g_src.count-(double)g_src.pos/g_src.den);
        UINT32 want=missing>0 ? (UINT32)ceil(missing) : 0;
        UINT32 room=(UINT32)(SRC_CAP-g_src.count);
        if (want>room) want=room;
        if (want>SCRATCH_FRAMES) want=SCRATCH_FRAMES;
        if (want) {
            UINT32 got=ring_take(g_inb,want);
            if (got) {
                to_float(g_inb,g_fin,got);
                if (src_push(g_fin,(int)got,g_sink.in_channels)!=(int)got) return E_UNEXPECTED;
                if (g_sink.h_space) SetEvent(g_sink.h_space);
            }
        }
        ready=src_ready(g_sink.in_channels);
        if (ready>0) n=(UINT32)ready<space ? (UINT32)ready : space;
    } else {
        EnterCriticalSection(&g_lock);
        n=g_sink.w-g_sink.r;
        UINT32 end=g_sink.cap-g_sink.r%g_sink.cap;
        if(n>end)n=end;
        LeaveCriticalSection(&g_lock);
        if(n>space)n=space;
        if(n>SCRATCH_FRAMES)n=SCRATCH_FRAMES;
    }
    if(!n)return S_FALSE;
    BYTE *dst=NULL;
    g->stage=STAGE_GETBUF;
    hr=g->real_render->lpVtbl->GetBuffer(g->real_render,n,&dst);
    /* The call above is the one that can park for the lifetime of the process.
       Check before anything else reads shared state. */
    if(g->retired)return E_ABORT;
    if(FAILED(hr))return hr;
    if(g_sink.resample) {
        src_pull(g_fout,(int)n,g_sink.in_channels);
        quantize(g_fout,n,dst);
    } else {
        UINT32 got=ring_take(g_inb,n);
        if(got!=n) {
            g->real_render->lpVtbl->ReleaseBuffer(g->real_render,0,0);
            return E_UNEXPECTED;
        }
        convert(g_inb,dst,n);
    }
    apply_fade(dst,n);
    dump_frames(dst,n);
    g->stage=STAGE_RELEASE;
    hr=g->real_render->lpVtbl->ReleaseBuffer(g->real_render,n,0);
    if(g->retired)return E_ABORT;
    if(FAILED(hr))return hr;
    g->submitted+=n;
    {
        DWORD t=GetTickCount();
        g->last_submit=(LONG)t;
        if(g_last_write) { DWORD gap=t-g_last_write; if(gap>g_max_gap) g_max_gap=gap; }
        g_last_write=t;
        /* Only speak up when the window shows a problem: an empty device buffer
           or a gap longer than the buffer itself. A healthy stream is silent. */
        if((++g_writes%5000)==0) {
            if(g_starve || g_max_gap>50)
                log_line("drain: %lu writes, %lu empty-buffer, max gap %lu ms",
                         (unsigned long)g_writes,(unsigned long)g_starve,(unsigned long)g_max_gap);
            g_max_gap=0;
        }
    }
    if(g_sink.h_space)SetEvent(g_sink.h_space);
    if(g_sink.h_client)SetEvent(g_sink.h_client);
    if(!g->device_started) {
        /* Start only after real audio has been committed to the device. */
        g->stage=STAGE_START;
        hr=O_START(g->real);
        if(g->retired)return E_ABORT;
        if(FAILED(hr))return hr;
        g->device_started=1;
        log_line("sink: hardware started after %lu prefilled frames",(unsigned long)n);
    }
    g->stage=STAGE_IDLE;
    return S_OK;
}
static DWORD WINAPI drain_thread(LPVOID param) {
    SinkGen *g=(SinkGen *)param;
    HRESULT co=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    g->stage=STAGE_IDLE;
    log_line("drain: thread up (gen %llu), out=%d Hz/%d bit",
             (unsigned long long)g->id,g_sink.out_rate,g_sink.out_bits);
    while(g->running && !g->retired) {
        HRESULT hr=drain_once(g);
        if(g->retired) break;
        if(hr==AUDCLNT_E_DEVICE_INVALIDATED) { sink_device_lost(g,"submission"); break; }
        /* An error that arrives after our own Stop is the expected answer to a
           submission interrupted mid-flight, not a broken stream. Faulting here
           would poison a sink that is being rebuilt on purpose. */
        if(FAILED(hr)) { if(!g->running) break; sink_fault(g,hr,"device submission"); break; }
        if(hr==S_FALSE) {
            if(g_sink.h_data)WaitForSingleObject(g_sink.h_data,3);
            else Sleep(2);
        }
    }
    if(FAILED(g_sink.fault)&&g->device_started) {
        O_STOP(g->real); g->device_started=0;
    }
    log_line("drain: thread exit (gen %llu, running=%ld, retired=%ld, fault=0x%08lX)",
             (unsigned long long)g->id,(long)g->running,(long)g->retired,
             (unsigned long)g_sink.fault);
    /* The control path only closes this handle when it has already joined us,
       in which case the exchange here returns NULL. Whoever gets the handle
       owns the close, so an abandoned thread cleans up after itself. */
    {
        HANDLE h=(HANDLE)InterlockedExchangePointer((PVOID volatile *)&g->h_thread,NULL);
        if(h)CloseHandle(h);
    }
    gen_release(g);            /* the thread's own reference */
    if(SUCCEEDED(co))CoUninitialize();
    return 0;
}

/* ---- fake IAudioRenderClient ------------------------------------------- */
struct FakeObj {
    void **vtbl;
    LONG ref;
    UINT64 generation;
    BYTE *staging; /* client's outstanding buffer survives stream teardown */
    UINT32 capacity;
    UINT32 reserved;
    DWORD writer_tid;
    /* Set when Stop/Reset threw away an outstanding reservation. It lets a
       late ReleaseBuffer be acknowledged as a no-op while a release that never
       had a GetBuffer is still reported as OUT_OF_ORDER. */
    int discarded;
};

static FakeObj g_fake_render;
static FakeObj g_fake_clock;

static int proxy_current(FakeObj *obj) {
    return g_sink.engaged && obj && (obj->generation==g_sink.generation);
}
static ULONG STDMETHODCALLTYPE fr_AddRef(void *s) { return (ULONG)InterlockedIncrement(&((FakeObj *)s)->ref); }
static ULONG STDMETHODCALLTYPE fr_Release(void *s) {
    FakeObj *obj=(FakeObj *)s;
    LONG n=InterlockedDecrement(&obj->ref);
    if(!n && obj!=&g_fake_render && obj!=&g_fake_clock) { free(obj->staging);free(obj); }
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE fr_QI(void *s, const GUID *iid, void **out) {
    if(!out)return E_POINTER;
    *out=NULL;
    if(iid && (IsEqualGUID(iid,&IID_IAudioRenderClient_l)||IsEqualGUID(iid,&IID_IUnknown))) {
        *out=s;fr_AddRef(s);return S_OK;
    }
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE fr_GetBuffer(void *s, UINT32 frames, BYTE **pp) {
    if(!pp)return E_POINTER;
    *pp=NULL;
    FakeObj *obj=s ? s : &g_fake_render;
    /* Wait out an endpoint migration before touching the control lock: the
       migration itself needs that lock, so waiting inside it would deadlock.
       Without this the client would see DEVICE_INVALIDATED the instant a
       device is unplugged and abandon a track that is about to be resumed. */
    if (g_h_migrating &&
        (g_sink.device_lost || WaitForSingleObject(g_h_migrating, 0) == WAIT_OBJECT_0)) {
        for (int i = 0; i < 200; i++) {          /* at most ~1 s */
            if (WaitForSingleObject(g_h_migrating, 0) != WAIT_OBJECT_0 && !g_sink.device_lost)
                break;
            Sleep(5);
        }
    }
    EnterCriticalSection(&g_control);
    HRESULT hr=S_OK;
    if(!proxy_current(obj)) hr=AUDCLNT_E_DEVICE_INVALIDATED;
    else if(FAILED(g_sink.fault)) hr=g_sink.fault;
    else if(obj->reserved) hr=AUDCLNT_E_OUT_OF_ORDER;
    else if(!frames) hr=AUDCLNT_E_INVALID_SIZE; /* WASAPI rejects a zero-frame request */
    else {
        EnterCriticalSection(&g_lock);
        UINT32 used=g_sink.w-g_sink.r;
        UINT32 free_frames=used<g_sink.reported_frames ? g_sink.reported_frames-used : 0;
        LeaveCriticalSection(&g_lock);
        if(frames>free_frames) hr=AUDCLNT_E_BUFFER_TOO_LARGE;
        else {
            if(obj->capacity<frames) {
                BYTE *p=realloc(obj->staging,(size_t)frames*g_sink.in_block);
                if(!p)hr=E_OUTOFMEMORY;
                else { obj->staging=p;obj->capacity=frames; }
            }
            if(SUCCEEDED(hr)) {
                obj->reserved=frames;obj->writer_tid=GetCurrentThreadId();obj->discarded=0;
                *pp=obj->staging;
            }
        }
    }
    LeaveCriticalSection(&g_control);
    if (hr != S_OK) {
        /* "Client goes silent after a seek" has one silent cause: GetBuffer
           starts failing and the client never reports why. Rate-limited so a
           broken stream cannot flood the log. */
        static DWORD last_log;
        DWORD now = GetTickCount();
        if (now - last_log > 250) {
            last_log = now;
            log_line("GetBuffer(%u) -> 0x%08lX (reserved=%u)",
                     frames, (unsigned long)hr,
                     (unsigned)(obj ? obj->reserved : 0));
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE fr_ReleaseBuffer(void *s, UINT32 written, DWORD flags) {
    if(flags & ~AUDCLNT_BUFFERFLAGS_SILENT)return E_INVALIDARG;
    FakeObj *obj=s ? s : &g_fake_render;
    EnterCriticalSection(&g_control);
    HRESULT hr=S_OK;
    if(!proxy_current(obj)) hr=AUDCLNT_E_DEVICE_INVALIDATED;
    else if(FAILED(g_sink.fault)) hr=g_sink.fault;
    /* Stop/Reset discards the reservation on purpose (a seek never releases the
       frames it was holding). A release arriving after that is the tail of a
       write that has already been thrown away: acknowledging it keeps the
       client on the stream instead of treating it as a protocol violation.
       A different writer thread still is one, and is reported as such. */
    else if(written && !obj->reserved) hr=obj->discarded ? S_OK : AUDCLNT_E_OUT_OF_ORDER;
    else if(written && obj->writer_tid!=GetCurrentThreadId()) hr=AUDCLNT_E_OUT_OF_ORDER;
    else if(written>obj->reserved) hr=AUDCLNT_E_INVALID_SIZE;
    else if(written) {
        BYTE *dst=ring_reserve(written);
        if(!dst)hr=AUDCLNT_E_DEVICE_INVALIDATED;
        else {
            if(flags&AUDCLNT_BUFFERFLAGS_SILENT) memset(dst,0,(size_t)written*g_sink.in_block);
            else memcpy(dst,obj->staging,(size_t)written*g_sink.in_block);
            EnterCriticalSection(&g_lock);
            g_sink.w+=written;
            LeaveCriticalSection(&g_lock);
            SetEvent(g_sink.h_data);
        }
    }
    if(SUCCEEDED(hr)) { obj->reserved=0;obj->writer_tid=0;obj->discarded=0; }
    LeaveCriticalSection(&g_control);
    return hr;
}

static void *g_fake_render_vtbl[5] = {
    (void *)fr_QI, (void *)fr_AddRef, (void *)fr_Release,
    (void *)fr_GetBuffer, (void *)fr_ReleaseBuffer,
};

/* ---- fake IAudioClock --------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE fc_QI(void *s, const GUID *iid, void **out) {
    if(!out)return E_POINTER;
    *out=NULL;
    if(iid && (IsEqualGUID(iid,&IID_IAudioClock_l)||IsEqualGUID(iid,&IID_IUnknown))) {
        *out=s;fr_AddRef(s);return S_OK;
    }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fc_AddRef(void *s) { return fr_AddRef(s); }
static ULONG STDMETHODCALLTYPE fc_Release(void *s) { return fr_Release(s); }

static HRESULT STDMETHODCALLTYPE fc_GetFrequency(void *s, UINT64 *f) {
    (void)s;
    if (!f) return E_POINTER;
    *f = 10000000ULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fc_GetPosition(void *s, UINT64 *pos, UINT64 *qpc) {
    if(!pos)return E_POINTER;
    FakeObj *obj=s ? s : &g_fake_clock;
    EnterCriticalSection(&g_control);
    HRESULT hr=S_OK;
    UINT64 p=0,q=0;
    if(!proxy_current(obj)) hr=AUDCLNT_E_DEVICE_INVALIDATED;
    else if(FAILED(g_sink.fault)) hr=g_sink.fault;
    /* During a migration there is no device clock to read. Reporting the last
       known position keeps the client's timeline where it was; a failure here
       would look like the track ended. */
    else if(g_sink.device_lost) { *pos=g_sink.clock_last; if(qpc)*qpc=0; }
    else if(!g_sink.gen || !g_sink.gen->real_clock || !g_sink.gen->clock_frequency) hr=E_UNEXPECTED;
    else {
        IAudioClock *clk=g_sink.gen->real_clock;
        hr=clk->lpVtbl->GetPosition(clk,&p,&q);
        if(SUCCEEDED(hr)) {
            UINT64 f=g_sink.gen->clock_frequency;
            UINT64 ticks=g_sink.clock_base+(p/f)*10000000ULL
                         +(UINT64)((long double)(p%f)*10000000.0L/f);
            UINT64 prev=g_sink.clock_last;
            if(ticks<prev) {
                /* Only a real backwards jump is worth a line. A seek resets the
                   device clock, and this clamp is what stops the client's
                   timeline from snapping back mid-track. */
                log_line("GetPosition: device clock went backwards (%.3f s -> %.3f s), clamped",
                         (double)prev/10000000.0,(double)ticks/10000000.0);
                ticks=prev;
            }
            g_sink.clock_last=ticks;
            *pos=ticks;
            if(qpc)*qpc=q; /* WASAPI supplies matching QPC in 100 ns units */
        }
    }
    LeaveCriticalSection(&g_control);
    return hr;
}
static HRESULT STDMETHODCALLTYPE fc_GetCharacteristics(void *s, DWORD *c) {
    (void)s;
    if (c) *c = 0;
    return S_OK;
}

static void *g_fake_clock_vtbl[6] = {
    (void *)fc_QI, (void *)fc_AddRef, (void *)fc_Release,
    (void *)fc_GetFrequency, (void *)fc_GetPosition, (void *)fc_GetCharacteristics,
};

