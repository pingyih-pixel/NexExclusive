/* ---- endpoint negotiation, migration and stream engage ------------------ */
/* One concrete format, end to end: the driver has to accept it in exclusive
   mode and Initialize has to succeed. A format that fails either check leaves
   the endpoint untouched.
   container is the declared byte width, valid the precision inside it. They
   only differ for 24-in-32, which most USB DACs accept and some interfaces do
   not - probing both is the difference between "the device takes 24-bit" and
   "the device takes 24-bit the way we happened to spell it". */
static HRESULT try_exclusive(SinkGen *g, IMMDevice *dev, int rate, int container, int valid,
                             int is_float, int ch) {
    IAudioClient *ac = NULL;
    HRESULT hr = dev_activate(dev, &IID_IAudioClient, (void **)&ac);
    if (FAILED(hr) || !ac) {
        log_line("probe %d/%d.%d%s: activate failed 0x%08lX", rate, container, valid,
                 is_float ? "f" : "", (unsigned long)hr);
        return hr;
    }

    WAVEFORMATEXTENSIBLE we;
    build_fmt_cv(&we, rate, container, valid, is_float, ch,
                 g_sink.channel_mask ? g_sink.channel_mask : mask_for(ch));

    hr = O_ISFORMAT(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX *)&we, NULL);
    if (hr != S_OK) {
        /* The pointer triple below is the whole diagnosis: if vt[7] is still
           H_IsFormatSupported while orig is a plausible module address, the
           probe went through the real API and the driver said no. If orig and
           vt[7] are the same value, ac_original() failed to find the patch
           record and we are about to recurse into our own hook. */
        void **vt = *(void ***)ac;
        log_line("probe %d/%d.%d%s ch=%d mask=0x%lX: IsFormatSupported 0x%08lX "
                 "(vt=%p vt[7]=%p orig=%p)",
                 rate, container, valid, is_float ? "f" : "", ch,
                 (unsigned long)(g_sink.channel_mask ? g_sink.channel_mask : mask_for(ch)),
                 (unsigned long)hr, (void *)vt, vt[AC_ISFORMATSUPPORTED],
                 ac_original(ac, AC_ISFORMATSUPPORTED));
        if (ac) ac->lpVtbl->Release(ac);
        return FAILED(hr) ? hr : E_FAIL;
    }

    /* Event-driven exclusive output. Keep a bounded retry for endpoint
       alignment: Windows reports the aligned frame count through the failed
       client, then requires a fresh activation before Initialize is retried. */
    HANDLE audio_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!audio_event) {
        if (ac) ac->lpVtbl->Release(ac);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    REFERENCE_TIME period = 1000000;
    for (int attempt = 0; attempt < 8; attempt++) {
        hr = O_INITIALIZE(ac, AUDCLNT_SHAREMODE_EXCLUSIVE,
                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                          period, 0, (WAVEFORMATEX *)&we, NULL);
        if (SUCCEEDED(hr)) break;
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned = 0;
            if (FAILED(O_GETBUFFERSIZE(ac, &aligned)) || !aligned) break;
            period = (REFERENCE_TIME)(((UINT64)aligned * 10000000ull) / (UINT32)rate);
            ac->lpVtbl->Release(ac);
            ac = NULL;
            hr = dev_activate(dev, &IID_IAudioClient, (void **)&ac);
            if (FAILED(hr) || !ac) { if (SUCCEEDED(hr)) hr = E_FAIL; break; }
            continue;
        }
        /* the previous stream's teardown does not free the endpoint instantly */
        if ((unsigned long)hr != 0x8889000Aul) break;
        Sleep(50);
    }
    if (FAILED(hr)) {
        log_line("probe %d/%d.%d%s: Initialize 0x%08lX", rate, container, valid,
                 is_float ? "f" : "", (unsigned long)hr);
        CloseHandle(audio_event);
        if (ac) ac->lpVtbl->Release(ac);
        return hr;
    }
    hr = O_SETEVENT(ac, audio_event);
    if (FAILED(hr)) {
        log_line("probe %d/%d.%d%s: SetEventHandle 0x%08lX", rate, container, valid,
                 is_float ? "f" : "", (unsigned long)hr);
        CloseHandle(audio_event);
        ac->lpVtbl->Release(ac);
        return hr;
    }

    UINT32 frames = 0;
    if (FAILED(O_GETBUFFERSIZE(ac, &frames)) || !frames) {
        log_line("sink: %d Hz/%d.%d bit%s passed the format check but has no buffer",
                 rate, container, valid, is_float ? " float" : "");
        CloseHandle(audio_event);
        ac->lpVtbl->Release(ac);
        return E_FAIL;
    }

    IAudioRenderClient *rc = NULL;
    hr = O_GETSERVICE(ac, &IID_IAudioRenderClient_l, (void **)&rc);
    if (FAILED(hr) || !rc) {
        log_line("probe %d/%d.%d%s: GetService(RenderClient) 0x%08lX rc=%p frames=%u",
                 rate, container, valid, is_float ? "f" : "", (unsigned long)hr,
                 (void *)rc, frames);
        CloseHandle(audio_event);
        ac->lpVtbl->Release(ac);
        return FAILED(hr) ? hr : E_POINTER;
    }

    IAudioClock *clock=NULL;
    hr=O_GETSERVICE(ac,&IID_IAudioClock_l,(void **)&clock);
    UINT64 frequency=0;
    if(FAILED(hr)||!clock || FAILED(clock->lpVtbl->GetFrequency(clock,&frequency)) || !frequency) {
        log_line("probe %d/%d.%d%s: GetService(Clock) 0x%08lX clock=%p freq=%llu",
                 rate, container, valid, is_float ? "f" : "", (unsigned long)hr,
                 (void *)clock, (unsigned long long)frequency);
        if(clock)clock->lpVtbl->Release(clock);
        rc->lpVtbl->Release(rc); CloseHandle(audio_event); ac->lpVtbl->Release(ac);
        return FAILED(hr)?hr:E_FAIL;
    }
    REFERENCE_TIME lat = 0;
    O_GETLATENCY(ac, &lat);
    g->real_clock      = clock;
    g->clock_frequency = frequency;
    g->real_evt        = audio_event;
    g->real            = ac;
    g->real_render     = rc;
    g->real_frames     = frames;
    g->real_latency    = lat;
    return S_OK;
}

