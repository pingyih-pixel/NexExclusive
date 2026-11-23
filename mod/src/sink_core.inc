/* ---- config ------------------------------------------------------------- */
static int g_enable      = 1;
static int g_out_bits    = 0;   /* 0 = follow the source */
static int g_force_rate  = 0;
static int g_out_frames  = 0;   /* 0 = derive from client's requested duration */
static int g_compat      = 1;   /* allow rate/depth conversion when the device refuses the source format */
static int g_dither      = 2;   /* 0 off, 1 on, 2 auto */
static int g_fade_ms     = 0;   /* explicit processing; 0 (default) leaves samples untouched */
static int g_shared_fallback = 0; /* strict exclusive by default */
static int g_hifi        = 0;   /* strict native client-format exclusive path */
static int g_audiohost   = 0;   /* optional out-of-process exclusive renderer */
static int g_dedicated_core = 0; /* pin the drain thread to one fast core */
static int g_process_priority = 0; /* HIGH_PRIORITY_CLASS while engaged */
static int g_event       = 1;   /* try event-driven exclusive first, timer polling if refused */
static int g_memlock     = 1;   /* VirtualLock the buffers the drain path touches */
static int g_prime       = 1;   /* one device buffer of silence before a fresh start */
static int g_resync_ms   = 200; /* extra silence when the DAC changes clock family */
/* Which rate to prefer when the device refuses the source rate.
     0 = bandwidth: nearest higher first, then lower from high to low.
     1 = simplicity: integer conversion ratios first, then bandwidth order.
   The source rate always stays first; this setting only orders conversions. */
static int g_src_prefer  = 0;
static FILETIME g_cfg_mtime;
static int g_cfg_loaded  = 0;
static UINT64 g_cfg_epoch = 0;
static UINT64 g_cfg_policy_epoch = 0;
static int g_cfg_policy_valid = 0;
static struct {
    int out_bits;
    int force_rate;
    int compat;
    int dither;
    int fade_ms;
    int src_prefer;
    int hifi;
} g_cfg_policy;

/* ---- module location ---------------------------------------------------- */
/* 默认的 hook.cfg 与 sink.log 都放在「本 DLL 所在目录」，不再写死开发机的
   绝对路径。注入之后 GetModuleFileNameW 仍然返回 DLL 的完整路径，不受目标
   进程工作目录影响；换盘符、换机器、换用户名都不必改代码。
   __ImageBase 是本 PE 的映像基址，由链接器提供，比把函数指针强转成 LPCWSTR
   干净（后者在 -Wall 下会报指针类型不匹配）。 */
extern IMAGE_DOS_HEADER __ImageBase;

static int module_dir(char *out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&__ImageBase, &self))
        return 0;
    WCHAR wpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return 0;
    WCHAR *slash = wcsrchr(wpath, L'\\');
    if (!slash) return 0;
    slash[1] = L'\0';
    BOOL used_default = FALSE;
    int converted = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wpath, -1,
                                        out, (int)n, NULL, &used_default);
    if (converted <= 0 || used_default || (size_t)converted > n) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

/* ---- logging ------------------------------------------------------------ */
static CRITICAL_SECTION g_lock;
static CRITICAL_SECTION g_control; /* lifecycle and producer calls; never held by drain */
static CRITICAL_SECTION g_patch_lock;
static char g_log[MAX_PATH];
static UINT64 g_generation;
typedef struct FakeObj FakeObj;

/* The log is written by a background thread. log_line() only formats the
   line and appends it to a bounded in-memory queue under its own small lock.
   It used to open, write and close sink.log while holding g_lock - the lock
   that also guards the client ring - so any log line from any thread could
   hold up audio for the length of a disk write (or an antivirus scan of
   sink.log). A full queue drops lines and counts them; it never blocks. */
#define LOG_QUEUE_LINES 256
#define LOG_LINE_BYTES  640
typedef struct LogQueue {
    char   line[LOG_QUEUE_LINES][LOG_LINE_BYTES];
    UINT32 head, tail;
    UINT32 dropped;
} LogQueue;

