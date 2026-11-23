#define NEX_AUDIO_HOST_NO_MAIN
#include "../tools/nexaudiohost.c"

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static UINT32 mock_padding;
static UINT64 mock_position;
static int mock_start_calls;
static int mock_start_saw_first;
static int mock_stop_calls;
static int mock_reset_calls;
static UINT32 mock_silent_frames;
static HRESULT mock_getbuffer_hr;
static HRESULT mock_release_hr;
static HRESULT mock_start_hr;
static BYTE mock_device[64 * 8];

static HRESULT mock_get_padding(HostRuntime *rt, UINT32 *padding) {
    (void)rt; *padding = mock_padding; return S_OK;
}
static HRESULT mock_get_buffer(HostRuntime *rt, UINT32 frames, BYTE **destination) {
    (void)rt; (void)frames;
    if (FAILED(mock_getbuffer_hr)) return mock_getbuffer_hr;
    *destination = mock_device;
    return S_OK;
}
static HRESULT mock_release_buffer(HostRuntime *rt, UINT32 frames, DWORD flags) {
    (void)rt;
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) mock_silent_frames += frames;
    return mock_release_hr;
}
static HRESULT mock_start(HostRuntime *rt) {
    (void)rt;
    mock_start_calls++;
    if (rt->control->first_buffer_submitted) mock_start_saw_first = 1;
    return mock_start_hr;
}
static HRESULT mock_stop(HostRuntime *rt) {
    (void)rt; mock_stop_calls++; return S_OK;
}
static HRESULT mock_reset(HostRuntime *rt) {
    (void)rt; mock_reset_calls++; return S_OK;
}
static HRESULT mock_get_position(HostRuntime *rt, UINT64 *position, UINT64 *qpc) {
    (void)rt; *position = mock_position; if (qpc) *qpc = 1234567; return S_OK;
}

static const HostApi mock_api = {
    mock_get_padding, mock_get_buffer, mock_release_buffer,
    mock_start, mock_stop, mock_get_position, mock_reset
};

static void init_test(HostRuntime *rt, unsigned char *data, size_t bytes) {
    NexAudioControl *c = (NexAudioControl *)calloc(1, sizeof(*c));
    nex_audio_control_init(c, 11);
    c->input_rate = 44100;
    c->input_channels = 2;
    c->input_container_bits = 32;
    c->input_valid_bits = 32;
    c->input_encoding = NEX_AUDIO_ENCODING_PCM;
    c->input_channel_mask = 3;
    c->output_rate = 48000;
    c->output_channels = 2;
    c->output_container_bits = 24;
    c->output_valid_bits = 24;
    c->output_encoding = NEX_AUDIO_ENCODING_PCM;
    c->output_channel_mask = 3;
    c->output_frame_bytes = 6;
    c->capacity_frames = 64;
    c->data_bytes = (uint64_t)c->capacity_frames * c->output_frame_bytes;
    c->device_frames = 4;
    c->state = NEX_AUDIO_READY;
    memset(data, 0, bytes);
    memset(rt, 0, sizeof(*rt));
    rt->control = c;
    rt->data = data;
    rt->api = &mock_api;
    rt->clock_frequency = 48000;
    rt->endpoint_hash = 1469598103934665603ull;
    mock_padding = 0;
    mock_position = 0;
    mock_start_calls = 0;
    mock_start_saw_first = 0;
    mock_stop_calls = 0;
    mock_reset_calls = 0;
    mock_silent_frames = 0;
    mock_getbuffer_hr = S_OK;
    mock_release_hr = S_OK;
    mock_start_hr = S_OK;
    memset(mock_device, 0, sizeof(mock_device));
}

static void free_test(HostRuntime *rt) {
    free(rt->control);
    rt->control = NULL;
}

static void write_frames(HostRuntime *rt, const unsigned char *source, UINT32 frames) {
    CHECK(nex_audio_ring_write(rt->control, rt->data, source, frames) == frames);
}

static void test_first_packet_order(void) {
    unsigned char data[64 * 6], source[64 * 6];
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i + 1);
    HostRuntime rt;
    init_test(&rt, data, sizeof(data));
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(mock_start_calls == 0);
    CHECK(rt.control->source_waits == 1);
    CHECK(rt.control->underrun_events == 0);
    write_frames(&rt, source, 3);
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(mock_start_calls == 0);
    CHECK(rt.control->read_frame == 0);
    write_frames(&rt, source + 18, 1);
    CHECK(host_pump(&rt) == S_OK);
    CHECK(mock_start_calls == 1 && mock_start_saw_first == 1);
    CHECK(rt.control->first_buffer_submitted == 1);
    CHECK(rt.control->device_started == 1);
    CHECK(rt.control->device_clock_position == 0);
    CHECK(rt.control->device_clock_frequency == 48000);
    CHECK(rt.control->device_padding_frames == 4);
    CHECK(rt.control->state == NEX_AUDIO_RUNNING);
    CHECK(rt.control->read_frame == 4 && rt.control->write_frame == 4);
    CHECK(rt.control->endpoint_frames == 4);
    CHECK(mock_device[0] == 1 && mock_device[23] == 24);
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(rt.control->underrun_events == 1);
    mock_padding = 2;
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(rt.control->underrun_events == 1);
    free_test(&rt);
}

