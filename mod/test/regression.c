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
    g_sink.ring = (BYTE *)malloc(32 * 8);
    g_sink.conv = (BYTE *)malloc(32 * 8);
    memset(g_sink.ring, 0x5A, 32 * 8);
    g_sink.h_data = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_sink.h_space = CreateEventA(NULL, FALSE, FALSE, NULL);
    memset(&g_fake_render, 0, sizeof(g_fake_render));
    g_fake_render.vtbl = g_fake_render_vtbl;
    g_fake_render.ref = 1;
    g_fake_render.generation = g_sink.generation;
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
    printf("PASS valid_pcm_format: rejects tag/geometry/valid-bit mismatches\n");
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
    float zero[2048] = {0}, output[2048];
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
static LONG   abandon_ac_released, abandon_rc_released;
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
static HRESULT STDMETHODCALLTYPE abandon_ok(void *s) { (void)s; return S_OK; }

static void test_generation_abandon(void) {
    prepare_ring();
    reset_patches();

    void *ac_vt[15] = {0}, *ac_orig[24] = {0}, *ac_obj[1];
    ac_vt[SLOT_RELEASE] = (void *)abandon_release_ac;
    ac_vt[AC_STOP] = (void *)abandon_ok;
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
    gen->device_started = 1;
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
            if (hr != S_OK) InterlockedIncrement(&stress_errors);
        } else if (hr != AUDCLNT_E_BUFFER_TOO_LARGE) {
            InterlockedIncrement(&stress_errors);
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
        if (fc_GetPosition(stress_clock, &pos, &qpc) != S_OK)
            InterlockedIncrement(&stress_errors);
        if (H_GetBufferSize(stress_owner, &frames) != S_OK || frames != 16)
            InterlockedIncrement(&stress_errors);
        if (H_GetService(stress_owner, &IID_IAudioRenderClient_l, &service) != S_OK)
            InterlockedIncrement(&stress_errors);
        else fr_Release(service);
        if ((i & 31) == 0) SwitchToThread();
    }
    return 0;
}
static DWORD WINAPI stress_lifecycle(LPVOID arg) {
    (void)arg;
    WaitForSingleObject(stress_go, INFINITE);
    for (int i = 0; i < 300; i++) {
        if (H_Start(stress_owner) != S_OK) InterlockedIncrement(&stress_errors);
        SwitchToThread();
        if (H_Stop(stress_owner) != S_OK) InterlockedIncrement(&stress_errors);
        if (H_Reset(stress_owner) != S_OK) InterlockedIncrement(&stress_errors);
    }
    return 0;
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
    /* Stop keeps the generation so a pause/resume does not renegotiate the
       device; what must be gone is the thread. */
    CHECK(g_sink.gen && g_sink.gen->h_thread == NULL && !g_sink.started);
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
    float *in  = (float *)malloc((size_t)in_frames * ch * sizeof(float));
    float *out = (float *)malloc((size_t)out_cap * ch * sizeof(float));
    if (!in || !out) { free(in); free(out); return 0; }
    if (!src_init(in_rate, out_rate, ch)) { free(in); free(out); return 0; }

    for (int n = 0; n < in_frames; n++) {
        float v = (float)sin(M_PI * r * (double)n);
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
    float *in  = (float *)malloc((size_t)in_frames * ch * sizeof(float));
    float *out = (float *)malloc((size_t)(out_rate / 4 + 8192) * ch * sizeof(float));
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
    float *in  = (float *)calloc((size_t)in_frames * ch, sizeof(float));
    float *out = (float *)calloc((size_t)out_cap * ch, sizeof(float));
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

static void test_src_response(void) {
    static const struct { int in, out; const char *name; } ratios[] = {
        {44100, 48000, "44.1k->48k"},
        {48000, 44100, "48k->44.1k"},
        {88200, 96000, "88.2k->96k"},
        {192000, 96000, "192k->96k"},
    };
    printf("SRC passband / stopband (dB, 0 = unity)\n");

    for (int i = 0; i < 4; i++) {
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
            CHECK(b < -50.0);
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
    g_log[0] = 0; /* No on-disk playback log is modified by this test. */
    test_fade();
    test_silent();
    test_stale_proxy();
    test_ring_compact_wrap();
    test_ring_reserve_limit();
    test_clock();
    test_valid_format();
    test_unregistered_vtable_and_internal_activate();
    test_patch_records();
    test_drain();
    test_reset();
    test_stop_serializes_with_proxy_lock();
    test_lifecycle_entrypoint_serialization();
    test_teardown_with_outstanding_proxy_buffer();
    test_generation_abandon();
    test_src_response();
    test_gate_waits_for_previous_holder();
    test_module_dir();
    test_lifecycle_stress();
    DeleteCriticalSection(&g_lock);
    DeleteCriticalSection(&g_control);
    DeleteCriticalSection(&g_patch_lock);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