static int log_queue_push(LogQueue *q, const char *text) {
    if (q->head - q->tail >= LOG_QUEUE_LINES) { q->dropped++; return 0; }
    snprintf(q->line[q->head % LOG_QUEUE_LINES], LOG_LINE_BYTES, "%s", text);
    q->head++;
    return 1;
}
static int log_queue_take(LogQueue *q, char *out, size_t n) {
    if (q->head == q->tail) return 0;
    snprintf(out, n, "%s", q->line[q->tail % LOG_QUEUE_LINES]);
    q->tail++;
    return 1;
}

static LogQueue g_logq;
static CRITICAL_SECTION g_log_qlock;
static HANDLE g_log_event;
static volatile LONG g_log_async;
static int g_log_enabled = 1;          /* hook.cfg log=0 turns file logging off */

static DWORD WINAPI log_writer(LPVOID unused) {
    static char batch[LOG_QUEUE_LINES][LOG_LINE_BYTES];
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_log_event, 1000);
        int n = 0;
        UINT32 dropped;
        EnterCriticalSection(&g_log_qlock);
        while (n < LOG_QUEUE_LINES && log_queue_take(&g_logq, batch[n], LOG_LINE_BYTES)) n++;
        dropped = g_logq.dropped;
        g_logq.dropped = 0;
        LeaveCriticalSection(&g_log_qlock);
        if (!n && !dropped) continue;
        FILE *f = fopen(g_log, "a");
        if (!f) continue;
        for (int i = 0; i < n; i++) fputs(batch[i], f);
        if (dropped) fprintf(f, "log: %lu line(s) dropped (queue full)\n", (unsigned long)dropped);
        fclose(f);
    }
    return 0;
}

/* Started once the log path is known (DllMain). Before that, and in the
   regression binary, lines are written synchronously under the queue lock. */
static void log_start_writer(void) {
    g_log_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_log_event) return;
    HANDLE t = CreateThread(NULL, 0, log_writer, NULL, 0, NULL);
    if (!t) return;
    CloseHandle(t);
    InterlockedExchange(&g_log_async, 1);
}

static void log_line(const char *fmt, ...) {
    if (!g_log[0] || !g_log_enabled) return;
    char buf[LOG_LINE_BYTES - 48];
    char line[LOG_LINE_BYTES];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%lu] %s\n", st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
    EnterCriticalSection(&g_log_qlock);
    if (g_log_async) {
        log_queue_push(&g_logq, line);
        LeaveCriticalSection(&g_log_qlock);
        SetEvent(g_log_event);
    } else {
        FILE *f = fopen(g_log, "a");
        if (f) { fputs(line, f); fclose(f); }
        LeaveCriticalSection(&g_log_qlock);
    }
    OutputDebugStringA(buf);
}

/* ---- audio buffers stay resident (Orion: VirtualLock) ---------------------
   A page of the ring or the resampler that has been trimmed from the working
   set costs a hard fault on the audio thread - milliseconds on a busy disk,
   which is an audible gap at a 100 ms device buffer. The buffers the drain
   path touches are locked after allocation and unlocked before free. The
   working-set minimum is raised as needed; failure only logs. memlock=0 in
   hook.cfg turns this off. */
#define AUDIO_LOCK_SLOTS 16
typedef struct AudioLock { void *volatile p; SIZE_T n; } AudioLock;
static AudioLock g_audio_locks[AUDIO_LOCK_SLOTS];
static volatile LONG64 g_audio_locked_bytes;
static volatile LONG g_audio_lock_failures;

static int audio_mem_grow_working_set(SIZE_T n) {
    SIZE_T lo = 0, hi = 0;
    HANDLE self = GetCurrentProcess();
    if (!GetProcessWorkingSetSize(self, &lo, &hi)) return 0;
    SIZE_T want_lo = lo + n + 256 * 1024;
    SIZE_T want_hi = hi > want_lo + n ? hi : want_lo + n;
    return SetProcessWorkingSetSize(self, want_lo, want_hi) ? 1 : 0;
}

