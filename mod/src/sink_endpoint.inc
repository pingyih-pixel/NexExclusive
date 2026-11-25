/* ---- endpoint negotiation, migration and stream engage ------------------ */
/* One concrete format, end to end: the driver has to accept it in exclusive
   mode and Initialize has to succeed. A format that fails either check leaves
   the endpoint untouched.
   container is the declared byte width, valid the precision inside it. They
   only differ for 24-in-32, which most USB DACs accept and some interfaces do
   not - probing both is the difference between "the device takes 24-bit" and
   "the device takes 24-bit the way we happened to spell it". */
/* An event-driven WASAPI client prefills one complete public buffer before it
   waits for the hardware. Never advertise a buffer smaller than the real
   exclusive buffer: the device would wait for more frames while the client
   waits for a hardware event, so playback would remain paused. */
static UINT32 event_safe_client_frames(UINT32 want, const SinkGen *g) {
    if (g && want < g->real_frames) return g->real_frames;
    return want;
}

/* Take an initialized IAudioClient into the generation: buffer size, render
   client, clock. Shared by the exclusive probe and the shared open; on failure
   the client and the event handle are released. */
static HRESULT adopt_stream(SinkGen *g, IAudioClient *ac, HANDLE evt, int rate, int container,
                            int valid, int is_float) {
    HRESULT hr;
    UINT32 frames = 0;
    if (FAILED(O_GETBUFFERSIZE(ac, &frames)) || !frames) {
        log_line("sink: %d Hz/%d.%d bit%s passed the format check but has no buffer",
                 rate, container, valid, is_float ? " float" : "");
        if (evt) CloseHandle(evt);
        ac->lpVtbl->Release(ac);
        return E_FAIL;
    }

    IAudioRenderClient *rc = NULL;
    hr = O_GETSERVICE(ac, &IID_IAudioRenderClient_l, (void **)&rc);
    if (FAILED(hr) || !rc) {
        log_line("probe %d/%d.%d%s: GetService(RenderClient) 0x%08lX rc=%p frames=%u",
                 rate, container, valid, is_float ? "f" : "", (unsigned long)hr,
                 (void *)rc, frames);
        if (evt) CloseHandle(evt);
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
        if(evt)CloseHandle(evt);
        rc->lpVtbl->Release(rc); ac->lpVtbl->Release(ac);
        return FAILED(hr)?hr:E_FAIL;
    }
    REFERENCE_TIME lat = 0;
    O_GETLATENCY(ac, &lat);
    g->real_clock      = clock;
    g->clock_frequency = frequency;
    g->real_evt        = evt;
    g->evt_fired       = 0;
    g->real            = ac;
    g->real_render     = rc;
    g->real_frames     = frames;
    g->real_latency    = lat;
    return S_OK;
}

