/* ---- drain thread, fault handling and the fake client objects ----------- */
static void sink_fault(SinkGen *g, HRESULT hr, const char *operation) {
    EnterCriticalSection(&g_lock);
    if (SUCCEEDED(g_sink.fault)) g_sink.fault = FAILED(hr) ? hr : E_FAIL;
    if (g) InterlockedExchange(&g->running, 0);
    if (g_sink.h_data) SetEvent(g_sink.h_data);
    if (g_sink.h_space) SetEvent(g_sink.h_space);
    if (g_sink.h_client) SetEvent(g_sink.h_client);
    if (g && g->real_evt) SetEvent(g->real_evt);
    LeaveCriticalSection(&g_lock);
    log_line("sink fault: %s 0x%08lX; client will receive failure (real_frames=%lu evt=%p)",
             operation, (unsigned long)hr,
             (unsigned long)(g ? g->real_frames : 0), (void *)(g ? g->real_evt : NULL));
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
    if (g && g->real_evt) SetEvent(g->real_evt);
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
                log_line("dump: device frames -> %s (%d.%d bit, %d ch)",
                         path, out_container(), out_valid(), g_sink.in_channels);
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
static UINT32 g_starve_reported;   /* g_starve at the previous report */

/* The underrun counter runs for the life of the drain thread; the periodic
   report shows how many happened in that window, with the total beside it,
   so one old underrun is not read as a new one every ten seconds. */
static UINT32 drain_window_count(UINT32 total, UINT32 *reported) {
    UINT32 window = total - *reported;
    *reported = total;
    return window;
}
static DWORD  g_last_write, g_last_report;
static UINT64 g_last_submit_qpc, g_max_gap_qpc;

static UINT64 hifi_changed_bytes(const BYTE *a, const BYTE *b, size_t n) {
    UINT64 changed = 0;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) changed++;
    return changed;
}