static void audio_mem_lock(void *p, SIZE_T n) {
    if (!g_memlock || !p || !n) return;
    int slot = -1;
    for (int i = 0; i < AUDIO_LOCK_SLOTS; i++)
        if (InterlockedCompareExchangePointer((PVOID volatile *)&g_audio_locks[i].p, p, NULL) == NULL) {
            slot = i;
            break;
        }
    if (slot < 0) return;
    BOOL ok = VirtualLock(p, n);
    if (!ok && GetLastError() == ERROR_WORKING_SET_QUOTA && audio_mem_grow_working_set(n))
        ok = VirtualLock(p, n);
    if (!ok) {
        DWORD e = GetLastError();
        InterlockedExchangePointer((PVOID volatile *)&g_audio_locks[slot].p, NULL);
        if (InterlockedIncrement(&g_audio_lock_failures) <= 3)
            log_line("memlock: VirtualLock(%lu bytes) failed %lu; buffer stays pageable",
                     (unsigned long)n, (unsigned long)e);
        return;
    }
    g_audio_locks[slot].n = n;
    InterlockedExchangeAdd64(&g_audio_locked_bytes, (LONG64)n);
}

static void audio_mem_unlock(void *p) {
    if (!p) return;
    for (int i = 0; i < AUDIO_LOCK_SLOTS; i++) {
        if (g_audio_locks[i].p != p) continue;
        SIZE_T n = g_audio_locks[i].n;
        VirtualUnlock(p, n);
        InterlockedExchangeAdd64(&g_audio_locked_bytes, -(LONG64)n);
        g_audio_locks[i].n = 0;
        InterlockedExchangePointer((PVOID volatile *)&g_audio_locks[i].p, NULL);
        return;
    }
}

static void audio_free(void *p) {
    audio_mem_unlock(p);
    free(p);
}

/* ---- device generation -------------------------------------------------- */
/* One incarnation of the device side. The client's ring, its proxies and its
   timeline survive a device change; everything below belongs to one endpoint
   and one negotiated format, and is replaced as a unit.

   It is a heap object with a reference count because the drain thread can be
   parked inside IAudioRenderClient::GetBuffer on an endpoint the OS is
   reconfiguring. That call returns only once the stream is released, and the
   stream cannot be released while the thread is inside it - so the control
   path has to be able to walk away from the thread instead of waiting for a
   call that is not coming back. Whoever holds the last reference releases the
   device objects, and that is normally the thread itself on its way out. */
typedef struct SinkGen {
    LONG                ref;
    UINT64              id;
    IAudioClient       *real;
    IAudioRenderClient *real_render;
    IAudioClock        *real_clock;
    UINT64              clock_frequency;
    UINT32              real_frames;
    HANDLE              real_evt;
    volatile LONG       evt_fired;      /* the device asked for its next period */
    REFERENCE_TIME      real_latency;
    HANDLE              endpoint_gate;  /* per-endpoint cross-process lock file */
    volatile LONG       device_started;
    volatile LONG       running;        /* the drain loop's own flag, not the sink's */
    volatile LONG       retired;        /* the control path let go; stop submitting */
    HANDLE              h_thread;
    UINT64              submitted;
    volatile LONG       last_submit;    /* GetTickCount of the last ReleaseBuffer */
    volatile LONG       stage;          /* where the thread is, for diagnosis */
    int                 remote;         /* audiohost owns the endpoint */
} SinkGen;

/* ---- sink state --------------------------------------------------------- */
typedef struct {
    int            engaged;
    void          *owner;               /* the IAudioClient we intercepted */

    /* what the client believes it is writing */
    int            in_rate;
    int            in_channels;
    int            in_container_bits;   /* 16 / 24 / 32 */
    int            in_valid_bits;
    DWORD          channel_mask;
    int            in_is_float;
    int            in_block;            /* bytes per input frame */

    /* what we actually push to the device */
    int            out_rate;
    int            out_bits;            /* valid bits: what quantisation targets */
    int            out_container_bits;  /* bits actually written; 0 = same as out_bits */
    int            out_valid_bits;      /* 0 = same as out_bits */
    int            out_is_float;
    int            out_block;           /* bytes per output frame */
    int            resample;            /* out_rate != in_rate */
    int            dither;              /* TPDF on the final quantize */
    int            hifi_map;             /* 0 off, 1 client-native, 2 converted */
    UINT64         hifi_verified_frames;
    UINT64         hifi_changed_bytes;
    UINT64         hifi_converted_frames;
    UINT64         hifi_client_frames;
    UINT64         hifi_endpoint_frames;
    UINT64         hifi_dropped_frames;
    UINT64         hifi_underrun_events;
    UINT64         hifi_first_mismatch_frame;
    UINT32         hifi_first_mismatch_byte;
    UINT64         hifi_client_hash;
    UINT64         hifi_endpoint_hash;

    UINT64              clock_last;
    UINT64              clock_base;      /* position played before the current device */
    UINT64              endpoint_key;    /* FNV-1a of the endpoint we are playing on */
    volatile LONG       device_lost;     /* the endpoint went away under us */
    UINT64              generation;
    FakeObj            *proxy_render;
    FakeObj            *proxy_clock;
    HRESULT             fault;
    SinkGen            *gen;             /* the current device incarnation */

    BYTE              *ring;            /* frames in the CLIENT's format */
    UINT32             cap;             /* frames */
    UINT32             reported_frames; /* what GetBufferSize tells the client */
    volatile UINT32    w;
    volatile UINT32    r;

    HANDLE             h_space;         /* signalled when the consumer frees room */
    HANDLE             h_data;          /* signalled when the producer writes */
    HANDLE             h_client;        /* the client's event handle */
    volatile LONG      started;
    int                handoff_pending_start;
    UINT64              config_epoch;    /* config used to build this sink */
    UINT64              config_file_epoch;
    int                 config_enable;
    int                 config_frames;
    int                 config_audiohost;

    BYTE              *conv;            /* scratch for one conversion pass */

    UINT32             prime_left;      /* silent frames still to submit before client audio */
    UINT64             skip_frames;     /* silent frames the device clock has counted */

    UINT32             fade_total;      /* frames in the opening ramp */
    UINT32             fade_left;
} Sink;

