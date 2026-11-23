#ifndef NEX_AUDIO_LINK_H
#define NEX_AUDIO_LINK_H

#include <windows.h>
#include <stdint.h>
#include <wchar.h>
#include <string.h>
#include <limits.h>

/* v3 separates the fixed control page from the sized PCM mapping. Both sides
   must come from the same wire build; a version-only match is not enough. */
#define NEX_AUDIO_MAGIC                 0x4E455841u /* NEXA */
#define NEX_AUDIO_PROTOCOL_MAJOR        3u
#define NEX_AUDIO_PROTOCOL_MINOR        1u /* 3.1: transport */
#define NEX_AUDIO_LAYOUT_VERSION        3u
/* The build system supplies this from the wire sources.  A Host binary carries
   its own expected value through NEX_AUDIO_EXPECTED_BUILD_ID instead of
   borrowing a header literal at runtime. */
#ifndef NEX_AUDIO_BUILD_ID
#error "NEX_AUDIO_BUILD_ID must be supplied by the build"
#endif
#ifndef NEX_AUDIO_EXPECTED_BUILD_ID
#define NEX_AUDIO_EXPECTED_BUILD_ID     NEX_AUDIO_BUILD_ID
#endif
#define NEX_AUDIO_ENDPOINT_CHARS        256u
#define NEX_AUDIO_NETWORK_CHARS         512u
#define NEX_AUDIO_MAPPING_CHARS         96u
#define NEX_AUDIO_MIN_RING_FRAMES       16384u
#define NEX_AUDIO_BACKPRESSURE_TIMEOUT_MS 1000u
#define NEX_AUDIO_MAX_DATA_BYTES        (512ULL * 1024ULL * 1024ULL)

#define NEX_AUDIO_ENGINE_WASAPI         0u
#define NEX_AUDIO_ENGINE_DLNA           1u /* reserved, not implemented */
#define NEX_AUDIO_ENGINE_RAAT           2u /* reserved, not implemented */
#define NEX_AUDIO_ENGINE_NONE           0xFFFFFFFFu

#define NEX_AUDIO_DEPLOYMENT_IN_PROCESS 0u
#define NEX_AUDIO_DEPLOYMENT_AUDIOHOST  1u

/* v3.1 transport, written by the client and applied by the Host, which is
   the only side that owns the device. */
#define NEX_AUDIO_TRANSPORT_PLAY        0u
#define NEX_AUDIO_TRANSPORT_PAUSE       1u

#define NEX_AUDIO_ENCODING_PCM          0u
#define NEX_AUDIO_ENCODING_FLOAT        1u

#define NEX_AUDIO_INIT                  0L
#define NEX_AUDIO_NEGOTIATING           1L
#define NEX_AUDIO_READY                 2L
#define NEX_AUDIO_PREFILLED             3L
#define NEX_AUDIO_RUNNING               4L
#define NEX_AUDIO_DRAINING              5L
#define NEX_AUDIO_COMPLETE              6L
#define NEX_AUDIO_CLOSING               7L
#define NEX_AUDIO_FAILED                8L

#define NEX_AUDIO_FALLBACK_NONE                 0u
#define NEX_AUDIO_FALLBACK_PROTOCOL_MISMATCH    1u
#define NEX_AUDIO_FALLBACK_LAYOUT_MISMATCH      2u
#define NEX_AUDIO_FALLBACK_BUILD_MISMATCH       3u
#define NEX_AUDIO_FALLBACK_UNSUPPORTED_ENGINE   4u
#define NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT   5u
#define NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED 6u
#define NEX_AUDIO_FALLBACK_RING_BACKPRESSURE    7u
#define NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST  8u
#define NEX_AUDIO_FALLBACK_HOST_EXIT            9u
#define NEX_AUDIO_FALLBACK_MAPPING_FAILED       10u
#define NEX_AUDIO_FALLBACK_UNSUPPORTED_DEPLOYMENT 11u

/* All counters are monotonic within one stream generation. Positions are in
   output frames, never bytes. The control page contains no variable PCM array. */
