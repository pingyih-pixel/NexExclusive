/* Regression tests exercise the actual DLL implementation, without opening
   a sound device, starting a player, or injecting into any process.

   The suite deliberately drives the same entry points the client drives:
   the IAudioClient hooks, the proxy IAudioRenderClient, and the IAudioClock
   shim. Anything that can only be checked with a real endpoint (exclusive
   format acceptance, actual underrun behaviour) stays out of here. */
#include "../src/nexsink.c"

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static void store24(BYTE *p, int v) {
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16);
}
static int load24(const BYTE *p) {
    int v = p[0] | (p[1] << 8) | (p[2] << 16);
    return (v & 0x800000) ? v | ~0xFFFFFF : v;
}

/* Register a vtable's real entry points without installing our hooks. Used by
   the tests that need drain_once() to call a mock endpoint. */
static void reset_patches(void) {
    EnterCriticalSection(&g_patch_lock);
    memset(g_audio_patches, 0, sizeof(g_audio_patches));
    g_audio_patch_count = 0;
    g_ac_vtbl = NULL;
    memset(g_ac_orig, 0, sizeof(g_ac_orig));
    LeaveCriticalSection(&g_patch_lock);
}

static void register_originals(void **vt, void **orig) {
    EnterCriticalSection(&g_patch_lock);
    AudioPatch *p = &g_audio_patches[g_audio_patch_count++];
    memset(p, 0, sizeof(*p));
    p->vt = vt;
    for (int i = 0; i < 24; i++) p->orig[i] = orig[i];
    LeaveCriticalSection(&g_patch_lock);
}

/* ---- fade --------------------------------------------------------------- */
static void test_fade(void) {
    const UINT32 frames = 480;
    BYTE data[480 * 2 * 4];
    int samples = 0;
    for (int mode = 0; mode < 4; mode++) {
        int bits = mode == 0 ? 16 : mode == 1 ? 24 : 32;
        int is_float = mode == 3;
        int bytes = bits / 8;
        g_sink.in_channels = 2;
        g_sink.out_bits = bits;
        g_sink.out_is_float = is_float;
        g_sink.fade_total = g_sink.fade_left = frames;
        for (UINT32 f = 0; f < frames; f++) {
            for (int c = 0; c < 2; c++) {
                BYTE *p = data + ((size_t)f * 2 + c) * bytes;
                int sign = c ? -1 : 1;
                if (is_float) { float x = sign * 0.75f; memcpy(p, &x, 4); }
                else if (bits == 16) { short x = (short)(sign * 24576); memcpy(p, &x, 2); }
                else if (bits == 24) store24(p, sign * 6291456);
                else { int x = sign * 1610612736; memcpy(p, &x, 4); }
            }
        }
        apply_fade(data, frames);
        CHECK(g_sink.fade_left == 0);
        for (UINT32 f = 0; f < frames; f++) {
            int gain = (int)(((UINT64)f * 65536) / frames);
            for (int c = 0; c < 2; c++) {
                BYTE *p = data + ((size_t)f * 2 + c) * bytes;
                int sign = c ? -1 : 1;
                if (is_float) {
                    float x; memcpy(&x, p, 4);
                    CHECK(fabs((double)x - sign * 0.75 * gain / 65536.0) < 1e-7);
                } else {
                    int full = bits == 16 ? 24576 : bits == 24 ? 6291456 : 1610612736;
                    int expected = (int)(((INT64)(sign * full) * gain) >> 16);
                    int actual;
                    if (bits == 16) { short x; memcpy(&x, p, 2); actual = x; }
                    else if (bits == 24) actual = load24(p);
                    else memcpy(&actual, p, 4);
                    CHECK(actual == expected);
                }
                samples++;
            }
        }
    }
    /* A deterministic example of the old 32-bit wrap, without invoking UB. */
    UINT32 old_product = (UINT32)6291456 * (UINT32)32768;
    int old_result = (int)old_product >> 16;
    int new_result = (int)(((INT64)6291456 * 32768) >> 16);
    CHECK(old_result == 0);
    CHECK(new_result == 3145728);
    g_sink.fade_left = g_sink.fade_total = 0;
    printf("PASS fade: %d stereo sample checks (16/24/32/float), old midpoint=%d, corrected=%d\n",
           samples, old_result, new_result);
}

static void test_valid_bits_alignment(void) {
    BYTE packed24[3], packed32[4];
    sample_write(0.5, packed24, 24, 16, 0, 0);
    CHECK(packed24[0] == 0 && packed24[1] == 0 && packed24[2] == 0x40);
    CHECK(fabs(sample_read(packed24, 24, 16, 0) - 0.5) < 1e-9);
    sample_write(-0.5, packed32, 32, 16, 0, 0);
    CHECK(packed32[0] == 0 && packed32[1] == 0 &&
          packed32[2] == 0x00 && packed32[3] == 0xC0);
    CHECK(fabs(sample_read(packed32, 32, 16, 0) + 0.5) < 1e-9);
    printf("PASS valid bits alignment: 16-bit samples stay left-aligned in 24/32-bit containers\n");
}

/* ---- ring + proxy ------------------------------------------------------- */
static void prepare_ring(void) {
    memset(&g_sink, 0, sizeof(g_sink));
    g_sink.engaged = 1;
    g_sink.owner = (void *)(UINT_PTR)1;
    g_sink.cap = 32;
    g_sink.reported_frames = 16;
    g_sink.in_channels = 2;
    g_sink.in_block = 8;            /* 2 ch * 32-bit container */
    g_sink.in_container_bits = 32;
    g_sink.in_valid_bits = 32;
    g_sink.out_bits = 32;
    g_sink.generation = 7;
    g_sink.config_epoch = g_cfg_policy_epoch;
    g_sink.config_file_epoch = g_cfg_epoch;
    g_sink.config_enable = g_enable;
    g_sink.config_frames = g_out_frames;
    g_sink.config_audiohost = g_audiohost;
    g_sink.ring = (BYTE *)malloc(32 * 8);
    g_sink.conv = (BYTE *)malloc(32 * 8);
    memset(g_sink.ring, 0x5A, 32 * 8);
    g_sink.h_data = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_sink.h_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    memset(&g_fake_render, 0, sizeof(g_fake_render));
    g_fake_render.vtbl = g_fake_render_vtbl;
    g_fake_render.ref = 1;
    g_fake_render.generation = g_sink.generation;
    g_fake_render.capacity = g_sink.reported_frames;
    g_fake_render.staging = (BYTE *)malloc((size_t)g_fake_render.capacity * g_sink.in_block);
    g_fake_render.staging_bytes = (size_t)g_fake_render.capacity * g_sink.in_block;
    g_sink.proxy_render = &g_fake_render;
}
static void free_ring(void) {
    free(g_sink.ring);
    free(g_sink.conv);
    free(g_fake_render.staging);
    CloseHandle(g_sink.h_data);
    CloseHandle(g_sink.h_space);
    memset(&g_sink, 0, sizeof(g_sink));
    memset(&g_fake_render, 0, sizeof(g_fake_render));
}

/* The device side is a reference-counted generation; a test that drives the
   drain thread needs one installed in the sink, exactly as engage does. */
static SinkGen *mock_gen(void) {
    SinkGen *g = gen_new();
    if (!g) { fprintf(stderr, "FAIL: gen_new returned NULL\n"); ExitProcess(1); }
    g_sink.gen = g;
    return g;
}

static void test_silent(void) {
    prepare_ring();
    BYTE *p = NULL;
    CHECK(fr_GetBuffer(NULL, 4, &p) == S_OK);
    CHECK(p == g_fake_render.staging);
    BYTE *other = NULL;
    CHECK(fr_GetBuffer(NULL, 4, &other) == AUDCLNT_E_OUT_OF_ORDER);
    /* The client's buffer is staging, not the ring: writing SILENT must clear
       the frames that actually reach the device, not the scratch copy. */
    CHECK(fr_ReleaseBuffer(NULL, 4, AUDCLNT_BUFFERFLAGS_SILENT) == S_OK);
    for (int i = 0; i < 4 * 8; i++) CHECK(g_sink.ring[i] == 0);
    CHECK(g_sink.ring[4 * 8] == 0x5A);
    CHECK(g_sink.w == 4 && g_fake_render.reserved == 0);
    CHECK(fr_ReleaseBuffer(NULL, 1, 0) == AUDCLNT_E_OUT_OF_ORDER);
    CHECK(fr_GetBuffer(NULL, 4, &p) == S_OK);
    CHECK(fr_ReleaseBuffer(NULL, 5, 0) == AUDCLNT_E_INVALID_SIZE);
    CHECK(g_sink.w == 4 && g_fake_render.reserved == 4);
    CHECK(fr_ReleaseBuffer(NULL, 0, 0) == S_OK);
    CHECK(g_sink.w == 4 && g_fake_render.reserved == 0);
    free_ring();
    printf("PASS silent: recycled samples cleared, bounds/order/cancellation checked\n");
}

typedef struct {
    HANDLE entered;
} RingLockThread;
static DWORD WINAPI ring_lock_holder(LPVOID arg) {
    RingLockThread *ctx = (RingLockThread *)arg;
    EnterCriticalSection(&g_lock);
    SetEvent(ctx->entered);
    Sleep(50);
    LeaveCriticalSection(&g_lock);
    return 0;
}

/* A short overlap with the drain's ring copy must not turn a valid client
   packet into DEVICE_INVALIDATED. The audio thread may wait for this tiny
   critical section; dropping the packet starts the same-format rebuild loop
   seen in the live client. */
static void test_ring_reserve_waits_for_drain(void) {
    prepare_ring();
    RingLockThread ctx = { CreateEventA(NULL, TRUE, FALSE, NULL) };
    HANDLE thread = CreateThread(NULL, 0, ring_lock_holder, &ctx, 0, NULL);
    CHECK(thread != NULL);
    if (thread) {
        CHECK(WaitForSingleObject(ctx.entered, 1000) == WAIT_OBJECT_0);
        BYTE *p = ring_reserve(4);
        CHECK(p != NULL);
        CHECK(WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0);
        CloseHandle(thread);
    }
    CloseHandle(ctx.entered);
    free_ring();
    printf("PASS ring_reserve: transient drain lock does not drop a client packet\n");
}

/* A stale proxy from a previous stream must not be able to write into the new
   ring, even though the pointer is still valid memory. */
static void test_stale_proxy(void) {
    prepare_ring();
    g_fake_render.generation = g_sink.generation - 1;
    BYTE *p = (BYTE *)(UINT_PTR)0x1234;
    CHECK(fr_GetBuffer(NULL, 4, &p) == AUDCLNT_E_DEVICE_INVALIDATED);
    CHECK(p == NULL);
    CHECK(fr_ReleaseBuffer(NULL, 4, 0) == AUDCLNT_E_DEVICE_INVALIDATED);
    CHECK(g_sink.w == 0);
    free_ring();
    printf("PASS stale proxy: previous stream's handle rejected after rebuild\n");
}

/* A policy edit may arrive together with a client-contract or engine edit.
   Those latter changes still belong to the next Initialize, so the reload
   path must defer the whole combination instead of rebuilding mid-stream. */
static void test_config_reload_defer_matrix(void) {
    CHECK(config_reload_should_defer(0, 0, 0) == 0);
    CHECK(config_reload_should_defer(0, 1, 0) == 1);
    CHECK(config_reload_should_defer(0, 0, 1) == 1);
    CHECK(config_reload_should_defer(1, 0, 0) == 0);
    CHECK(config_reload_should_defer(1, 1, 0) == 1);
    CHECK(config_reload_should_defer(1, 0, 1) == 1);
    printf("PASS config reload: contract and engine changes defer even with policy changes\n");
}

/* A policy rebuild replaces the device generation while the client keeps its
   IAudioRenderClient, IAudioClock and event handle. The public proxy pointer
   must remain usable after the generation token is refreshed. */
static void test_policy_rebuild_preserves_client_state(void) {
    prepare_ring();
    HANDLE client_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(client_event != NULL);
    g_sink.owner = (void *)(UINT_PTR)0x1234;
    g_sink.h_client = client_event;
    FakeObj *render = g_sink.proxy_render;
    BYTE *staging = render->staging;
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    UINT64 next_generation = g_sink.generation + 1;
    CHECK(sink_rebind_client_state(next_generation) == S_OK);
    CHECK(g_sink.owner == (void *)(UINT_PTR)0x1234);
    CHECK(g_sink.h_client == client_event);
    CHECK(g_sink.proxy_render == render);
    CHECK(g_sink.proxy_render->generation == next_generation);
    CHECK(g_sink.proxy_render->capacity == g_sink.reported_frames);
    CHECK(g_sink.proxy_render->staging == staging);
    BYTE *p = NULL;
    CHECK(fr_GetBuffer(render, 4, &p) == S_OK);
    CHECK(p == staging);
    CHECK(fr_ReleaseBuffer(render, 0, 0) == S_OK);
    CloseHandle(client_event);
    g_sink.h_client = NULL;
    free_ring();
    printf("PASS policy rebuild: client owner, event and proxy identity survive generation refresh\n");
}

/* NetEase can create the next IAudioClient while the previous same-format
   client is still submitting.  The device side must stay alive across that
   client handoff; tearing it down here produces an audible gap. */
typedef struct {
    void **vtbl;
    LONG refs;
} HandoffClient;
static ULONG STDMETHODCALLTYPE handoff_addref(void *self) {
    return (ULONG)InterlockedIncrement(&((HandoffClient *)self)->refs);
}
static ULONG STDMETHODCALLTYPE handoff_release(void *self) {
    return (ULONG)InterlockedDecrement(&((HandoffClient *)self)->refs);
}
static void *handoff_vtbl[3] = { NULL, (void *)handoff_addref, (void *)handoff_release };

static void test_same_format_stream_handoff(void) {
    prepare_ring();
    load_config();
    g_sink.config_epoch = g_cfg_policy_epoch;
    g_sink.config_file_epoch = g_cfg_epoch;
    g_sink.config_enable = g_enable;
    g_sink.config_frames = g_out_frames;
    g_sink.config_audiohost = g_audiohost;
    SinkGen *gen = mock_gen();
    HandoffClient old_client = { handoff_vtbl, 1 };
    HandoffClient new_client = { handoff_vtbl, 1 };
    g_sink.owner = &old_client;
    g_sink.started = 1;
    g_sink.in_rate = 44100;
    g_sink.in_channels = 2;
    g_sink.in_block = 8;
    g_sink.in_container_bits = 32;
    g_sink.in_valid_bits = 32;
    g_sink.channel_mask = mask_for(2);
    g_sink.in_is_float = 0;
    FakeObj *old_proxy = g_sink.proxy_render;
    fr_AddRef(old_proxy);
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    BYTE *pending = NULL;
    CHECK(fr_GetBuffer(old_proxy, 1, &pending) == S_OK);
    if (pending) memset(pending, 0xA7, g_sink.in_block);
    WAVEFORMATEXTENSIBLE fmt;
    build_fmt_ex(&fmt, 44100, 32, 0, 2, mask_for(2));

    HRESULT hr = sink_engage_controlled((IAudioClient *)&new_client,
                                        &fmt.Format, 300000, NULL);
    CHECK(hr == S_OK);
    CHECK(g_sink.engaged);
    CHECK(g_sink.gen == gen);
    CHECK(g_sink.owner == &new_client);
    /* Each IAudioClient needs an independent staging buffer. The device
       generation stays shared during a same-format handoff, so the previous
       client may finish its outstanding packet and submit another packet
       while the successor is already active. */
    CHECK(g_sink.proxy_render != old_proxy);
    FakeObj *new_proxy = g_sink.proxy_render;
    CHECK(g_sink.started == 1);
    CHECK(H_Stop(&new_client) == S_FALSE);
    CHECK(g_sink.started == 1);
    CHECK(H_Start(&new_client) == S_OK);
    CHECK(g_sink.started == 1);
    CHECK(old_client.refs == 0);
    CHECK(new_client.refs == 2);
    BYTE *successor = NULL;
    CHECK(fr_GetBuffer(new_proxy, 1, &successor) == S_OK);
    CHECK(successor != NULL && successor != pending);
    memset(successor, 0x5A, g_sink.in_block);
    CHECK(fr_ReleaseBuffer(new_proxy, 1, 0) == S_OK);
    CHECK(fr_ReleaseBuffer(old_proxy, 1, 0) == S_OK);
    CHECK(g_sink.w == 2);
    BYTE *continued = NULL;
    HRESULT continued_hr = fr_GetBuffer(old_proxy, 1, &continued);
    CHECK(continued_hr == S_OK);
    if (continued_hr == S_OK && continued) {
        memset(continued, 0x3C, g_sink.in_block);
        CHECK(fr_ReleaseBuffer(old_proxy, 1, 0) == S_OK);
        CHECK(g_sink.w == 3);
    }
    fr_Release(old_proxy);

    sink_teardown_controlled();
    CHECK(new_client.refs == 1);
    printf("PASS same-format handoff: device generation, playback state and pending client data survive a new client\n");
}

/* The occupied span wrapping past cap must survive compaction: the naive
   two-copy version overwrote the head it still needed to read. */