static Sink g_sink;

/* Container and valid depth of the output stream.
   A device may accept 24-bit data packed into three bytes, or left-justified in
   a 32-bit container with the low eight bits unused. Those are two different
   WAVEFORMATEXTENSIBLE declarations and drivers differ in which one they take:
   most USB DACs want the 32-bit container, some pro interfaces only the packed
   form. out_bits carries the valid depth (what quantisation targets) and
   out_container_bits the container. Zero means "same as valid", which is also
   what the device-free regression tests assume when they set only out_bits. */
static int out_container(void) {
    return g_sink.out_container_bits ? g_sink.out_container_bits : g_sink.out_bits;
}
static int out_valid(void) {
    return g_sink.out_valid_bits ? g_sink.out_valid_bits : g_sink.out_bits;
}
static _Thread_local IMMDevice *g_activation_dev; /* borrowed only during Activate */
static _Thread_local LONG g_internal; /* never hides other threads' client calls */
typedef struct { void *client; IMMDevice *dev; } EndpointBinding;
static EndpointBinding g_bindings[128];
static HRESULT sink_stop(void);
static void proxy_release_owned(void);
static SinkGen *gen_new(void);
static void gen_retain(SinkGen *g);
static void gen_release(SinkGen *g);
static FakeObj *proxy_render_new(UINT64 generation, UINT32 capacity, int block);

/* Defined next to the IMMDevice hooks, which own the original Activate. Called
   from sink_engage, i.e. before the hooks file is included, hence the forward
   declaration. */
static void ep_describe(IMMDevice *dev, char *name, size_t n, int *hw_bits);

/* Endpoint migration. A device can be unplugged, or the user can pick another
   default output, while a track is playing. Both are recoverable: the ring the
   client writes into is ours and does not belong to any device, so only the
   device side has to be rebuilt. g_h_migrating is manual-reset and marks the
   window in which the device side is not usable; the client's writer waits on
   it instead of being told the stream failed. */
static HANDLE g_h_migrating;
static HANDLE g_h_wake;              /* auto-reset: a drain thread saw a loss */
static volatile LONG g_watch_running;

/* The vtable is process-wide, so every IAudioClient in the process — including
   streams the client created before we were injected — lands in these hooks.
   Only the instance that we actually took over may be answered from g_sink;
   everything else has to go to the real API, or an unrelated stream's Stop
   will tear down our drain thread. */
static int owns(void *self) {
    return g_sink.engaged && self && (void *)self == g_sink.owner;
}

/* ---- forward decls ------------------------------------------------------ */
static void patch_audio_client(void *iface, int is_v3);
static HRESULT sink_teardown(void);
static HRESULT sink_teardown_controlled(void);
static HRESULT sink_retarget(const char *reason);
static HRESULT sink_engage(IAudioClient *self, const WAVEFORMATEX *fmt,
                           REFERENCE_TIME dur);