static HRESULT try_exclusive(SinkGen *g, IMMDevice *dev, int rate, int container, int valid,
                             int is_float, int ch) {
    WAVEFORMATEXTENSIBLE we;
    build_fmt_cv(&we, rate, container, valid, is_float, ch,
                 g_sink.channel_mask ? g_sink.channel_mask : mask_for(ch));
    if (g_audiohost && !g_remote_bypass) {
        HRESULT remote_hr = remote_try_exclusive(g, dev, (const WAVEFORMATEX *)&we);
        if (SUCCEEDED(remote_hr)) return remote_hr;
        if (!g_remote_bypass) return remote_hr;
        log_line("audiohost: %s; retrying %d/%d.%d%s in process",
                 nex_audio_fallback_name(g_remote_reason), rate, container, valid,
                 is_float ? "f" : "");
    }
    IAudioClient *ac = NULL;
    HRESULT hr = dev_activate(dev, &IID_IAudioClient, (void **)&ac);
    if (FAILED(hr) || !ac) {
        log_line("probe %d/%d.%d%s: activate failed 0x%08lX", rate, container, valid,
                 is_float ? "f" : "", (unsigned long)hr);
        return hr;
    }

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

    /* 100 ms device buffer. Event-driven first (Orion's EVENTCALLBACK path):
       the device wakes the drain thread when it wants the next period, instead
       of being polled every 3 ms. Some drivers accept the format but refuse the
       event contract (seen: refused again after alignment negotiation); they
       fall back to the timer-driven contract on a fresh IAudioClient. A busy
       device stays busy either way and is not retried in timer mode. */
    REFERENCE_TIME buffer_duration = 1000000;
    int use_event = g_event &&
        nex_audio_policy_event_fits(g_sink.in_rate, rate,
                                    (long)((UINT64)rate * (UINT64)buffer_duration / 10000000ull),
                                    SRC_CAP, SRC_MAX_HALF);
    HANDLE evt = NULL;
    if (use_event) {
        REFERENCE_TIME d = buffer_duration;
        for (int attempt = 0; attempt < 8; attempt++) {
            hr = O_INITIALIZE(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              d, d, (WAVEFORMATEX *)&we, NULL);
            if (SUCCEEDED(hr)) break;
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 aligned = 0;
                if (FAILED(O_GETBUFFERSIZE(ac, &aligned)) || !aligned) break;
                d = (REFERENCE_TIME)(((UINT64)aligned * 10000000ull + (UINT32)rate / 2) / (UINT32)rate);
                ac->lpVtbl->Release(ac);
                ac = NULL;
                hr = dev_activate(dev, &IID_IAudioClient, (void **)&ac);
                if (FAILED(hr) || !ac) { if (SUCCEEDED(hr)) hr = E_FAIL; break; }
                continue;
            }
            if ((unsigned long)hr != 0x8889000Aul) break;
            Sleep(50);
        }
        if (SUCCEEDED(hr)) {
            evt = CreateEventA(NULL, FALSE, FALSE, NULL);
            hr = evt ? O_SETEVENT(ac, evt) : E_OUTOFMEMORY;
            if (FAILED(hr)) { if (evt) CloseHandle(evt); evt = NULL; }
        }
        if (SUCCEEDED(hr)) {
            log_line("probe %d/%d.%d%s: event-driven exclusive", rate, container, valid,
                     is_float ? "f" : "");
        } else if (!ac || (unsigned long)hr == 0x8889000Aul) {
            log_line("probe %d/%d.%d%s: Initialize(event) 0x%08lX", rate, container, valid,
                     is_float ? "f" : "", (unsigned long)hr);
            if (ac) ac->lpVtbl->Release(ac);
            return hr;
        } else {
            log_line("probe %d/%d.%d%s: event mode refused 0x%08lX, using timer polling",
                     rate, container, valid, is_float ? "f" : "", (unsigned long)hr);
            ac->lpVtbl->Release(ac);
            ac = NULL;
            hr = dev_activate(dev, &IID_IAudioClient, (void **)&ac);
            if (FAILED(hr) || !ac) return FAILED(hr) ? hr : E_FAIL;
            use_event = 0;
        }
    }
    for (int attempt = 0; !use_event && attempt < 8; attempt++) {
        hr = O_INITIALIZE(ac, AUDCLNT_SHAREMODE_EXCLUSIVE, 0,
                          buffer_duration, 0, (WAVEFORMATEX *)&we, NULL);
        if (SUCCEEDED(hr)) break;
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned = 0;
            if (FAILED(O_GETBUFFERSIZE(ac, &aligned)) || !aligned) break;
            buffer_duration = (REFERENCE_TIME)(((UINT64)aligned * 10000000ull) / (UINT32)rate);
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
        if (ac) ac->lpVtbl->Release(ac);
        return hr;
    }

    return adopt_stream(g, ac, evt, rate, container, valid, is_float);
}

/* auto_shared: open the endpoint in shared mode with the client's own format.
   AUTOCONVERTPCM lets the system mixer take it to the mix format, so nothing
   is converted here; the stream is plainly not bit-perfect and is labelled
   [shared]. Never through the separate host: that path exists to own the
   device exclusively. Timer-driven (no event handle), as the drain loop's
   padding arithmetic is the same for both share modes. */