static int push_uniq(int *dst, int n, int cap, int v) {
    for (int i = 0; i < n; i++) if (dst[i] == v) return n;
    if (n < cap) dst[n++] = v;
    return n;
}

/* Under simplicity preference, keep the source rate first, then prioritize
   rate pairs with an integer conversion ratio. Preserve the existing bandwidth
   order among equal-ratio and non-integer candidates. */
static int integer_rate_ratio(int a, int b) {
    if (a <= 0 || b <= 0) return 0;
    return (a % b == 0) || (b % a == 0);
}

static int rate_ratio_factor(int a, int b) {
    if (a <= 0 || b <= 0) return 0;
    return a > b ? a / b : b / a;
}

static void prefer_integer_ratios(int *rates, int count, int source_rate) {
    for (int i = 1; i < count; i++) {
        int value = rates[i];
        int value_integer = integer_rate_ratio(source_rate, value);
        int value_factor = value_integer ? rate_ratio_factor(source_rate, value) : 0;
        int j = i;
        while (j > 1) {
            int prev = rates[j - 1];
            int prev_integer = integer_rate_ratio(source_rate, prev);
            int prev_factor = prev_integer ? rate_ratio_factor(source_rate, prev) : 0;
            int before = value_integer > prev_integer ||
                         (value_integer == prev_integer && value_integer && value_factor < prev_factor);
            if (!before) break;
            rates[j] = prev;
            j--;
        }
        rates[j] = value;
    }
}

/* Identity of an endpoint, cheap enough to recompute on every poll. Two
   different devices never share an id, so a changed hash means the default
   output really moved. */
static UINT64 endpoint_hash(IMMDevice *dev) {
    LPWSTR id = NULL;
    UINT64 h = 1469598103934665603ULL;
    if (!dev || FAILED(dev->lpVtbl->GetId(dev, &id)) || !id) return 0;
    for (const WCHAR *p = id; *p; p++) h = (h ^ *p) * 1099511628211ULL;
    CoTaskMemFree(id);
    return h;
}

/* Bound for a migration probe: the client is blocked behind g_control for the
   whole search, and the endpoint it is being moved to was working a moment
   ago, so an exhaustive scan is not worth the stall. */
static int g_probe_limit;

/* Probing costs an Activate plus an IsFormatSupported per candidate, and the
   client blocks in Initialize for the whole search. Remembering what worked for
   a given input shape turns every later track into a single attempt. */
static struct {
    int valid;
    int in_rate, in_bits, in_valid, in_float;
    DWORD channel_mask;
    int channels, force_rate, compat, want_bits, src_prefer, hifi;
    UINT64 endpoint_key;
    int out_rate, out_container, out_valid, out_float;
} g_negotiated;

/* Candidate search. Order follows the compatibility spec:
     - the user's requested output depth comes first: it is the only meaning
       "output bit depth" has, and without it the setting is inert
     - then the source representation (native passthrough costs nothing)
     - then float32, then packed 24, then 24-in-32, then 16
     - inside a group: source rate, then the nearest higher rates, then lower
       rates from high to low
   The endpoint's own mix rate is a candidate like any other; it is never taken
   as evidence that exclusive mode will accept it.
   Packed 24 and 24-in-32 are separate declarations in the audio engine. A
   source that arrives in one of them may only be accepted in the other, so both
   are probed rather than assuming the container the client happened to use. */