static UINT64 hifi_hash_bytes(const BYTE *p, size_t n, UINT64 hash) {
    for (size_t i = 0; i < n; i++) hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

static void hifi_record_native(const BYTE *client, const BYTE *endpoint,
                               UINT32 frames, UINT32 block) {
    const UINT64 bytes = (UINT64)frames * block;
    const UINT64 base_frame = g_sink.hifi_endpoint_frames;
    const UINT64 changed = hifi_changed_bytes(client, endpoint, (size_t)bytes);
    if (!g_sink.hifi_client_hash) g_sink.hifi_client_hash = 1469598103934665603ull;
    if (!g_sink.hifi_endpoint_hash) g_sink.hifi_endpoint_hash = 1469598103934665603ull;
    g_sink.hifi_client_hash = hifi_hash_bytes(client, (size_t)bytes, g_sink.hifi_client_hash);
    g_sink.hifi_endpoint_hash = hifi_hash_bytes(endpoint, (size_t)bytes, g_sink.hifi_endpoint_hash);
    g_sink.hifi_client_frames += frames;
    g_sink.hifi_endpoint_frames += frames;
    if (changed) {
        g_sink.hifi_changed_bytes += changed;
        if (g_sink.hifi_first_mismatch_frame == ~(UINT64)0) {
            for (UINT64 i = 0; i < bytes; i++) {
                if (client[i] == endpoint[i]) continue;
                g_sink.hifi_first_mismatch_frame = base_frame + i / block;
                g_sink.hifi_first_mismatch_byte = (UINT32)(i % block);
                break;
            }
        }
    } else {
        g_sink.hifi_verified_frames += frames;
    }
}

/* One timer-driven exclusive submission. GetBuffer and ReleaseBuffer agree on
   size, no fabricated silence, and data already buffered in SRC is not skipped.
   Every blocking call is followed by a retired check: once the control path has
   let go of this generation, the thread must not touch the ring, the resampler
   or the negotiated format again - they may already belong to the next stream. */
static HRESULT drain_once(SinkGen *g) {
    UINT32 pad=0;
    g->stage=STAGE_PADDING;
    HRESULT hr=S_OK;
    UINT32 space=0;
    if (g->remote) {
        /* The producer owns a complete device period.  Transport availability
           is handled by remote_write(), which waits for the whole batch or
           fails it; it must not shorten this request before conversion. */
        space = g->real_frames;
        if (remote_state()==NEX_AUDIO_FAILED)
            return FAILED(g_remote_error) ? g_remote_error : E_FAIL;
    } else {
        hr=O_GETPADDING(g->real,&pad);
        if(g->retired)return E_ABORT;
        if (FAILED(hr)) return hr;
        if (g->device_started && pad==0 && g->submitted) g_starve++;
        if (pad>g->real_frames) return E_UNEXPECTED;
        space=g->real_frames-pad;
    }
    if (!space) return S_FALSE;
    if (!g->remote && g_sink.prime_left) {
        /* Start-up silence (Orion primeWithSilence): the first device buffer
           of a fresh stream is silence, so a DAC that is waking up or relocking
           to a new clock family does not swallow the opening notes. The
           hardware is still started only once a full buffer of client audio is
           waiting, so the silence is followed by music, never by an underrun. */
        if (!g->device_started) {
            UINT64 need = g_sink.out_rate && g_sink.in_rate != g_sink.out_rate
                ? ((UINT64)g->real_frames * (UINT64)g_sink.in_rate + g_sink.out_rate - 1) / g_sink.out_rate
                : g->real_frames;
            if (need > g_sink.cap) need = g_sink.cap;
            EnterCriticalSection(&g_lock);
            UINT32 have = g_sink.w - g_sink.r;
            LeaveCriticalSection(&g_lock);
            if (have < need) {
                if (g_sink.h_client) SetEvent(g_sink.h_client);
                return S_FALSE;
            }
        }
        UINT32 m = g_sink.prime_left < space ? g_sink.prime_left : space;
        if (g->real_evt) {
            if (pad != 0 || (g->device_started && !g->evt_fired)) return S_FALSE;
            m = g->real_frames;
            InterlockedExchange(&g->evt_fired, 0);
        }
        BYTE *silent = NULL;
        g->stage = STAGE_GETBUF;
        hr = g->real_render->lpVtbl->GetBuffer(g->real_render, m, &silent);
        if (g->retired) return E_ABORT;
        if (FAILED(hr)) return hr;
        g->stage = STAGE_RELEASE;
        hr = g->real_render->lpVtbl->ReleaseBuffer(g->real_render, m, AUDCLNT_BUFFERFLAGS_SILENT);
        if (g->retired) return E_ABORT;
        if (FAILED(hr)) return hr;
        g_sink.prime_left = m >= g_sink.prime_left ? 0 : g_sink.prime_left - m;
        g_sink.skip_frames += m;
        if (!g->device_started) {
            g->stage = STAGE_START;
            hr = O_START(g->real);
            if (g->retired) return E_ABORT;
            if (FAILED(hr)) return hr;
            g->device_started = 1;
            log_line("sink: hardware started on %lu frames of silence (%lu more to come)",
                     (unsigned long)m, (unsigned long)g_sink.prime_left);
        }
        g->stage = STAGE_IDLE;
        return S_OK;
    }
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
                if (g->remote) remote_note_client(g_inb, got);
                to_double(g_inb,g_fin,got);
                if (src_push(g_fin,(int)got,g_sink.in_channels)!=(int)got) return E_UNEXPECTED;
                if (g_sink.h_space) SetEvent(g_sink.h_space);
            }
        }
        ready=src_ready(g_sink.in_channels);
        if (ready>0) n=(UINT32)ready<space ? (UINT32)ready : space;
    } else {
        EnterCriticalSection(&g_lock);
        n=g_sink.w-g_sink.r;            /* ring_peek copies across the wrap */
        LeaveCriticalSection(&g_lock);
        if(n>space)n=space;
        if(n>SCRATCH_FRAMES)n=SCRATCH_FRAMES;
    }
    /* An event-driven exclusive stream is signalled for one complete device
       buffer. Several drivers reject a short GetBuffer request with
       AUDCLNT_E_BUFFER_SIZE_ERROR even when the current padding leaves room.
       Wait for a full period and submit exactly the negotiated buffer size;
       the client ring remains untouched while it is still filling. */
    if (g->real_evt) {
        if (n < g->real_frames) {
            /* The client may write in packets smaller than one device period.
               Keep its event pump moving while the ring prefills, including
               before the hardware has been started for the first time. */
            if (g_sink.h_client) SetEvent(g_sink.h_client);
            return S_FALSE;
        }
        /* Submit on the device's request only. Padding alone is not trusted:
           several exclusive drivers report 0 between events. */
        if (pad != 0 || (g->device_started && !g->evt_fired)) return S_FALSE;
        n = g->real_frames;
        InterlockedExchange(&g->evt_fired, 0);
    }
    if(!n) {
        if (!g->real_evt && space && g_sink.h_client) {
            EnterCriticalSection(&g_lock);
            int empty = g_sink.w == g_sink.r;
            LeaveCriticalSection(&g_lock);
            if (empty) SetEvent(g_sink.h_client);
        }
        return S_FALSE;
    }
    BYTE *dst=g->remote ? g_remote_out : NULL;
    if (!g->remote) {
        g->stage=STAGE_GETBUF;
        hr=g->real_render->lpVtbl->GetBuffer(g->real_render,n,&dst);
        /* The call above is the one that can park for the lifetime of the process.
           Check before anything else reads shared state. */
        if(g->retired)return E_ABORT;
        if(FAILED(hr))return hr;
    }
    if(g_sink.resample) {
        src_pull(g_fout,(int)n,g_sink.in_channels);
        quantize(g_fout,n,dst);
    } else {
        UINT32 got = g->remote ? ring_peek(g_inb,n) : ring_take(g_inb,n);
        if(got!=n) {
            if (!g->remote) g->real_render->lpVtbl->ReleaseBuffer(g->real_render,0,0);
            return E_UNEXPECTED;
        }
        convert(g_inb,dst,n);
        if (g_sink.hifi_map == 1) {
            if (!g->remote) hifi_record_native(g_inb, dst, n, (UINT32)g_sink.in_block);
        } else if (g_sink.hifi_map == 2) {
            g_sink.hifi_converted_frames += n;
        }
    }
    apply_fade(dst,n);
    dump_frames(dst,n);
    g->stage=STAGE_RELEASE;
    if (g->remote) {
        if (remote_write(g_sink.resample ? NULL : g_inb, dst, n) != n)
            return FAILED(g_remote_error) ? g_remote_error : E_FAIL;
        if (!g_sink.resample) ring_commit(n);
        if (!g_sink.resample) remote_note_client(g_inb, n);
        g->device_started = g_remote_ipc && g_remote_ipc->device_started;
    } else {
        hr=g->real_render->lpVtbl->ReleaseBuffer(g->real_render,n,0);
        if(g->retired)return E_ABORT;
        if(FAILED(hr))return hr;
    }
    g->submitted+=n;
    {
        DWORD t=GetTickCount();
        LARGE_INTEGER qpc_now, qpc_freq;
        UINT64 qpc_gap = 0;
        if (QueryPerformanceCounter(&qpc_now)) {
            UINT64 qpc = (UINT64)qpc_now.QuadPart;
            if (g_last_submit_qpc && qpc > g_last_submit_qpc) {
                qpc_gap = qpc - g_last_submit_qpc;
                if (qpc_gap > g_max_gap_qpc) g_max_gap_qpc = qpc_gap;
            }
            g_last_submit_qpc = qpc;
        }
        g->last_submit=(LONG)t;
        if(g_last_write) { DWORD gap=t-g_last_write; if(gap>g_max_gap) g_max_gap=gap; }
        g_last_write=t;
        /* Polling can submit many small batches per second. Report by elapsed
           time so the log stays bounded and gaps use a consistent window. */
        g_writes++;
        if (t - g_last_report >= 10000) {
            UINT64 ring_used = 0;
            UINT32 device_padding = 0;
            const char *state = "in_process";
            if (g->remote && g_remote_ipc) {
                ring_used = nex_audio_ring_available(g_remote_ipc);
                device_padding = g_remote_ipc->device_padding_frames;
                state = nex_audio_state_name(remote_state());
            }
            QueryPerformanceFrequency(&qpc_freq);
            UINT32 starve_window = drain_window_count(g_starve, &g_starve_reported);
            log_line("drain: gen=%llu writes=%lu empty-buffer=%lu (total %lu) max gap=%lu ms "
                     "max_gap_qpc=%llu qpc_hz=%llu ring_used=%llu padding=%lu "
                     "backpressure=%llu state=%s",
                     (unsigned long long)g->id,
                     (unsigned long)g_writes,(unsigned long)starve_window,(unsigned long)g_starve,
                     (unsigned long)g_max_gap,
                     (unsigned long long)g_max_gap_qpc,
                     (unsigned long long)qpc_freq.QuadPart,
                     (unsigned long long)ring_used,
                     (unsigned long)device_padding,
                     g->remote && g_remote_ipc
                         ? (unsigned long long)nex_audio_read64(&g_remote_ipc->backpressure_events) : 0,
                     state);
            g_max_gap=0;
            g_max_gap_qpc=0;
            g_last_report=t;
        }
    }
    if(g_sink.h_space)SetEvent(g_sink.h_space);
    /* The intercepted client is event driven. Every successful device-period
       submission creates space in its logical buffer, so every submission must
       wake its producer. Waiting until the ring is empty leaves the producer
       asleep while the endpoint is already consuming frames. */
    if(g_sink.h_client) SetEvent(g_sink.h_client);
    if(!g->device_started && !g->remote) {
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
/* The audio engine schedules its own render threads through MMCSS; a thread
   that only raises its priority can still be preempted by DPC-heavy drivers and
   other "Pro Audio" work, which is heard as a dropout. NexAudioHost already
   registers; the in-process drain thread does the same. */
static HANDLE drain_mmcss_enter(const char **task) {
    DWORD index = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    if (task) *task = h ? "Pro Audio" : NULL;
    if (!h) {
        index = 0;
        h = AvSetMmThreadCharacteristicsW(L"Audio", &index);
        if (task) *task = h ? "Audio" : NULL;
    }
    if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
    return h;
}
static void drain_mmcss_leave(HANDLE h) {
    if (h) AvRevertMmThreadCharacteristics(h);
}

/* Orion's LocalPlaybackTuning, both opt-in: a core of its own for the drain
   thread, and a raised process priority class while a stream is engaged. */
static DWORD g_prio_saved;              /* 0: not raised */

static void drain_pin_core(void) {
    DWORD bytes = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &bytes);
    if (!bytes) return;
    BYTE *buf = (BYTE *)malloc(bytes);
    if (!buf) return;
    NexAudioCoreInfo cores[64];
    int count = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                                         (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &bytes)) {
        for (DWORD off = 0; off < bytes && count < 64;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e =
                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (e->Relationship == RelationProcessorCore && e->Processor.GroupCount &&
                e->Processor.GroupMask[0].Group == 0) {
                cores[count].efficiency = e->Processor.EfficiencyClass;
                cores[count].mask = (unsigned long long)e->Processor.GroupMask[0].Mask;
                count++;
            }
            if (!e->Size) break;
            off += e->Size;
        }
    }
    free(buf);
    int pick = nex_audio_policy_pick_core(cores, count);
    if (pick < 0) { log_line("drain: dedicated_core: no suitable core (%d cores)", count); return; }
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)cores[pick].mask);
    log_line("drain: pinned to core %d mask=0x%llX class=%u%s", pick,
             cores[pick].mask, (unsigned)cores[pick].efficiency, old ? "" : " (failed)");
}

