#include "../tools/nexaudiolink.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static void valid_control(NexAudioControl *c) {
    nex_audio_control_init(c, 7);
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
    c->capacity_frames = 24000;
    c->data_bytes = (uint64_t)c->capacity_frames * c->output_frame_bytes;
}

static void test_layout(void) {
    NexAudioControl c;
    valid_control(&c);
    CHECK(NEX_AUDIO_MAGIC != 0);
    CHECK(NEX_AUDIO_PROTOCOL_MAJOR == 3);
    CHECK(NEX_AUDIO_PROTOCOL_MINOR == 0);
    CHECK(sizeof(NexAudioControl) == NEX_AUDIO_CONTROL_BYTES);
    CHECK(sizeof(c.state) == sizeof(uint32_t));
    CHECK(sizeof(c.error) == sizeof(uint32_t));
    CHECK(sizeof(c.write_frame) == sizeof(uint64_t));
    CHECK(sizeof(c.read_frame) == sizeof(uint64_t));
    CHECK(c.build_id != 0);
    CHECK(c.output_frame_bytes > 0);
    CHECK(nex_audio_data_bytes_for(&c) == c.data_bytes);
    CHECK(c.engine == NEX_AUDIO_ENGINE_WASAPI);
    CHECK(c.deployment == NEX_AUDIO_DEPLOYMENT_AUDIOHOST);
    CHECK(c.stream_generation == 7);
    CHECK(c.format_generation == 7);
    CHECK(nex_audio_next_format_generation(&c) == 8);
    CHECK(c.format_generation == 8);
    CHECK(nex_audio_handshake_error(&c) == NEX_AUDIO_FALLBACK_NONE);
    CHECK(nex_audio_output_format_valid(&c));
}

static void test_handshake_rejections(void) {
    NexAudioControl c;
    valid_control(&c);
    c.protocol_major = 2;
    CHECK(nex_audio_handshake_error(&c) == NEX_AUDIO_FALLBACK_PROTOCOL_MISMATCH);
    valid_control(&c); c.layout_version++;
    CHECK(nex_audio_handshake_error(&c) == NEX_AUDIO_FALLBACK_LAYOUT_MISMATCH);
    valid_control(&c); c.build_id++;
    CHECK(nex_audio_handshake_error(&c) == NEX_AUDIO_FALLBACK_BUILD_MISMATCH);
    valid_control(&c); c.engine = NEX_AUDIO_ENGINE_DLNA;
    CHECK(nex_audio_handshake_error(&c) == NEX_AUDIO_FALLBACK_UNSUPPORTED_ENGINE);
    valid_control(&c); c.deployment = NEX_AUDIO_DEPLOYMENT_IN_PROCESS;
    CHECK(nex_audio_handshake_error(&c) == NEX_AUDIO_FALLBACK_UNSUPPORTED_DEPLOYMENT);
    valid_control(&c); c.magic = 0;
    CHECK(nex_audio_handshake_error(&c) == NEX_AUDIO_FALLBACK_PROTOCOL_MISMATCH);
}

static void test_format_and_mapping_rejections(void) {
    NexAudioControl c;
    valid_control(&c); c.output_frame_bytes = 0;
    CHECK(!nex_audio_output_format_valid(&c));
    valid_control(&c); c.output_frame_bytes = 8;
    CHECK(!nex_audio_output_format_valid(&c));
    valid_control(&c); c.capacity_frames = 0; c.data_bytes = 0;
    CHECK(!nex_audio_output_format_valid(&c));
    valid_control(&c); c.output_valid_bits = 25;
    CHECK(!nex_audio_output_format_valid(&c));
    valid_control(&c); c.output_encoding = NEX_AUDIO_ENCODING_FLOAT;
    CHECK(!nex_audio_output_format_valid(&c));
    valid_control(&c); c.output_container_bits = 32; c.output_frame_bytes = 8;
    c.output_encoding = NEX_AUDIO_ENCODING_FLOAT; c.output_valid_bits = 32;
    c.data_bytes = (uint64_t)c.capacity_frames * c.output_frame_bytes;
    CHECK(nex_audio_output_format_valid(&c));
    valid_control(&c); c.capacity_frames = UINT32_MAX;
    c.output_frame_bytes = UINT32_MAX;
    CHECK(nex_audio_data_bytes_for(&c) == 0);
    CHECK(!nex_audio_output_format_valid(&c));
    valid_control(&c); c.data_bytes--;
    CHECK(!nex_audio_output_format_valid(&c));
}

static void test_ring_all_or_nothing(void) {
    NexAudioControl c;
    valid_control(&c);
    c.capacity_frames = 4;
    c.output_frame_bytes = 6;
    c.data_bytes = 24;
    unsigned char data[24], source[36], destination[24];
    memset(data, 0, sizeof(data));
    memset(destination, 0, sizeof(destination));
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (unsigned char)(i + 1);
    CHECK(nex_audio_ring_write(&c, data, source, 3) == 3);
    CHECK(c.write_frame == 3 && c.read_frame == 0);
    CHECK(nex_audio_ring_write(&c, data, source + 18, 2) == 0);
    CHECK(c.write_frame == 3 && c.read_frame == 0);
    CHECK(c.dropped_frames == 0);
    CHECK(nex_audio_ring_read(&c, data, destination, 2) == 2);
    CHECK(c.read_frame == 2 && c.write_frame == 3);
    CHECK(memcmp(destination, source, 12) == 0);
    CHECK(nex_audio_ring_write(&c, data, source + 12, 3) == 3);
    CHECK(c.write_frame == 6 && c.read_frame == 2);
    CHECK(nex_audio_ring_read(&c, data, destination, 4) == 4);
    CHECK(destination[0] == 13 && destination[11] == 18);
    CHECK(destination[12] == 19 && destination[23] == 30);
    CHECK(c.read_frame == 6 && c.write_frame == 6);
}

static void test_eos_generation(void) {
    NexAudioControl c;
    valid_control(&c);
    c.stream_generation = 10;
    c.eos_submitted_generation = 10;
    c.eos_complete_generation = 9;
    CHECK(!nex_audio_eos_complete_for(&c, 10));
    c.eos_complete_generation = 10;
    CHECK(nex_audio_eos_complete_for(&c, 10));
    CHECK(!nex_audio_eos_complete_for(&c, 11));
    c.stream_generation = 11;
    CHECK(!nex_audio_eos_complete_for(&c, 11));
}

int main(void) {
    test_layout();
    test_handshake_rejections();
    test_format_and_mapping_rejections();
    test_ring_all_or_nothing();
    test_eos_generation();
    if (failures) {
        fprintf(stderr, "%d nexaudiolink test(s) failed\n", failures);
        return 1;
    }
    printf("PASS nexaudiolink: v3 layout, handshake, mapping, ring and EOS generation\n");
    return 0;
}