static HRESULT negotiate(SinkGen *g, IMMDevice *dev, int ch, int *out_rate,
                         int *out_container, int *out_valid, int *out_float, int *tried) {
    static const int pool[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
    const int npool = (int)(sizeof(pool) / sizeof(pool[0]));

    int depc[10], depv[10], dfl[10], nd = 0;
#define ADD_DEPTH(C, V, F) do {                                               \
        int _dup = 0;                                                         \
        for (int _i = 0; _i < nd; _i++)                                       \
            if (depc[_i] == (C) && depv[_i] == (V) && dfl[_i] == (F)) _dup = 1; \
        if (!_dup && nd < 10) { depc[nd] = (C); depv[nd] = (V); dfl[nd] = (F); nd++; } \
    } while (0)

    if (g_out_bits == 32) ADD_DEPTH(32, 32, 1);   /* 32 is float in practice */
    else                  ADD_DEPTH(g_out_bits, g_out_bits, 0);
    if (g_sink.in_is_float) ADD_DEPTH(32, 32, 1);
    else {
        ADD_DEPTH(g_sink.in_container_bits, g_sink.in_valid_bits, 0);
        /* The other spelling of the same precision. */
        if (g_sink.in_valid_bits == 24 && g_sink.in_container_bits != 32)
            ADD_DEPTH(32, 24, 0);
        else if (g_sink.in_container_bits == 32 && g_sink.in_valid_bits == 24)
            ADD_DEPTH(24, 24, 0);
    }
    ADD_DEPTH(32, 32, 1);        /* float32 */
    ADD_DEPTH(24, 24, 0);        /* packed 24 */
    ADD_DEPTH(32, 24, 0);        /* 24-in-32 */
    ADD_DEPTH(16, 16, 0);
#undef ADD_DEPTH

    if (g_hifi) {
        depc[0] = g_sink.in_is_float ? 32 : g_sink.in_container_bits;
        depv[0] = g_sink.in_is_float ? 32 : g_sink.in_valid_bits;
        dfl[0] = g_sink.in_is_float;
        nd = 1;
    }

    int rates[16], nr = 0;
    rates[nr++] = g_force_rate ? g_force_rate : g_sink.in_rate;
    if(!g_compat) nd=1; /* exact representation only when conversion is disabled */
    for (int i = 0; i < npool; i++)                       /* nearest higher */
        if (pool[i] > g_sink.in_rate) nr = push_uniq(rates, nr, 16, pool[i]);
    for (int i = npool - 1; i >= 0; i--)                  /* lower, high to low */
        if (pool[i] < g_sink.in_rate) nr = push_uniq(rates, nr, 16, pool[i]);

    /* endpoint mix rate: candidate only */
    {
        IAudioClient *ac = NULL;
        WAVEFORMATEX *mixf = NULL;
        if (SUCCEEDED(dev_activate(dev, &IID_IAudioClient, (void **)&ac)) && ac) {
            if (SUCCEEDED(O_GETMIXFORMAT(ac, &mixf)) && mixf) {
                nr = push_uniq(rates, nr, 16, (int)mixf->nSamplesPerSec);
                CoTaskMemFree(mixf);
            }
            ac->lpVtbl->Release(ac);
        }
    }

    if(g_hifi || !g_compat || g_force_rate) nr=1;
    else if(g_src_prefer==1) prefer_integer_ratios(rates,nr,g_sink.in_rate);
    UINT64 endpoint_key=endpoint_hash(dev);
    if(!endpoint_key)return E_FAIL;
    *tried = 0;

    /* Cache belongs to an endpoint and a policy, never just an input shape. */
    if (g_negotiated.valid &&
        g_negotiated.in_rate == g_sink.in_rate &&
        g_negotiated.in_bits == (g_sink.in_is_float ? 32 : g_sink.in_container_bits) &&
        g_negotiated.in_valid == g_sink.in_valid_bits &&
        g_negotiated.in_float == g_sink.in_is_float &&
        g_negotiated.channel_mask == g_sink.channel_mask &&
        g_negotiated.channels==ch && g_negotiated.force_rate==g_force_rate &&
        g_negotiated.compat==g_compat && g_negotiated.want_bits==g_out_bits &&
        g_negotiated.src_prefer==g_src_prefer &&
        g_negotiated.hifi==g_hifi &&
        g_negotiated.endpoint_key==endpoint_key) {
        (*tried)++;
        if (SUCCEEDED(try_exclusive(g, dev, g_negotiated.out_rate, g_negotiated.out_container,
                                    g_negotiated.out_valid, g_negotiated.out_float, ch))) {
            *out_rate      = g_negotiated.out_rate;
            *out_container = g_negotiated.out_container;
            *out_valid     = g_negotiated.out_valid;
            *out_float     = g_negotiated.out_float;
            return S_OK;
        }
    }

    /* Source RATE precedes precision preferences: try all depths at that rate. */
    for (int ri = 0; ri < nr; ri++) {
        for (int di = 0; di < nd; di++) {
            if (*tried >= 64) return AUDCLNT_E_UNSUPPORTED_FORMAT;
            if (g_probe_limit && *tried >= g_probe_limit) return AUDCLNT_E_UNSUPPORTED_FORMAT;
            if(rates[ri]!=g_sink.in_rate) {
                long long gcd=src_gcd(g_sink.in_rate,rates[ri]);
                if(rates[ri]/gcd>SRC_MAX_PHASES ||
                   (double)SRC_TAPS/2.0*fmax(1.0,(double)g_sink.in_rate/rates[ri])>SRC_MAX_HALF)continue;
            }
            (*tried)++;
            HRESULT hr = try_exclusive(g, dev, rates[ri], depc[di], depv[di], dfl[di], ch);
            if (SUCCEEDED(hr)) {
                *out_rate      = rates[ri];
                *out_container = depc[di];
                *out_valid     = depv[di];
                *out_float     = dfl[di];
                g_negotiated.valid    = 1;
                g_negotiated.in_rate  = g_sink.in_rate;
                g_negotiated.in_bits  = g_sink.in_is_float ? 32 : g_sink.in_container_bits;
                g_negotiated.in_valid = g_sink.in_valid_bits;
                g_negotiated.in_float = g_sink.in_is_float;
                g_negotiated.channel_mask = g_sink.channel_mask;
                g_negotiated.channels=ch;g_negotiated.force_rate=g_force_rate;
                g_negotiated.compat=g_compat;g_negotiated.endpoint_key=endpoint_key;
                g_negotiated.want_bits=g_out_bits;g_negotiated.src_prefer=g_src_prefer;
                g_negotiated.hifi=g_hifi;
                g_negotiated.out_rate = *out_rate;
                g_negotiated.out_container = *out_container;
                g_negotiated.out_valid = *out_valid;
                g_negotiated.out_float = *out_float;
                return S_OK;
            }
        }
    }
    return E_FAIL;
}