static HRESULT sink_engage_controlled(IAudioClient *self, const WAVEFORMATEX *fmt,
                                      REFERENCE_TIME dur, int *was_engaged);
static HRESULT dev_activate(IMMDevice *dev, const GUID *iid, void **out);
static UINT64 endpoint_hash(IMMDevice *dev);
static HANDLE g_remote_space;
static long remote_state(void);
static void remote_note_client(const BYTE *source, UINT32 frames);
static UINT32 remote_write(const BYTE *client, const BYTE *converted, UINT32 frames);
static HRESULT g_remote_error;
static NexAudioControl *g_remote_ipc;
static BYTE *g_remote_out;

/* The vtable is shared by every IAudioClient in the process, including the one
   backing our own exclusive stream. So the real stream must never be driven
   through ->lpVtbl->, or it lands straight back in our hook. Every call below
   goes through the saved originals instead. */
static void **g_ac_vtbl = NULL;
static void *g_ac_orig[24]; /* test fallback; production uses per-vtable records */
typedef struct { void **vt; void *orig[24]; int v3; } AudioPatch;
static AudioPatch g_audio_patches[32];
static int g_audio_patch_count;
static void *ac_original(void *self, int slot) {
    if (slot < 0 || slot >= 24) return NULL;
    void *fn = NULL;
    EnterCriticalSection(&g_patch_lock);
    if (self) {
        void **vt = *(void ***)self;
        int found = 0;
        for (int i = 0; i < g_audio_patch_count; i++) {
            if (g_audio_patches[i].vt == vt) {
                fn = g_audio_patches[i].orig[slot];
                found = 1;
                break;
            }
        }
        /* Internal dev_activate() deliberately bypasses H_Activate(), so its
           returned IAudioClient may have an unregistered, untouched vtable.
           In that case its own slot is the original. Never borrow another
           interface's function pointer from the global test fallback. */
        if (!found) fn = vt[slot];
    } else {
        /* Kept only for device-free tests that stub the real interface. */
        fn = g_ac_orig[slot];
    }
    LeaveCriticalSection(&g_patch_lock);
    return fn;
}

#define O_INITIALIZE(S,M,F,D,P,W,G) ((HRESULT (STDMETHODCALLTYPE *)(void *, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX *, LPCGUID))ac_original(S,AC_INITIALIZE))(S,M,F,D,P,W,G)
#define O_GETBUFFERSIZE(S,N) ((HRESULT (STDMETHODCALLTYPE *)(void *,UINT32 *))ac_original(S,AC_GETBUFFERSIZE))(S,N)
#define O_GETLATENCY(S,N) ((HRESULT (STDMETHODCALLTYPE *)(void *,REFERENCE_TIME *))ac_original(S,AC_GETSTREAMLATENCY))(S,N)
#define O_GETPADDING(S,N) ((HRESULT (STDMETHODCALLTYPE *)(void *,UINT32 *))ac_original(S,AC_GETCURRENTPADDING))(S,N)
#define O_START(S) ((HRESULT (STDMETHODCALLTYPE *)(void *))ac_original(S,AC_START))(S)
#define O_STOP(S) ((HRESULT (STDMETHODCALLTYPE *)(void *))ac_original(S,AC_STOP))(S)
#define O_RESET(S) ((HRESULT (STDMETHODCALLTYPE *)(void *))ac_original(S,AC_RESET))(S)
#define O_SETEVENT(S,H) ((HRESULT (STDMETHODCALLTYPE *)(void *,HANDLE))ac_original(S,AC_SETEVENTHANDLE))(S,H)
#define O_GETSERVICE(S,I,O) ((HRESULT (STDMETHODCALLTYPE *)(void *,const GUID *,void **))ac_original(S,AC_GETSERVICE))(S,I,O)
#define O_ISFORMAT(S,M,W,C) ((HRESULT (STDMETHODCALLTYPE *)(void *,AUDCLNT_SHAREMODE,const WAVEFORMATEX *,WAVEFORMATEX **))ac_original(S,AC_ISFORMATSUPPORTED))(S,M,W,C)
#define O_GETMIXFORMAT(S,W) ((HRESULT (STDMETHODCALLTYPE *)(void *,WAVEFORMATEX **))ac_original(S,AC_GETMIXFORMAT))(S,W)