typedef struct NexAudioControl {
    uint32_t magic;
    uint16_t protocol_major;
    uint16_t protocol_minor;
    uint32_t layout_version;
    uint64_t build_id;
    uint32_t engine;
    uint32_t deployment;
    volatile uint32_t state;
    volatile uint32_t error;
    uint64_t stream_generation;
    uint64_t format_generation;

    uint32_t input_rate;
    uint16_t input_channels;
    uint16_t input_container_bits;
    uint16_t input_valid_bits;
    uint16_t input_encoding;
    uint32_t input_channel_mask;
    uint32_t output_rate;
    uint16_t output_channels;
    uint16_t output_container_bits;
    uint16_t output_valid_bits;
    uint16_t output_encoding;
    uint32_t output_channel_mask;
    uint32_t output_frame_bytes;
    uint32_t capacity_frames;
    uint64_t data_bytes;
    wchar_t data_mapping_name[NEX_AUDIO_MAPPING_CHARS];

    volatile uint64_t write_frame;
    volatile uint64_t read_frame;
    volatile uint8_t first_buffer_submitted;
    volatile uint8_t device_started;
    uint16_t reserved_flags;
    uint32_t device_frames;
    uint64_t device_latency_100ns;
    volatile uint64_t eos_submitted_generation;
    volatile uint64_t eos_complete_generation;

    volatile uint64_t source_waits;
    volatile uint64_t underrun_events;
    volatile uint64_t dropped_frames;
    volatile uint64_t backpressure_events;
    volatile uint64_t getbuffer_failures;
    volatile uint64_t releasebuffer_failures;
    uint32_t active_engine;
    uint32_t fallback_reason;

    volatile uint64_t client_frames;
    volatile uint64_t converted_frames;
    volatile uint64_t endpoint_frames;
    /* Host-owned device clock snapshot. Position is in clock ticks returned by
       IAudioClock, qpc is the matching performance-counter sample, and
       padding remains in output frames. */
    volatile uint64_t device_clock_position;
    volatile uint64_t device_clock_qpc;
    uint64_t device_clock_frequency;
    volatile uint32_t device_padding_frames;
    uint32_t reserved_clock;
    volatile uint64_t verified_frames;
    volatile uint64_t changed_bytes;
    volatile uint64_t client_hash;
    volatile uint64_t converted_hash;
    volatile uint64_t endpoint_hash;

    wchar_t target_id[NEX_AUDIO_ENDPOINT_CHARS];
    wchar_t network_location[NEX_AUDIO_NETWORK_CHARS];
    /* v3.1 transport (carved from the reserved tail; the page size is unchanged).
       transport: the client's Start/Stop. A pause stops the device and keeps
       the buffered audio. flush_request_generation: the client's Reset (seek);
       the Host stops and resets the device, discards the ring, prefills again
       and copies the value into flush_ack_generation. */
    volatile uint32_t transport;
    volatile uint32_t device_paused;
    volatile uint64_t flush_request_generation;
    volatile uint64_t flush_ack_generation;
    /* Start-up silence: the client asks for prime_request_frames of silence
       before the next first buffer; the Host submits it, clears the request and
       adds what it submitted to primed_frames_total (reset by a flush), which
       the client subtracts from the device clock. */
    volatile uint32_t prime_request_frames;
    uint32_t reserved_prime;
    volatile uint64_t primed_frames_total;
    unsigned char reserved[88];
} NexAudioControl;

#define NEX_AUDIO_CONTROL_BYTES ((uint32_t)sizeof(NexAudioControl))

static __attribute__((unused)) void nex_audio_control_init(NexAudioControl *c, uint64_t generation) {
    memset(c, 0, sizeof(*c));
    c->magic = NEX_AUDIO_MAGIC;
    c->protocol_major = NEX_AUDIO_PROTOCOL_MAJOR;
    c->protocol_minor = NEX_AUDIO_PROTOCOL_MINOR;
    c->layout_version = NEX_AUDIO_LAYOUT_VERSION;
    c->build_id = NEX_AUDIO_BUILD_ID;
    c->engine = NEX_AUDIO_ENGINE_WASAPI;
    c->deployment = NEX_AUDIO_DEPLOYMENT_AUDIOHOST;
    c->state = NEX_AUDIO_NEGOTIATING;
    c->stream_generation = generation;
    c->format_generation = generation;
    c->active_engine = NEX_AUDIO_ENGINE_NONE;
}