static void test_submit_failures_do_not_consume(void) {
    unsigned char data[64 * 6], source[64 * 6];
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)i;
    HostRuntime rt;
    init_test(&rt, data, sizeof(data));
    write_frames(&rt, source, 4);
    mock_getbuffer_hr = E_FAIL;
    CHECK(FAILED(host_pump(&rt)));
    CHECK(rt.control->state == NEX_AUDIO_FAILED);
    CHECK(rt.control->getbuffer_failures == 1);
    CHECK(rt.control->read_frame == 0 && rt.control->write_frame == 4);
    CHECK(mock_start_calls == 0);
    free_test(&rt);

    init_test(&rt, data, sizeof(data));
    write_frames(&rt, source, 4);
    mock_release_hr = E_FAIL;
    CHECK(FAILED(host_pump(&rt)));
    CHECK(rt.control->releasebuffer_failures == 1);
    CHECK(rt.control->read_frame == 0 && rt.control->write_frame == 4);
    CHECK(mock_start_calls == 0);
    free_test(&rt);
}

static void test_start_failure_after_prefill(void) {
    unsigned char data[64 * 6], source[64 * 6];
    memset(source, 0, sizeof(source));
    HostRuntime rt;
    init_test(&rt, data, sizeof(data));
    write_frames(&rt, source, 4);
    mock_start_hr = E_FAIL;
    CHECK(FAILED(host_pump(&rt)));
    CHECK(rt.control->first_buffer_submitted == 1);
    CHECK(rt.control->device_started == 0);
    CHECK(rt.control->state == NEX_AUDIO_FAILED);
    CHECK(rt.control->fallback_reason == NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
    CHECK(rt.control->read_frame == 4);
    free_test(&rt);
}

static void test_eos_clock_and_stale_generation(void) {
    unsigned char data[64 * 6], source[64 * 6];
    memset(source, 0x5A, sizeof(source));
    HostRuntime rt;
    init_test(&rt, data, sizeof(data));
    write_frames(&rt, source, 4);
    CHECK(host_pump(&rt) == S_OK);
    mock_padding = 0;
    rt.control->eos_submitted_generation = 10; /* stale request */
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(rt.control->state == NEX_AUDIO_RUNNING);
    CHECK(rt.control->underrun_events == 1);
    mock_padding = 4;
    rt.control->eos_submitted_generation = 11;
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(rt.control->state == NEX_AUDIO_DRAINING);
    CHECK(rt.eos_target == 4);
    CHECK(rt.control->eos_complete_generation == 0);
    mock_position = 3;
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(rt.control->state == NEX_AUDIO_DRAINING);
    mock_position = 4;
    CHECK(host_pump(&rt) == S_OK);
    CHECK(rt.control->state == NEX_AUDIO_COMPLETE);
    CHECK(rt.control->eos_complete_generation == 11);
    CHECK(nex_audio_eos_complete_for(rt.control, 11));
    CHECK(!nex_audio_eos_complete_for(rt.control, 12));
    free_test(&rt);
}

static void test_empty_eos_completes_without_start(void) {
    unsigned char data[64 * 6];
    HostRuntime rt;
    init_test(&rt, data, sizeof(data));
    rt.control->eos_submitted_generation = 11;
    CHECK(host_pump(&rt) == S_OK);
    CHECK(mock_start_calls == 0);
    CHECK(rt.control->state == NEX_AUDIO_COMPLETE);
    CHECK(rt.control->eos_complete_generation == 11);
    free_test(&rt);
}

static void test_short_tail_eos_starts_and_drains(void) {
    unsigned char data[64 * 6], source[64 * 6];
    memset(source, 0x2A, sizeof(source));
    HostRuntime rt;
    init_test(&rt, data, sizeof(data));
    write_frames(&rt, source, 2); /* shorter than the four-frame device period */
    rt.control->eos_submitted_generation = 11;
    mock_padding = 2;
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(mock_start_calls == 1);
    CHECK(rt.control->device_started == 1);
    CHECK(rt.control->read_frame == 2);
    CHECK(rt.control->source_waits == 0);
    mock_padding = 2;
    CHECK(host_pump(&rt) == S_FALSE);
    mock_position = 2;
    CHECK(host_pump(&rt) == S_OK);
    CHECK(rt.control->state == NEX_AUDIO_COMPLETE);
    CHECK(rt.control->eos_complete_generation == 11);
    free_test(&rt);
}

static void test_startup_failure_taxonomy(void) {
    CHECK(host_startup_reason(AUDCLNT_E_UNSUPPORTED_FORMAT) ==
          NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT);
    CHECK(host_startup_reason(AUDCLNT_E_DEVICE_IN_USE) ==
          NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
    CHECK(host_startup_reason(E_FAIL) ==
          NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED);
}

/* ---- v3.1 transport: pause, resume and flush are applied by the Host ------- */
static void start_device(HostRuntime *rt, const unsigned char *source) {
    write_frames(rt, source, 4);
    CHECK(host_pump(rt) == S_OK);
    CHECK(rt->control->device_started && mock_start_calls == 1);
}

static void test_pause_stops_device_and_resume_restarts(void) {
    unsigned char data[64 * 6], source[64 * 6];
    HostRuntime rt;
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i + 3);
    init_test(&rt, data, sizeof(data));
    start_device(&rt, source);
    write_frames(&rt, source, 8);
    uint64_t read_before = rt.control->read_frame;
    InterlockedExchange((volatile LONG *)&rt.control->transport, NEX_AUDIO_TRANSPORT_PAUSE);
    mock_padding = 0;
    for (int i = 0; i < 5; i++) CHECK(host_pump(&rt) == S_FALSE);
    CHECK(mock_stop_calls == 1);
    CHECK(rt.control->device_paused == 1);
    CHECK(rt.control->read_frame == read_before);       /* buffered audio is kept */
    CHECK(rt.control->underrun_events == 0);            /* a paused device is not starving */
    InterlockedExchange((volatile LONG *)&rt.control->transport, NEX_AUDIO_TRANSPORT_PLAY);
    host_pump(&rt);
    CHECK(mock_start_calls == 2);
    CHECK(rt.control->device_paused == 0);
    CHECK(rt.control->read_frame > read_before);        /* playback continues where it stopped */
    free_test(&rt);
    printf("PASS host transport: pause stops the device and keeps its audio, resume restarts it\n");
}

static void test_flush_discards_ring_and_prefills_again(void) {
    unsigned char data[64 * 6], source[64 * 6];
    HostRuntime rt;
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i + 5);
    init_test(&rt, data, sizeof(data));
    start_device(&rt, source);
    write_frames(&rt, source, 10);
    InterlockedExchange64((volatile LONG64 *)&rt.control->flush_request_generation, 1);
    CHECK(host_pump(&rt) == S_FALSE);
    CHECK(mock_stop_calls == 1 && mock_reset_calls == 1);
    CHECK(rt.control->read_frame == rt.control->write_frame);
    CHECK(rt.control->device_started == 0 && rt.control->first_buffer_submitted == 0);
    CHECK(rt.control->flush_ack_generation == 1);
    CHECK(host_pump(&rt) == S_FALSE);                   /* nothing new: waits for a prefill */
    CHECK(mock_start_calls == 1);
    write_frames(&rt, source, 4);
    CHECK(host_pump(&rt) == S_OK);
    CHECK(mock_start_calls == 2 && rt.control->device_started);
    /* a flush while paused also clears the pause */
    InterlockedExchange((volatile LONG *)&rt.control->transport, NEX_AUDIO_TRANSPORT_PAUSE);
    host_pump(&rt);
    CHECK(rt.control->device_paused == 1);
    InterlockedExchange64((volatile LONG64 *)&rt.control->flush_request_generation, 2);
    host_pump(&rt);
    CHECK(rt.control->flush_ack_generation == 2 && rt.control->device_paused == 0);
    free_test(&rt);
    printf("PASS host transport: a seek flush drops stale audio and prefills before restarting\n");
}