static void test_ring_compact_wrap(void) {
    prepare_ring();
    g_sink.cap = 8;
    g_sink.w = 11;          /* 3 frames of history already consumed */
    g_sink.r = 7;           /* rmod = 7, used = 4 -> spans 7,0,1,2 */
    for (int i = 0; i < 8; i++) {
        BYTE *f = g_sink.ring + (size_t)i * g_sink.in_block;
        for (int b = 0; b < g_sink.in_block; b++) f[b] = (BYTE)(0x10 + i);
    }
    ring_compact();
    CHECK(g_sink.r == 0 && g_sink.w == 4);
    for (int f = 0; f < 4; f++) {
        BYTE expect = (BYTE)(0x10 + ((7 + f) % 8));
        for (int b = 0; b < g_sink.in_block; b++)
            CHECK(g_sink.ring[(size_t)f * g_sink.in_block + b] == expect);
    }
    CHECK(g_sink.ring[(size_t)4 * g_sink.in_block] == 0x10 + 4); /* untouched tail */
    free_ring();
    printf("PASS ring_compact: wrapped span preserved across the tail boundary\n");
}

static void test_ring_reserve_limit(void) {
    prepare_ring();
    CHECK(ring_reserve(33) == NULL);          /* larger than the ring */
    CHECK(ring_reserve(32) != NULL);          /* exactly the ring, at offset 0 */
    free_ring();
    printf("PASS ring_reserve: impossible reservations fail instead of spinning\n");
}

/* ---- clock -------------------------------------------------------------- */
static UINT64 mock_clock_pos;
static HRESULT STDMETHODCALLTYPE mock_get_position(void *s, UINT64 *pos, UINT64 *qpc) {
    (void)s;
    if (pos) *pos = mock_clock_pos;
    if (qpc) *qpc = 4242;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE mock_get_frequency(void *s, UINT64 *f) {
    (void)s;
    if (f) *f = 48000;
    return S_OK;
}
static void *mock_clock_vtbl[6] = {
    NULL, NULL, NULL,
    (void *)mock_get_frequency, (void *)mock_get_position, NULL,
};
static void *mock_clock_obj[1];

static void test_clock(void) {
    prepare_ring();
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    mock_clock_obj[0] = mock_clock_vtbl;      /* COM object: first word is the vtable */
    mock_gen()->real_clock = (IAudioClock *)mock_clock_obj;
    g_sink.gen->clock_frequency = 48000;

    UINT64 pos = 0, qpc = 0;
    mock_clock_pos = 0;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 0 && qpc == 4242);

    mock_clock_pos = 24000;                  /* half a second */
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 5000000ULL);

    mock_clock_pos = 48000;                  /* exactly one second */
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 10000000ULL);

    /* The value is in frames, not 100 ns ticks: a raw pass-through of 48001
       would report 48001 here instead of ~10000208. */
    mock_clock_pos = 48001;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 10000208ULL);

    /* The client compares positions; a value that goes backwards makes it
       discard its buffer and stutter. */
    mock_clock_pos = 0;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 10000208ULL);
    CHECK(qpc == 4242);

    /* A rebuilt stream starts its own monotonic sequence. */
    g_sink.clock_last = 0;
    mock_clock_pos = 1;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 208ULL);                    /* 1/48000 s, truncated */

    UINT64 freq = 0;
    CHECK(fc_GetFrequency(NULL, &freq) == S_OK && freq == 10000000ULL);
    free_ring();
    printf("PASS clock: frames converted to 100 ns units, monotonic across resets\n");
}

/* ---- format validation -------------------------------------------------- */
static void test_valid_format(void) {
    WAVEFORMATEXTENSIBLE we;
    build_fmt_ex(&we, 44100, 16, 0, 2, mask_for(2));
    CHECK(valid_pcm_format((WAVEFORMATEX *)&we));
    build_fmt_ex(&we, 192000, 24, 0, 2, mask_for(2));
    CHECK(valid_pcm_format((WAVEFORMATEX *)&we));
    build_fmt_ex(&we, 96000, 32, 1, 2, mask_for(2));
    CHECK(valid_pcm_format((WAVEFORMATEX *)&we));
    build_fmt_cv(&we, 96000, 32, 24, 0, 6, 0x3F);
    CHECK(we.Format.nChannels == 6);
    CHECK(we.Samples.wValidBitsPerSample == 24);
    CHECK(we.dwChannelMask == 0x3F);
    CHECK(valid_pcm_format((WAVEFORMATEX *)&we));

    build_fmt_ex(&we, 44100, 16, 0, 2, mask_for(2));
    we.Format.wFormatTag = WAVE_FORMAT_ALAW;
    CHECK(!valid_pcm_format((WAVEFORMATEX *)&we));

    build_fmt_ex(&we, 44100, 16, 0, 2, mask_for(2));
    we.Format.nBlockAlign = 6;               /* inconsistent with channels/bits */
    CHECK(!valid_pcm_format((WAVEFORMATEX *)&we));

    build_fmt_ex(&we, 44100, 16, 0, 2, mask_for(2));
    we.Samples.wValidBitsPerSample = 20;     /* more valid than container */
    CHECK(!valid_pcm_format((WAVEFORMATEX *)&we));

    build_fmt_ex(&we, 0, 16, 0, 2, mask_for(2));
    CHECK(!valid_pcm_format((WAVEFORMATEX *)&we));
    CHECK(!valid_pcm_format(NULL));

    build_fmt_ex(&we, 44100, 16, 0, 2, mask_for(2));
    we.Format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; /* float must be 32-bit */
    CHECK(!valid_pcm_format((WAVEFORMATEX *)&we));
    build_fmt_ex(&we, 44100, 16, 0, 2, mask_for(2));
    we.dwChannelMask = 0x4;                 /* one speaker bit for two channels */
    CHECK(!valid_pcm_format((WAVEFORMATEX *)&we));

    build_fmt_ex(&we, 44100, 24, 0, 2, mask_for(2));
    we.Samples.wValidBitsPerSample = 20;
    CHECK(valid_pcm_format((WAVEFORMATEX *)&we));
    printf("PASS valid_pcm_format: validates valid bits and channel-mask geometry\n");
}

/* ---- negotiation: occupied is not unsupported ----------------------------- */
typedef struct {
    int busy_rate;        /* this rate answers DEVICE_IN_USE while busy_left > 0 */
    int busy_left;
    int accept_rate[2], accept_container[2], accept_valid[2];
    int calls;
} PolicyEndpoint;

static int policy_endpoint_probe(void *context, const NexAudioPolicyCandidate *c) {
    PolicyEndpoint *e = (PolicyEndpoint *)context;
    e->calls++;
    if (e->busy_rate == c->rate && e->busy_left > 0) {
        e->busy_left--;
        return nex_audio_policy_classify_failure((unsigned long)AUDCLNT_E_DEVICE_IN_USE);
    }
    for (int i = 0; i < 2; i++) {
        if (e->accept_rate[i] == c->rate && e->accept_container[i] == c->container_bits &&
            e->accept_valid[i] == c->valid_bits && !c->is_float)
            return NEX_AUDIO_PROBE_ACCEPTED;
    }
    return nex_audio_policy_classify_failure((unsigned long)AUDCLNT_E_UNSUPPORTED_FORMAT);
}

static void test_negotiate_busy_is_not_remembered(void) {
    int save_bits = g_out_bits, save_rate = g_force_rate, save_compat = g_compat;
    int save_hifi = g_hifi, save_prefer = g_src_prefer;
    NexAudioPolicyInput policy;
    NexAudioPolicyPlan plan;
    int rate = 0, cont = 0, valid = 0, fl = 0, tried = 0;
    const UINT64 key = 0x1234;

    memset(&g_sink, 0, sizeof(g_sink));
    memset(&g_negotiated, 0, sizeof(g_negotiated));
    g_out_bits = 24; g_force_rate = 0; g_compat = 1; g_hifi = 0; g_src_prefer = 0;
    g_sink.in_rate = 44100;
    g_sink.in_container_bits = 16;
    g_sink.in_valid_bits = 16;
    g_sink.in_channels = 2;
    g_sink.channel_mask = mask_for(2);
    negotiate_policy_input(&policy);
    nex_audio_policy_build_plan(&policy, 48000, &plan);

    /* The endpoint takes 44.1/16, but the previous stream holds it for the
       whole search. Playback continues at 48 kHz; nothing is remembered. */
    PolicyEndpoint held = {44100, 1000, {44100, 48000}, {16, 24}, {16, 24}, 0};
    CHECK(negotiate_run(&plan, policy_endpoint_probe, &held, 2, key,
                        &rate, &cont, &valid, &fl, &tried) == S_OK);
    CHECK(rate == 48000 && cont == 24 && valid == 24);
    CHECK(!g_negotiated.valid);

    /* Next stream, endpoint free: the source format is back and is remembered. */
    PolicyEndpoint idle = {0, 0, {44100, 48000}, {16, 24}, {16, 24}, 0};
    CHECK(negotiate_run(&plan, policy_endpoint_probe, &idle, 2, key,
                        &rate, &cont, &valid, &fl, &tried) == S_OK);
    CHECK(rate == 44100 && cont == 16 && valid == 16 && tried == 1);
    CHECK(g_negotiated.valid && g_negotiated.out_rate == 44100);

    /* Held again: the remembered 44.1 kHz entry survives the detour. */
    held.busy_left = 1000; held.calls = 0;
    CHECK(negotiate_run(&plan, policy_endpoint_probe, &held, 2, key,
                        &rate, &cont, &valid, &fl, &tried) == S_OK);
    CHECK(rate == 48000);
    CHECK(g_negotiated.valid && g_negotiated.out_rate == 44100);

    /* Released during the search: the source rate is retried before converting. */
    memset(&g_negotiated, 0, sizeof(g_negotiated));
    held.busy_left = 2; held.calls = 0;
    CHECK(negotiate_run(&plan, policy_endpoint_probe, &held, 2, key,
                        &rate, &cont, &valid, &fl, &tried) == S_OK);
    CHECK(rate == 44100 && cont == 16 && valid == 16);
    CHECK(g_negotiated.valid && g_negotiated.out_rate == 44100);

    /* A real refusal of the source rate is a conversion worth remembering. */
    memset(&g_negotiated, 0, sizeof(g_negotiated));
    PolicyEndpoint only48 = {0, 0, {48000, 0}, {24, 0}, {24, 0}, 0};
    CHECK(negotiate_run(&plan, policy_endpoint_probe, &only48, 2, key,
                        &rate, &cont, &valid, &fl, &tried) == S_OK);
    CHECK(rate == 48000 && g_negotiated.valid && g_negotiated.out_rate == 48000);

    /* Strict native with the endpoint held reports occupied, not unsupported. */
    memset(&g_negotiated, 0, sizeof(g_negotiated));
    g_hifi = 1; g_compat = 0;
    negotiate_policy_input(&policy);
    nex_audio_policy_build_plan(&policy, 48000, &plan);
    held.busy_left = 1000;
    CHECK(negotiate_run(&plan, policy_endpoint_probe, &held, 2, key,
                        &rate, &cont, &valid, &fl, &tried) == AUDCLNT_E_DEVICE_IN_USE);
    PolicyEndpoint none = {0, 0, {0, 0}, {0, 0}, {0, 0}, 0};
    CHECK(negotiate_run(&plan, policy_endpoint_probe, &none, 2, key,
                        &rate, &cont, &valid, &fl, &tried) == E_FAIL);

    memset(&g_sink, 0, sizeof(g_sink));
    memset(&g_negotiated, 0, sizeof(g_negotiated));
    g_out_bits = save_bits; g_force_rate = save_rate; g_compat = save_compat;
    g_hifi = save_hifi; g_src_prefer = save_prefer;
    printf("PASS negotiate: an occupied endpoint neither settles nor poisons the remembered format\n");
}

static void test_config_values_are_normalized_by_policy(void) {
    WCHAR temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) { printf("SKIP config policy: no temp path\n"); return; }
    WCHAR path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsNexExclusive-policy-%lu.cfg", temp,
               (unsigned long)GetCurrentProcessId());
    FILE *f = _wfopen(path, L"wb");
    if (!f) { printf("SKIP config policy: cannot create config\n"); return; }
    fputs("hifi=0\ncompat=1\nbits=20\nrate=5\nbuffer=99999\ndither=9\nfade_ms=500\n", f);
    fclose(f);
    char utf8[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
    _putenv_s("NEX_CFG", utf8);
    g_cfg_loaded = 0;
    load_config();
    NexAudioPolicyConfig d = nex_audio_policy_config_defaults();
    CHECK(g_out_bits == d.out_bits);
    CHECK(g_force_rate == 0);            /* 5 Hz is not a rate */
    CHECK(g_out_frames == d.out_frames);
    CHECK(g_dither == d.dither);
    CHECK(g_fade_ms == d.fade_ms);
    DeleteFileW(path);
    _putenv_s("NEX_CFG", "");
    g_cfg_loaded = 0;
    printf("PASS config: out-of-range values fall back to the policy defaults\n");
}

/* The remote wire check stays in nexaudiolink.h; it must not drift from the
   policy's legal range. */
static void test_policy_range_matches_wire_range(void) {
    const int rates[] = {7999, 8000, 44100, 384000, 384001};
    const int chans[] = {0, 1, 8, 9};
    const int conts[] = {8, 16, 24, 32, 40};
    for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) for (int b = 0; b < 5; b++)
        for (int v = 0; v <= 40; v += 8) for (int fl = 0; fl < 2; fl++) {
            int policy = nex_audio_policy_format_valid(rates[r], chans[c], conts[b], v, fl,
                                                       chans[c] * (conts[b] / 8));
            int wire = nex_audio_encoding_layout_valid((uint32_t)rates[r], (uint16_t)chans[c],
                           (uint16_t)conts[b], (uint16_t)v,
                           fl ? NEX_AUDIO_ENCODING_FLOAT : NEX_AUDIO_ENCODING_PCM);
            /* The wire additionally insists that float carries 32 valid bits. */
            if (fl && v != 32) { CHECK(!wire); continue; }
            CHECK(policy == wire);
        }
    printf("PASS policy legal range and remote wire range agree\n");
}

static void test_src_prefer_order(void) {
    int rates[] = {88200, 96000, 44100, 176400, 48000};
    nex_audio_policy_prefer_integer_ratios(rates, 5, 88200);
    CHECK(rates[0] == 88200);
    CHECK(rates[1] == 44100);
    CHECK(rates[2] == 176400);
    CHECK(rates[3] == 96000);
    CHECK(rates[4] == 48000);
    printf("PASS src_prefer: integer-ratio candidates precede bandwidth candidates\n");
}

static void test_dither_policy(void) {
    CHECK(nex_audio_policy_dither_selected(0, 24, 0, 16, 2, 0));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 0, 16, 2, 5));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 0, 24, 2, 0));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 1, 16, 2, 0));
    CHECK(nex_audio_policy_dither_selected(0, 24, 0, 24, 1, 5));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 0, 16, 0, 0));
    CHECK(!nex_audio_policy_dither_selected(1, 32, 0, 24, 2, 0));
    CHECK(nex_audio_policy_dither_selected(1, 32, 0, 16, 2, 0));
    printf("PASS dither policy: auto requires integer depth reduction and fade off\n");
}

static void test_hifi_config_policy(void) {
    WCHAR temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) { printf("SKIP hifi policy: no temp path\n"); return; }
    WCHAR path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsNexExclusive-hifi-%lu.cfg", temp,
               (unsigned long)GetCurrentProcessId());
    FILE *f = _wfopen(path, L"wb");
    if (!f) { printf("SKIP hifi policy: cannot create config\n"); return; }
    fputs("hifi=1\nrate=96000\ncompat=1\ndither=2\nfade_ms=5\nshared_fallback=1\n", f);
    fclose(f);
    char utf8[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
    _putenv_s("NEX_CFG", utf8);
    g_cfg_loaded = 0;
    load_config();
    CHECK(g_force_rate == 0);
    CHECK(g_compat == 0);
    CHECK(g_dither == 0);
    CHECK(g_fade_ms == 0);
    CHECK(g_shared_fallback == 0);
    DeleteFileW(path);
    _putenv_s("NEX_CFG", "");
    g_cfg_loaded = 0;
    f = _wfopen(path, L"wb");
    if (!f) { printf("SKIP hifi policy: cannot create compatibility config\n"); return; }
    fputs("hifi=0\ncompat=1\naudiohost=1\nshared_fallback=1\n", f);
    fclose(f);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
    _putenv_s("NEX_CFG", utf8);
    g_cfg_loaded = 0;
    load_config();
    CHECK(g_hifi == 0);
    CHECK(g_compat == 1);
    CHECK(g_audiohost == 1);
    CHECK(g_shared_fallback == 0);
    DeleteFileW(path);
    _putenv_s("NEX_CFG", "");
    g_cfg_loaded = 0;
    printf("PASS output policy: strict native disables processing; audiohost permits converted PCM; shared fallback stays off\n");
}

