/* ---- config ------------------------------------------------------------- */
static int g_enable      = 1;
static int g_out_bits    = 24;
static int g_force_rate  = 0;
static int g_out_frames  = 0;   /* 0 = derive from client's requested duration */
static int g_compat      = 1;   /* allow rate/depth conversion when the device refuses the source format */
static int g_dither      = 2;   /* 0 off, 1 on, 2 auto */
static int g_fade_ms     = 5;   /* explicit processing; 0 disables sample changes */
static int g_shared_fallback = 0; /* strict exclusive by default */
static int g_hifi        = 0;   /* strict native client-format exclusive path */
static int g_audiohost   = 0;   /* optional out-of-process exclusive renderer */
/* Which rate to prefer when the device refuses the source rate.
     0 = bandwidth: nearest higher first, then lower from high to low.
     1 = simplicity: integer conversion ratios first, then bandwidth order.
   The source rate always stays first; this setting only orders conversions. */
static int g_src_prefer  = 0;
static FILETIME g_cfg_mtime;
static int g_cfg_loaded  = 0;

/* ---- module location ---------------------------------------------------- */
/* 默认的 hook.cfg 与 sink.log 都放在「本 DLL 所在目录」，不再写死开发机的
   绝对路径。注入之后 GetModuleFileNameW 仍然返回 DLL 的完整路径，不受目标
   进程工作目录影响；换盘符、换机器、换用户名都不必改代码。
   __ImageBase 是本 PE 的映像基址，由链接器提供，比把函数指针强转成 LPCWSTR
   干净（后者在 -Wall 下会报指针类型不匹配）。 */
extern IMAGE_DOS_HEADER __ImageBase;

static void module_dir(char *out, size_t n) {
    out[0] = 0;
    HMODULE self = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&__ImageBase, &self))
        return;
    WCHAR wpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(self, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    WCHAR *slash = wcsrchr(wpath, L'\\');
    if (!slash) return;
    slash[1] = L'\0';
    if (WideCharToMultiByte(CP_ACP, 0, wpath, -1, out, (int)n, NULL, NULL) <= 0)
        out[0] = 0;
}

/* ---- logging ------------------------------------------------------------ */
static CRITICAL_SECTION g_lock;
static CRITICAL_SECTION g_control; /* lifecycle and producer calls; never held by drain */
static CRITICAL_SECTION g_patch_lock;
static char g_log[MAX_PATH];
static UINT64 g_generation;
typedef struct FakeObj FakeObj;

static void log_line(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    FILE *f = fopen(g_log, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, GetCurrentThreadId(), buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_lock);
    OutputDebugStringA(buf);
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

    BYTE              *conv;            /* scratch for one conversion pass */

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
static HRESULT sink_engage(IAudioClient *self, const WAVEFORMATEX *fmt,
                           REFERENCE_TIME dur);
static HRESULT sink_engage_controlled(IAudioClient *self, const WAVEFORMATEX *fmt,
                                      REFERENCE_TIME dur, int *was_engaged);
static HRESULT dev_activate(IMMDevice *dev, const GUID *iid, void **out);
static HRESULT remote_drain_once(SinkGen *g);
static UINT64 endpoint_hash(IMMDevice *dev);
static HANDLE g_remote_space;

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
    module_dir(out, n);
    strncat(out, "hook.cfg", n - strlen(out) - 1);
}

static void load_config(void) {
    char path[MAX_PATH];
    config_path(path, sizeof(path));
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
    if (g_src_prefer != 0 && g_src_prefer != 1) g_src_prefer = 0;
    if (g_out_bits != 16 && g_out_bits != 24 && g_out_bits != 32) g_out_bits = 24;
    if (g_out_frames < 0 || g_out_frames > 32768) g_out_frames = 0;
    if (g_force_rate < 0 || g_force_rate > 384000) g_force_rate = 0;
    if (g_dither < 0 || g_dither > 2) g_dither = 2;
    if (g_fade_ms < 0 || g_fade_ms > 100) g_fade_ms = 5;
    if (g_hifi) {
        /* Strict native mode follows the client's format and refuses processing
           that would make the output differ from the client PCM. */
        g_force_rate = 0;
        g_compat = 0;
        g_dither = 0;
        g_fade_ms = 0;
    }
    /* Shared output is never a product path. Keep reading the legacy key so old
       configs remain parseable, then force the invariant here. */
    g_shared_fallback = 0;
    log_line("config: enable=%d bits=%d rate=%d buffer=%d compat=%d dither=%d fade_ms=%d shared_fallback=%d src_prefer=%s hifi=%d audiohost=%d",
             g_enable, g_out_bits, g_force_rate, g_out_frames, g_compat, g_dither, g_fade_ms,
             g_shared_fallback, g_src_prefer ? "simplicity" : "bandwidth", g_hifi, g_audiohost);
}