static void sink_raise_priority(void) {
    if (!g_process_priority || g_prio_saved) return;
    DWORD current = GetPriorityClass(GetCurrentProcess());
    if (!current || current == HIGH_PRIORITY_CLASS || current == REALTIME_PRIORITY_CLASS) return;
    if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        g_prio_saved = current;
        log_line("sink: process priority raised to HIGH (was 0x%lX)", (unsigned long)current);
    }
}

static void sink_restore_priority(void) {
    if (!g_prio_saved) return;
    SetPriorityClass(GetCurrentProcess(), g_prio_saved);
    log_line("sink: process priority restored to 0x%lX", (unsigned long)g_prio_saved);
    g_prio_saved = 0;
}

static DWORD WINAPI drain_thread(LPVOID param) {
    SinkGen *g=(SinkGen *)param;
    HRESULT co=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    const char *mm_task = NULL;
    HANDLE mmcss = drain_mmcss_enter(&mm_task);
    if (g_dedicated_core) drain_pin_core();
    sink_raise_priority();
    g->stage=STAGE_IDLE;
    log_line("drain: thread up (gen %llu), out=%d Hz/%d.%d bit, mmcss=%s",
             (unsigned long long)g->id,g_sink.out_rate,out_container(),out_valid(),
             mm_task ? mm_task : "unavailable");
    g_last_write = 0;
    g_last_report = GetTickCount();
    g_max_gap = 0;
    g_last_submit_qpc = 0;
    g_max_gap_qpc = 0;
    g_writes = 0;
    g_starve = 0;
    g_starve_reported = 0;
    while(g->running && !g->retired) {
        HRESULT hr=drain_once(g);
        if(g->retired) break;
        if(hr==AUDCLNT_E_DEVICE_INVALIDATED) { sink_device_lost(g,"submission"); break; }
        /* An error that arrives after our own Stop is the expected answer to a
           submission interrupted mid-flight, not a broken stream. Faulting here
           would poison a sink that is being rebuilt on purpose. */
        if(FAILED(hr)) {
            if(!g->running) break;
            log_line("sink: device submission detail hr=0x%08lX stage=%ld real_frames=%lu evt=%p",
                     (unsigned long)hr, (long)g->stage,
                     (unsigned long)g->real_frames, (void *)g->real_evt);
            sink_fault(g,hr,"device submission");
            break;
        }
        if(hr==S_FALSE) {
            HANDLE waiters[3]; DWORD count = 0;
            if (g->real_evt) waiters[count++] = g->real_evt;
            if (g_sink.h_data) waiters[count++] = g_sink.h_data;
            if (g->remote && g_remote_space) waiters[count++] = g_remote_space;
            if (g_h_wake) waiters[count++] = g_h_wake;
            if (count) {
                /* The device event is the primary wake-up. A short polling
                   fallback bounds delivery lateness if the event or this thread
                   is scheduled after the buffer has already run empty. */
                DWORD woke = WaitForMultipleObjects(count, waiters, FALSE, 3);
                if (g->real_evt && woke == WAIT_OBJECT_0) InterlockedExchange(&g->evt_fired, 1);
            } else Sleep(2);
        }
    }
    if(FAILED(g_sink.fault)&&g->device_started && g->real) {
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
    drain_mmcss_leave(mmcss);
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
    size_t staging_bytes;
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
    return g_sink.engaged && obj && obj->generation == g_sink.generation;
}
static int render_proxy_current(FakeObj *obj) {
    /* A same-format successor shares the device generation with the previous
       client. Keep every proxy from that generation usable until its owner
       releases it; invalidating the previous proxy here makes the client
       rebuild again and turns a handoff into a repeating audible gap. */
    return proxy_current(obj);
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
    /* Do not park the client audio thread while an endpoint generation is
       being replaced. Returning invalidated lets the client rebuild against
       the successor generation without a Sleep loop. */
    if (g_h_migrating &&
        (g_sink.device_lost || WaitForSingleObject(g_h_migrating, 0) == WAIT_OBJECT_0)) {
        return AUDCLNT_E_DEVICE_INVALIDATED;
    }
    EnterCriticalSection(&g_control);
    HRESULT hr=S_OK;
    UINT32 used_frames=0, free_frames=0, queue_frames=g_sink.reported_frames;
    if(!render_proxy_current(obj)) hr=AUDCLNT_E_DEVICE_INVALIDATED;
    else if(FAILED(g_sink.fault)) hr=g_sink.fault;
    else if(obj->reserved) hr=AUDCLNT_E_OUT_OF_ORDER;
    else if(!frames) hr=AUDCLNT_E_INVALID_SIZE; /* WASAPI rejects a zero-frame request */
    else {
        EnterCriticalSection(&g_lock);
        used_frames=g_sink.w-g_sink.r;
        queue_frames = g_sink.reported_frames;
        /* Event-driven exclusive prefill can span several fixed client packets
           before one full device period is committed. Keep the public buffer
           size and per-request capacity unchanged, but let those packets use
           the ring storage that was allocated for exactly this queueing. */
        if (g_sink.gen && g_sink.gen->real_evt && g_sink.cap > queue_frames)
            queue_frames = g_sink.cap;
        free_frames=used_frames<queue_frames ? queue_frames-used_frames : 0;
        LeaveCriticalSection(&g_lock);
        if(frames>free_frames) hr=AUDCLNT_E_BUFFER_TOO_LARGE;
        else {
            if(frames > obj->capacity) hr=AUDCLNT_E_BUFFER_TOO_LARGE;
            if(SUCCEEDED(hr)) {
                obj->reserved=frames;obj->writer_tid=GetCurrentThreadId();obj->discarded=0;
                *pp=obj->staging;
            }
        }
    }
    LeaveCriticalSection(&g_control);
    return hr;
}

static HRESULT STDMETHODCALLTYPE fr_ReleaseBuffer(void *s, UINT32 written, DWORD flags) {
    if(flags & ~AUDCLNT_BUFFERFLAGS_SILENT)return E_INVALIDARG;
    FakeObj *obj=s ? s : &g_fake_render;
    EnterCriticalSection(&g_control);
    HRESULT hr=S_OK;
    /* Same-format handoffs keep all proxies in the generation usable. A full
       generation teardown invalidates outstanding packets, so their staging
       memory remains alive only until the caller releases the proxy. */
    int pending_old = g_sink.engaged && obj && obj->generation == g_sink.generation
                   && obj->reserved && obj != g_sink.proxy_render;
    int discarded_old = g_sink.engaged && obj && obj->generation == g_sink.generation
                     && obj->discarded && obj != g_sink.proxy_render;
    if(!render_proxy_current(obj) && !pending_old && !discarded_old)
        hr=AUDCLNT_E_DEVICE_INVALIDATED;
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
        if(flags&AUDCLNT_BUFFERFLAGS_SILENT)
            memset(obj->staging,0,(size_t)written*g_sink.in_block);
        if(!ring_write_copy(obj->staging,written)) {
            hr=AUDCLNT_E_DEVICE_INVALIDATED;
            obj->reserved=0;
            obj->writer_tid=0;
            obj->discarded=1;
        }
        else {
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

/* A same-format successor owns a distinct client-facing render proxy. The
   device generation and ring are shared, but each IAudioClient needs its own
   reservation state and staging buffer so an overlapping handoff cannot make
   one client's packet overwrite the other one's packet. */
static FakeObj *proxy_render_new(UINT64 generation, UINT32 capacity, int block) {
    if (!generation || !capacity || block <= 0) return NULL;
    FakeObj *obj = (FakeObj *)calloc(1, sizeof(*obj));
    if (!obj) return NULL;
    obj->vtbl = g_fake_render_vtbl;
    obj->ref = 1; /* sink-owned until the next handoff or teardown */
    obj->generation = generation;
    obj->capacity = capacity;
    obj->staging_bytes = (size_t)capacity * (size_t)block;
    obj->staging = (BYTE *)malloc(obj->staging_bytes);
    if (!obj->staging) { free(obj); return NULL; }
    return obj;
}

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
    /* Same for the window between an abandoned generation (sink_stop) and its
       rebuild in the next Start: the stream is alive, only its device is not. */
    else if(!g_sink.gen) { *pos=g_sink.clock_last; if(qpc)*qpc=0; }
    else {
        UINT64 f=0;
        if(g_sink.gen->remote) {
            /* The Host publishes the actual IAudioClock snapshot. Endpoint
               submission counters describe queue traffic, not the playhead,
               so they must not be converted into a client timeline. */
            if(!g_remote_ipc || !g_remote_ipc->device_clock_frequency) hr=E_UNEXPECTED;
            else {
                p=nex_audio_read64((volatile UINT64 *)&g_remote_ipc->device_clock_position);
                f=g_remote_ipc->device_clock_frequency;
                q=nex_audio_read64((volatile UINT64 *)&g_remote_ipc->device_clock_qpc);
            }
        } else {
            if(!g_sink.gen->real_clock || !g_sink.gen->clock_frequency) hr=E_UNEXPECTED;
            else {
                IAudioClock *clk=g_sink.gen->real_clock;
                f=g_sink.gen->clock_frequency;
                hr=clk->lpVtbl->GetPosition(clk,&p,&q);
            }
        }
        if(SUCCEEDED(hr)) {
            /* Both paths read an IAudioClock position in units of its own
               frequency (the Host publishes the raw snapshot), converted to
               100 ns. Start-up silence counted by the device clock is not
               client audio and is taken back out. */
            UINT64 played=(p/f)*10000000ULL
                         +(UINT64)((long double)(p%f)*10000000.0L/f);
            UINT64 skip=g_sink.gen->remote
                ? (g_remote_ipc ? nex_audio_read64(&g_remote_ipc->primed_frames_total) : 0)
                : g_sink.skip_frames;
            UINT64 skip_ticks=g_sink.out_rate
                ? skip*10000000ULL/(UINT64)g_sink.out_rate : 0;
            played=played>skip_ticks ? played-skip_ticks : 0;
            UINT64 ticks=g_sink.clock_base+played;
            UINT64 prev=g_sink.clock_last;
            if(ticks<prev) {
                /* A new remote device generation can restart its clock at zero.
                   Move the base forward once to preserve continuity across that
                   generation boundary; no queue-period estimate is involved. */
                if (g_sink.gen->remote && g_sink.gen->device_started) {
                    UINT64 delta = prev - ticks;
                    g_sink.clock_base += delta;
                    ticks += delta;
                    log_line("GetPosition: remote device clock adjusted clock base by %.3f s",
                             (double)delta / 10000000.0);
                } else {
                    /* A local reset or migration can still report a stale
                       device position. Keep the public timeline monotonic. */
                    log_line("GetPosition: device clock went backwards (%.3f s -> %.3f s), clamped",
                             (double)prev/10000000.0,(double)ticks/10000000.0);
                    ticks=prev;
                }
            }
            g_sink.clock_last=ticks;
            *pos=ticks;
            if(qpc)*qpc=q; /* matching QueryPerformanceCounter units */
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