static void test_hifi_verifier(void) {
    BYTE source[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    BYTE same[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    BYTE changed[8] = {0, 1, 2, 9, 4, 5, 6, 7};
    CHECK(hifi_changed_bytes(source, same, sizeof(source)) == 0);
    CHECK(hifi_changed_bytes(source, changed, sizeof(source)) == 1);
    printf("PASS hifi verifier: native client PCM comparison counts changed bytes\n");
}

/* ---- IAudioClient3 ABI -------------------------------------------------- */
static DWORD seen_flags;
static UINT32 seen_period;
static const WAVEFORMATEX *seen_fmt;
static LPCGUID seen_session;
static HRESULT STDMETHODCALLTYPE mock_init_shared(void *s, DWORD flags, UINT32 period,
                                                  const WAVEFORMATEX *fmt, LPCGUID session) {
    (void)s;
    seen_flags = flags; seen_period = period; seen_fmt = fmt; seen_session = session;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE mock_start(void *s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE mock_stop(void *s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE mock_reset(void *s) { (void)s; return S_OK; }

static int internal_activate_calls;
static int internal_client_start_calls;
static int global_fallback_start_calls;
static void *internal_client_vtbl[15];
static void *internal_client_obj[1];
static HRESULT STDMETHODCALLTYPE mock_internal_client_start(void *s) {
    (void)s; internal_client_start_calls++; return S_OK;
}
static HRESULT STDMETHODCALLTYPE mock_global_fallback_start(void *s) {
    (void)s; global_fallback_start_calls++; return E_FAIL;
}
static HRESULT STDMETHODCALLTYPE mock_internal_activate(void *self, const GUID *iid,
                                                        DWORD clsctx, void *params,
                                                        void **out) {
    (void)self; (void)iid; (void)clsctx; (void)params;
    internal_activate_calls++;
    if (out) *out = internal_client_obj;
    return out ? S_OK : E_POINTER;
}

static void test_unregistered_vtable_and_internal_activate(void) {
    reset_patches();
    memset(g_bindings, 0, sizeof(g_bindings));
    memset(internal_client_vtbl, 0, sizeof(internal_client_vtbl));
    internal_client_vtbl[AC_START] = (void *)mock_internal_client_start;
    internal_client_obj[0] = internal_client_vtbl;
    g_ac_orig[AC_START] = (void *)mock_global_fallback_start;

    void *device_vtbl[8] = {0};
    void *device_obj[1];
    device_vtbl[DEV_ACTIVATE] = (void *)H_Activate; /* patched table entry */
    device_obj[0] = device_vtbl;
    g_dev_vtbl = device_vtbl;
    g_dev_orig_activate = mock_internal_activate;
    g_activation_dev = NULL;
    internal_activate_calls = 0;
    internal_client_start_calls = 0;
    global_fallback_start_calls = 0;

    void *created = NULL;
    CHECK(dev_activate((IMMDevice *)device_obj, &IID_IAudioClient, &created) == S_OK);
    CHECK(created == internal_client_obj);
    CHECK(internal_activate_calls == 1);
    CHECK(g_activation_dev == NULL); /* H_Activate was bypassed */
    for (int i = 0; i < 128; i++) CHECK(g_bindings[i].client == NULL);

    /* Internal client is intentionally not patched or registered. Its own
       vtable slot must win over a deliberately different global test fallback. */
    CHECK(ac_original(created, AC_START) == (void *)mock_internal_client_start);
    CHECK(O_START(created) == S_OK);
    CHECK(internal_client_start_calls == 1);
    CHECK(global_fallback_start_calls == 0);
    CHECK(ac_original(NULL, AC_START) == (void *)mock_global_fallback_start);

    g_dev_vtbl = NULL;
    g_dev_orig_activate = NULL;
    reset_patches();
    printf("PASS unregistered vtable: internal Activate bypasses host binding and uses its own original slot\n");
}

/* Two independently patched vtables. The old code kept a single global array
   of originals, so patching the second table made the first one's calls land
   in the second table's entry points - or in our own hook, recursively. */
static void test_patch_records(void) {
    /* These vtables live on this frame, and the patch table outlives it. Start
       from a clean table so a later test cannot match a recycled address. */
    reset_patches();
    void *vt1[21], *vt2[21];
    for (int i = 0; i < 21; i++) {
        vt1[i] = (void *)(UINT_PTR)(0x1000 + i);
        vt2[i] = (void *)(UINT_PTR)(0x2000 + i);
    }
    vt1[AC_START] = (void *)mock_start;
    vt2[AC_START] = (void *)mock_stop;
    vt1[AC3_INITSHARED] = (void *)mock_init_shared;
    vt2[AC3_INITSHARED] = (void *)mock_init_shared;
    void *obj1[1], *obj2[1];
    obj1[0] = vt1; obj2[0] = vt2;

    int before = g_audio_patch_count;
    patch_audio_client(obj1, 1);
    patch_audio_client(obj2, 1);
    CHECK(g_audio_patch_count == before + 2);
    CHECK(vt1[AC_START] == (void *)H_Start);
    CHECK(vt2[AC_START] == (void *)H_Start);
    CHECK(ac_original(obj1, AC_START) == (void *)mock_start);
    CHECK(ac_original(obj2, AC_START) == (void *)mock_stop);
    CHECK(ac_original(obj1, AC_STOP) == (void *)(UINT_PTR)(0x1000 + AC_STOP));
    CHECK(ac_original(obj2, AC_STOP) == (void *)(UINT_PTR)(0x2000 + AC_STOP));

    /* Patching an already-patched table must not shift the originals. */
    patch_audio_client(obj1, 1);
    CHECK(g_audio_patch_count == before + 2);
    CHECK(ac_original(obj1, AC_START) == (void *)mock_start);

    /* A v1 client seen first, then the same table asked for as v3. */
    void *vt3[21];
    for (int i = 0; i < 21; i++) vt3[i] = (void *)(UINT_PTR)(0x3000 + i);
    vt3[AC3_INITSHARED] = (void *)mock_init_shared;
    void *obj3[1];
    obj3[0] = vt3;
    patch_audio_client(obj3, 0);
    CHECK(ac_original(obj3, AC3_INITSHARED) == NULL);
    patch_audio_client(obj3, 1);
    CHECK(vt3[AC3_INITSHARED] == (void *)H_InitializeShared);

    /* Slot 20 called the way COM calls it: four arguments. */
    WAVEFORMATEXTENSIBLE we;
    build_fmt_ex(&we, 48000, 32, 1, 2, mask_for(2));
    GUID session = {0};
    seen_fmt = NULL; seen_period = 0; seen_flags = 0; seen_session = NULL;
    g_enable = 0;   /* forward instead of engaging */
    HRESULT hr = ((HRESULT (STDMETHODCALLTYPE *)(void *, DWORD, UINT32,
                    const WAVEFORMATEX *, LPCGUID))vt1[AC3_INITSHARED])
                 (obj1, 0x1234, 480, (WAVEFORMATEX *)&we, &session);
    CHECK(hr == S_OK);
    CHECK(seen_flags == 0x1234);
    CHECK(seen_period == 480);
    CHECK(seen_fmt == (WAVEFORMATEX *)&we);
    CHECK(seen_session == &session);
    g_enable = 1;
    printf("PASS patch records: originals kept per vtable, v3 slot takes 4 arguments\n");
}

/* ---- drain_once --------------------------------------------------------- */
static BYTE mock_dev_buf[64 * 8];
static UINT32 mock_padding;
static UINT32 mock_gb_frames, mock_rb_frames;
static DWORD  mock_rb_flags;
static int    mock_start_calls, mock_gb_calls;
static int    mock_event_full;
static UINT32 mock_event_capacity;

static HRESULT STDMETHODCALLTYPE mock_get_padding(void *s, UINT32 *p) {
    (void)s; if (p) *p = mock_padding; return S_OK;
}
static HRESULT STDMETHODCALLTYPE mock_get_buffer_size(void *s, UINT32 *n) {
    (void)s; if (n) *n = 64; return S_OK;
}
static HRESULT STDMETHODCALLTYPE mock_get_latency(void *s, REFERENCE_TIME *l) {
    (void)s; if (l) *l = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE mock_set_event(void *s, HANDLE h) { (void)s; (void)h; return S_OK; }
static HRESULT STDMETHODCALLTYPE mock_start_counted(void *s) { (void)s; mock_start_calls++; return S_OK; }

static HRESULT STDMETHODCALLTYPE mock_render_get(void *s, UINT32 frames, BYTE **pp) {
    (void)s;
    mock_gb_calls++; mock_gb_frames = frames;
    if (mock_event_full && frames != mock_event_capacity) return AUDCLNT_E_BUFFER_SIZE_ERROR;
    if (pp) *pp = mock_dev_buf;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE mock_render_release(void *s, UINT32 written, DWORD flags) {
    (void)s;
    mock_rb_frames = written; mock_rb_flags = flags;
    return S_OK;
}

static void test_drain(void) {
    prepare_ring();
    reset_patches();
    SinkGen *gen = mock_gen();
    gen->real_frames = 64;
    g_sink.cap = 64;
    g_sink.reported_frames = 32;
    gen->submitted = 0;
    gen->device_started = 0;

    void *ac_vt[15] = {0}, *ac_orig[24] = {0};
    ac_vt[AC_GETBUFFERSIZE] = (void *)mock_get_buffer_size;
    ac_vt[AC_GETSTREAMLATENCY] = (void *)mock_get_latency;
    ac_vt[AC_GETCURRENTPADDING] = (void *)mock_get_padding;
    ac_vt[AC_START] = (void *)mock_start_counted;
    ac_vt[AC_STOP] = (void *)mock_stop;
    ac_vt[AC_RESET] = (void *)mock_reset;
    ac_vt[AC_SETEVENTHANDLE] = (void *)mock_set_event;
    for (int i = 0; i < 15; i++) ac_orig[i] = ac_vt[i];
    register_originals(ac_vt, ac_orig);
    void *ac_obj[1];

    void *rc_vt[5] = {0}, *rc_orig[24] = {0};
    rc_vt[RC_GETBUFFER] = (void *)mock_render_get;
    rc_vt[RC_RELEASEBUFFER] = (void *)mock_render_release;
    for (int i = 0; i < 5; i++) rc_orig[i] = rc_vt[i];
    register_originals(rc_vt, rc_orig);
    void *rc_obj[1];

    ac_obj[0] = ac_vt;
    rc_obj[0] = rc_vt;
    gen->real = (IAudioClient *)ac_obj;
    gen->real_render = (IAudioRenderClient *)rc_obj;
    free(g_inb);

    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);

    /* Fill 8 frames with a recognisable ramp. */
    for (int f = 0; f < 8; f++)
        for (int b = 0; b < g_sink.in_block; b++)
            g_sink.ring[(size_t)f * g_sink.in_block + b] = (BYTE)(0x30 + f);
    g_sink.w = 8; g_sink.r = 0;

    mock_padding = 0; mock_gb_calls = 0; mock_start_calls = 0;
    mock_rb_frames = 0xFFFFFFFF; mock_rb_flags = 0xFF;
    memset(mock_dev_buf, 0xEE, sizeof(mock_dev_buf));

    CHECK(drain_once(gen) == S_OK);
    CHECK(mock_gb_calls == 1);
    CHECK(mock_gb_frames == 8);
    CHECK(mock_rb_frames == mock_gb_frames);   /* sizes must agree */
    CHECK(mock_rb_flags == 0);
    CHECK(gen->submitted == 8);
    CHECK(g_sink.r == 8 && g_sink.w == 8);
    CHECK(mock_start_calls == 1);              /* started after the commit */
    CHECK(gen->device_started == 1);
    /* Nothing was fabricated: the device buffer holds the ring's ramp. */
    for (int f = 0; f < 8; f++)
        for (int b = 0; b < g_sink.in_block; b++)
            CHECK(mock_dev_buf[(size_t)f * g_sink.in_block + b] == (BYTE)(0x30 + f));

    /* Empty ring: no submission, no silence, device stays started. */
    mock_gb_calls = 0;
    CHECK(drain_once(gen) == S_FALSE);
    CHECK(mock_gb_calls == 0);
    CHECK(mock_start_calls == 1);

    /* Device full: nothing to write yet, and no call into GetBuffer. */
    g_sink.w = 16; g_sink.r = 8;
    mock_padding = 64; mock_gb_calls = 0;
    CHECK(drain_once(gen) == S_FALSE);
    CHECK(mock_gb_calls == 0);

    /* Fewer frames free than queued: submit exactly what fits. */
    mock_padding = 60; mock_gb_calls = 0;
    CHECK(drain_once(gen) == S_OK);
    CHECK(mock_gb_frames == 4);
    CHECK(mock_rb_frames == 4);

    /* A padding larger than the real buffer is a broken endpoint, not a
       reason to compute a negative space. */
    mock_padding = 65;
    CHECK(drain_once(gen) == E_UNEXPECTED);

    free(g_inb); g_inb = NULL;
    free_ring();
    printf("PASS drain: sizes agree, no fabricated silence, start deferred, space clamped\n");
}

/* ---- remote v3 producer path -------------------------------------------- */
static void remote_fixture_begin(NexAudioControl *c, BYTE *data, UINT32 capacity,
                                 UINT32 frame_bytes) {
    memset(c, 0, sizeof(*c));
    nex_audio_control_init(c, 9);
    c->state = NEX_AUDIO_READY;
    c->capacity_frames = capacity;
    c->output_frame_bytes = frame_bytes;
    c->data_bytes = (UINT64)capacity * frame_bytes;
    c->client_hash = c->converted_hash = c->endpoint_hash = 1469598103934665603ull;
    memset(data, 0, (size_t)c->data_bytes);
    g_remote_ipc = c;
    g_remote_pcm_data = data;
    g_remote_data = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_remote_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_remote_error = S_OK;
    g_remote_reason = NEX_AUDIO_FALLBACK_NONE;
}

static void remote_fixture_end(void) {
    if (g_remote_data) CloseHandle(g_remote_data);
    if (g_remote_space) CloseHandle(g_remote_space);
    g_remote_data = g_remote_space = NULL;
    g_remote_ipc = NULL;
    g_remote_pcm_data = NULL;
    g_remote_error = E_FAIL;
    g_remote_reason = NEX_AUDIO_FALLBACK_NONE;
}

/* ---- v3.1 transport from the client side ----------------------------------- */
static volatile LONG flush_acker_stop;
static DWORD WINAPI flush_acker(LPVOID arg) {
    NexAudioControl *c = (NexAudioControl *)arg;
    while (!flush_acker_stop) {
        UINT64 req = nex_audio_read64(&c->flush_request_generation);
        if (req != nex_audio_read64(&c->flush_ack_generation))
            InterlockedExchange64((volatile LONG64 *)&c->flush_ack_generation, (LONG64)req);
        Sleep(1);
    }
    return 0;
}

static void test_remote_transport_signals(void) {
    NexAudioControl control;
    BYTE pcm[64 * 6];
    SinkGen gen;
    prepare_ring();
    remote_fixture_begin(&control, pcm, 64, 6);
    memset(&gen, 0, sizeof(gen));
    gen.remote = 1;
    gen.ref = 2;
    g_sink.gen = &gen;
    g_sink.started = 1;
    CHECK(control.transport == NEX_AUDIO_TRANSPORT_PLAY);
    CHECK(sink_stop() == S_OK);
    CHECK(control.transport == NEX_AUDIO_TRANSPORT_PAUSE);
    remote_set_transport(NEX_AUDIO_TRANSPORT_PLAY);
    CHECK(control.transport == NEX_AUDIO_TRANSPORT_PLAY);

    /* without a Host the flush request is still published, and times out */
    CHECK(!remote_request_flush(30));
    CHECK(control.flush_request_generation == 1);
    /* with a Host acknowledging, the request completes */
    flush_acker_stop = 0;
    HANDLE t = CreateThread(NULL, 0, flush_acker, &control, 0, NULL);
    CHECK(remote_request_flush(500));
    CHECK(control.flush_request_generation == 2 && control.flush_ack_generation == 2);
    flush_acker_stop = 1;
    WaitForSingleObject(t, 1000);
    CloseHandle(t);

    /* teardown drains the Host only when the stream was still playing */
    CHECK(sink_teardown_waits_for_eos(1, 1));
    CHECK(!sink_teardown_waits_for_eos(1, 0));
    CHECK(!sink_teardown_waits_for_eos(0, 1));

    g_sink.gen = NULL;
    remote_fixture_end();
    free_ring();
    printf("PASS remote transport: Stop pauses, Start plays, Reset flushes the Host, stopped teardown skips EOS\n");
}

static void test_remote_converted_drain(void) {
    prepare_ring();
    g_sink.in_rate = 44100;
    g_sink.out_rate = 44100;
    g_sink.out_container_bits = 24;
    g_sink.out_valid_bits = 24;
    g_sink.out_bits = 24;
    g_sink.out_block = 6;
    g_sink.hifi_map = 2;
    g_sink.resample = 0;
    free(g_inb);
    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);
    g_remote_out = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.out_block);
    BYTE original[8 * 8], expected[8 * 6];
    for (UINT32 f = 0; f < 8; f++)
        for (UINT32 b = 0; b < g_sink.in_block; b++) {
            BYTE value = (BYTE)(f * 11 + b + 1);
            g_sink.ring[f * g_sink.in_block + b] = value;
            original[f * g_sink.in_block + b] = value;
        }
    g_sink.w = 8;
    convert(original, expected, 8);

    NexAudioControl control;
    BYTE pcm[64 * 8];
    remote_fixture_begin(&control, pcm, 64, 6);
    SinkGen gen;
    memset(&gen, 0, sizeof(gen));
    gen.remote = 1;
    gen.real_frames = 8;
    g_sink.gen = &gen;
    CHECK(drain_once(&gen) == S_OK);
    CHECK(control.client_frames == 8);
    CHECK(control.converted_frames == 8);
    CHECK(control.endpoint_frames == 0);
    CHECK(control.write_frame == 8 && control.read_frame == 0);
    CHECK(control.client_hash != control.converted_hash);
    CHECK(memcmp(pcm, expected, sizeof(expected)) == 0);
    CHECK(gen.submitted == 8 && g_sink.r == 8);
    CHECK(gen.device_started == 0); /* only the host may publish this flag */
    CHECK(g_sink.hifi_client_frames == 8 && g_sink.hifi_converted_frames == 8);

    /* An event-driven client keeps waiting for the next device period even
       while its producer ring still holds frames. A remote submission must
       release that wait regardless of the ring's instantaneous fill level. */
    HANDLE client_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(client_event != NULL);
    g_sink.h_client = client_event;
    for (UINT32 f = 8; f < 24; f++)
        for (UINT32 b = 0; b < g_sink.in_block; b++)
            g_sink.ring[f * g_sink.in_block + b] = (BYTE)(f * 11 + b + 1);
    g_sink.w = 24;
    CHECK(drain_once(&gen) == S_OK);
    CHECK(g_sink.r == 16 && g_sink.w == 24);
    CHECK(WaitForSingleObject(client_event, 0) == WAIT_OBJECT_0);
    CloseHandle(client_event);
    g_sink.h_client = NULL;

    free(g_remote_out);
    g_remote_out = NULL;
    remote_fixture_end();
    free(g_inb);
    g_inb = NULL;
    free_ring();
    printf("PASS remote converted drain: existing DSP feeds independent PCM mapping\n");
}

static void test_remote_resample_respects_device_period(void) {
    prepare_ring();
    g_sink.in_rate = 44100;
    g_sink.out_rate = 48000;
    g_sink.out_container_bits = 24;
    g_sink.out_valid_bits = 24;
    g_sink.out_bits = 24;
    g_sink.out_block = 6;
    g_sink.hifi_map = 2;
    CHECK(src_init(g_sink.in_rate, g_sink.out_rate, 2));
    g_sink.resample = 1;
    free(g_inb);
    free(g_fin);
    free(g_fout);
    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);
    g_fin = (double *)malloc((size_t)SCRATCH_FRAMES * 2 * sizeof(double));
    g_fout = (double *)malloc(64 * 2 * sizeof(double));
    g_remote_out = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.out_block);
    CHECK(g_inb && g_fin && g_fout && g_remote_out);
    memset(g_fout, 0xA5, 64 * 2 * sizeof(double));
    for (UINT32 f = 0; f < 16; f++)
        for (UINT32 b = 0; b < g_sink.in_block; b++)
            g_sink.ring[f * g_sink.in_block + b] = (BYTE)(f * 13 + b + 1);
    g_sink.w = 16;

    NexAudioControl control;
    BYTE pcm[64 * 6];
    remote_fixture_begin(&control, pcm, 64, 6);
    SinkGen gen;
    memset(&gen, 0, sizeof(gen));
    gen.remote = 1;
    gen.real_frames = 8;
    g_sink.gen = &gen;
    CHECK(drain_once(&gen) == S_OK);

    const BYTE *fout_bytes = (const BYTE *)g_fout;
    int canary_intact = 1;
    for (size_t i = 8 * 2 * sizeof(double); i < 64 * 2 * sizeof(double); i++)
        if (fout_bytes[i] != 0xA5) { canary_intact = 0; break; }
    CHECK(canary_intact);
    CHECK(control.converted_frames == 8);
    CHECK(control.write_frame == 8 && gen.submitted == 8);

    free(g_remote_out);
    g_remote_out = NULL;
    remote_fixture_end();
    scratch_free();
    src_free();
    free_ring();
    printf("PASS remote resample: one device period bounds conversion scratch and PCM submission\n");
}

static void test_remote_clock_uses_host_device_clock(void) {
    prepare_ring();
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    SinkGen *gen = mock_gen();
    gen->remote = 1;
    gen->real_clock = NULL;
    gen->clock_frequency = 0;
    g_sink.out_rate = 48000;

    NexAudioControl control;
    BYTE pcm[64 * 6];
    remote_fixture_begin(&control, pcm, 64, 6);
    control.output_rate = 48000;
    control.device_clock_frequency = 10000000ULL;
    control.device_clock_position = 5000000ULL;
    control.device_clock_qpc = 900ULL;
    UINT64 pos = 0, qpc = 0;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 5000000ULL);
    CHECK(qpc == 900ULL);
    control.device_clock_position = 10000208ULL;
    control.device_clock_qpc = 901ULL;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 10000208ULL);

    /* Once the endpoint is running, one device period has been queued ahead of
       the playhead. Correct the remote clock base at that transition so the
       public position stays continuous while later values follow the queue. */
    gen->device_started = 1;
    gen->real_frames = 4800;
    control.device_clock_position = 10000208ULL;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 10000208ULL);
    control.device_clock_position = 11000208ULL;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 11000208ULL);

    control.device_clock_position = 0;
    CHECK(fc_GetPosition(NULL, &pos, &qpc) == S_OK);
    CHECK(pos == 11000208ULL);

    remote_fixture_end();
    free_ring();
    printf("PASS remote clock: Host IAudioClock provides the client timeline\n");
}