static __attribute__((unused)) uint32_t nex_audio_handshake_error(const NexAudioControl *c) {
    if (!c || c->magic != NEX_AUDIO_MAGIC ||
        c->protocol_major != NEX_AUDIO_PROTOCOL_MAJOR ||
        c->protocol_minor != NEX_AUDIO_PROTOCOL_MINOR)
        return NEX_AUDIO_FALLBACK_PROTOCOL_MISMATCH;
    if (c->layout_version != NEX_AUDIO_LAYOUT_VERSION)
        return NEX_AUDIO_FALLBACK_LAYOUT_MISMATCH;
    if (c->build_id != NEX_AUDIO_EXPECTED_BUILD_ID)
        return NEX_AUDIO_FALLBACK_BUILD_MISMATCH;
    if (c->engine != NEX_AUDIO_ENGINE_WASAPI)
        return NEX_AUDIO_FALLBACK_UNSUPPORTED_ENGINE;
    if (c->deployment != NEX_AUDIO_DEPLOYMENT_AUDIOHOST)
        return NEX_AUDIO_FALLBACK_UNSUPPORTED_DEPLOYMENT;
    return NEX_AUDIO_FALLBACK_NONE;
}

static __attribute__((unused)) uint64_t nex_audio_data_bytes_for(const NexAudioControl *c) {
    if (!c || !c->capacity_frames || !c->output_frame_bytes) return 0;
    uint64_t bytes = (uint64_t)c->capacity_frames * (uint64_t)c->output_frame_bytes;
    if (!bytes || bytes > NEX_AUDIO_MAX_DATA_BYTES || bytes > (uint64_t)SIZE_MAX)
        return 0;
    return bytes;
}

static __attribute__((unused)) int nex_audio_encoding_layout_valid(uint32_t rate, uint16_t channels,
                                           uint16_t container, uint16_t valid,
                                           uint16_t encoding) {
    if (rate < 8000 || rate > 384000 || !channels || channels > 8) return 0;
    if (container != 16 && container != 24 && container != 32) return 0;
    if (!valid || valid > container) return 0;
    if (encoding != NEX_AUDIO_ENCODING_PCM && encoding != NEX_AUDIO_ENCODING_FLOAT)
        return 0;
    if (encoding == NEX_AUDIO_ENCODING_FLOAT && (container != 32 || valid != 32))
        return 0;
    return 1;
}

static __attribute__((unused)) int nex_audio_output_format_valid(const NexAudioControl *c) {
    if (!c || !nex_audio_encoding_layout_valid(c->input_rate, c->input_channels,
            c->input_container_bits, c->input_valid_bits, c->input_encoding) ||
        !nex_audio_encoding_layout_valid(c->output_rate, c->output_channels,
            c->output_container_bits, c->output_valid_bits, c->output_encoding))
        return 0;
    uint32_t frame_bytes = (uint32_t)c->output_channels *
                           ((uint32_t)c->output_container_bits / 8u);
    uint64_t bytes = nex_audio_data_bytes_for(c);
    return frame_bytes && c->output_frame_bytes == frame_bytes && bytes &&
           c->data_bytes == bytes && c->stream_generation &&
           c->format_generation;
}

static __attribute__((unused)) uint64_t nex_audio_read64(volatile uint64_t *value) {
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)value, 0, 0);
}

static __attribute__((unused)) uint64_t nex_audio_next_format_generation(
    NexAudioControl *c) {
    if (!c) return 0;
    uint64_t old = nex_audio_read64(&c->format_generation);
    uint64_t next = old + 1;
    if (!next) next = 1;
    InterlockedExchange64((volatile LONG64 *)&c->format_generation,
                          (LONG64)next);
    return next;
}

static __attribute__((unused)) uint64_t nex_audio_ring_available(const NexAudioControl *c) {
    uint64_t write = nex_audio_read64((volatile uint64_t *)&c->write_frame);
    uint64_t read = nex_audio_read64((volatile uint64_t *)&c->read_frame);
    if (write < read) return 0;
    uint64_t used = write - read;
    return used > c->capacity_frames ? c->capacity_frames : used;
}

static __attribute__((unused)) uint32_t nex_audio_ring_write(NexAudioControl *c, unsigned char *data,
                                     const unsigned char *source, uint32_t frames) {
    if (!c || !data || !source || !frames || !c->capacity_frames ||
        !c->output_frame_bytes || c->data_bytes != nex_audio_data_bytes_for(c))
        return 0;
    uint64_t write = nex_audio_read64((volatile uint64_t *)&c->write_frame);
    uint64_t read = nex_audio_read64((volatile uint64_t *)&c->read_frame);
    if (write < read) return 0;
    uint64_t used = write - read;
    if (used > c->capacity_frames || frames > c->capacity_frames - (uint32_t)used)
        return 0;
    size_t block = c->output_frame_bytes;
    size_t total = (size_t)frames * block;
    size_t start = (size_t)(write % c->capacity_frames) * block;
    size_t first = (size_t)c->data_bytes - start;
    if (first > total) first = total;
    memcpy(data + start, source, first);
    if (first < total) memcpy(data, source + first, total - first);
    MemoryBarrier();
    InterlockedExchange64((volatile LONG64 *)&c->write_frame, (LONG64)(write + frames));
    return frames;
}