static HRESULT try_shared(SinkGen *g, IMMDevice *dev, int ch, int *out_rate, int *out_container,
                          int *out_valid, int *out_float) {
    WAVEFORMATEXTENSIBLE we;
    build_fmt_cv(&we, g_sink.in_rate, g_sink.in_container_bits, g_sink.in_valid_bits,
                 g_sink.in_is_float, ch, g_sink.channel_mask ? g_sink.channel_mask : mask_for(ch));
    IAudioClient *ac = NULL;
    HRESULT hr = dev_activate(dev, &IID_IAudioClient, (void **)&ac);
    if (FAILED(hr) || !ac) {
        log_line("shared open: activate failed 0x%08lX", (unsigned long)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }
    hr = O_INITIALIZE(ac, AUDCLNT_SHAREMODE_SHARED,
                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                      1000000, 0, (WAVEFORMATEX *)&we, NULL);
    if (FAILED(hr)) {
        log_line("shared open %d/%d.%d%s: Initialize 0x%08lX", g_sink.in_rate,
                 g_sink.in_container_bits, g_sink.in_valid_bits, g_sink.in_is_float ? "f" : "",
                 (unsigned long)hr);
        ac->lpVtbl->Release(ac);
        return hr;
    }
    hr = adopt_stream(g, ac, NULL, g_sink.in_rate, g_sink.in_container_bits,
                      g_sink.in_valid_bits, g_sink.in_is_float);
    if (FAILED(hr)) return hr;
    g->shared = 1;
    g->remote = 0;
    *out_rate = g_sink.in_rate;
    *out_container = g_sink.in_container_bits;
    *out_valid = g_sink.in_valid_bits;
    *out_float = g_sink.in_is_float;
    log_line("shared open %d/%d.%d%s: system mixer converts to the mix format",
             g_sink.in_rate, g_sink.in_container_bits, g_sink.in_valid_bits,
             g_sink.in_is_float ? "f" : "");
    return S_OK;
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

/* Candidate search. Which formats are tried, and in which order, is decided by
   audio_policy.h (nex_audio_policy_build_plan): every depth at the source rate
   first, then the conversion rates. This file only probes.

   The endpoint's own mix rate is a candidate like any other; it is never taken
   as evidence that exclusive mode will accept it.

   A probe that fails with DEVICE_IN_USE says the previous stream still holds
   the endpoint. It is not a refusal of the format, so it neither settles the
   search nor is a conversion chosen behind it remembered. */
typedef struct NegotiateProbe {
    SinkGen *gen;
    IMMDevice *dev;
    int channels;
    HRESULT last;
} NegotiateProbe;

static int negotiate_probe(void *context, const NexAudioPolicyCandidate *c) {
    NegotiateProbe *p = (NegotiateProbe *)context;
    if (c->rate != g_sink.in_rate) {
        /* Conversion ratios the resampler cannot build are not candidates. */
        long long gcd = src_gcd(g_sink.in_rate, c->rate);
        if (c->rate / gcd > SRC_MAX_PHASES ||
            (double)SRC_TAPS / 2.0 * fmax(1.0, (double)g_sink.in_rate / c->rate) / SRC_EDGE > SRC_MAX_HALF)
            return NEX_AUDIO_PROBE_SKIPPED;
    }
    p->last = try_exclusive(p->gen, p->dev, c->rate, c->container_bits, c->valid_bits,
                            c->is_float, p->channels);
    if (SUCCEEDED(p->last)) return NEX_AUDIO_PROBE_ACCEPTED;
    return nex_audio_policy_classify_failure((unsigned long)p->last);
}

static int negotiated_matches(int ch, UINT64 endpoint_key) {
    return g_negotiated.valid &&
           g_negotiated.in_rate == g_sink.in_rate &&
           g_negotiated.in_bits == (g_sink.in_is_float ? 32 : g_sink.in_container_bits) &&
           g_negotiated.in_valid == g_sink.in_valid_bits &&
           g_negotiated.in_float == g_sink.in_is_float &&
           g_negotiated.channel_mask == g_sink.channel_mask &&
           g_negotiated.channels == ch && g_negotiated.force_rate == g_force_rate &&
           g_negotiated.compat == g_compat && g_negotiated.want_bits == g_out_bits &&
           g_negotiated.src_prefer == g_src_prefer &&
           g_negotiated.hifi == g_hifi &&
           g_negotiated.endpoint_key == endpoint_key;
}

static void negotiated_remember(int ch, UINT64 endpoint_key, const NexAudioPolicyCandidate *c) {
    g_negotiated.valid    = 1;
    g_negotiated.in_rate  = g_sink.in_rate;
    g_negotiated.in_bits  = g_sink.in_is_float ? 32 : g_sink.in_container_bits;
    g_negotiated.in_valid = g_sink.in_valid_bits;
    g_negotiated.in_float = g_sink.in_is_float;
    g_negotiated.channel_mask = g_sink.channel_mask;
    g_negotiated.channels = ch;
    g_negotiated.force_rate = g_force_rate;
    g_negotiated.compat = g_compat;
    g_negotiated.endpoint_key = endpoint_key;
    g_negotiated.want_bits = g_out_bits;
    g_negotiated.src_prefer = g_src_prefer;
    g_negotiated.hifi = g_hifi;
    g_negotiated.out_rate = c->rate;
    g_negotiated.out_container = c->container_bits;
    g_negotiated.out_valid = c->valid_bits;
    g_negotiated.out_float = c->is_float;
}

static void negotiate_policy_input(NexAudioPolicyInput *policy) {
    memset(policy, 0, sizeof(*policy));
    policy->source_rate = g_sink.in_rate;
    policy->source_container_bits = g_sink.in_container_bits;
    policy->source_valid_bits = g_sink.in_valid_bits;
    policy->source_is_float = g_sink.in_is_float;
    policy->requested_output_bits = g_out_bits;
    policy->forced_rate = g_force_rate;
    policy->compatibility = g_compat;
    policy->hifi = g_hifi;
    policy->source_preference = g_src_prefer;
}

/* The remembered format first, then the plan. Separated from the device so the
   regression test can drive it with a probe of its own. */
static HRESULT negotiate_run(const NexAudioPolicyPlan *plan, NexAudioPolicyProbe probe,
                             void *context, int ch, UINT64 endpoint_key,
                             int *out_rate, int *out_container, int *out_valid,
                             int *out_float, int *tried) {
    NexAudioPolicySearch found;
    NexAudioPolicyCandidate chosen;
    int cached_busy = 0;
    int budget = g_probe_limit ? g_probe_limit : 64;
    int result;
    *tried = 0;

    /* Cache belongs to an endpoint and a policy, never just an input shape. */
    if (negotiated_matches(ch, endpoint_key)) {
        NexAudioPolicyCandidate cached;
        int r;
        cached.rate = g_negotiated.out_rate;
        cached.container_bits = g_negotiated.out_container;
        cached.valid_bits = g_negotiated.out_valid;
        cached.is_float = g_negotiated.out_float;
        r = probe(context, &cached);
        if (r != NEX_AUDIO_PROBE_SKIPPED) (*tried)++;
        if (r == NEX_AUDIO_PROBE_ACCEPTED) {
            *out_rate      = cached.rate;
            *out_container = cached.container_bits;
            *out_valid     = cached.valid_bits;
            *out_float     = cached.is_float;
            return S_OK;
        }
        cached_busy = (r == NEX_AUDIO_PROBE_BUSY);
    }

    result = nex_audio_policy_search(plan, probe, context, budget - *tried, cached_busy, &found);
    *tried += found.tried;
    if (result == NEX_AUDIO_SEARCH_LIMIT) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (result == NEX_AUDIO_SEARCH_BUSY) return AUDCLNT_E_DEVICE_IN_USE;
    if (result != NEX_AUDIO_SEARCH_FOUND) return E_FAIL;

    chosen = plan->items[found.index];
    *out_rate      = chosen.rate;
    *out_container = chosen.container_bits;
    *out_valid     = chosen.valid_bits;
    *out_float     = chosen.is_float;
    if (found.cacheable) {
        negotiated_remember(ch, endpoint_key, &chosen);
    } else {
        log_line("negotiate: %d Hz/%d.%d%s chosen while an earlier candidate was busy; "
                 "not remembered, the next stream starts from the source format",
                 chosen.rate, chosen.container_bits, chosen.valid_bits,
                 chosen.is_float ? "f" : "");
    }
    return S_OK;
}

static HRESULT negotiate(SinkGen *g, IMMDevice *dev, int ch, int *out_rate,
                         int *out_container, int *out_valid, int *out_float, int *tried) {
    NexAudioPolicyInput policy;
    NexAudioPolicyPlan plan;
    NegotiateProbe probe;
    int endpoint_mix_rate = 0;

    /* endpoint mix rate: candidate only */
    {
        IAudioClient *ac = NULL;
        WAVEFORMATEX *mixf = NULL;
        if (SUCCEEDED(dev_activate(dev, &IID_IAudioClient, (void **)&ac)) && ac) {
            if (SUCCEEDED(O_GETMIXFORMAT(ac, &mixf)) && mixf) {
                endpoint_mix_rate = (int)mixf->nSamplesPerSec;
                CoTaskMemFree(mixf);
            }
            ac->lpVtbl->Release(ac);
        }
    }

    negotiate_policy_input(&policy);
    nex_audio_policy_build_plan(&policy, endpoint_mix_rate, &plan);
    UINT64 endpoint_key=endpoint_hash(dev);
    if(!endpoint_key)return E_FAIL;
    *tried = 0;
    g_remote_bypass = 0;

    probe.gen = g;
    probe.dev = dev;
    probe.channels = ch;
    probe.last = E_FAIL;
    return negotiate_run(&plan, negotiate_probe, &probe, ch, endpoint_key,
                         out_rate, out_container, out_valid, out_float, tried);
}

static int channel_mask_channels(DWORD mask) {
    int n = 0;
    while (mask) { n += (int)(mask & 1u); mask >>= 1; }
    return n;
}

/* WAVEFORMATEX unpacking only; which layouts are legal is audio_policy.h's
   decision. A zero wValidBitsPerSample means "the whole container". */
static int valid_pcm_format(const WAVEFORMATEX *fmt) {
    if(!fmt)return 0;
    int fl=fmt->wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
    int valid=fmt->wBitsPerSample;
    if(fmt->wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
        if(fmt->cbSize<22)return 0;
        const WAVEFORMATEXTENSIBLE *we=(const WAVEFORMATEXTENSIBLE *)fmt;
        if(!IsEqualGUID(&we->SubFormat,&SUB_PCM) && !IsEqualGUID(&we->SubFormat,&SUB_FLOAT))return 0;
        if(we->Samples.wValidBitsPerSample)valid=we->Samples.wValidBitsPerSample;
        if(we->dwChannelMask && channel_mask_channels(we->dwChannelMask)!=fmt->nChannels)return 0;
        fl=IsEqualGUID(&we->SubFormat,&SUB_FLOAT);
    } else if(fmt->wFormatTag!=WAVE_FORMAT_PCM && !fl)return 0;
    return nex_audio_policy_format_valid((int)fmt->nSamplesPerSec,fmt->nChannels,
                                         fmt->wBitsPerSample,valid,fl,fmt->nBlockAlign);
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
static HRESULT sink_retarget_endpoint(const char *reason, int keep_bound);

/* 0 = off, SINK_DITHER_FORCED, or SINK_DITHER_AUTO (skips exact samples). */
static int sink_dither_mode(int in_float, int in_valid, int out_float, int out_valid,
                            int mode, int fade_ms) {
    if (!nex_audio_policy_dither_selected(in_float, in_valid, out_float, out_valid, mode, fade_ms))
        return 0;
    return mode == 1 ? SINK_DITHER_FORCED : SINK_DITHER_AUTO;
}

/* Move the live stream to whatever is now the default output (device removed). */
static HRESULT sink_retarget(const char *reason) {
    return sink_retarget_endpoint(reason, 0);
}

/* The output device chosen in the panel (hook.cfg endpoint=), AddRef'd, or
   NULL when none is set or it is not currently active - then the caller uses
   the client's own device, so an unplugged pick never silences playback. */
static IMMDevice *sink_pinned_device_q(int quiet) {
    if (!g_endpoint_id[0]) return NULL;
    WCHAR wid[200];
    if (!MultiByteToWideChar(CP_UTF8, 0, g_endpoint_id, -1, wid, 200)) return NULL;
    IMMDevice *dev = NULL;
    IMMDeviceEnumerator *e = NULL;
    InterlockedIncrement(&g_internal);
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &IID_IMMDeviceEnumerator, (void **)&e)) && e) {
        if (FAILED(e->lpVtbl->GetDevice(e, wid, &dev))) dev = NULL;
        e->lpVtbl->Release(e);
    }
    InterlockedDecrement(&g_internal);
    if (dev) {
        DWORD state = 0;
        if (FAILED(dev->lpVtbl->GetState(dev, &state)) || state != DEVICE_STATE_ACTIVE) {
            dev->lpVtbl->Release(dev);
            dev = NULL;
        }
    }
    if (!dev && !quiet)
        log_line("sink: chosen endpoint %s unavailable, using the client's device", g_endpoint_id);
    return dev;
}
static IMMDevice *sink_pinned_device(void) { return sink_pinned_device_q(0); }