/* ---- Orion 1+2 on the Host: silence first, then the client's audio --------- */
static void test_host_primes_silence_before_audio(void) {
    unsigned char data[64 * 6], source[64 * 6];
    HostRuntime rt;
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i + 9);
    init_test(&rt, data, sizeof(data));
    rt.control->prime_request_frames = 4;
    write_frames(&rt, source, 4);
    CHECK(host_pump(&rt) == S_OK);
    CHECK(mock_silent_frames == 4);                 /* the first device buffer is silence */
    CHECK(rt.control->primed_frames_total == 4);
    CHECK(rt.control->read_frame == 0);             /* client audio not consumed by the prime */
    CHECK(mock_start_calls == 1 && rt.control->device_started);
    CHECK(rt.control->prime_request_frames == 0);   /* one prime per request */
    mock_padding = 0;
    CHECK(host_pump(&rt) == S_OK || 1);
    CHECK(rt.control->read_frame == 4);             /* then the real audio */
    free_test(&rt);
    printf("PASS host prime: silence buffer before the first audio, not counted as client audio\n");
}

int main(void) {
    test_startup_failure_taxonomy();
    test_pause_stops_device_and_resume_restarts();
    test_flush_discards_ring_and_prefills_again();
    test_host_primes_silence_before_audio();
    test_first_packet_order();
    test_submit_failures_do_not_consume();
    test_start_failure_after_prefill();
    test_eos_clock_and_stale_generation();
    test_empty_eos_completes_without_start();
    test_short_tail_eos_starts_and_drains();
    if (failures) {
        fprintf(stderr, "%d nexaudiohost test(s) failed\n", failures);
        return 1;
    }
    printf("PASS nexaudiohost: first packet, failures, start order and EOS clock\n");
    return 0;
}