static __attribute__((unused)) uint32_t nex_audio_ring_peek(const NexAudioControl *c, const unsigned char *data,
                                    unsigned char *destination, uint32_t frames) {
    if (!c || !data || !destination || !frames || !c->capacity_frames ||
        !c->output_frame_bytes || c->data_bytes != nex_audio_data_bytes_for(c))
        return 0;
    uint64_t write = nex_audio_read64((volatile uint64_t *)&c->write_frame);
    uint64_t read = nex_audio_read64((volatile uint64_t *)&c->read_frame);
    if (write < read || write - read < frames || frames > c->capacity_frames)
        return 0;
    size_t block = c->output_frame_bytes;
    size_t total = (size_t)frames * block;
    size_t start = (size_t)(read % c->capacity_frames) * block;
    size_t first = (size_t)c->data_bytes - start;
    if (first > total) first = total;
    memcpy(destination, data + start, first);
    if (first < total) memcpy(destination + first, data, total - first);
    return frames;
}

static __attribute__((unused)) uint32_t nex_audio_ring_commit_read(NexAudioControl *c, uint32_t frames) {
    if (!c || !frames) return 0;
    uint64_t write = nex_audio_read64((volatile uint64_t *)&c->write_frame);
    uint64_t read = nex_audio_read64((volatile uint64_t *)&c->read_frame);
    if (write < read || write - read < frames) return 0;
    MemoryBarrier();
    InterlockedExchange64((volatile LONG64 *)&c->read_frame, (LONG64)(read + frames));
    return frames;
}

static __attribute__((unused)) uint32_t nex_audio_ring_read(NexAudioControl *c, const unsigned char *data,
                                    unsigned char *destination, uint32_t frames) {
    if (nex_audio_ring_peek(c, data, destination, frames) != frames) return 0;
    return nex_audio_ring_commit_read(c, frames);
}

static __attribute__((unused)) int nex_audio_eos_complete_for(const NexAudioControl *c, uint64_t generation) {
    return c && generation && c->stream_generation == generation &&
           nex_audio_read64((volatile uint64_t *)&c->eos_complete_generation) == generation;
}

static __attribute__((unused)) const char *nex_audio_state_name(long state) {
    switch (state) {
        case NEX_AUDIO_INIT: return "init";
        case NEX_AUDIO_NEGOTIATING: return "negotiating";
        case NEX_AUDIO_READY: return "ready";
        case NEX_AUDIO_PREFILLED: return "prefilled";
        case NEX_AUDIO_RUNNING: return "running";
        case NEX_AUDIO_DRAINING: return "draining";
        case NEX_AUDIO_COMPLETE: return "complete";
        case NEX_AUDIO_CLOSING: return "closing";
        case NEX_AUDIO_FAILED: return "failed";
        default: return "unknown";
    }
}

static __attribute__((unused)) const char *nex_audio_fallback_name(uint32_t reason) {
    switch (reason) {
        case NEX_AUDIO_FALLBACK_NONE: return "none";
        case NEX_AUDIO_FALLBACK_PROTOCOL_MISMATCH: return "protocol_mismatch";
        case NEX_AUDIO_FALLBACK_LAYOUT_MISMATCH: return "layout_mismatch";
        case NEX_AUDIO_FALLBACK_BUILD_MISMATCH: return "build_mismatch";
        case NEX_AUDIO_FALLBACK_UNSUPPORTED_ENGINE: return "unsupported_engine";
        case NEX_AUDIO_FALLBACK_UNSUPPORTED_FORMAT: return "unsupported_format";
        case NEX_AUDIO_FALLBACK_ENDPOINT_OPEN_FAILED: return "endpoint_open_failed";
        case NEX_AUDIO_FALLBACK_RING_BACKPRESSURE: return "ring_backpressure";
        case NEX_AUDIO_FALLBACK_RUNTIME_DEVICE_LOST: return "runtime_device_lost";
        case NEX_AUDIO_FALLBACK_HOST_EXIT: return "host_exit";
        case NEX_AUDIO_FALLBACK_MAPPING_FAILED: return "mapping_failed";
        case NEX_AUDIO_FALLBACK_UNSUPPORTED_DEPLOYMENT: return "unsupported_deployment";
        default: return "unknown";
    }
}

#endif