/* keep_bound: rebuild on the endpoint the client bound through
   IMMDevice::Activate, falling back to the default when there is none. */
static HRESULT sink_retarget_endpoint(const char *reason, int keep_bound) {
    if (!g_sink.engaged || !g_sink.owner) return E_UNEXPECTED;

    /* Keep the client's clock where it was: the new device starts at zero. */
    UINT64 keep_clock = g_sink.clock_last;
    int was_remote = g_sink.gen && g_sink.gen->remote;

    /* The previous generation is already gone (sink_stop dropped it, or it was
       abandoned to its thread). Anything still here would be a bug. */
    if (g_sink.gen) { gen_release(g_sink.gen); g_sink.gen = NULL; }
    if (was_remote) remote_close();

    IMMDevice *dev = sink_resolve_device(keep_bound, 0);
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
    if (FAILED(sink_rebind_client_state(gen->id))) {
        dev->lpVtbl->Release(dev);
        sink_teardown();
        return E_INVALIDARG;
    }

    HRESULT why = S_OK;
    gen->endpoint_gate = acquire_endpoint_gate(dev, &why);
    if (!gen->endpoint_gate) {
        dev->lpVtbl->Release(dev);
        log_line("retarget(%s): endpoint gate unavailable 0x%08lX", reason, (unsigned long)why);
        sink_teardown();
        return why;
    }

    UINT64 key = endpoint_hash(dev);
    {
        int hw = -1;
        char name[192] = "";
        ep_describe(dev, name, sizeof(name), &hw);
        if (name[0]) snprintf(g_sink.ep_name, sizeof(g_sink.ep_name), "%s", name);
    }
    int rate = 0, cont = 0, valid = 0, fl = 0, tried = 0;
    if (sink_pick_output(dev, reason, g_sink.idle_released) == NEX_AUDIO_OUTPUT_SHARED) {
        hr = try_shared(gen, dev, g_sink.in_channels, &rate, &cont, &valid, &fl);
        tried = 1;
    } else {
        g_probe_limit = 12;
        hr = negotiate(gen, dev, g_sink.in_channels, &rate, &cont, &valid, &fl, &tried);
        g_probe_limit = 0;
    }
    dev->lpVtbl->Release(dev);
    if (FAILED(hr)) {
        log_line("retarget(%s): %d candidates rejected, stream cannot continue", reason, tried);
        sink_teardown();
        return hr;
    }
    log_line("retarget(%s): %d Hz/%d.%d bit%s after %d candidates output=%s",
             reason, rate, cont, valid, fl ? " float" : "", tried,
             gen->shared ? "shared" : "exclusive");

    g_sink.out_rate           = rate;
    g_sink.out_container_bits = cont;
    g_sink.out_valid_bits     = valid;
    g_sink.out_bits           = valid;
    g_sink.out_is_float       = fl;
    g_sink.out_block          = g_sink.in_channels * cont / 8;
    g_sink.resample           = (rate != g_sink.in_rate);

    /* Same rule as the first engage: depth reduction only, and only when the
       integer-domain opening ramp is off (see the note in sink_engage). */
    g_sink.dither = sink_dither_mode(g_sink.in_is_float, g_sink.in_valid_bits,
                                    fl, valid, g_dither, g_fade_ms);

    /* The resampler is rebuilt from scratch: its history belongs to the old
       device's clock. g_fout is sized from the new device's buffer. */
    if (g_sink.resample) {
        if (!src_init(g_sink.in_rate, rate, g_sink.in_channels)) { sink_teardown(); return E_OUTOFMEMORY; }
        audio_free(g_fout);
        g_fout = (double *)malloc((size_t)gen->real_frames * g_sink.in_channels * sizeof(double));
        audio_mem_lock(g_fout, (SIZE_T)gen->real_frames * g_sink.in_channels * sizeof(double));
        if (!g_fin) {
            g_fin = (double *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_channels * sizeof(double));
            audio_mem_lock(g_fin, (SIZE_T)SCRATCH_FRAMES * g_sink.in_channels * sizeof(double));
        }
        if (!g_fout || !g_fin) { sink_teardown(); return E_OUTOFMEMORY; }
    } else {
        src_free();
        audio_free(g_fout); g_fout = NULL;
        audio_free(g_fin);  g_fin  = NULL;
    }

    g_sink.clock_base   = keep_clock;
    if (gen->shared) { g_sink.skip_frames = 0; g_sink.prime_left = 0; }
    else sink_arm_prime(gen, rate);
    /* The byte verifier compares client and endpoint bytes; after a shared
       stream the system mixer sits in between, so it has nothing to verify. */
    if (gen->shared) g_sink.hifi_map = 0;
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

/* Rebuild the device side for the current client after sink_stop abandoned a
   generation whose drain thread was parked in GetBuffer. The client keeps its
   IAudioClient, ring, proxies and timeline; only the endpoint stream is new.
   It stays on the endpoint the client bound, as a pause would. */
static HRESULT sink_rebuild_device(const char *reason) {
    return sink_retarget_endpoint(reason, 1);
}
/* Indirection so the regression test can supply a mock device. */
static HRESULT (*g_rebuild_device)(const char *reason) = sink_rebuild_device;

static int sink_input_format_matches(const WAVEFORMATEX *fmt) {
    if (!fmt) return 0;
    int is_float = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    int valid_bits = fmt->wBitsPerSample;
    DWORD channel_mask = mask_for(fmt->nChannels);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *we = (const WAVEFORMATEXTENSIBLE *)fmt;
        is_float = (we->SubFormat.Data1 == 3);
        if (we->Samples.wValidBitsPerSample) valid_bits = we->Samples.wValidBitsPerSample;
        if (we->dwChannelMask) channel_mask = we->dwChannelMask;
    }
    return (int)fmt->nSamplesPerSec == g_sink.in_rate
        && fmt->nBlockAlign == g_sink.in_block
        && (int)fmt->nChannels == g_sink.in_channels
        && fmt->wBitsPerSample == g_sink.in_container_bits
        && valid_bits == g_sink.in_valid_bits
        && channel_mask == g_sink.channel_mask
        && is_float == g_sink.in_is_float;
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
            if (sink_input_format_matches(fmt)) {
                log_line("sink: re-initialize with the same format, keeping the sink");
                return S_OK;
            }
            log_line("sink: re-initialize format change self=%p owner=%p started=%d; rebuilding",
                     self, g_sink.owner, g_sink.started);
            HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
        } else if (!g_sink.started) {
            /* A new track usually comes with a fresh IAudioClient, and the old
               one has already been stopped by then. Release the exclusive
               stream first, otherwise the newcomer's own Initialize gets
               DEVICE_IN_USE and the client ends up with no working stream. */
            log_line("sink: new client while owner idle self=%p owner=%p; handing over",
                     self, g_sink.owner);
            HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
        } else {
            if (sink_input_format_matches(fmt)) {
                return sink_handoff_same_format(self);
            }
            /* A changed client contract still requires a fresh device side. */
            log_line("sink: active owner conflict self=%p owner=%p started=%d; format change -> teardown",
                     self, g_sink.owner, g_sink.started);
            HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
        }
    }

    if(g_sink.gen) {
        HRESULT clear=sink_teardown_controlled();if(FAILED(clear))return clear;
    }
    IMMDevice *dev = sink_pinned_device();
    EnterCriticalSection(&g_patch_lock);
    for(int i=0;!dev && i<128;i++)if(g_bindings[i].client==self && g_bindings[i].dev) {
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
    snprintf(g_sink.ep_name, sizeof(g_sink.ep_name), "%s", ep_name);
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

    int used_rate = 0, used_cont = 0, used_bits = 0, used_float = 0;
    HRESULT hr;

    int tried=0;
    if(!g_compat && g_force_rate && g_force_rate!=g_sink.in_rate) {
        dev->lpVtbl->Release(dev);sink_teardown_controlled();return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    if (sink_pick_output(dev, "new stream", 0) == NEX_AUDIO_OUTPUT_SHARED) {
        hr = try_shared(gen, dev, g_sink.in_channels, &used_rate, &used_cont, &used_bits, &used_float);
        tried = 1;
    } else {
        hr=negotiate(gen,dev,g_sink.in_channels,&used_rate,&used_cont,&used_bits,&used_float,&tried);
    }
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
    g_sink.dither = sink_dither_mode(g_sink.in_is_float, g_sink.in_valid_bits,
                                    used_float, used_bits, g_dither, g_fade_ms);

    if (g_sink.resample &&
        !src_init(g_sink.in_rate, used_rate, g_sink.in_channels)) {
        sink_teardown_controlled();
        return E_OUTOFMEMORY;
    }
    scratch_free();
    g_inb=(BYTE *)malloc((size_t)SCRATCH_FRAMES*g_sink.in_block);
    if (g_sink.resample) {
        g_fin=(double *)malloc((size_t)SCRATCH_FRAMES*g_sink.in_channels*sizeof(double));
        g_fout=(double *)malloc((size_t)gen->real_frames*g_sink.in_channels*sizeof(double));
    }
    if(!g_inb || (g_sink.resample && (!g_fin || !g_fout))) {
        sink_teardown_controlled();return E_OUTOFMEMORY;
    }
    audio_mem_lock(g_inb, (SIZE_T)SCRATCH_FRAMES*g_sink.in_block);
    if (g_sink.resample) {
        audio_mem_lock(g_fin, (SIZE_T)SCRATCH_FRAMES*g_sink.in_channels*sizeof(double));
        audio_mem_lock(g_fout, (SIZE_T)gen->real_frames*g_sink.in_channels*sizeof(double));
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
    want = event_safe_client_frames(want, gen);
    g_sink.cap = want * 8;
    if (g_sink.cap < 16384) g_sink.cap = 16384;

    g_sink.ring = (BYTE *)calloc((size_t)g_sink.cap, (size_t)g_sink.in_block);
    g_sink.conv = (BYTE *)malloc((size_t)g_sink.cap*g_sink.in_block);
    if (!g_sink.ring || !g_sink.conv) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    audio_mem_lock(g_sink.ring, (SIZE_T)g_sink.cap * g_sink.in_block);
    audio_mem_lock(g_sink.conv, (SIZE_T)g_sink.cap * g_sink.in_block);
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
    g_sink.proxy_render->staging_bytes = (size_t)want * g_sink.in_block;
    if (!g_sink.proxy_render->staging) { sink_teardown_controlled(); return E_OUTOFMEMORY; }
    g_sink.proxy_clock->vtbl = g_fake_clock_vtbl;
    g_sink.proxy_clock->ref = 1;
    g_sink.proxy_clock->generation = g_sink.generation;

    g_sink.engaged = 1;
    g_sink.owner = (void *)self;
    g_sink.config_epoch = g_cfg_policy_epoch;
    g_sink.config_file_epoch = g_cfg_epoch;
    g_sink.config_enable = g_enable;
    g_sink.config_frames = g_out_frames;
    g_sink.config_audiohost = g_audiohost;
    ((IUnknown *)self)->lpVtbl->AddRef((IUnknown *)self); /* the sink outlives the caller's handle */
    g_sink.reported_frames = want;
    /* A rebuilt stream starts its own timeline; the watchdog uses the key to
       tell "the default moved" from "we are already on it". */
    g_sink.endpoint_key = key;
    g_sink.clock_base   = 0;
    g_sink.device_lost  = 0;
    if (gen->shared) { g_sink.skip_frames = 0; g_sink.prime_left = 0; }
    else sink_arm_prime(gen, used_rate);

    char a[160], b[240];
    describe("in", fmt, a, sizeof(a));
    /* "native" means the stream we opened is exactly what the client produces:
       same rate, same container, same precision, same sample type, identical
       bytes. "repacked" keeps every value but changes the container width
       (16-in-32 -> 16/16); the byte verifier only applies to native. An
       opening fade changes the first samples of each start and is shown in
       the tag rather than hidden behind [native]. */
    int path = nex_audio_policy_path_kind(g_sink.in_rate, g_sink.in_container_bits,
                                          g_sink.in_valid_bits, g_sink.in_is_float,
                                          used_rate, used_cont, used_bits, used_float);
    int native = path == NEX_AUDIO_PATH_NATIVE;
    g_sink.hifi_map = g_hifi && !gen->shared ? (native ? 1 : 2) : 0;
    g_sink.hifi_first_mismatch_frame = ~(UINT64)0;
    g_sink.hifi_client_hash = 1469598103934665603ull;
    g_sink.hifi_endpoint_hash = 1469598103934665603ull;
    snprintf(b, sizeof(b), "out=rate:%d/cont:%d.%d/ch:%d%s hifi=%d map=%s  %s engine=%s output=%s",
             used_rate, used_cont, used_bits, g_sink.in_channels,
             used_float ? "/float" : "",
             g_hifi,
             g_sink.hifi_map ? (native ? "client-native" : "converted") : "off",
             gen->shared ? "[shared]" : nex_audio_policy_path_tag(path, g_fade_ms),
             gen->remote ? "audiohost" : "in-process",
             gen->shared ? "shared" : "exclusive");
    log_line("sink ENGAGED  generation=%llu  %s  ->  %s  (client buffer %lu frames, ring %lu)  ep=\"%s\" hw=%d",
             (unsigned long long)g_sink.generation,
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