static void test_remote_backpressure_is_atomic(void) {
    memset(&g_sink, 0, sizeof(g_sink));
    NexAudioControl control;
    BYTE pcm[64 * 8], source[4 * 8];
    memset(source, 0x6C, sizeof(source));
    remote_fixture_begin(&control, pcm, 4, 8);
    CHECK(nex_audio_ring_write(&control, pcm, source, 3) == 3);
    CHECK(remote_write(NULL, source, 2) == 0);
    CHECK(control.state == NEX_AUDIO_FAILED);
    CHECK(control.fallback_reason == NEX_AUDIO_FALLBACK_RING_BACKPRESSURE);
    CHECK(control.backpressure_events == 1);
    CHECK(control.write_frame == 3 && control.read_frame == 0);
    CHECK(control.converted_frames == 0 && control.dropped_frames == 0);
    CHECK(g_remote_error == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    remote_fixture_end();
    printf("PASS remote backpressure: full write refuses truncation without dropped frames\n");
}

static void test_remote_drain_backpressure_is_atomic(void) {
    prepare_ring();
    g_sink.in_rate = g_sink.out_rate = 48000;
    g_sink.out_container_bits = g_sink.out_valid_bits = g_sink.out_bits = 32;
    g_sink.out_block = g_sink.in_block = 8;
    g_sink.resample = 0;
    g_sink.w = 2;
    memset(g_sink.ring, 0x39, (size_t)g_sink.w * g_sink.in_block);
    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);
    g_remote_out = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.out_block);
    CHECK(g_inb && g_remote_out);

    NexAudioControl control;
    BYTE pcm[4 * 8], source[3 * 8];
    memset(source, 0x71, sizeof(source));
    remote_fixture_begin(&control, pcm, 4, 8);
    CHECK(nex_audio_ring_write(&control, pcm, source, 3) == 3);
    SinkGen gen;
    memset(&gen, 0, sizeof(gen));
    gen.remote = 1;
    gen.real_frames = 2;
    g_sink.gen = &gen;
    CHECK(drain_once(&gen) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    CHECK(control.state == NEX_AUDIO_FAILED);
    CHECK(control.backpressure_events == 1);
    CHECK(control.write_frame == 3 && control.read_frame == 0);
    CHECK(g_sink.r == 0 && g_sink.w == 2);
    CHECK(control.dropped_frames == 0);

    free(g_remote_out); g_remote_out = NULL;
    free(g_inb); g_inb = NULL;
    remote_fixture_end();
    free_ring();
    printf("PASS remote drain backpressure: source ring stays unconsumed on timeout\n");
}

static void test_remote_host_exit_is_visible_to_drain(void) {
    NexAudioControl control;
    BYTE pcm[4 * 8];
    remote_fixture_begin(&control, pcm, 4, 8);
    control.state = NEX_AUDIO_RUNNING;
    HANDLE exited = CreateEventA(NULL, TRUE, TRUE, NULL);
    CHECK(exited != NULL);
    g_remote_process.hProcess = exited;
    CHECK(remote_state() == NEX_AUDIO_FAILED);
    CHECK(control.fallback_reason == NEX_AUDIO_FALLBACK_HOST_EXIT);
    CHECK(control.error == (UINT32)HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED));
    g_remote_process.hProcess = NULL;
    CloseHandle(exited);
    remote_fixture_end();
    printf("PASS remote host exit: drain observes a dead Host despite stale shared state\n");
}

static void test_remote_fallback_policy_and_eos_stamp(void) {
    CHECK(remote_fallback_is_local(NEX_AUDIO_FALLBACK_PROTOCOL_MISMATCH));
    CHECK(remote_fallback_is_local(NEX_AUDIO_FALLBACK_BUILD_MISMATCH));
    CHECK(remote_fallback_is_local(NEX_AUDIO_FALLBACK_MAPPING_FAILED));
    CHECK(remote_fallback_is_local(NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT));
    CHECK(remote_fallback_is_local(NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED));
    CHECK(!remote_fallback_is_local(NEX_AUDIO_FALLBACK_RING_BACKPRESSURE));
    CHECK(!remote_fallback_is_local(NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST));

    memset(&g_sink, 0, sizeof(g_sink));
    NexAudioControl control;
    BYTE pcm[64 * 8];
    remote_fixture_begin(&control, pcm, 4, 8);
    remote_submit_eos();
    CHECK(control.eos_submitted_generation == control.stream_generation);
    CHECK(control.eos_complete_generation == 0);
    control.eos_complete_generation = control.stream_generation;
    CHECK(remote_wait_eos_complete(control.stream_generation) == 1);
    remote_fixture_end();
    printf("PASS remote fallback: startup can retry locally; runtime failures and EOS keep generation\n");
}

static void test_watch_does_not_follow_default_change(void) {
    CHECK(watch_should_retarget(0, 0) == 0);
    CHECK(watch_should_retarget(0, 1) == 0);
    CHECK(watch_should_retarget(1, 0) == 1);
    CHECK(watch_should_retarget(1, 1) == 1);
    printf("PASS endpoint watch: default changes wait for client rebind; device loss still migrates\n");
}

static void test_event_buffer_floor(void) {
    SinkGen gen;
    memset(&gen, 0, sizeof(gen));
    gen.real_frames = 4410;
    /* The client can be event-driven even when the real endpoint uses polling. */
    CHECK(event_safe_client_frames(1408, &gen) == 4410);
    gen.real_evt = (HANDLE)(UINT_PTR)1;
    CHECK(event_safe_client_frames(1408, &gen) == 4410);
    CHECK(event_safe_client_frames(8192, &gen) == 8192);
    printf("PASS event buffer: public prefill never below the real device buffer\n");
}

/* Event-driven exclusive endpoints reject partial GetBuffer requests. The
   drain loop must keep asking the client for fixed-size packets until one
   complete device buffer is ready, without rejecting a packet merely because
   it crosses the smaller client-visible buffer boundary. */
static void test_event_drain_requires_full_buffer(void) {
    prepare_ring();
    reset_patches();
    SinkGen *gen = mock_gen();
    gen->real_frames = 16;
    gen->real_evt = (HANDLE)(UINT_PTR)1;
    gen->device_started = 0;
    g_sink.h_client = CreateEventA(NULL, FALSE, FALSE, NULL);

    void *ac_vt[15] = {0}, *ac_orig[24] = {0};
    ac_vt[AC_GETCURRENTPADDING] = (void *)mock_get_padding;
    ac_vt[AC_START] = (void *)mock_start_counted;
    ac_vt[AC_STOP] = (void *)mock_stop;
    for (int i = 0; i < 15; i++) ac_orig[i] = ac_vt[i];
    register_originals(ac_vt, ac_orig);
    void *ac_obj[1] = { ac_vt };

    void *rc_vt[5] = {0}, *rc_orig[24] = {0};
    rc_vt[RC_GETBUFFER] = (void *)mock_render_get;
    rc_vt[RC_RELEASEBUFFER] = (void *)mock_render_release;
    for (int i = 0; i < 5; i++) rc_orig[i] = rc_vt[i];
    register_originals(rc_vt, rc_orig);
    void *rc_obj[1] = { rc_vt };
    gen->real = (IAudioClient *)ac_obj;
    gen->real_render = (IAudioRenderClient *)rc_obj;

    free(g_inb);
    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);
    mock_padding = 0;
    mock_gb_calls = 0;
    mock_start_calls = 0;
    mock_event_full = 1;
    mock_event_capacity = gen->real_frames;

    /* The real client uses fixed packets (512 frames in the captured stream),
       while the device period is not an integer multiple of that packet size.
       Three packets are therefore needed before the first 16-frame commit. */
    for (int packet = 0; packet < 3; packet++) {
        CHECK(drain_once(gen) == S_FALSE);
        CHECK(mock_gb_calls == 0);
        CHECK(WaitForSingleObject(g_sink.h_client, 0) == WAIT_OBJECT_0);
        BYTE *p = NULL;
        HRESULT packet_hr = fr_GetBuffer(NULL, 6, &p);
        CHECK(packet_hr == S_OK);
        if (FAILED(packet_hr) || !p) break;
        for (int i = 0; i < 6 * g_sink.in_block; i++) p[i] = (BYTE)(packet + i);
        CHECK(fr_ReleaseBuffer(NULL, 6, 0) == S_OK);
    }
    CHECK(g_sink.w == 18 && g_sink.r == 0);
    CHECK(drain_once(gen) == S_OK);
    CHECK(mock_gb_frames == 16);
    CHECK(mock_rb_frames == 16);
    CHECK(gen->submitted == 16);
    CHECK(g_sink.w == 18 && g_sink.r == 16 && g_sink.w - g_sink.r == 2);
    CHECK(mock_start_calls == 1);
    CHECK(gen->device_started == 1);

    mock_event_full = 0;
    CloseHandle(g_sink.h_client);
    g_sink.h_client = NULL;
    free(g_inb);
    g_inb = NULL;
    free_ring();
    printf("PASS event drain: fixed client packets prefill one complete device buffer\n");
}

/* ---- reset -------------------------------------------------------------- */
static HRESULT reset_result;
static HRESULT STDMETHODCALLTYPE mock_reset_result(void *self) {
    (void)self;
    return reset_result;
}
static void test_reset(void) {
    prepare_ring();
    /* A stack vtable address is reused across tests, and ac_original() matches
       on that address: a record left behind by an earlier test would be found
       first, and its slot 12 may be empty. */
    reset_patches();
    CHECK(src_init(192000, 96000, 2));
    g_sink.resample = 1;
    void *ac_vt[15] = {0}, *ac_orig[24] = {0}, *ac_obj[1];
    ac_vt[AC_RESET] = (void *)mock_reset_result;
    for (int i = 0; i < 15; i++) ac_orig[i] = ac_vt[i];
    /* Registered explicitly: ac_original() falls back to vt[slot] only for an
       unregistered vtable, and a stack address can collide with one a previous
       test registered. */
    register_originals(ac_vt, ac_orig);
    ac_obj[0] = ac_vt;
    mock_gen()->real = (IAudioClient *)ac_obj;
    for (int i = 0; i < SRC_CAP * 2; i++) g_src.buf[i] = 0.75f;
    g_src.count = 4096;
    g_src.pos = 2048 * g_src.den;
    g_sink.w = 8;
    g_sink.started = 1;
    CHECK(H_Reset(g_sink.owner) == AUDCLNT_E_NOT_STOPPED);
    CHECK(g_src.count == 4096 && g_sink.w == 8);
    g_sink.started = 0;
    reset_result = E_FAIL;
    CHECK(H_Reset(g_sink.owner) == E_FAIL);
    CHECK(g_src.count == 4096 && g_sink.w == 8);
    reset_result = S_OK;
    CHECK(H_Reset(g_sink.owner) == S_OK);
    CHECK(g_src.count == 2 * (int)ceil(g_src.half) + 1);
    CHECK(g_src.pos == (long long)ceil(g_src.half) * g_src.den);
    CHECK(g_sink.w == 0 && g_sink.r == 0);
    for (int i = 0; i < SRC_CAP * 2; i++) CHECK(g_src.buf[i] == 0.0f);
    for (int i = 0; i < 32 * 8; i++) CHECK(g_sink.ring[i] == 0);
    double zero[2048] = {0}, output[2048];
    CHECK(src_push(zero, 1024, 2) == 1024);
    int ready = src_ready(2);
    CHECK(ready > 0 && ready <= 1024);
    src_pull(output, ready, 2);
    for (int i = 0; i < ready * 2; i++) CHECK(output[i] == 0.0f);
    src_free();
    /* The mock vtable has no Release slot: drop the pointer before the
       generation's last reference releases its objects. */
    if (g_sink.gen) { g_sink.gen->real = NULL; gen_release(g_sink.gen); g_sink.gen = NULL; }
    free_ring();
    printf("PASS reset: running/failed reset preserves data; successful reset removes old-track history\n");
}