/* ---- format helpers ----------------------------------------------------- */
static const GUID SUB_PCM =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID SUB_FLOAT =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static const GUID IID_IAudioRenderClient_l =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID IID_IAudioClock_l =
    {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};

static void describe(const char *tag, const WAVEFORMATEX *w, char *out, size_t n) {
    if (!w) { snprintf(out, n, "%s=(null)", tag); return; }
    unsigned valid = w->wBitsPerSample;
    unsigned sub = 0;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)w;
        valid = we->Samples.wValidBitsPerSample;
        sub = we->SubFormat.Data1;
    }
    snprintf(out, n, "%s=rate:%lu/cont:%u/valid:%u/ch:%u/tag:0x%04X/sub:0x%08X",
             tag, (unsigned long)w->nSamplesPerSec, w->wBitsPerSample, valid,
             w->nChannels, w->wFormatTag, sub);
}

/* container is the byte width declared to the driver, valid is the precision
   inside it. They differ only for 24-in-32 (container 32, valid 24), which is a
   distinct format from packed 24 as far as the audio engine is concerned. */
static void build_fmt_cv(WAVEFORMATEXTENSIBLE *we, int rate, int container, int valid,
                         int is_float, int ch, DWORD mask) {
    memset(we, 0, sizeof(*we));
    we->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    we->Format.nChannels       = (WORD)ch;
    we->Format.nSamplesPerSec  = (DWORD)rate;
    we->Format.wBitsPerSample  = (WORD)container;
    we->Format.nBlockAlign     = (WORD)(ch * container / 8);
    we->Format.nAvgBytesPerSec = (DWORD)rate * we->Format.nBlockAlign;
    we->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    we->Samples.wValidBitsPerSample = (WORD)valid;
    we->dwChannelMask          = mask;
    we->SubFormat              = is_float ? SUB_FLOAT : SUB_PCM;
}

static void __attribute__((unused)) build_fmt_ex(WAVEFORMATEXTENSIBLE *we, int rate, int bits,
                                                 int is_float, int ch, DWORD mask) {
    build_fmt_cv(we, rate, bits, bits, is_float, ch, mask);
}

static DWORD mask_for(int ch) {
    switch (ch) {
        case 1: return 0x4;                 /* FC */
        case 2: return 0x3;                 /* FL|FR */
        case 4: return 0x33;                /* quad */
        case 6: return 0x3F;
        case 8: return 0x63F;
        default: return 0;
    }
}

/* ---- config ------------------------------------------------------------- */
static void config_path(char *out, size_t n) {
    const char *env = getenv("NEX_CFG");
    if (env && *env) { snprintf(out, n, "%s", env); return; }
    if (!module_dir(out, n)) { out[0] = 0; return; }
    size_t used = strlen(out);
    if (used + strlen("hook.cfg") + 1 > n) out[0] = 0;
    else memcpy(out + used, "hook.cfg", strlen("hook.cfg") + 1);
}