static int channel_mask_channels(DWORD mask) {
    int n = 0;
    while (mask) { n += (int)(mask & 1u); mask >>= 1; }
    return n;
}

static int dither_selected(int input_float, int input_valid, int output_float,
                           int output_valid, int mode, int fade_ms) {
    if (output_float) return 0;
    if (mode == 1) return 1;
    return mode == 2 && output_valid < (input_float ? 24 : input_valid) && fade_ms == 0;
}

static int valid_pcm_format(const WAVEFORMATEX *fmt) {
    if(!fmt || fmt->nSamplesPerSec<8000 || fmt->nSamplesPerSec>384000 ||
       fmt->nChannels<1 || fmt->nChannels>8)return 0;
    if(fmt->wBitsPerSample!=16 && fmt->wBitsPerSample!=24 && fmt->wBitsPerSample!=32)return 0;
    if(fmt->nBlockAlign!=fmt->nChannels*(fmt->wBitsPerSample/8))return 0;
    int fl=fmt->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
    if(fmt->wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
        if(fmt->cbSize<22)return 0;
        const WAVEFORMATEXTENSIBLE *we=(const WAVEFORMATEXTENSIBLE *)fmt;
        if(!IsEqualGUID(&we->SubFormat,&SUB_PCM) && !IsEqualGUID(&we->SubFormat,&SUB_FLOAT))return 0;
        if(we->Samples.wValidBitsPerSample>fmt->wBitsPerSample)return 0;
        if(we->dwChannelMask && channel_mask_channels(we->dwChannelMask)!=fmt->nChannels)return 0;
        fl=IsEqualGUID(&we->SubFormat,&SUB_FLOAT);
    } else if(fmt->wFormatTag!=WAVE_FORMAT_PCM && !fl)return 0;
    return !fl || fmt->wBitsPerSample==32;
}
/* A zero-share handle is released by the OS even when a process crashes.
   Unlike a semaphore, it cannot leave an endpoint permanently locked. The
   caller stores it in the generation that is about to use the endpoint. */

/* 上一代的拆除是异步的。它若是被「放弃」的那一代（drain 线程卡在 GetBuffer
   里），那个线程还会继续持有这个句柄几毫秒——而客户端的下一次 Initialize 早就
   到了。实测一次：16.781 闸门被拒，16.786 被放弃的线程才退出，只差 5 ms；客户
   端因此拿到 DEVICE_IN_USE，回退到共享流，而共享流又装不下它的格式
   （0x88890008），等于这一跳完全没有流。
   所以重试要放在这里：try_exclusive 里那个 8×50 ms 的重试根本轮不到，因为失败
   发生在它之前。

   抽成独立函数是为了让回归测试能直接喂一个自己占着的路径——真设备的哈希在桩测
   试里造不出来，而这里要验的恰恰是「等」这件事本身。 */
static HANDLE open_gate_path(const WCHAR *path, int *attempts) {
    HANDLE gate=INVALID_HANDLE_VALUE;
    int i;
    for(i=0;i<20;i++){
        gate=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,NULL);
        if(gate!=INVALID_HANDLE_VALUE)break;
        /* 只有「别人还开着」值得等；路径类错误等多久都一样。 */
        DWORD err=GetLastError();
        if(err!=ERROR_SHARING_VIOLATION && err!=ERROR_ACCESS_DENIED)break;
        Sleep(10);
    }
    if(attempts)*attempts=i;
    return gate;
}