typedef struct {
    HANDLE entered;
    HANDLE done;
    HRESULT result;
    void *self;
} LifecycleThread;

static DWORD WINAPI blocked_stop_worker(LPVOID arg) {
    LifecycleThread *ctx = (LifecycleThread *)arg;
    SetEvent(ctx->entered);
    ctx->result = H_Stop(ctx->self);
    SetEvent(ctx->done);
    return 0;
}

static void test_stop_serializes_with_proxy_lock(void) {
    prepare_ring();
    g_sink.started = 0;
    LifecycleThread ctx = { CreateEventA(NULL, TRUE, FALSE, NULL),
                             CreateEventA(NULL, TRUE, FALSE, NULL), E_FAIL,
                             g_sink.owner };
    EnterCriticalSection(&g_control);
    HANDLE thread = CreateThread(NULL, 0, blocked_stop_worker, &ctx, 0, NULL);
    CHECK(thread != NULL);
    if (thread) {
        CHECK(WaitForSingleObject(ctx.entered, 1000) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(ctx.done, 100) == WAIT_TIMEOUT);
        LeaveCriticalSection(&g_control);
        CHECK(WaitForSingleObject(ctx.done, 1000) == WAIT_OBJECT_0);
        CHECK(ctx.result == S_FALSE);
        CloseHandle(thread);
    } else {
        LeaveCriticalSection(&g_control);
    }
    CloseHandle(ctx.entered);
    CloseHandle(ctx.done);
    free_ring();
}

typedef enum { LC_START, LC_RESET, LC_ENGAGE } LifecycleCall;
typedef struct { LifecycleThread base; LifecycleCall call; } LifecycleCallThread;
static DWORD WINAPI blocked_lifecycle_worker(LPVOID arg) {
    LifecycleCallThread *ctx = (LifecycleCallThread *)arg;
    SetEvent(ctx->base.entered);
    if (ctx->call == LC_START) ctx->base.result = H_Start(ctx->base.self);
    else if (ctx->call == LC_RESET) ctx->base.result = H_Reset(ctx->base.self);
    else ctx->base.result = sink_engage_controlled((IAudioClient *)ctx->base.self,
                                                   NULL, 0, NULL);
    SetEvent(ctx->base.done);
    return 0;
}
static void run_lifecycle_lock_case(LifecycleCall call, HRESULT expected, int started) {
    prepare_ring();
    g_sink.started = started;
    LifecycleCallThread ctx = {
        { CreateEventA(NULL, TRUE, FALSE, NULL), CreateEventA(NULL, TRUE, FALSE, NULL),
          E_FAIL, g_sink.owner }, call
    };
    EnterCriticalSection(&g_control);
    HANDLE thread = CreateThread(NULL, 0, blocked_lifecycle_worker, &ctx, 0, NULL);
    CHECK(thread != NULL);
    if (thread) {
        CHECK(WaitForSingleObject(ctx.base.entered, 1000) == WAIT_OBJECT_0);
        CHECK(WaitForSingleObject(ctx.base.done, 100) == WAIT_TIMEOUT);
        LeaveCriticalSection(&g_control);
        CHECK(WaitForSingleObject(ctx.base.done, 1000) == WAIT_OBJECT_0);
        CHECK(ctx.base.result == expected);
        CloseHandle(thread);
    } else {
        LeaveCriticalSection(&g_control);
    }
    CloseHandle(ctx.base.entered);
    CloseHandle(ctx.base.done);
    free_ring();
}
static void test_lifecycle_entrypoint_serialization(void) {
    run_lifecycle_lock_case(LC_START, S_FALSE, 1);
    run_lifecycle_lock_case(LC_RESET, AUDCLNT_E_NOT_STOPPED, 1);
    run_lifecycle_lock_case(LC_ENGAGE, AUDCLNT_E_UNSUPPORTED_FORMAT, 0);
    printf("PASS lifecycle entry points: Start, Reset and engage wait for control ownership\n");
}

typedef struct {
    FakeObj *proxy;
    HANDLE acquired;
    HANDLE allow_release;
    HANDLE done;
    HRESULT get_result;
    HRESULT release_result;
} OutstandingBufferThread;

static DWORD WINAPI outstanding_buffer_worker(LPVOID arg) {
    OutstandingBufferThread *ctx = (OutstandingBufferThread *)arg;
    BYTE *p = NULL;
    ctx->get_result = fr_GetBuffer(ctx->proxy, 1, &p);
    if (SUCCEEDED(ctx->get_result)) {
        memset(p, 0, g_sink.in_block);
        SetEvent(ctx->acquired);
        WaitForSingleObject(ctx->allow_release, 2000);
        ctx->release_result = fr_ReleaseBuffer(ctx->proxy, 1, 0);
    }
    fr_Release(ctx->proxy); /* caller's external reference */
    SetEvent(ctx->done);
    return 0;
}

static void test_teardown_with_outstanding_proxy_buffer(void) {
    prepare_ring();
    g_sink.owner = NULL; /* this test has no host COM owner */
    FakeObj *proxy = (FakeObj *)calloc(1, sizeof(FakeObj));
    CHECK(proxy != NULL);
    if (!proxy) { free_ring(); return; }
    proxy->vtbl = g_fake_render_vtbl;
    proxy->ref = 2; /* sink-owned plus the simulated client's service reference */
    proxy->generation = g_sink.generation;
    proxy->capacity = 16;
    proxy->staging = (BYTE *)malloc((size_t)proxy->capacity * g_sink.in_block);
    g_sink.proxy_render = proxy;

    OutstandingBufferThread ctx = { proxy,
        CreateEventA(NULL, TRUE, FALSE, NULL),
        CreateEventA(NULL, TRUE, FALSE, NULL),
        CreateEventA(NULL, TRUE, FALSE, NULL), E_FAIL, E_FAIL };
    HANDLE thread = CreateThread(NULL, 0, outstanding_buffer_worker, &ctx, 0, NULL);
    CHECK(thread != NULL);
    if (thread) {
        CHECK(WaitForSingleObject(ctx.acquired, 1000) == WAIT_OBJECT_0);
        CHECK(ctx.get_result == S_OK);
        /* The client holds a valid staging pointer outside GetBuffer's lock.
           Teardown must invalidate the generation but leave the referenced
           proxy/staging allocation alive until the client releases it. */
        CHECK(sink_teardown_controlled() == S_OK);
        CHECK(!g_sink.engaged && g_sink.ring == NULL);
        SetEvent(ctx.allow_release);
        CHECK(WaitForSingleObject(ctx.done, 1000) == WAIT_OBJECT_0);
        CHECK(ctx.release_result == AUDCLNT_E_DEVICE_INVALIDATED);
        CloseHandle(thread);
    } else {
        free_ring();
    }
    CloseHandle(ctx.acquired);
    CloseHandle(ctx.allow_release);
    CloseHandle(ctx.done);
    printf("PASS teardown race: outstanding proxy buffer is invalidated without freeing client-owned memory early\n");
}

/* ---- generation abandonment -------------------------------------------- */
/* The whole point of holding the device side in a reference-counted object is
   the case this test builds: a drain thread parked inside GetBuffer on an
   endpoint the OS is reconfiguring. The call does not return, so the control
   path must be able to stop, give up, and carry on with a new generation -
   without freeing anything the parked thread still holds. */