static void load_config(void) {
    char path[MAX_PATH];
    config_path(path, sizeof(path));
    if (!path[0]) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return;
    if (g_cfg_loaded && CompareFileTime(&fad.ftLastWriteTime, &g_cfg_mtime) == 0) return;
    g_cfg_mtime  = fad.ftLastWriteTime;
    g_cfg_loaded = 1;

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "enable=", 7) == 0)  g_enable     = atoi(p + 7) != 0;
        else if (_strnicmp(p, "bits=", 5) == 0)   g_out_bits   = atoi(p + 5);
        else if (_strnicmp(p, "rate=", 5) == 0)   g_force_rate = atoi(p + 5);
        else if (_strnicmp(p, "buffer=", 7) == 0) g_out_frames = atoi(p + 7);
        else if (_strnicmp(p, "compat=", 7) == 0) g_compat     = atoi(p + 7) != 0;
        else if (_strnicmp(p, "dither=", 7) == 0) g_dither     = atoi(p + 7);
        else if (_strnicmp(p, "fade_ms=", 8) == 0) g_fade_ms = atoi(p + 8);
        else if (_strnicmp(p, "shared_fallback=", 16) == 0) g_shared_fallback = atoi(p + 16) != 0;
        else if (_strnicmp(p, "hifi=", 5) == 0) g_hifi = atoi(p + 5) != 0;
        else if (_strnicmp(p, "audiohost=", 10) == 0) g_audiohost = atoi(p + 10) != 0;
        else if (_strnicmp(p, "log=", 4) == 0) g_log_enabled = atoi(p + 4) != 0;
        else if (_strnicmp(p, "prime=", 6) == 0) g_prime = atoi(p + 6) != 0;
        else if (_strnicmp(p, "memlock=", 8) == 0) g_memlock = atoi(p + 8) != 0;
        else if (_strnicmp(p, "event=", 6) == 0) g_event = atoi(p + 6) != 0;
        else if (_strnicmp(p, "dedicated_core=", 15) == 0) g_dedicated_core = atoi(p + 15) != 0;
        else if (_strnicmp(p, "process_priority=", 17) == 0) g_process_priority = atoi(p + 17) != 0;
        else if (_strnicmp(p, "resync_ms=", 10) == 0) g_resync_ms = atoi(p + 10);
        else if (_strnicmp(p, "src_prefer=", 11) == 0) {
            char v[32];
            snprintf(v, sizeof(v), "%s", p + 11);
            for (char *q = v; *q; q++) if (*q == '\r' || *q == '\n' || *q == ' ') { *q = 0; break; }
            /* Numeric form is what the panel and nexhost write; the word form
               stays accepted so a hand-edited file still reads naturally. */
            if (_stricmp(v, "simplicity") == 0)      g_src_prefer = 1;
            else if (_stricmp(v, "bandwidth") == 0)  g_src_prefer = 0;
            else                                     g_src_prefer = atoi(v) ? 1 : 0;
        }
    }
    fclose(f);
    if (g_resync_ms < 0 || g_resync_ms > 2000) g_resync_ms = 200;
    {
        /* Legal ranges, defaults and the strict-native overrides are policy:
           see nex_audio_policy_normalize_config. */
        NexAudioPolicyConfig cfg;
        cfg.out_bits = g_out_bits;
        cfg.force_rate = g_force_rate;
        cfg.out_frames = g_out_frames;
        cfg.compat = g_compat;
        cfg.dither = g_dither;
        cfg.fade_ms = g_fade_ms;
        cfg.src_prefer = g_src_prefer;
        cfg.hifi = g_hifi;
        nex_audio_policy_normalize_config(&cfg);
        g_out_bits = cfg.out_bits;
        g_force_rate = cfg.force_rate;
        g_out_frames = cfg.out_frames;
        g_compat = cfg.compat;
        g_dither = cfg.dither;
        g_fade_ms = cfg.fade_ms;
        g_src_prefer = cfg.src_prefer;
        g_hifi = cfg.hifi;
    }
    /* audiohost selects who owns the endpoint. It is independent of hifi:
       converted streams use the same SRC and quantiser before entering the host. */
    /* Shared output is never a product path. Keep reading the legacy key so old
       configs remain parseable, then force the invariant here. */
    g_shared_fallback = 0;
    g_cfg_epoch++;
    {
        int changed = !g_cfg_policy_valid
                   || g_cfg_policy.out_bits != g_out_bits
                   || g_cfg_policy.force_rate != g_force_rate
                   || g_cfg_policy.compat != g_compat
                   || g_cfg_policy.dither != g_dither
                   || g_cfg_policy.fade_ms != g_fade_ms
                   || g_cfg_policy.src_prefer != g_src_prefer
                   || g_cfg_policy.hifi != g_hifi;
        g_cfg_policy.out_bits = g_out_bits;
        g_cfg_policy.force_rate = g_force_rate;
        g_cfg_policy.compat = g_compat;
        g_cfg_policy.dither = g_dither;
        g_cfg_policy.fade_ms = g_fade_ms;
        g_cfg_policy.src_prefer = g_src_prefer;
        g_cfg_policy.hifi = g_hifi;
        g_cfg_policy_valid = 1;
        if (changed) g_cfg_policy_epoch++;
    }
    log_line("config: enable=%d bits=%d rate=%d buffer=%d compat=%d dither=%d fade_ms=%d shared_fallback=%d src_prefer=%s hifi=%d audiohost=%d",
             g_enable, g_out_bits, g_force_rate, g_out_frames, g_compat, g_dither, g_fade_ms,
             g_shared_fallback, g_src_prefer ? "simplicity" : "bandwidth", g_hifi, g_audiohost);
}

