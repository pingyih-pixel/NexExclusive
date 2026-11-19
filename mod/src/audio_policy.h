/*
 * NexExclusive audio negotiation policy.
 *
 * This header intentionally has no Windows dependency so the candidate rules
 * can be exercised from both the injected C translation unit and a C++ test.
 * It describes candidates only; endpoint probing and sample conversion remain
 * in the caller.
 */
#ifndef NEX_AUDIO_POLICY_H
#define NEX_AUDIO_POLICY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEX_AUDIO_POLICY_MAX_RATES 16
#define NEX_AUDIO_POLICY_MAX_DEPTHS 12

typedef struct NexAudioPolicyInput {
    int source_rate;
    int source_container_bits;
    int source_valid_bits;
    int source_is_float;
    int requested_output_bits;
    int forced_rate;
    int compatibility;
    int hifi;
    int source_preference;
} NexAudioPolicyInput;

typedef struct NexAudioPolicyDepth {
    int container_bits;
    int valid_bits;
    int is_float;
} NexAudioPolicyDepth;

static int nex_audio_policy_push_rate(int *out, int count, int capacity, int rate) {
    int i;
    if (!out || rate <= 0 || count >= capacity) return count;
    for (i = 0; i < count; i++) if (out[i] == rate) return count;
    out[count] = rate;
    return count + 1;
}

static int nex_audio_policy_integer_ratio(int a, int b) {
    if (a <= 0 || b <= 0) return 0;
    return (a % b == 0) || (b % a == 0);
}

static int nex_audio_policy_ratio_factor(int a, int b) {
    if (a <= 0 || b <= 0) return 0;
    return a > b ? a / b : b / a;
}

/* Keep the source rate in slot zero. Among converted rates, integer ratios
   precede non-integer ratios and smaller integer factors precede larger ones. */
static void nex_audio_policy_prefer_integer_ratios(int *rates, int count, int source_rate) {
    int i;
    if (!rates || count < 2) return;
    for (i = 1; i < count; i++) {
        int value = rates[i];
        int value_integer = nex_audio_policy_integer_ratio(source_rate, value);
        int value_factor = value_integer ? nex_audio_policy_ratio_factor(source_rate, value) : 0;
        int j = i;
        while (j > 1) {
            int previous = rates[j - 1];
            int previous_integer = nex_audio_policy_integer_ratio(source_rate, previous);
            int previous_factor = previous_integer ?
                nex_audio_policy_ratio_factor(source_rate, previous) : 0;
            int before = value_integer > previous_integer ||
                         (value_integer == previous_integer && value_integer &&
                          value_factor < previous_factor);
            if (!before) break;
            rates[j] = previous;
            j--;
        }
        rates[j] = value;
    }
}

/* Build the rate order used by endpoint negotiation. A forced rate and strict
   native mode intentionally produce one candidate only. */
static int nex_audio_policy_build_rates(const NexAudioPolicyInput *policy,
                                        const int *pool, int pool_count,
                                        int endpoint_mix_rate, int *out, int capacity) {
    int count = 0;
    int source_rate;
    int i;
    if (!policy || !out || capacity <= 0) return 0;
    source_rate = policy->forced_rate > 0 ? policy->forced_rate : policy->source_rate;
    count = nex_audio_policy_push_rate(out, count, capacity, source_rate);
    if (policy->forced_rate > 0 || policy->hifi || !policy->compatibility) return count;
    for (i = 0; i < pool_count; i++) {
        if (pool[i] > policy->source_rate)
            count = nex_audio_policy_push_rate(out, count, capacity, pool[i]);
    }
    for (i = pool_count - 1; i >= 0; i--) {
        if (pool[i] < policy->source_rate)
            count = nex_audio_policy_push_rate(out, count, capacity, pool[i]);
    }
    count = nex_audio_policy_push_rate(out, count, capacity, endpoint_mix_rate);
    if (policy->source_preference == 1)
        nex_audio_policy_prefer_integer_ratios(out, count, policy->source_rate);
    return count;
}

