#include "../src/audio_policy.h"
#include <cstdio>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } \
} while (0)

static bool has_depth(const NexAudioPolicyDepth *depths, int count,
                     int container_bits, int valid_bits, int is_float) {
    for (int i = 0; i < count; ++i) {
        if (depths[i].container_bits == container_bits &&
            depths[i].valid_bits == valid_bits &&
            depths[i].is_float == is_float) return true;
    }
    return false;
}

static int depth_index(const NexAudioPolicyDepth *depths, int count,
                       int container_bits, int valid_bits, int is_float) {
    for (int i = 0; i < count; ++i) {
        if (depths[i].container_bits == container_bits &&
            depths[i].valid_bits == valid_bits &&
            depths[i].is_float == is_float) return i;
    }
    return count;
}

static void test_same_rate_depths() {
    NexAudioPolicyInput p{};
    NexAudioPolicyDepth depths[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    p.source_rate = 44100;
    p.source_container_bits = 32;
    p.source_valid_bits = 16;
    p.requested_output_bits = 24;
    p.compatibility = 1;
    int count = nex_audio_policy_build_depths(&p, depths, NEX_AUDIO_POLICY_MAX_DEPTHS);

    CHECK(has_depth(depths, count, 16, 16, 0));
    CHECK(has_depth(depths, count, 24, 16, 0));
    CHECK(has_depth(depths, count, 32, 16, 0));
    CHECK(depth_index(depths, count, 16, 16, 0) <
          depth_index(depths, count, 24, 16, 0));
    CHECK(depth_index(depths, count, 24, 16, 0) <
          depth_index(depths, count, 32, 16, 0));
    std::printf("PASS same-rate depth candidates preserve 16-bit source\n");
}

static void test_different_source_depths() {
    NexAudioPolicyInput p{};
    NexAudioPolicyDepth depths[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    p.source_rate = 44100;
    p.source_container_bits = 24;
    p.source_valid_bits = 24;
    p.requested_output_bits = 24;
    p.compatibility = 1;
    int count = nex_audio_policy_build_depths(&p, depths, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(has_depth(depths, count, 24, 24, 0));
    CHECK(has_depth(depths, count, 32, 24, 0));
    CHECK(!has_depth(depths, count, 16, 24, 0));
    std::printf("PASS same-rate 16/24/32 source depth separation\n");
}

static void test_strict_source() {
    NexAudioPolicyInput p{};
    NexAudioPolicyDepth depths[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    p.source_rate = 44100;
    p.source_container_bits = 32;
    p.source_valid_bits = 24;
    p.requested_output_bits = 16;
    p.compatibility = 0;
    int count = nex_audio_policy_build_depths(&p, depths, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(count == 1 && depths[0].container_bits == 32 && depths[0].valid_bits == 24);
    p.compatibility = 1;
    p.hifi = 1;
    count = nex_audio_policy_build_depths(&p, depths, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(count == 1 && depths[0].container_bits == 32 && depths[0].valid_bits == 24);
    std::printf("PASS strict policy keeps source representation\n");
}

static void test_source_rate_first() {
    const int pool[] = {44100, 48000, 88200, 96000};
    NexAudioPolicyInput p{};
    int rates[NEX_AUDIO_POLICY_MAX_RATES]{};
    p.source_rate = 44100;
    p.compatibility = 1;
    int count = nex_audio_policy_build_rates(&p, pool, 4, 48000,
                                             rates, NEX_AUDIO_POLICY_MAX_RATES);
    CHECK(count > 0 && rates[0] == 44100);
    CHECK(nex_audio_policy_integer_ratio(88200, 44100));
    std::printf("PASS source rate first\n");
}

int main() {
    test_same_rate_depths();
    test_different_source_depths();
    test_strict_source();
    test_source_rate_first();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