static HANDLE abandon_entered;   /* set when the drain thread is inside GetBuffer */
static HANDLE abandon_release;   /* set by the test to let the call return      */
static LONG   abandon_ac_released, abandon_rc_released, abandon_stop_calls;
static HRESULT STDMETHODCALLTYPE abandon_get_buffer(void *s, UINT32 frames, BYTE **pp) {
    (void)s;
    SetEvent(abandon_entered);
    WaitForSingleObject(abandon_release, INFINITE);
    if (pp) *pp = mock_dev_buf;
    mock_gb_frames = frames;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE abandon_release_buffer(void *s, UINT32 written, DWORD flags) {
    (void)s; (void)written; (void)flags;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE abandon_release_ac(void *s) {
    (void)s; InterlockedIncrement(&abandon_ac_released); return 1;
}
static ULONG STDMETHODCALLTYPE abandon_release_rc(void *s) {
    (void)s; InterlockedIncrement(&abandon_rc_released); return 1;
}
static HRESULT STDMETHODCALLTYPE abandon_stop(void *s) {
    (void)s; InterlockedIncrement(&abandon_stop_calls); return S_OK;
}
static HRESULT STDMETHODCALLTYPE abandon_ok(void *s) { (void)s; return S_OK; }

static void test_generation_abandon(void) {
    prepare_ring();
    reset_patches();

    void *ac_vt[15] = {0}, *ac_orig[24] = {0}, *ac_obj[1];
    ac_vt[SLOT_RELEASE] = (void *)abandon_release_ac;
    ac_vt[AC_STOP] = (void *)abandon_stop;
    ac_vt[AC_RESET] = (void *)abandon_ok;
    ac_vt[AC_GETCURRENTPADDING] = (void *)mock_get_padding;
    for (int i = 0; i < 15; i++) ac_orig[i] = ac_vt[i];
    register_originals(ac_vt, ac_orig);
    ac_obj[0] = ac_vt;

    void *rc_vt[5] = {0}, *rc_orig[24] = {0}, *rc_obj[1];
    rc_vt[SLOT_RELEASE] = (void *)abandon_release_rc;
    rc_vt[RC_GETBUFFER] = (void *)abandon_get_buffer;
    rc_vt[RC_RELEASEBUFFER] = (void *)abandon_release_buffer;
    for (int i = 0; i < 5; i++) rc_orig[i] = rc_vt[i];
    register_originals(rc_vt, rc_orig);
    rc_obj[0] = rc_vt;

    SinkGen *gen = mock_gen();
    gen->real = (IAudioClient *)ac_obj;
    gen->real_render = (IAudioRenderClient *)rc_obj;
    gen->real_frames = 64;
    gen->device_started = 0;
    abandon_stop_calls = 0;
    g_sink.reported_frames = 32;
    g_sink.w = 8; g_sink.r = 0;
    mock_padding = 0;
    abandon_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    abandon_release = CreateEventA(NULL, TRUE, FALSE, NULL);

    /* 给这一代挂一个真的闸门句柄。放弃时必须把它提前放掉：卡住的线程可能还要
       几百毫秒才回来，而客户端的下一个 Initialize 已经在门口了。实测 09:43:59
       闸门等满 20×10 ms 仍是 SHARING_VIOLATION，客户端因此丢了独占流。 */
    WCHAR gate_tmp[MAX_PATH], gate_path[MAX_PATH];
    HANDLE gate = INVALID_HANDLE_VALUE;
    gate_path[0] = 0;
    if (GetTempPathW(MAX_PATH, gate_tmp)) {
        _snwprintf(gate_path, MAX_PATH, L"%lsNexExclusive-test-abandon-%lu.lock",
                   gate_tmp, (unsigned long)GetCurrentProcessId());
        gate = CreateFileW(gate_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (gate != INVALID_HANDLE_VALUE) gen->endpoint_gate = gate;
    }

    InterlockedExchange(&gen->running, 1);
    gen_retain(gen);
    gen->h_thread = CreateThread(NULL, 0, drain_thread, gen, 0, NULL);
    CHECK(gen->h_thread != NULL);
    CHECK(WaitForSingleObject(abandon_entered, 2000) == WAIT_OBJECT_0);

    DWORD t0 = GetTickCount();
    HRESULT hr = sink_stop();
    DWORD took = GetTickCount() - t0;
    CHECK(hr == S_OK);                 /* a gone device is not a broken stream */
    CHECK(took >= 800 && took < 2000); /* both join windows ran, then gave up */
    CHECK(g_sink.gen == NULL);         /* handed to the thread, not kept */
    CHECK(gen->retired == 1);
    CHECK(abandon_ac_released == 0);   /* nothing freed under the parked call */
    CHECK(abandon_rc_released == 0);
    CHECK(abandon_stop_calls >= 1);    /* Stop must interrupt GetBuffer even before Start */
    if (gate != INVALID_HANDLE_VALUE) {
        /* 闸门在这一代被真正回收之前一直攥着。被放弃的那条独占流还开着，设备确实
           还占着——闸门说的是实话。提前放掉会让下一个 engage 对着一台还被占着的
           设备把 19 个候选格式各试 8 次（实测 3.5 s），比立刻失败更糟。 */
        CHECK(gen->endpoint_gate != NULL);
        HANDLE again = CreateFileW(gate_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY |
                                   FILE_FLAG_DELETE_ON_CLOSE, NULL);
        CHECK(again == INVALID_HANDLE_VALUE);
        if (again != INVALID_HANDLE_VALUE) CloseHandle(again);
    }

    /* Let the parked call return. The thread must bail before touching the
       ring or the resampler, then release the generation on its way out. */
    SetEvent(abandon_release);
    for (int i = 0; i < 200 && abandon_ac_released == 0; i++) Sleep(10);
    CHECK(abandon_ac_released == 1);
    CHECK(abandon_rc_released == 1);

    /* 线程回来把这一代连同闸门一起收掉，路径重新可用（Release 与 CloseHandle
       之间有几毫秒的窗口，所以重试几次而不是只试一次）。 */
    if (gate != INVALID_HANDLE_VALUE) {
        HANDLE freed = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 50 && freed == INVALID_HANDLE_VALUE; i++) {
            freed = CreateFileW(gate_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY |
                                FILE_FLAG_DELETE_ON_CLOSE, NULL);
            if (freed == INVALID_HANDLE_VALUE) Sleep(10);
        }
        CHECK(freed != INVALID_HANDLE_VALUE);
        if (freed != INVALID_HANDLE_VALUE) CloseHandle(freed);
    }

    CloseHandle(abandon_entered);
    CloseHandle(abandon_release);
    free_ring();
    printf("PASS generation abandon: parked GetBuffer is handed off, not waited on\n");
}

static LONG stress_errors;
static volatile LONG stress_noted;
static char stress_first[160];
/* Count a failure and remember the first one, so a rare failure says which
   call broke instead of only that something did. */
static void stress_fail(const char *what, HRESULT hr, int iteration) {
    InterlockedIncrement(&stress_errors);
    if (InterlockedCompareExchange(&stress_noted, 1, 0) == 0)
        snprintf(stress_first, sizeof(stress_first), "%s -> 0x%08lX at iteration %d (started=%d gen=%p)",
                 what, (unsigned long)hr, iteration, (int)g_sink.started, (void *)g_sink.gen);
}
static HANDLE stress_go;
static void *stress_owner;
static FakeObj *stress_render;
static FakeObj *stress_clock;
static HRESULT STDMETHODCALLTYPE stress_padding(void *self, UINT32 *p) {
    (void)self; *p = 32; return S_OK; /* full mock device, no real submission */
}
static ULONG STDMETHODCALLTYPE stress_ref(void *self) { (void)self; return 1; }
static DWORD WINAPI stress_producer(LPVOID arg) {
    (void)arg;
    WaitForSingleObject(stress_go, INFINITE);
    for (int i = 0; i < 6000; i++) {
        BYTE *p = NULL;
        HRESULT hr = fr_GetBuffer(stress_render, 1, &p);
        if (hr == S_OK) {
            memset(p, 0, 8);
            SwitchToThread(); /* let Reset/Stop interleave with outstanding data */
            hr = fr_ReleaseBuffer(stress_render, 1, AUDCLNT_BUFFERFLAGS_SILENT);
            if (hr != S_OK && hr != AUDCLNT_E_DEVICE_INVALIDATED) stress_fail("ReleaseBuffer", hr, i);
        } else if (hr != AUDCLNT_E_BUFFER_TOO_LARGE &&
                   hr != AUDCLNT_E_DEVICE_INVALIDATED) {
            stress_fail("GetBuffer", hr, i);
        }
        if ((i & 31) == 0) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI stress_clock_reader(LPVOID arg) {
    (void)arg;
    WaitForSingleObject(stress_go, INFINITE);
    for (int i = 0; i < 6000; i++) {
        UINT64 pos = 0, qpc = 0;
        UINT32 frames = 0;
        void *service = NULL;
        HRESULT hr = fc_GetPosition(stress_clock, &pos, &qpc);
        if (hr != S_OK) stress_fail("GetPosition", hr, i);
        hr = H_GetBufferSize(stress_owner, &frames);
        if (hr != S_OK || frames != 16) stress_fail("GetBufferSize", hr, i);
        hr = H_GetService(stress_owner, &IID_IAudioRenderClient_l, &service);
        if (hr != S_OK) stress_fail("GetService", hr, i);
        else fr_Release(service);
        if ((i & 31) == 0) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI stress_lifecycle(LPVOID arg) {
    (void)arg;
    WaitForSingleObject(stress_go, INFINITE);
    for (int i = 0; i < 300; i++) {
        HRESULT hr = H_Start(stress_owner);
        if (hr != S_OK) stress_fail("Start", hr, i);
        SwitchToThread();
        hr = H_Stop(stress_owner);
        if (hr != S_OK) stress_fail("Stop", hr, i);
        hr = H_Reset(stress_owner);
        if (hr != S_OK) stress_fail("Reset", hr, i);
    }
    return 0;
}
/* ---- 6c: Start after an abandoned generation ------------------------------ */
/* Stop hands a generation whose drain thread is parked in GetBuffer to that
   thread and drops it from the sink. The client then calls Start on the same
   IAudioClient. Before the fix that Start returned E_FAIL ("no device
   generation") and the track stayed silent until the next format change. */
static int rebuild_calls;
static void *rebuild_ac_vt[15], *rebuild_ac_obj[1];
static void *rebuild_clock_vt[6], *rebuild_clock_obj[1];
static HRESULT mock_rebuild_device(const char *reason) {
    (void)reason;
    rebuild_calls++;
    SinkGen *g = mock_gen();
    g->real = (IAudioClient *)rebuild_ac_obj;
    g->real_clock = (IAudioClock *)rebuild_clock_obj;
    g->real_frames = 32;
    g->clock_frequency = 48000;
    return S_OK;
}
static HRESULT busy_rebuild_device(const char *reason) {
    (void)reason;
    rebuild_calls++;
    return AUDCLNT_E_DEVICE_IN_USE;
}
static void mock_rebuild_setup(void) {
    memset(rebuild_ac_vt, 0, sizeof(rebuild_ac_vt));
    memset(rebuild_clock_vt, 0, sizeof(rebuild_clock_vt));
    rebuild_ac_vt[SLOT_RELEASE] = (void *)stress_ref;
    rebuild_ac_vt[AC_GETCURRENTPADDING] = (void *)stress_padding;
    rebuild_ac_vt[AC_STOP] = (void *)mock_stop;
    rebuild_ac_vt[AC_RESET] = (void *)mock_reset;
    rebuild_ac_obj[0] = rebuild_ac_vt;
    rebuild_clock_vt[SLOT_RELEASE] = (void *)stress_ref;
    rebuild_clock_vt[CLK_GETPOSITION] = (void *)mock_get_position;
    rebuild_clock_obj[0] = rebuild_clock_vt;
}
static void test_start_rebuilds_abandoned_generation(void) {
    HRESULT (*saved)(const char *) = g_rebuild_device;
    HandoffClient client = { handoff_vtbl, 1 };
    prepare_ring();
    reset_patches();
    mock_rebuild_setup();
    g_sink.owner = &client;
    g_sink.out_rate = 48000;
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    /* state left by sink_stop's abandonment branch */
    g_sink.gen = NULL;
    g_sink.started = 0;

    g_rebuild_device = mock_rebuild_device;
    rebuild_calls = 0;
    CHECK(H_Start(&client) == S_OK);
    CHECK(rebuild_calls == 1);
    CHECK(g_sink.gen != NULL && g_sink.started == 1);
    CHECK(H_Stop(&client) == S_OK);
    CHECK(g_sink.gen && g_sink.gen->h_thread == NULL && !g_sink.started);
    /* an ordinary pause/resume keeps the generation: no second rebuild */
    CHECK(H_Start(&client) == S_OK);
    CHECK(rebuild_calls == 1);
    CHECK(H_Stop(&client) == S_OK);

    /* a rebuild that cannot get the endpoint reports why, and does not start */
    gen_release(g_sink.gen);
    g_sink.gen = NULL;
    g_rebuild_device = busy_rebuild_device;
    CHECK(H_Start(&client) == AUDCLNT_E_DEVICE_IN_USE);
    CHECK(rebuild_calls == 2 && !g_sink.started);

    g_rebuild_device = saved;
    sink_teardown_controlled();
    printf("PASS 6c: Start after an abandoned generation rebuilds the device side\n");
}

/* idle_release_ms: a long pause gives the device back; the next Start takes
   it again through the same rebuild path, and anything that ends the pause
   first (a Start, a rebuilt device side) voids the timer. */
static void test_idle_release_gives_device_back(void) {
    HRESULT (*saved)(const char *) = g_rebuild_device;
    int saved_idle = g_idle_release_ms, saved_auto = g_auto_shared;
    HandoffClient client = { handoff_vtbl, 1 };
    prepare_ring();
    reset_patches();
    mock_rebuild_setup();
    g_sink.owner = &client;
    g_sink.engaged = 1;
    g_sink.out_rate = 48000;
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    g_sink.gen = NULL;
    g_rebuild_device = mock_rebuild_device;
    g_auto_shared = 0;
    g_idle_release_ms = 1000;
    rebuild_calls = 0;

    CHECK(H_Start(&client) == S_OK);
    CHECK(rebuild_calls == 1);
    CHECK(H_Stop(&client) == S_OK);
    CHECK(g_sink.pause_token == g_sink.generation && g_sink.gen != NULL);

    /* not yet */
    g_sink.pause_tick = GetTickCount() - 400;
    sink_watch_share_tick();
    CHECK(g_sink.gen != NULL && !g_sink.idle_released);

    /* due: the device side goes, the stream stays */
    g_sink.pause_tick = GetTickCount() - 1500;
    sink_watch_share_tick();
    CHECK(g_sink.gen == NULL);
    CHECK(g_sink.idle_released == 1 && g_sink.pause_token == 0);
    CHECK(g_sink.engaged && g_sink.proxy_render && g_sink.proxy_clock);
    {   /* the client's clock is held, not failed, while released */
        UINT64 pos = 1;
        CHECK(fc_GetPosition(&g_fake_clock, &pos, NULL) == S_OK);
        CHECK(pos == g_sink.clock_last);
    }
    sink_watch_share_tick();            /* nothing left to release */
    CHECK(g_sink.gen == NULL);

    /* resume takes the device again */
    CHECK(H_Start(&client) == S_OK);
    CHECK(rebuild_calls == 2 && g_sink.gen && g_sink.started);
    CHECK(!g_sink.idle_released && g_sink.pause_token == 0);

    /* a short pause that ends before the timer: nothing happens */
    CHECK(H_Stop(&client) == S_OK);
    CHECK(H_Start(&client) == S_OK);
    g_sink.pause_tick = GetTickCount() - 5000;
    CHECK(H_Stop(&client) == S_OK);
    g_sink.pause_token = 0;             /* as after the Start above, before this Stop */
    sink_watch_share_tick();
    CHECK(g_sink.gen != NULL);

    /* the device side was rebuilt during the pause: the old timer is void */
    g_sink.pause_token = g_sink.generation;
    g_sink.generation++;
    g_sink.pause_tick = GetTickCount() - 5000;
    sink_watch_share_tick();
    CHECK(g_sink.gen != NULL);
    g_sink.generation--;

    /* feature off: a long pause keeps the device */
    g_idle_release_ms = 0;
    g_sink.pause_token = g_sink.generation;
    sink_watch_share_tick();
    CHECK(g_sink.gen != NULL);

    /* a shared device side returns to exclusive at the next resume once
       auto_shared no longer wants it */
    g_sink.gen->shared = 1;
    CHECK(H_Start(&client) == S_OK);
    CHECK(rebuild_calls == 3 && g_sink.gen && !g_sink.gen->shared);
    CHECK(H_Stop(&client) == S_OK);

    g_rebuild_device = saved;
    g_idle_release_ms = saved_idle;
    g_auto_shared = saved_auto;
    sink_teardown_controlled();
    printf("PASS idle release: long pause gives the device back, resume takes it again\n");
}

/* Between the abandonment in Stop and the rebuild in the next Start there is
   no device clock. The client keeps polling it; an error there reads as "the
   track ended", so the last position is reported instead. */
static void test_clock_holds_position_while_generation_abandoned(void) {
    UINT64 pos = 0, qpc = 7;
    prepare_ring();
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    g_sink.gen = NULL;
    g_sink.clock_last = 12345678;
    CHECK(fc_GetPosition(&g_fake_clock, &pos, &qpc) == S_OK);
    CHECK(pos == 12345678 && qpc == 0);
    g_sink.engaged = 0;                       /* torn down: no stream to report */
    CHECK(fc_GetPosition(&g_fake_clock, &pos, &qpc) != S_OK);
    g_sink.engaged = 1;
    free_ring();
    printf("PASS 6c: clock holds the last position while the generation is being rebuilt\n");
}

static void test_lifecycle_stress(void) {
    prepare_ring();
    reset_patches();
    void *ac_vt[15] = {0}, *ac_obj[1] = {ac_vt};
    void *clock_vt[6] = {0}, *clock_obj[1] = {clock_vt};
    ac_vt[SLOT_RELEASE] = (void *)stress_ref;
    ac_vt[AC_GETCURRENTPADDING] = (void *)stress_padding;
    ac_vt[AC_STOP] = (void *)mock_stop;
    ac_vt[AC_RESET] = (void *)mock_reset;
    clock_vt[SLOT_RELEASE] = (void *)stress_ref;
    clock_vt[CLK_GETPOSITION] = (void *)mock_get_position;
    SinkGen *gen = mock_gen();
    gen->real = (IAudioClient *)ac_obj;
    gen->real_clock = (IAudioClock *)clock_obj;
    gen->real_frames = 32;
    gen->clock_frequency = 48000;
    g_sink.out_rate = 48000;
    g_sink.owner = stress_owner = ac_obj;
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    stress_render = &g_fake_render;
    stress_clock = &g_fake_clock;
    mock_clock_pos = 48000;
    stress_errors = 0;
    stress_noted = 0;
    stress_first[0] = 0;
    stress_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE threads[3] = {
        CreateThread(NULL, 0, stress_producer, NULL, 0, NULL),
        CreateThread(NULL, 0, stress_clock_reader, NULL, 0, NULL),
        CreateThread(NULL, 0, stress_lifecycle, NULL, 0, NULL)
    };
    for (int i = 0; i < 3; i++) CHECK(threads[i] != NULL);
    SetEvent(stress_go);
    DWORD joined = WaitForMultipleObjects(3, threads, TRUE, 15000);
    if (joined != WAIT_OBJECT_0) {
        /* Fail the process rather than free state beneath an unjoined thread. */
        fprintf(stderr, "FAIL lifecycle stress: worker join 0x%08lX\n", (unsigned long)joined);
        ExitProcess(1);
    }
    CHECK(stress_errors == 0);
    if (stress_errors) fprintf(stderr, "  lifecycle stress: %ld error(s), first: %s\n",
                               (long)stress_errors, stress_first);
    /* Stop keeps the generation so a pause/resume does not renegotiate the
       device; what must be gone is the thread. */
    /* Under load the final Stop may abandon the generation to its thread
       (ROADMAP 6c); then there is no generation left to inspect. */
    CHECK(!g_sink.started && (!g_sink.gen || g_sink.gen->h_thread == NULL));
    for (int i = 0; i < 3; i++) CloseHandle(threads[i]);
    CloseHandle(stress_go);
    CHECK(sink_teardown_controlled() == S_OK);
    free(g_fake_render.staging); g_fake_render.staging = NULL;
    printf("PASS lifecycle stress: 300 Start/Stop/Reset cycles with 12000 producer/clock iterations\n");
}

/* ---- SRC 全频 ----------------------------------------------------------- */
/* 只测重采样器本身：不碰设备、不碰 sink 状态，直接喂正弦、拉出结果，用单频
   DFT 量幅度。r 是「相对奈奎斯特」的比例，同一个 r 在输入域和输出域对应同一个
   物理频率（2πf/fs = πr），所以可以拿 r 直接相关输出。
   跳过开头 4096 个输出帧：src_init 用 K 帧零历史起步，滤波器的建立过程
   （最长 half ≤ 256 帧）全落在这一段里。 */
static int src_tone(int in_rate, int out_rate, double r, double *gain_db, int *frames_out) {
    const int ch = 2;
    const int in_frames = in_rate / 2;                     /* 0.5 s */
    const int out_cap   = out_rate / 2 + 8192;
    double *in  = (double *)malloc((size_t)in_frames * ch * sizeof(double));
    double *out = (double *)malloc((size_t)out_cap * ch * sizeof(double));
    if (!in || !out) { free(in); free(out); return 0; }
    if (!src_init(in_rate, out_rate, ch)) { free(in); free(out); return 0; }

    for (int n = 0; n < in_frames; n++) {
        double v = sin(M_PI * r * (double)n);
        in[n * 2] = v; in[n * 2 + 1] = v;
    }

    int fed = 0, out_n = 0, guard = 0;
    while (guard++ < 100000) {
        if (fed < in_frames) {
            int chunk = in_frames - fed;
            if (chunk > 4096) chunk = 4096;
            if (!src_push(in + (size_t)fed * ch, chunk, ch)) break;
            fed += chunk;
        }
        int ready = src_ready(ch);
        if (ready <= 0) {
            if (fed >= in_frames) break;
            continue;
        }
        int take = ready;
        if (out_n + take > out_cap) take = out_cap - out_n;
        if (take <= 0) break;
        src_pull(out + (size_t)out_n * ch, take, ch);
        out_n += take;
    }

    const int skip = 4096;
    int n0 = skip < out_n ? skip : 0;
    /* 物理频率 f = r × in_rate/2。它在输出域的归一化角频率是 2πf/out_rate，
       也就是 πr·(in/out)——只有 in==out 时才等于 πr。第一版就错在这里：按 πr
       去相关，输出里明明是满幅正弦（RMS 0.707），算出来却是 -78 dB。 */
    const double w = M_PI * r * (double)in_rate / (double)out_rate;
    double sc = 0.0, ss = 0.0;
    for (int n = n0; n < out_n; n++) {
        double ph = w * (double)n;
        double v  = (double)out[(size_t)n * ch];
        sc += v * cos(ph);
        ss += v * sin(ph);
    }
    int N = out_n - n0;
    double amp = N > 0 ? 2.0 * sqrt(sc * sc + ss * ss) / (double)N : 0.0;
    *gain_db = 20.0 * log10(amp > 1e-12 ? amp : 1e-12);
    if (getenv("NEX_SRC_DEBUG")) {
        double rms = 0.0;
        for (int n = n0; n < out_n; n++) rms += (double)out[(size_t)n * ch] * out[(size_t)n * ch];
        printf("    [dbg] r=%.3f in=%d out=%d out_n=%d rms=%.4f amp=%.4f first=%.4f\n",
               r, in_rate, out_rate, out_n, sqrt(rms / (N > 0 ? N : 1)), amp,
               out_n > 0 ? (double)out[(size_t)n0 * ch] : 0.0);
    }
    if (frames_out) *frames_out = out_n;
    free(in); free(out);
    return 1;
}

/* 直流增益：常数输入经过滤波器后应当还原成同一个常数（通带增益 1）。 */
static int src_dc(int in_rate, int out_rate, double *settled) {
    const int ch = 2;
    const int in_frames = in_rate / 4;
    double *in  = (double *)malloc((size_t)in_frames * ch * sizeof(double));
    double *out = (double *)malloc((size_t)(out_rate / 4 + 8192) * ch * sizeof(double));
    if (!in || !out) { free(in); free(out); return 0; }
    if (!src_init(in_rate, out_rate, ch)) { free(in); free(out); return 0; }
    for (int n = 0; n < in_frames; n++) { in[n * 2] = 0.5f; in[n * 2 + 1] = 0.5f; }
    int fed = 0, out_n = 0, guard = 0;
    while (guard++ < 100000) {
        if (fed < in_frames) {
            int chunk = in_frames - fed; if (chunk > 4096) chunk = 4096;
            if (!src_push(in + (size_t)fed * ch, chunk, ch)) break;
            fed += chunk;
        }
        int ready = src_ready(ch);
        if (ready <= 0) { if (fed >= in_frames) break; continue; }
        int take = ready;
        src_pull(out + (size_t)out_n * ch, take, ch);
        out_n += take;
    }
    *settled = out_n > 0 ? (double)out[(size_t)(out_n - 1) * ch] : 0.0;
    free(in); free(out);
    return 1;
}

/* 线性相位的准确判据：同一个连续核在不同相位上的采样，必须满足
     ① 幅频响应完全一致（所有相位共用一条 |H(ω)|）；
     ② 相位差恰好是 -ω·frac（群延迟 = 该相位的位置）。
   不能拿「索引对称」去卡小数相位——核的中心在 t=0，而 t = kk - frac，
   frac≠0 时中心落在两个样本之间，索引上本来就不对称。整数相位（frac=0）
   才额外要求索引严格对称。 */
static void kern_response(Phase *p, double w, double *mag, double *ph) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < p->n; i++) {
        double kk = (double)(p->kmin + i);
        re += p->h[i] * cos(w * kk);
        im -= p->h[i] * sin(w * kk);
    }
    *mag = sqrt(re * re + im * im);
    *ph  = atan2(im, re);
}

static int src_kernel_linear_phase(int in_rate, int out_rate) {
    const int ch = 2;
    if (!src_init(in_rate, out_rate, ch)) return 0;
    int ok = 1;
    const int nph = g_src.phases;
    const int probes[3] = {0, nph / 3, nph / 2};
    static const double ws[3] = {0.1 * 3.14159265358979323846,
                                 0.3 * 3.14159265358979323846,
                                 0.6 * 3.14159265358979323846};

    Phase *p0 = &g_src.ph[0];
    src_phase_build(0);
    for (int t = 0; t < 3; t++) {
        int idx = probes[t];
        if (idx == 0) continue;
        Phase *p = &g_src.ph[idx];
        src_phase_build(idx);
        double frac = (double)idx / (double)nph;
        for (int k = 0; k < 3; k++) {
            double m0, f0, m1, f1;
            kern_response(p0, ws[k], &m0, &f0);
            kern_response(p,  ws[k], &m1, &f1);
            if (fabs(m0 - m1) > 1e-6) { ok = 0; break; }        /* 幅频一致 */
            double d = (f1 - f0) + ws[k] * frac;                 /* 应为 2πk */
            if (sqrt(2.0 - 2.0 * cos(d)) > 1e-6) { ok = 0; break; }
        }
        if (!ok) break;
    }
    /* 整数相位：索引上必须严格对称，两端落在窗函数零点上 */
    {
        int K = (p0->n - 1) / 2;
        for (int j = 1; j <= K; j++)
            if (fabs(p0->h[K + j] - p0->h[K - j]) > 1e-6) { ok = 0; break; }
        if (fabs(p0->h[0]) > 1e-9 || fabs(p0->h[p0->n - 1]) > 1e-9) ok = 0;
    }
    return ok;
}

/* 冲激：单个 1.0 样本进来，看峰值是否过冲、响应中心落在哪里。 */
static int src_impulse(int in_rate, int out_rate, double *peak, int *peak_at,
                       double *center) {
    const int ch = 2, in_frames = 4096;
    const int out_cap = in_frames + 4096;
    double *in  = (double *)calloc((size_t)in_frames * ch, sizeof(double));
    double *out = (double *)calloc((size_t)out_cap * ch, sizeof(double));
    if (!in || !out) { free(in); free(out); return 0; }
    if (!src_init(in_rate, out_rate, ch)) { free(in); free(out); return 0; }
    in[0] = 1.0f; in[1] = 1.0f;                 /* 第 0 帧的左右声道各一个冲激 */
    CHECK(src_push(in, in_frames, ch) == in_frames);
    int out_n = 0, ready;
    while ((ready = src_ready(ch)) > 0 && out_n + ready <= out_cap) {
        src_pull(out + (size_t)out_n * ch, ready, ch);
        out_n += ready;
    }
    int best = 0;
    double best_v = 0.0;
    for (int n = 0; n < out_n; n++) {
        double v = fabs((double)out[(size_t)n * ch]);
        if (v > best_v) { best_v = v; best = n; }
    }
    *peak = best_v; *peak_at = best;
    /* 能量重心（整数域即可，只用来报告响应落在哪） */
    {
        double num = 0.0, den = 0.0;
        for (int n = 0; n < out_n; n++) {
            double v = (double)out[(size_t)n * ch];
            num += v * v * (double)n;
            den += v * v;
        }
        *center = den > 0.0 ? num / den : 0.0;
    }
    free(in); free(out);
    return 1;
}

/* ---- item 5: the conversion pipeline keeps 32-bit integer precision -------- */
static void test_float_pipeline_keeps_32_bit_precision(void) {
    int src[4] = {0x12345679, -0x7654321F, 1, -2};
    int dst[4] = {0};
    double mid[4];
    memset(&g_sink, 0, sizeof(g_sink));
    g_sink.in_channels = 2;
    g_sink.in_container_bits = 32;
    g_sink.in_valid_bits = 32;
    g_sink.out_container_bits = 32;
    g_sink.out_valid_bits = 32;
    g_sink.out_bits = 32;
    to_double((const BYTE *)src, mid, 2);
    quantize(mid, 2, (BYTE *)dst);
    for (int i = 0; i < 4; i++) CHECK(dst[i] == src[i]);
    memset(&g_sink, 0, sizeof(g_sink));
    printf("PASS conversion pipeline: 32-bit integer samples survive unchanged\n");
}

/* ---- item 3: the in-process drain thread joins MMCSS like NexAudioHost ----- */
static void test_drain_thread_registers_mmcss(void) {
    const char *task = NULL;
    HANDLE h = drain_mmcss_enter(&task);
    CHECK(h != NULL);
    CHECK(task && (!strcmp(task, "Pro Audio") || !strcmp(task, "Audio")));
    drain_mmcss_leave(h);
    printf("PASS drain thread: MMCSS %s task\n", task ? task : "(none)");
}

/* ---- auto dither leaves samples that are already exact alone --------------- */
/* A 32-bit client stream that only carries 24 bits of data reduces to 24 bits
   without loss. Auto dither must not add noise to it; forced dither still does. */
static void test_auto_dither_keeps_exact_samples(void) {
    BYTE a[3], b[3];
    const double exact = (double)0x123456 / 8388608.0;
    int forced_moved = 0, auto_moved = 0;
    for (int i = 0; i < 64; i++) {
        sample_write(exact, a, 24, 24, 0, SINK_DITHER_AUTO);
        CHECK(a[0] == 0x56 && a[1] == 0x34 && a[2] == 0x12);
        sample_write(exact, b, 24, 24, 0, SINK_DITHER_FORCED);
        if (b[0] != 0x56 || b[1] != 0x34 || b[2] != 0x12) forced_moved = 1;
    }
    CHECK(forced_moved);

    memset(&g_sink, 0, sizeof(g_sink));
    g_sink.in_channels = 2;
    g_sink.in_container_bits = 32;
    g_sink.in_valid_bits = 32;
    g_sink.out_container_bits = 24;
    g_sink.out_valid_bits = 24;
    g_sink.out_bits = 24;
    g_sink.dither = SINK_DITHER_AUTO;
    {
        const int src[8] = {0x12345600, -0x76543200, 0x7FFFFF00, (int)0x80000000,
                            0x00000100, -0x00000100, 0x40000000, 0};
        BYTE dst[8 * 3];
        convert((const BYTE *)src, dst, 4);
        for (int i = 0; i < 8; i++) {
            int v = dst[i * 3] | (dst[i * 3 + 1] << 8) | (dst[i * 3 + 2] << 16);
            if (v & 0x800000) v |= ~0xFFFFFF;
            CHECK(v == (src[i] >> 8));
        }
    }
    {
        /* real 32-bit content: half an output LSB below the grid -> dithered */
        const int src[2] = {0x12345680, 0x12345680};
        BYTE first[6], dst[6];
        convert((const BYTE *)src, first, 1);
        for (int i = 0; i < 64 && !auto_moved; i++) {
            convert((const BYTE *)src, dst, 1);
            if (memcmp(first, dst, sizeof(dst)) != 0) auto_moved = 1;
        }
        CHECK(auto_moved);
    }
    memset(&g_sink, 0, sizeof(g_sink));
    printf("PASS auto dither: exact samples pass unchanged, real fractional content is dithered\n");
}

/* ---- 6b: underruns per report window ---------------------------------------- */
static void test_drain_window_count(void) {
    UINT32 reported = 0;
    CHECK(drain_window_count(5, &reported) == 5 && reported == 5);
    CHECK(drain_window_count(5, &reported) == 0 && reported == 5);
    CHECK(drain_window_count(7, &reported) == 2 && reported == 7);
    printf("PASS drain stats: empty-buffer is reported per window, with the running total beside it\n");
}

/* ---- ROADMAP 4: multichannel layouts keep every channel in its place -------- */
static void test_multichannel_conversion_keeps_channels(void) {
    const int chans[] = {6, 8};
    for (int k = 0; k < 2; k++) {
        const int ch = chans[k];
        short src[8 * 4];
        BYTE dst[8 * 4 * 3];
        memset(&g_sink, 0, sizeof(g_sink));
        g_sink.in_channels = ch;
        g_sink.in_container_bits = 16;
        g_sink.in_valid_bits = 16;
        g_sink.out_container_bits = 24;
        g_sink.out_valid_bits = 24;
        g_sink.out_bits = 24;
        for (int f = 0; f < 4; f++)
            for (int c = 0; c < ch; c++) src[f * ch + c] = (short)(1000 * (c + 1) + f);
        convert((const BYTE *)src, dst, 4);
        for (int f = 0; f < 4; f++)
            for (int c = 0; c < ch; c++) {
                const BYTE *p = dst + ((size_t)f * ch + c) * 3;
                int v = p[0] | (p[1] << 8) | (p[2] << 16);
                if (v & 0x800000) v |= ~0xFFFFFF;
                CHECK(v == src[f * ch + c] * 256);
            }
    }
    memset(&g_sink, 0, sizeof(g_sink));
    printf("PASS multichannel conversion: 5.1 and 7.1 samples stay on their channels\n");
}

/* Each channel carries a different level through the resampler; any channel
   interleaving error shows up as a level landing on the wrong channel. */
static void test_multichannel_resample_keeps_channels(void) {
    const int ch = 8, in_rate = 96000, out_rate = 48000, in_frames = 9600;
    double *in = (double *)calloc((size_t)in_frames * ch, sizeof(double));
    double *out = (double *)calloc((size_t)in_frames * ch, sizeof(double));
    int out_n = 0, ready;
    CHECK(in && out);
    if (!in || !out) { free(in); free(out); return; }
    CHECK(src_init(in_rate, out_rate, ch));
    for (int f = 0; f < in_frames; f++)
        for (int c = 0; c < ch; c++) in[(size_t)f * ch + c] = 0.1 * (c + 1);
    CHECK(src_push(in, in_frames, ch) == in_frames);
    while ((ready = src_ready(ch)) > 0 && out_n + ready <= in_frames) {
        src_pull(out + (size_t)out_n * ch, ready, ch);
        out_n += ready;
    }
    CHECK(out_n > 2000);
    if (out_n > 2000) {
        const double *last = out + (size_t)(out_n - 1) * ch;
        for (int c = 0; c < ch; c++) CHECK(fabs(last[c] - 0.1 * (c + 1)) < 1e-6);
    }
    src_free();
    free(in); free(out);
    printf("PASS multichannel resample: 8 channels at different levels stay separate (96k->48k)\n");
}

/* ---- Orion 3: the log is written off the audio path ------------------------ */
static void test_log_queue_order_and_overflow(void) {
    static LogQueue q;
    char out[LOG_LINE_BYTES];
    memset(&q, 0, sizeof(q));
    CHECK(log_queue_push(&q, "first"));
    CHECK(log_queue_push(&q, "second"));
    CHECK(log_queue_take(&q, out, sizeof(out)) && strcmp(out, "first") == 0);
    CHECK(log_queue_take(&q, out, sizeof(out)) && strcmp(out, "second") == 0);
    CHECK(!log_queue_take(&q, out, sizeof(out)));
    for (int i = 0; i < LOG_QUEUE_LINES + 5; i++) log_queue_push(&q, "x");
    CHECK(q.dropped == 5);                       /* a full queue drops, never blocks */
    int taken = 0;
    while (log_queue_take(&q, out, sizeof(out))) taken++;
    CHECK(taken == LOG_QUEUE_LINES);
    /* a long line is truncated, not overrun */
    {
        char longline[LOG_LINE_BYTES * 2];
        memset(longline, 'a', sizeof(longline) - 1);
        longline[sizeof(longline) - 1] = 0;
        CHECK(log_queue_push(&q, longline));
        CHECK(log_queue_take(&q, out, sizeof(out)) && strlen(out) == LOG_LINE_BYTES - 1);
    }
    printf("PASS log queue: FIFO order, bounded, overflow counted instead of blocking\n");
}

/* ---- Orion 1+2: the device starts on silence, the clock does not count it -- */
static void test_drain_primes_silence(void) {
    prepare_ring();
    reset_patches();
    SinkGen *gen = mock_gen();
    gen->real_frames = 64;
    g_sink.cap = 64;
    free(g_sink.ring);
    g_sink.ring = (BYTE *)calloc(64, 8);
    g_sink.reported_frames = 32;
    g_sink.in_rate = 48000;
    g_sink.out_rate = 48000;
    void *ac_vt[15] = {0}, *ac_orig[24] = {0};
    ac_vt[AC_GETCURRENTPADDING] = (void *)mock_get_padding;
    ac_vt[AC_START] = (void *)mock_start_counted;
    ac_vt[AC_STOP] = (void *)mock_stop;
    ac_vt[AC_RESET] = (void *)mock_reset;
    for (int i = 0; i < 15; i++) ac_orig[i] = ac_vt[i];
    register_originals(ac_vt, ac_orig);
    void *rc_vt[5] = {0}, *rc_orig[24] = {0};
    rc_vt[RC_GETBUFFER] = (void *)mock_render_get;
    rc_vt[RC_RELEASEBUFFER] = (void *)mock_render_release;
    for (int i = 0; i < 5; i++) rc_orig[i] = rc_vt[i];
    register_originals(rc_vt, rc_orig);
    void *ac_obj[1] = {ac_vt}, *rc_obj[1] = {rc_vt};
    gen->real = (IAudioClient *)ac_obj;
    gen->real_render = (IAudioRenderClient *)rc_obj;
    free(g_inb);
    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);
    mock_padding = 0; mock_gb_calls = 0; mock_start_calls = 0;
    mock_rb_frames = 0xFFFFFFFF; mock_rb_flags = 0xFF;

    g_sink.prime_left = 64;
    g_sink.w = 8; g_sink.r = 0;
    /* not enough client audio yet: do not start the device on silence alone */
    CHECK(drain_once(gen) == S_FALSE);
    CHECK(mock_gb_calls == 0 && mock_start_calls == 0);
    g_sink.w = 64;
    CHECK(drain_once(gen) == S_OK);
    CHECK(mock_gb_calls == 1 && mock_gb_frames == 64);
    CHECK(mock_rb_frames == 64 && mock_rb_flags == AUDCLNT_BUFFERFLAGS_SILENT);
    CHECK(mock_start_calls == 1 && gen->device_started);
    CHECK(g_sink.prime_left == 0 && g_sink.skip_frames == 64);
    CHECK(g_sink.r == 0);                          /* no client audio was used */
    CHECK(drain_once(gen) == S_OK);                /* then the real audio */
    CHECK(mock_rb_flags == 0 && g_sink.r == 64);

    gen->real = NULL;                 /* stack mocks: nothing to Release */
    gen->real_render = NULL;
    g_sink.gen = NULL;
    gen_release(gen);
    free_ring();
    printf("PASS prime: silence fills the first device buffer, client audio follows unchanged\n");
}

static void test_clock_excludes_primed_silence(void) {
    UINT64 pos = 0, qpc = 0;
    prepare_ring();
    SinkGen *gen = mock_gen();
    void *clock_obj[1] = {mock_clock_vtbl};
    gen->real_clock = (IAudioClock *)clock_obj;
    gen->clock_frequency = 48000;
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    g_sink.out_rate = 48000;
    g_sink.skip_frames = 4800;                     /* 100 ms of primed silence */
    mock_clock_pos = 2400;
    CHECK(fc_GetPosition(&g_fake_clock, &pos, &qpc) == S_OK && pos == 0);
    mock_clock_pos = 4800 + 48000;
    CHECK(fc_GetPosition(&g_fake_clock, &pos, &qpc) == S_OK && pos == 10000000ull);
    gen->real_clock = NULL;
    g_sink.gen = NULL;
    gen_release(gen);
    free_ring();
    printf("PASS clock: primed silence is not reported as played client audio\n");
}

/* The Host publishes IAudioClock ticks at the device's own frequency. */
static void test_remote_clock_converts_device_ticks(void) {
    NexAudioControl control;
    BYTE pcm[64 * 6];
    SinkGen gen;
    UINT64 pos = 0, qpc = 0;
    prepare_ring();
    remote_fixture_begin(&control, pcm, 64, 6);
    memset(&gen, 0, sizeof(gen));
    gen.remote = 1;
    gen.ref = 2;
    g_sink.gen = &gen;
    memset(&g_fake_clock, 0, sizeof(g_fake_clock));
    g_fake_clock.vtbl = g_fake_clock_vtbl;
    g_fake_clock.ref = 1;
    g_fake_clock.generation = g_sink.generation;
    g_sink.proxy_clock = &g_fake_clock;
    g_sink.out_rate = 48000;
    control.device_clock_frequency = 48000;
    control.device_clock_position = 48000;        /* one second at 48 kHz ticks */
    CHECK(fc_GetPosition(&g_fake_clock, &pos, &qpc) == S_OK);
    CHECK(pos == 10000000ull);
    control.primed_frames_total = 4800;           /* Host primed 100 ms */
    g_sink.clock_last = 0;
    CHECK(fc_GetPosition(&g_fake_clock, &pos, &qpc) == S_OK);
    CHECK(pos == 9000000ull);
    g_sink.gen = NULL;
    remote_fixture_end();
    free_ring();
    printf("PASS remote clock: device ticks are converted to 100 ns, primed silence excluded\n");
}

/* ---- Orion 5: the drain path's buffers are locked in memory --------------- */
static void test_audio_buffers_locked(void) {
    LONG64 before = g_audio_locked_bytes;
    SIZE_T n = 4u << 20;                          /* larger than the default WS minimum */
    void *p = malloc(n);
    CHECK(p != NULL);
    audio_mem_lock(p, n);
    CHECK(g_audio_locked_bytes == before + (LONG64)n);
    audio_free(p);
    CHECK(g_audio_locked_bytes == before);
    g_memlock = 0;                                /* memlock=0 leaves buffers pageable */
    p = malloc(4096);
    audio_mem_lock(p, 4096);
    CHECK(g_audio_locked_bytes == before);
    audio_free(p);
    g_memlock = 1;
    printf("PASS memlock: buffers locked after allocation, unlocked before free, off switch honoured\n");
}

/* ---- Orion 6: event-driven submissions --------------------------------------- */
static void test_ring_peek_crosses_wrap(void) {
    BYTE out[16 * 8];
    prepare_ring();                                /* cap 32, block 8 */
    for (int f = 0; f < 32; f++) memset(g_sink.ring + f * 8, f, 8);
    g_sink.r = 28; g_sink.w = 36;                  /* 4 frames at the end, 4 at the start */
    CHECK(ring_peek(out, 16) == 8);
    CHECK(out[0] == 28 && out[3 * 8] == 31 && out[4 * 8] == 0 && out[7 * 8] == 3);
    free_ring();
    printf("PASS ring peek: one call returns the frames on both sides of the wrap\n");
}

static void test_event_mode_waits_for_device_request(void) {
    prepare_ring();
    reset_patches();
    SinkGen *gen = mock_gen();
    gen->real_frames = 16;
    g_sink.cap = 32;
    void *ac_vt[15] = {0}, *ac_orig[24] = {0};
    ac_vt[AC_GETCURRENTPADDING] = (void *)mock_get_padding;
    ac_vt[AC_START] = (void *)mock_start_counted;
    ac_vt[AC_STOP] = (void *)mock_stop;
    for (int i = 0; i < 15; i++) ac_orig[i] = ac_vt[i];
    register_originals(ac_vt, ac_orig);
    void *rc_vt[5] = {0}, *rc_orig[24] = {0};
    rc_vt[RC_GETBUFFER] = (void *)mock_render_get;
    rc_vt[RC_RELEASEBUFFER] = (void *)mock_render_release;
    for (int i = 0; i < 5; i++) rc_orig[i] = rc_vt[i];
    register_originals(rc_vt, rc_orig);
    void *ac_obj[1] = {ac_vt}, *rc_obj[1] = {rc_vt};
    gen->real = (IAudioClient *)ac_obj;
    gen->real_render = (IAudioRenderClient *)rc_obj;
    gen->real_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    free(g_inb);
    g_inb = (BYTE *)malloc((size_t)SCRATCH_FRAMES * g_sink.in_block);
    mock_padding = 0; mock_gb_calls = 0; mock_start_calls = 0;

    g_sink.r = 24; g_sink.w = 56;                  /* 32 frames, wrapping */
    CHECK(drain_once(gen) == S_OK);                /* first period before Start */
    CHECK(mock_gb_frames == 16 && mock_start_calls == 1 && g_sink.r == 40);
    mock_gb_calls = 0;
    CHECK(drain_once(gen) == S_FALSE);             /* padding 0, but no event yet */
    CHECK(mock_gb_calls == 0);
    gen->evt_fired = 1;                            /* the device asked */
    CHECK(drain_once(gen) == S_OK);
    CHECK(mock_gb_calls == 1 && mock_gb_frames == 16 && g_sink.r == 56);
    CHECK(gen->evt_fired == 0);

    /* Pause and resume without Reset: the device still holds a period, so the
       drain starts it on what is queued instead of waiting for room. */
    gen->device_started = 0;
    mock_padding = 16; mock_gb_calls = 0; mock_start_calls = 0;
    CHECK(drain_once(gen) == S_OK);
    CHECK(mock_start_calls == 1 && gen->device_started && mock_gb_calls == 0);
    mock_padding = 0;

    /* Timer-driven (a shared stream, or a driver that refused events), paused
       with its buffer full: no room ever appears while the device is stopped,
       so the resume must start it on the queued frames as well. Seen on a
       real device with auto_shared: the client event went silent forever. */
    CloseHandle(gen->real_evt);
    gen->real_evt = NULL;
    gen->device_started = 0;
    mock_padding = 16; mock_gb_calls = 0; mock_start_calls = 0;
    g_sink.r = 24; g_sink.w = 40;                  /* the client already has more queued */
    CHECK(drain_once(gen) == S_OK);
    CHECK(mock_start_calls == 1 && gen->device_started && mock_gb_calls == 0);
    mock_padding = 0;

    gen->real = NULL;
    gen->real_render = NULL;
    g_sink.gen = NULL;
    gen_release(gen);                              /* closes real_evt */
    free_ring();
    printf("PASS event mode: one full period per device request, padding alone does not submit\n");
}

static void test_src_response(void) {
    static const struct { int in, out; const char *name; } ratios[] = {
        {44100, 48000, "44.1k->48k"},
        {48000, 44100, "48k->44.1k"},
        {88200, 96000, "88.2k->96k"},
        {192000, 96000, "192k->96k"},
        /* ROADMAP 4: the top of the candidate pool, never measured on a device */
        {384000, 96000, "384k->96k"},
        {352800, 88200, "352.8k->88.2k"},
        {352800, 96000, "352.8k->96k"},
    };
    const int nratios = (int)(sizeof(ratios) / sizeof(ratios[0]));
    printf("SRC passband / stopband (dB, 0 = unity)\n");

    for (int i = 0; i < nratios; i++) {
        const int in = ratios[i].in, out = ratios[i].out;
        const double cut = out < in ? (double)out / (double)in : 1.0;

        /* 通带：0.05…0.80 × 截止。超过 0.8 就进过渡带，那是设计选择不是缺陷，
           所以只报数不断言。 */
        double worst = 0.0, worst_at = 0.0;
        static const double pass[] = {0.05, 0.25, 0.45, 0.65, 0.80};
        for (int k = 0; k < 5; k++) {
            double r = pass[k] * cut, db = 0.0;
            int n = 0;
            CHECK(src_tone(in, out, r, &db, &n));
            if (fabs(db) > fabs(worst)) { worst = db; worst_at = r; }
        }
        double db95 = 0.0; int n95 = 0;
        src_tone(in, out, 0.95 * cut, &db95, &n95);

        printf("  %-11s passband worst %+.2f dB @ r=%.3f   at 0.95*cut: %+.2f dB\n",
               ratios[i].name, worst, worst_at, db95);
        CHECK(fabs(worst) < 0.5);
        /* Flat right up to the band edge: 0.90 x cutoff is audible content. */
        {
            double db90 = 0.0; int n90 = 0;
            CHECK(src_tone(in, out, 0.90 * cut, &db90, &n90));
            CHECK(fabs(db90) < 0.1);
        }

        /* 阻带：只对降采样有意义——高于抽取后奈奎斯特的内容必须在下采样前被
           压掉，否则会折回通带。两个探针：输入奈奎斯特边上（0.98），以及截止
           与输入奈奎斯特的中点（48k→44.1k 只有 0.919…1.0 这一小段，所以取中点
           而不是固定比例，免得两个点重合成同一个数）。 */
        if (cut < 1.0) {
            double r1 = 0.98;
            double r2 = (cut + 1.0) / 2.0;
            double a = 0.0, b = 0.0;
            int na = 0, nb = 0;
            CHECK(src_tone(in, out, r1, &a, &na));
            CHECK(src_tone(in, out, r2, &b, &nb));
            printf("  %-11s stopband  r=%.3f: %+.1f dB   r=%.3f: %+.1f dB\n",
                   ratios[i].name, r1, a, r2, b);
            CHECK(a < -50.0);
            CHECK(b < -100.0);
            /* Just above the output Nyquist frequency nothing may fold back
               into the audible band: below the 16-bit floor. */
            {
                double edge = 0.0; int ne = 0;
                CHECK(src_tone(in, out, 1.01 * cut, &edge, &ne));
                printf("  %-11s stopband  r=%.3f (1.01 x output Nyquist): %+.1f dB\n",
                       ratios[i].name, 1.01 * cut, edge);
                CHECK(edge < -96.0);
            }
        }
    }

    /* 直流与曲尾 */
    double settled = 0.0;
    CHECK(src_dc(192000, 96000, &settled));
    CHECK(fabs(settled - 0.5) < 1e-3);

    double db = 0.0; int frames_out = 0;
    CHECK(src_tone(192000, 96000, 0.2, &db, &frames_out));
    /* 0.5 s 的 192k 输入应当产出 0.5 s 的 96k 输出，误差不超过滤波器半宽 */
    CHECK(frames_out >= 48000 - 2 * 256 - 8 && frames_out <= 48000 + 8);
    printf("  tail: 96000 in-frames -> %d out-frames (expected %d +- %d)\n",
           frames_out, 48000, 2 * 256);

    /* 冲激与峰值：不得过冲（上限 1.05）。降采样的峰值小于 1 是正常的：核按直流
       增益 1 归一化，192k->96k 的理论峰值约 0.32。线性相位另外直接查相位核。 */
    {
        double peak = 0.0, center = 0.0;
        int at = 0;
        CHECK(src_impulse(44100, 48000, &peak, &at, &center));
        printf("  impulse 44.1k->48k: peak %.3f at frame %d (energy centre %.2f)\n",
               peak, at, center);
        CHECK(peak > 0.5 && peak < 1.05);
        CHECK(src_impulse(192000, 96000, &peak, &at, &center));
        printf("  impulse 192k->96k : peak %.3f at frame %d (energy centre %.2f)\n",
               peak, at, center);
        CHECK(peak > 0.2 && peak < 1.05);
        CHECK(src_kernel_linear_phase(44100, 48000));
        CHECK(src_kernel_linear_phase(48000, 44100));
        CHECK(src_kernel_linear_phase(192000, 96000));
        printf("  kernel: all phases share one |H(w)|, phase lag exactly -w*frac (linear phase)\n");
    }

    /* 实时代价：192k->96k，量 0.5 s 输入要多久 */
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        int n = 0;
        CHECK(src_tone(192000, 96000, 0.3, &db, &n));
        QueryPerformanceCounter(&b);
        double secs = (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart;
        printf("  cost: 0.5 s of 192k->96k in %.1f ms -> %.1fx realtime\n",
               secs * 1000.0, 0.5 / (secs > 0 ? secs : 1e-9));
        CHECK(secs < 0.5);
    }
    src_free();
    printf("PASS src response: passband flat, stopband suppressed, DC unity, tail complete\n");
}

/* ---- endpoint gate ------------------------------------------------------ */
/* 上一代被放弃时，它的 drain 线程还会握着端点闸门几毫秒，而客户端的下一次
   Initialize 已经进来了。实测那次只差 5 ms（16.781 闸门被拒、16.786 线程退出），
   代价是客户端拿到 DEVICE_IN_USE、回退到一条装不下它格式的共享流。
   这里用两个句柄复现那个窗口：先独占打开同一路径，50 ms 后关掉，被验函数必须
   等到它释放而不是立刻放弃。 */
static DWORD WINAPI gate_holder(LPVOID p) {
    Sleep(50);
    CloseHandle((HANDLE)p);
    return 0;
}

static void test_gate_waits_for_previous_holder(void) {
    WCHAR tmp[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) { printf("SKIP gate retry: no temp path\n"); return; }
    _snwprintf(path, MAX_PATH, L"%lsNexExclusive-test-%lu.lock", tmp,
               (unsigned long)GetCurrentProcessId());
    HANDLE holder = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(holder != INVALID_HANDLE_VALUE);
    if (holder == INVALID_HANDLE_VALUE) return;

    HANDLE th = CreateThread(NULL, 0, gate_holder, holder, 0, NULL);
    CHECK(th != NULL);
    int attempts = 0;
    HANDLE got = open_gate_path(path, &attempts);
    CHECK(got != INVALID_HANDLE_VALUE);
    /* 50 ms 的占用至少要跨过几次 10 ms 的间隔，说明真的等了而不是撞运气 */
    CHECK(attempts >= 3);
    if (th) { WaitForSingleObject(th, 2000); CloseHandle(th); }
    if (got != INVALID_HANDLE_VALUE) CloseHandle(got);
    printf("PASS gate retry: waited %d attempts for the previous holder, then got the endpoint\n",
           attempts);
}

/* 默认的 hook.cfg / sink.log 位置由模块自身路径推出，不依赖工作目录，
   也不能是写死的开发机绝对路径——否则换个盘符、换台机器就静默失效。 */
static void test_module_dir(void) {
    /* config_path 会优先看 NEX_CFG；这个测试要验的是回退分支。 */
    SetEnvironmentVariableA("NEX_CFG", NULL);

    char dir[MAX_PATH];
    module_dir(dir, sizeof(dir));
    CHECK(dir[0] != 0);
    if (!dir[0]) return;

    size_t len = strlen(dir);
    CHECK(len > 0 && dir[len - 1] == '\\');

    /* 与 GetModuleFileNameW 推出的目录逐字比较。构建目录被搬走、或者路径
       里混进了硬编码，都会在这里对不上。 */
    WCHAR wpath[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    CHECK(n > 0 && n < MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    WCHAR *slash = wcsrchr(wpath, L'\\');
    CHECK(slash != NULL);
    if (!slash) return;
    slash[1] = L'\0';

    char expect[MAX_PATH];
    if (WideCharToMultiByte(CP_ACP, 0, wpath, -1, expect, sizeof(expect), NULL, NULL) <= 0) {
        CHECK(0);
        return;
    }
    CHECK(strcmp(dir, expect) == 0);

    char cfg[MAX_PATH];
    config_path(cfg, sizeof(cfg));
    CHECK(strncmp(cfg, dir, len) == 0);
    CHECK(strcmp(cfg + len, "hook.cfg") == 0);

    printf("PASS module dir: config and log resolve beside the module (%s)\n", dir);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_control);
    InitializeCriticalSection(&g_patch_lock);
    InitializeCriticalSection(&g_log_qlock);
    g_log[0] = 0; /* No on-disk playback log is modified by this test. */
    /* No test may rebuild a generation on a real endpoint (ROADMAP 6c path). */
    mock_rebuild_setup();
    g_rebuild_device = mock_rebuild_device;
    test_fade();
    test_valid_bits_alignment();
    test_silent();
    test_stale_proxy();
    test_config_reload_defer_matrix();
    test_policy_rebuild_preserves_client_state();
    test_same_format_stream_handoff();
    test_ring_compact_wrap();
    test_ring_reserve_limit();
    test_ring_reserve_waits_for_drain();
    test_clock();
    test_valid_format();
    test_src_prefer_order();
    test_dither_policy();
    test_float_pipeline_keeps_32_bit_precision();
    test_auto_dither_keeps_exact_samples();
    test_drain_window_count();
    test_remote_transport_signals();
    test_log_queue_order_and_overflow();
    test_drain_primes_silence();
    test_clock_excludes_primed_silence();
    test_remote_clock_converts_device_ticks();
    test_audio_buffers_locked();
    test_ring_peek_crosses_wrap();
    test_event_mode_waits_for_device_request();
    test_multichannel_conversion_keeps_channels();
    test_multichannel_resample_keeps_channels();
    test_drain_thread_registers_mmcss();
    test_negotiate_busy_is_not_remembered();
    test_config_values_are_normalized_by_policy();
    test_policy_range_matches_wire_range();
    test_hifi_config_policy();
    test_hifi_verifier();
    test_unregistered_vtable_and_internal_activate();
    test_patch_records();
    test_drain();
    test_remote_converted_drain();
    test_remote_resample_respects_device_period();
    test_remote_clock_uses_host_device_clock();
    test_remote_backpressure_is_atomic();
    test_remote_drain_backpressure_is_atomic();
    test_remote_host_exit_is_visible_to_drain();
    test_remote_fallback_policy_and_eos_stamp();
    test_watch_does_not_follow_default_change();
    test_event_buffer_floor();
    test_event_drain_requires_full_buffer();
    test_reset();
    test_stop_serializes_with_proxy_lock();
    test_lifecycle_entrypoint_serialization();
    test_teardown_with_outstanding_proxy_buffer();
    test_generation_abandon();
    test_src_response();
    test_gate_waits_for_previous_holder();
    test_module_dir();
    test_lifecycle_stress();
    test_start_rebuilds_abandoned_generation();
    test_idle_release_gives_device_back();
    test_clock_holds_position_while_generation_abandoned();
    DeleteCriticalSection(&g_lock);
    DeleteCriticalSection(&g_control);
    DeleteCriticalSection(&g_patch_lock);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