static HANDLE acquire_endpoint_gate(IMMDevice *dev, HRESULT *why) {
    if (why) *why = S_OK;
    UINT64 hash=endpoint_hash(dev);
    if(!hash) { if (why) *why = E_FAIL; return NULL; }
    WCHAR path[MAX_PATH],tmp[MAX_PATH];
    DWORD len=GetTempPathW(MAX_PATH,tmp);
    if(!len||len>=MAX_PATH-50) { if (why) *why = E_FAIL; return NULL; }
    _snwprintf(path,MAX_PATH,L"%lsNexExclusive-%016llx.lock",tmp,(unsigned long long)hash);
    int attempts=0;
    HANDLE gate=open_gate_path(path,&attempts);
    /* 等到了就记一笔：这是「上一代被放弃、线程还没退出」这个窗口真的出现过的
       证据。正常路径 attempts=0，不会有这行。 */
    if(gate!=INVALID_HANDLE_VALUE && attempts>0)
        log_line("endpoint gate: waited %d x 10 ms for the previous holder, then got it",
                 attempts);
    if(gate==INVALID_HANDLE_VALUE){
        char u8[MAX_PATH*3];
        int n=WideCharToMultiByte(CP_UTF8,0,path,-1,u8,sizeof(u8),NULL,NULL);
        log_line("endpoint gate busy after %d attempts (%lu): %s",
                 attempts,(unsigned long)GetLastError(),n>0?u8:"?");
        if (why) *why = AUDCLNT_E_DEVICE_IN_USE;
        return NULL;
    }
    return gate;
}
/* Move the live stream to whatever is now the default output. The client's
   ring, its buffer size, its proxies and its timeline are all preserved; only
   the device side is replaced. Called with g_control held and the previous
   generation already stopped, because the old device handle is about to be
   released. */
static HRESULT sink_retarget(const char *reason) {
    if (!g_sink.engaged || !g_sink.owner) return E_UNEXPECTED;

    /* Keep the client's clock where it was: the new device starts at zero. */
    UINT64 keep_clock = g_sink.clock_last;

    /* The previous generation is already gone (sink_stop dropped it, or it was
       abandoned to its thread). Anything still here would be a bug. */
    if (g_sink.gen) { gen_release(g_sink.gen); g_sink.gen = NULL; }

    IMMDevice *dev = NULL;
    InterlockedIncrement(&g_internal);
    IMMDeviceEnumerator *e = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &IID_IMMDeviceEnumerator, (void **)&e)) && e) {
        e->lpVtbl->GetDefaultAudioEndpoint(e, eRender, eConsole, &dev);
        e->lpVtbl->Release(e);
    }
    InterlockedDecrement(&g_internal);
    if (!dev) {
        log_line("retarget(%s): no default endpoint to move to", reason);
        sink_teardown();
        return E_FAIL;
    }

    /* Installed before it is filled so every failure path below releases it
       through the normal teardown instead of leaking a half-built generation. */
    HRESULT hr = S_OK;
    SinkGen *gen = gen_new();
    if (!gen) { dev->lpVtbl->Release(dev); sink_teardown(); return E_OUTOFMEMORY; }
    g_sink.gen = gen;

    HRESULT why = S_OK;
    gen->endpoint_gate = acquire_endpoint_gate(dev, &why);
    if (!gen->endpoint_gate) {
        dev->lpVtbl->Release(dev);
        log_line("retarget(%s): endpoint gate unavailable 0x%08lX", reason, (unsigned long)why);
        sink_teardown();
        return why;
    }

    UINT64 key = endpoint_hash(dev);
    int rate = 0, cont = 0, valid = 0, fl = 0, tried = 0;
    g_probe_limit = 12;
    hr = negotiate(gen, dev, g_sink.in_channels, &rate, &cont, &valid, &fl, &tried);
    g_probe_limit = 0;
    dev->lpVtbl->Release(dev);
    if (FAILED(hr)) {
        log_line("retarget(%s): %d candidates rejected, stream cannot continue", reason, tried);
        sink_teardown();
        return hr;
    }
    log_line("retarget(%s): %d Hz/%d.%d bit%s after %d candidates",
             reason, rate, cont, valid, fl ? " float" : "", tried);

    g_sink.out_rate           = rate;
    g_sink.out_container_bits = cont;
    g_sink.out_valid_bits     = valid;
    g_sink.out_bits           = valid;
    g_sink.out_is_float       = fl;
    g_sink.out_block          = g_sink.in_channels * cont / 8;
    g_sink.resample           = (rate != g_sink.in_rate);

    /* Same rule as the first engage: depth reduction only, and only when the
       integer-domain opening ramp is off (see the note in sink_engage). */
    g_sink.dither = dither_selected(g_sink.in_is_float, g_sink.in_valid_bits,
                                    fl, valid, g_dither, g_fade_ms);

    /* The resampler is rebuilt from scratch: its history belongs to the old
       device's clock. g_fout is sized from the new device's buffer. */
    if (g_sink.resample) {
        if (!src_init(g_sink.in_rate, rate, g_sink.in_channels)) { sink_teardown(); return E_OUTOFMEMORY; }
        free(g_fout);
        g_fout = (float *)malloc((size_t)gen->real_frames * g_sink.in_channels * sizeof(float));
        if (!g_fin)
            g_fin = (float *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_channels * sizeof(float));
        if (!g_fout || !g_fin) { sink_teardown(); return E_OUTOFMEMORY; }
    } else {
        src_free();
        free(g_fout); g_fout = NULL;
        free(g_fin);  g_fin  = NULL;
    }

    g_sink.clock_base   = keep_clock;
    g_sink.endpoint_key = key;
    g_sink.device_lost  = 0;
    g_sink.fault        = S_OK;

    /* Ramp in on the new device: it starts from silence mid-waveform, which is
       the same step the opening fade exists to remove. */
    g_sink.fade_total = (UINT32)((UINT64)g_sink.out_rate * (UINT64)g_fade_ms / 1000u);
    if (g_fade_ms > 0 && g_sink.fade_total < 32) g_sink.fade_total = 32;
    g_sink.fade_left  = g_sink.fade_total;

    if (g_sink.started) {
        InterlockedExchange(&gen->running, 1);
        gen_retain(gen);                 /* the thread's reference */
        gen->h_thread = CreateThread(NULL, 0, drain_thread, gen, 0, NULL);
        if (!gen->h_thread) {
            InterlockedExchange(&gen->running, 0);
            gen_release(gen);
            sink_teardown();
            return E_FAIL;
        }
        SetThreadPriority(gen->h_thread, THREAD_PRIORITY_TIME_CRITICAL);
        if (g_sink.h_client) SetEvent(g_sink.h_client);
    }
    log_line("retarget: playing on the new endpoint, clock continues at %.3f s",
             (double)keep_clock / 10000000.0);
    return S_OK;
}