static int nex_audio_policy_push_depth(NexAudioPolicyDepth *out, int count,
                                       int capacity, int container_bits,
                                       int valid_bits, int is_float) {
    int i;
    if (!out || count >= capacity || container_bits <= 0 || valid_bits <= 0 ||
        valid_bits > container_bits) return count;
    if (is_float && (container_bits != 32 || valid_bits != 32)) return count;
    for (i = 0; i < count; i++) {
        if (out[i].container_bits == container_bits &&
            out[i].valid_bits == valid_bits && out[i].is_float == is_float)
            return count;
    }
    out[count].container_bits = container_bits;
    out[count].valid_bits = valid_bits;
    out[count].is_float = is_float;
    return count + 1;
}

static int nex_audio_policy_valid_source_depth(const NexAudioPolicyInput *policy) {
    if (!policy) return 0;
    if (policy->source_is_float)
        return policy->source_container_bits == 32 && policy->source_valid_bits == 32;
    return (policy->source_container_bits == 16 ||
            policy->source_container_bits == 24 ||
            policy->source_container_bits == 32) &&
           (policy->source_valid_bits == 16 ||
            policy->source_valid_bits == 24 ||
            policy->source_valid_bits == 32) &&
           policy->source_valid_bits <= policy->source_container_bits;
}

/* Build depth candidates. The source representation is retained as a
   separate candidate, including 16-valid samples in 16, 24 and 32-bit
   containers. This keeps valid precision distinct from byte container width
   when devices expose several spellings at the same sample rate. */
static int nex_audio_policy_build_depths(const NexAudioPolicyInput *policy,
                                         NexAudioPolicyDepth *out, int capacity) {
    int count = 0;
    int requested;
    if (!policy || !out || capacity <= 0) return 0;

    if (policy->hifi || !policy->compatibility) {
        if (nex_audio_policy_valid_source_depth(policy)) {
            count = nex_audio_policy_push_depth(out, count, capacity,
                                                policy->source_container_bits,
                                                policy->source_valid_bits,
                                                policy->source_is_float);
        }
        return count;
    }

    requested = policy->requested_output_bits;
    if (requested == 32)
        count = nex_audio_policy_push_depth(out, count, capacity, 32, 32, 1);
    else if (requested == 16 || requested == 24)
        count = nex_audio_policy_push_depth(out, count, capacity, requested, requested, 0);

    if (nex_audio_policy_valid_source_depth(policy)) {
        if (!policy->source_is_float && policy->source_valid_bits == 16) {
            /* Keep all three container spellings together. The valid precision
               remains 16 bits even when an endpoint advertises 24/32 bytes. */
            count = nex_audio_policy_push_depth(out, count, capacity, 16, 16, 0);
            count = nex_audio_policy_push_depth(out, count, capacity, 24, 16, 0);
            count = nex_audio_policy_push_depth(out, count, capacity, 32, 16, 0);
        } else if (!policy->source_is_float && policy->source_valid_bits == 24) {
            count = nex_audio_policy_push_depth(out, count, capacity, 24, 24, 0);
            count = nex_audio_policy_push_depth(out, count, capacity, 32, 24, 0);
        } else {
            count = nex_audio_policy_push_depth(out, count, capacity,
                                                policy->source_container_bits,
                                                policy->source_valid_bits,
                                                policy->source_is_float);
        }
        if (policy->source_is_float)
            count = nex_audio_policy_push_depth(out, count, capacity, 32, 32, 1);
    }

    count = nex_audio_policy_push_depth(out, count, capacity, 32, 32, 1);
    count = nex_audio_policy_push_depth(out, count, capacity, 24, 24, 0);
    count = nex_audio_policy_push_depth(out, count, capacity, 32, 24, 0);
    count = nex_audio_policy_push_depth(out, count, capacity, 16, 16, 0);
    return count;
}

#ifdef __cplusplus
}
#endif

#endif