static HRESULT sink_engage(IAudioClient *self, const WAVEFORMATEX *fmt,
                           REFERENCE_TIME dur) {
    if (!valid_pcm_format(fmt)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (g_sink.engaged) {
        if ((void *)self == g_sink.owner) {
            /* The client reuses one IAudioClient across tracks and across
               quality switches. If the new format matches what the ring was
               built for there is nothing to do; if it does not, keeping the
               old sink is worse than useless - the client would write 192k
               float into a ring sized for 44.1k/16, the block size would be
               wrong, and the channels would land interleaved into the wrong
               places (audible as one dead channel). Rebuild instead. */
            int is_float = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
            int valid_bits = fmt->wBitsPerSample;
            DWORD channel_mask = mask_for(fmt->nChannels);
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
                const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)fmt;
                is_float = (we->SubFormat.Data1 == 3);
                if (we->Samples.wValidBitsPerSample) valid_bits = we->Samples.wValidBitsPerSample;
                if (we->dwChannelMask) channel_mask = we->dwChannelMask;
            }
            int same = (int)fmt->nSamplesPerSec  == g_sink.in_rate
                    && fmt->nBlockAlign          == g_sink.in_block
                    && (int)fmt->nChannels       == g_sink.in_channels
                    && fmt->wBitsPerSample       == g_sink.in_container_bits
                    && valid_bits               == g_sink.in_valid_bits
                    && channel_mask             == g_sink.channel_mask
                    && is_float                  == g_sink.in_is_float;
            if (same) {
                log_line("sink: re-initialize with the same format, keeping the sink");
                return S_OK;
            }
            log_line("sink: re-initialize with a new format, rebuilding the stream");
            HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
        } else if (!g_sink.started) {
            /* A new track usually comes with a fresh IAudioClient, and the old
               one has already been stopped by then. Release the exclusive
               stream first, otherwise the newcomer's own Initialize gets
               DEVICE_IN_USE and the client ends up with no working stream. */
            log_line("sink: previous owner is idle, handing the device over");
            HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
        } else {
            /* A new IAudioClient while ours is still running means the client
               moved on without stopping the previous stream - NetEase does
               exactly that on every track change. Passing the newcomer through
               cannot work: the endpoint is ours, so its own Initialize gets
               DEVICE_IN_USE and the listener ends up with a stale exclusive
               stream plus a shared one that never starts. Hand the device over
               instead; teardown invalidates the old proxy, so a late write from
               the previous stream fails cleanly (DEVICE_INVALIDATED) rather
               than crashing.

               Deliberately not "telling a replacement from a concurrent
               stream": the two are indistinguishable here. A track change, a
               quality switch and a notification all arrive as a new client
               while the old one is still submitting, and the exclusive device
               can only serve one of them anyway. Newest wins is the only policy
               that keeps playback working; the loser is told the device went
               away and rebuilds. The numbers in this line are what would show a
               genuinely concurrent stream if one ever appeared (a large idle
               gap with a different format), so the case stays diagnosable. */
            SinkGen *old = g_sink.gen;
            DWORD now = GetTickCount();
            DWORD idle = (old && old->last_submit)
                       ? (DWORD)(now - (DWORD)old->last_submit) : 0xFFFFFFFFu;
            log_line("sink: new stream while gen %llu is still running "
                     "(submitted=%llu, last submit %ld ms ago, %d/%d/%dch -> %d/%d/%dch), taking over",
                     (unsigned long long)(old ? old->id : 0),
                     (unsigned long long)(old ? old->submitted : 0),
                     idle == 0xFFFFFFFFu ? -1L : (long)idle,
                     g_sink.in_rate, g_sink.in_container_bits, g_sink.in_channels,
                     (int)fmt->nSamplesPerSec, (int)fmt->wBitsPerSample, (int)fmt->nChannels);
            HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
        }
    }

    if(g_sink.gen) {
        HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
    }
    IMMDevice *dev = NULL;
    EnterCriticalSection(&g_patch_lock);
    for(int i=0;i<128;i++)if(g_bindings[i].client==self && g_bindings[i].dev) {
        dev=g_bindings[i].dev;dev->lpVtbl->AddRef(dev);break;
    }
    LeaveCriticalSection(&g_patch_lock);
    if (!dev && g_activation_dev) {
        /* Same thread that called Activate: the binding table may have been
           full, but the endpoint we were just handed is still the right one. */
        dev = g_activation_dev;
        dev->lpVtbl->AddRef(dev);
    }
    if (!dev) {
        IMMDeviceEnumerator *e = NULL;
        InterlockedIncrement(&g_internal);
        HRESULT h = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                     &IID_IMMDeviceEnumerator, (void **)&e);
        if (SUCCEEDED(h) && e) {
            e->lpVtbl->GetDefaultAudioEndpoint(e, eRender, eConsole, &dev);
            e->lpVtbl->Release(e);
        }
        InterlockedDecrement(&g_internal);
    }
    if (!dev) { log_line("sink: no endpoint"); return E_FAIL; }

    SinkGen *gen = gen_new();
    if(!gen) { dev->lpVtbl->Release(dev); return E_OUTOFMEMORY; }
    g_sink.gen = gen;
    HRESULT why=S_OK;
    gen->endpoint_gate=acquire_endpoint_gate(dev,&why);
    if(!gen->endpoint_gate) {dev->lpVtbl->Release(dev);sink_teardown_controlled();return why;}
    UINT64 key=endpoint_hash(dev);
    /* Taken here because the device is released further down and the ENGAGED
       line is written long after: the panel needs this endpoint's name and
       hardware-volume capability, not the default endpoint's. */
    char ep_name[192] = "";
    int  ep_hw = -1;
    ep_describe(dev, ep_name, sizeof(ep_name), &ep_hw);
    /* remember the input format */
    g_sink.in_rate      = (int)fmt->nSamplesPerSec;
    g_sink.in_channels  = fmt->nChannels;
    g_sink.in_block     = fmt->nBlockAlign;
    g_sink.in_is_float  = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    g_sink.in_container_bits = fmt->wBitsPerSample;
    g_sink.in_valid_bits=fmt->wBitsPerSample;
    g_sink.channel_mask=mask_for(fmt->nChannels);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)fmt;
        g_sink.in_is_float = (we->SubFormat.Data1 == 3);
        if (we->Samples.wValidBitsPerSample) g_sink.in_valid_bits=we->Samples.wValidBitsPerSample;
        if(we->dwChannelMask)g_sink.channel_mask=we->dwChannelMask;
    }

    if (g_audiohost) {
        HRESULT remote_hr = remote_sink_engage(self, dev, fmt, dur, ep_name, ep_hw);
        dev->lpVtbl->Release(dev);
        return remote_hr;
    }

    int used_rate = 0, used_cont = 0, used_bits = 0, used_float = 0;
    HRESULT hr;

    int tried=0;
    if(!g_compat && g_force_rate && g_force_rate!=g_sink.in_rate) {
        dev->lpVtbl->Release(dev);sink_teardown_controlled();return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    hr=negotiate(gen,dev,g_sink.in_channels,&used_rate,&used_cont,&used_bits,&used_float,&tried);
    dev->lpVtbl->Release(dev);
    if(FAILED(hr)) {
        log_line("sink: %d candidates rejected, no shared fallback implied",tried);
        if (g_hifi) {
            log_line("strict native rejected: device does not accept the client PCM; "
                     "switch to source-preferred mode to continue playback");
        }
        sink_teardown_controlled();return hr;
    }
    log_line("sink: selected %d Hz/%d.%d bit%s after %d candidates",used_rate,used_cont,used_bits,
             used_float?" float":"",tried);

    g_sink.out_rate           = used_rate;
    g_sink.out_container_bits = used_cont;
    g_sink.out_valid_bits     = used_bits;
    g_sink.out_bits           = used_bits;
    g_sink.out_is_float       = used_float;
    g_sink.out_block          = g_sink.in_channels * used_cont / 8;
    g_sink.resample           = (used_rate != g_sink.in_rate);

    /* Dither buys something only when the depth actually drops. Resampling on
       its own does not need it: the polyphase kernel's stopband already sits
       below the 16-bit floor, so adding noise there raises the floor for
       nothing. dither=1 forces it everywhere, dither=0 turns it off.
       auto additionally requires fade_ms=0, because the opening ramp is applied
       to integer samples after quantisation (apply_fade). With both active the
       noise is multiplied by the ramp and stops being triangular for the first
       few milliseconds. Moving the ramp into the float path ahead of quantise
       would let this condition drop; until then the config decides. */
    g_sink.dither = dither_selected(g_sink.in_is_float, g_sink.in_valid_bits,
                                    used_float, used_bits, g_dither, g_fade_ms);

    if (g_sink.resample &&
        !src_init(g_sink.in_rate, used_rate, g_sink.in_channels)) {
        sink_teardown_controlled();
        return E_OUTOFMEMORY;
    }
    scratch_free();
    g_inb=(BYTE *)malloc((size_t)SCRATCH_FRAMES*g_sink.in_block);
    if (g_sink.resample) {
        g_fin=(float *)malloc((size_t)SCRATCH_FRAMES*g_sink.in_channels*sizeof(float));
        g_fout=(float *)malloc((size_t)gen->real_frames*g_sink.in_channels*sizeof(float));
    }
    if(!g_inb || (g_sink.resample && (!g_fin || !g_fout))) {
        sink_teardown_controlled();return E_OUTOFMEMORY;
    }

    /* buffer the client is told about */
    UINT32 want = (UINT32)g_out_frames;
    if (!want) {
        REFERENCE_TIME d = dur > 0 ? dur : 300000;
        want = (UINT32)((double)g_sink.in_rate * (double)d / 10000000.0);
        if (want < 1024) want = 1024;
        if (want > 8192) want = 8192;
    }
    want = (want + 127u) & ~127u;
    g_sink.cap = want * 8;
    if (g_sink.cap < 16384) g_sink.cap = 16384;

    g_sink.ring = (BYTE *)calloc((size_t)g_sink.cap, (size_t)g_sink.in_block);
    g_sink.conv = (BYTE *)malloc((size_t)g_sink.cap*g_sink.in_block);
    if (!g_sink.ring || !g_sink.conv) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    g_sink.w = g_sink.r = 0;

    g_sink.h_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_sink.h_data  = CreateEventA(NULL, FALSE, FALSE, NULL);
    /* Each stream gets its own proxy pair. A pointer the client kept from the
       previous stream then carries an old generation and is rejected instead
       of writing into the new ring. */
    g_sink.generation = (UINT64)InterlockedIncrement64((volatile LONG64 *)&g_generation);
    g_sink.proxy_render = (FakeObj *)calloc(1, sizeof(FakeObj));
    g_sink.proxy_clock  = (FakeObj *)calloc(1, sizeof(FakeObj));
    if (!g_sink.proxy_render || !g_sink.proxy_clock) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    g_sink.proxy_render->vtbl = g_fake_render_vtbl;
    g_sink.proxy_render->ref = 1;
    g_sink.proxy_render->generation = g_sink.generation;
    g_sink.proxy_render->capacity = want;
    g_sink.proxy_render->staging = (BYTE *)malloc((size_t)want * g_sink.in_block);
    if (!g_sink.proxy_render->staging) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    g_sink.proxy_clock->vtbl = g_fake_clock_vtbl;
    g_sink.proxy_clock->ref = 1;
    g_sink.proxy_clock->generation = g_sink.generation;

    g_sink.engaged = 1;
    g_sink.owner = (void *)self;
    ((IUnknown *)self)->lpVtbl->AddRef((IUnknown *)self); /* the sink outlives the caller's handle */
    g_sink.reported_frames = want;
    /* A rebuilt stream starts its own timeline; the watchdog uses the key to
       tell "the default moved" from "we are already on it". */
    g_sink.endpoint_key = key;
    g_sink.clock_base   = 0;
    g_sink.device_lost  = 0;

    char a[160], b[240];
    describe("in", fmt, a, sizeof(a));
    /* "native" means the stream we opened is exactly what the client produces:
       same rate, same container, same precision, same sample type. No
       conversion, no dither. Container matters: 24-in-32 is not native to a
       client that produced packed 24, even though both are 24-bit precision. */
    int native = (used_rate == g_sink.in_rate) && (used_cont == g_sink.in_container_bits)
                 && (used_bits == g_sink.in_valid_bits) && (used_float == g_sink.in_is_float);
    g_sink.hifi_map = g_hifi ? (native ? 1 : 2) : 0;
    g_sink.hifi_first_mismatch_frame = ~(UINT64)0;
    g_sink.hifi_client_hash = 1469598103934665603ull;
    g_sink.hifi_endpoint_hash = 1469598103934665603ull;
    snprintf(b, sizeof(b), "out=rate:%d/cont:%d.%d/ch:%d%s hifi=%d map=%s  %s",
             used_rate, used_cont, used_bits, g_sink.in_channels,
             used_float ? "/float" : "",
             g_hifi,
             g_hifi ? (native ? "client-native" : "converted") : "off",
             native ? "[native]" : (g_sink.resample ? "[converted]" : "[depth only]"));
    log_line("sink ENGAGED  %s  ->  %s  (client buffer %lu frames, ring %lu)  ep=\"%s\" hw=%d",
             a, b, (unsigned long)want, (unsigned long)g_sink.cap, ep_name, ep_hw);
    return S_OK;
}

static HRESULT sink_engage_controlled(IAudioClient *self, const WAVEFORMATEX *fmt,
                                      REFERENCE_TIME dur, int *was_engaged) {
    EnterCriticalSection(&g_control);
    if (was_engaged) *was_engaged = g_sink.engaged;
    HRESULT hr = sink_engage(self, fmt, dur);
    LeaveCriticalSection(&g_control);
    return hr;
}

