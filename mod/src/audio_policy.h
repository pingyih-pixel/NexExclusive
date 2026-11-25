/*
 * NexExclusive audio policy.
 *
 * This header decides which output formats may be tried, in which order, which
 * combinations are legal, and what a failed probe means for the rest of the
 * search. It has no Windows dependency so the same rules are exercised from the
 * injected C translation unit and from a C++ test.
 *
 * It does not probe endpoints (sink_endpoint.inc), convert samples
 * (sink_dsp.inc) or validate the remote wire layout (nexaudiolink.h).
 */
#ifndef NEX_AUDIO_POLICY_H
#define NEX_AUDIO_POLICY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NEX_AUDIO_POLICY_FN static __attribute__((unused))
#else
#define NEX_AUDIO_POLICY_FN static
#endif

/* ---- legal range ---------------------------------------------------------- */

#define NEX_AUDIO_POLICY_MIN_RATE 8000
#define NEX_AUDIO_POLICY_MAX_RATE 384000
#define NEX_AUDIO_POLICY_MAX_CHANNELS 8

#define NEX_AUDIO_POLICY_MAX_RATES 16
#define NEX_AUDIO_POLICY_MAX_DEPTHS 12
#define NEX_AUDIO_POLICY_MAX_CANDIDATES \
    (NEX_AUDIO_POLICY_MAX_RATES * NEX_AUDIO_POLICY_MAX_DEPTHS)

/* Rates offered as conversion targets when the endpoint refuses the source
   rate. Ascending; the order of use is decided by build_rates. */
NEX_AUDIO_POLICY_FN const int *nex_audio_policy_rate_pool(int *count) {
    static const int pool[] = {44100, 48000, 88200, 96000,
                               176400, 192000, 352800, 384000};
    if (count) *count = (int)(sizeof(pool) / sizeof(pool[0]));
    return pool;
}

NEX_AUDIO_POLICY_FN int nex_audio_policy_rate_valid(int rate) {
    return rate >= NEX_AUDIO_POLICY_MIN_RATE && rate <= NEX_AUDIO_POLICY_MAX_RATE;
}

/* One PCM layout. valid_bits is the precision inside container_bits; a caller
   holding a plain WAVEFORMATEX passes the container width for both. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_format_valid(int rate, int channels,
                                                      int container_bits, int valid_bits,
                                                      int is_float, int block_align) {
    if (!nex_audio_policy_rate_valid(rate)) return 0;
    if (channels < 1 || channels > NEX_AUDIO_POLICY_MAX_CHANNELS) return 0;
    if (container_bits != 16 && container_bits != 24 && container_bits != 32) return 0;
    if (valid_bits < 1 || valid_bits > container_bits) return 0;
    if (block_align != channels * (container_bits / 8)) return 0;
    return !is_float || container_bits == 32;
}

/* ---- configuration -------------------------------------------------------- */

typedef struct NexAudioPolicyConfig {
    int out_bits;     /* 0 = follow the source; 16/24 reduce; 32 = float32     */
    int force_rate;   /* 0 = follow the source                                */
    int out_frames;   /* 0 = derive from the client's requested duration      */
    int compat;       /* allow rate/depth conversion when the source is refused */
    int dither;       /* 0 off, 1 on, 2 auto                                  */
    int fade_ms;      /* opening ramp; 0 (default) leaves samples untouched   */
    int src_prefer;   /* 0 bandwidth, 1 integer ratios first                  */
    int hifi;         /* strict native: the client format or nothing          */
} NexAudioPolicyConfig;

NEX_AUDIO_POLICY_FN NexAudioPolicyConfig nex_audio_policy_config_defaults(void) {
    NexAudioPolicyConfig c;
    c.out_bits = 0;
    c.force_rate = 0;
    c.out_frames = 0;
    c.compat = 1;
    c.dither = 2;
    c.fade_ms = 0;
    c.src_prefer = 0;
    c.hifi = 0;
    return c;
}

/* Repair out-of-range values to their defaults, then apply the strict-native
   invariant: that mode follows the client's format and refuses any processing
   that would make the output differ from the client PCM. */
NEX_AUDIO_POLICY_FN void nex_audio_policy_normalize_config(NexAudioPolicyConfig *c) {
    NexAudioPolicyConfig d = nex_audio_policy_config_defaults();
    if (!c) return;
    c->compat = c->compat != 0;
    c->hifi = c->hifi != 0;
    if (c->src_prefer != 0 && c->src_prefer != 1) c->src_prefer = d.src_prefer;
    if (c->out_bits != 0 && c->out_bits != 16 && c->out_bits != 24 && c->out_bits != 32)
        c->out_bits = d.out_bits;
    if (c->out_frames < 0 || c->out_frames > 32768) c->out_frames = d.out_frames;
    if (c->force_rate != 0 && !nex_audio_policy_rate_valid(c->force_rate))
        c->force_rate = d.force_rate;
    if (c->dither < 0 || c->dither > 2) c->dither = d.dither;
    if (c->fade_ms < 0 || c->fade_ms > 100) c->fade_ms = d.fade_ms;
    if (c->hifi) {
        c->force_rate = 0;
        c->compat = 0;
        c->dither = 0;
        c->fade_ms = 0;
    }
}

/* Dither buys something only when the integer depth actually drops. auto also
   requires fade_ms=0, because the opening ramp is applied after quantisation
   and would shape the noise for the first few milliseconds. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_dither_selected(int input_float, int input_valid,
                                                         int output_float, int output_valid,
                                                         int mode, int fade_ms) {
    if (output_float) return 0;
    if (mode == 1) return 1;
    return mode == 2 && output_valid < (input_float ? 24 : input_valid) && fade_ms == 0;
}

/* ---- start-up silence (after Orion's primeWithSilence / resync_delay_ms) ---- */
/* DACs run two crystals: one for 44.1 kHz multiples, one for 48 kHz multiples.
   Changing family makes the DAC relock, which mutes or pops the first notes. */
NEX_AUDIO_POLICY_FN long nex_audio_policy_rate_family(long rate) {
    if (rate <= 0) return 0;
    if (rate % 11025 == 0) return 44100;
    if (rate % 8000 == 0) return 48000;
    return rate;
}

/* Frames of silence to submit before the first client audio of a fresh device
   stream: one device buffer (prime), plus resync_ms when the DAC changes clock
   family. Silence is added, never mixed into the music. */
NEX_AUDIO_POLICY_FN long nex_audio_policy_prime_frames(long previous_rate, long rate,
                                                       long device_frames, int prime,
                                                       int resync_ms) {
    long frames = prime && device_frames > 0 ? device_frames : 0;
    if (previous_rate > 0 && rate > 0 && resync_ms > 0 &&
        nex_audio_policy_rate_family(previous_rate) != nex_audio_policy_rate_family(rate))
        frames += (long)((long long)rate * resync_ms / 1000);
    return frames;
}

/* How long a teardown may wait for the separate host to play out what it
   holds: the audio still buffered (ring + device), plus a scheduling margin.
   A fixed 5 s held the client's teardown for seconds whenever the Host stalled,
   although nothing longer than the buffered audio can legitimately be pending. */
#define NEX_AUDIO_EOS_MARGIN_MS 250
#define NEX_AUDIO_EOS_MIN_MS    300
#define NEX_AUDIO_EOS_MAX_MS    5000
NEX_AUDIO_POLICY_FN unsigned long nex_audio_policy_eos_wait_ms(unsigned long long buffered_frames,
                                                               long rate) {
    if (rate <= 0) return NEX_AUDIO_EOS_MAX_MS;
    unsigned long long ms = buffered_frames * 1000ull / (unsigned long long)rate
                          + NEX_AUDIO_EOS_MARGIN_MS;
    if (ms < NEX_AUDIO_EOS_MIN_MS) ms = NEX_AUDIO_EOS_MIN_MS;
    if (ms > NEX_AUDIO_EOS_MAX_MS) ms = NEX_AUDIO_EOS_MAX_MS;
    return (unsigned long)ms;
}

/* Event-driven exclusive mode hands the device one complete period per event.
   With a resampler in front, one period must fit in the resampler's input
   history (period * in/out + both kernel halves + slack), or the period can
   never be completed. Otherwise the timer-driven contract is used. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_event_fits(long in_rate, long out_rate,
                                                    long period_frames, long src_cap,
                                                    long src_half) {
    if (in_rate <= 0 || out_rate <= 0 || period_frames <= 0) return 0;
    if (in_rate == out_rate) return 1;
    long long need = ((long long)period_frames * in_rate + out_rate - 1) / out_rate
                   + 2LL * src_half + 64;
    return need <= src_cap;
}

/* Optional dedicated core for the drain thread (hook.cfg dedicated_core=1).
   One entry per physical core: its efficiency class (higher = faster core on
   hybrid CPUs, all 0 elsewhere) and its logical-processor mask in group 0.
   Picks the last core of the fastest class that does not hold logical CPU 0,
   where most device interrupts and DPCs land. -1: nothing worth pinning to. */
typedef struct NexAudioCoreInfo {
    unsigned char efficiency;
    unsigned long long mask;
} NexAudioCoreInfo;

NEX_AUDIO_POLICY_FN int nex_audio_policy_pick_core(const NexAudioCoreInfo *cores, int count) {
    if (!cores || count < 2) return -1;
    int best = -1;
    for (int i = 0; i < count; i++) {
        if (!cores[i].mask || (cores[i].mask & 1ull)) continue;
        if (best < 0 || cores[i].efficiency >= cores[best].efficiency) best = i;
    }
    return best;
}

/* Depth of the separate host's shared ring (hook.cfg host_ring_ms). A deeper
   ring rides out longer stalls of the client process; the client's own volume
   and effects reach the device that much later, since they are applied before
   the ring. Seek and pause are not delayed: both act on the Host directly. */
#define NEX_AUDIO_HOST_RING_DEFAULT_MS 500
#define NEX_AUDIO_HOST_RING_MIN_MS     200
#define NEX_AUDIO_HOST_RING_MAX_MS     4000
NEX_AUDIO_POLICY_FN int nex_audio_policy_host_ring_ms(int ms) {
    if (ms <= 0) return NEX_AUDIO_HOST_RING_DEFAULT_MS;
    if (ms < NEX_AUDIO_HOST_RING_MIN_MS) return NEX_AUDIO_HOST_RING_MIN_MS;
    if (ms > NEX_AUDIO_HOST_RING_MAX_MS) return NEX_AUDIO_HOST_RING_MAX_MS;
    return ms;
}

/* ---- letting go of the device during a pause (hook.cfg idle_release_ms) ---
   A paused exclusive stream still owns the endpoint: Stop leaves it
   initialized, and every other application gets AUDCLNT_E_DEVICE_IN_USE
   until it is released (measured on a USB DAC, Windows 11). After this many
   milliseconds of pause the device side is dropped; the client keeps its
   stream, ring and timeline, and the next Start rebuilds the device side.
   0 = never (default). */
#define NEX_AUDIO_IDLE_RELEASE_MIN_MS 1000
#define NEX_AUDIO_IDLE_RELEASE_MAX_MS 3600000
NEX_AUDIO_POLICY_FN int nex_audio_policy_idle_release_ms(int ms) {
    if (ms <= 0) return 0;
    if (ms < NEX_AUDIO_IDLE_RELEASE_MIN_MS) return NEX_AUDIO_IDLE_RELEASE_MIN_MS;
    if (ms > NEX_AUDIO_IDLE_RELEASE_MAX_MS) return NEX_AUDIO_IDLE_RELEASE_MAX_MS;
    return ms;
}

/* The release is decided late, on a timer, so it re-checks that nothing moved
   since the pause began: the stream is still engaged and stopped, it still has
   a device side, and it is the same pause (stop_token, taken at Stop, still
   matches the current one). A Start, a seek that rebuilt the device, or a new
   track in between all invalidate the token, and the timer does nothing. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_idle_release_due(int idle_ms, int engaged, int started,
                                                          int has_device,
                                                          unsigned long long stop_token,
                                                          unsigned long long current_token,
                                                          unsigned long paused_for_ms) {
    if (idle_ms <= 0 || !engaged || started || !has_device) return 0;
    if (!stop_token || stop_token != current_token) return 0;
    return paused_for_ms >= (unsigned long)idle_ms;
}

/* ---- automatic shared output (hook.cfg auto_shared, share_quiet_ms) --------
   Off by default: the product is exclusive output. With auto_shared=1, each
   time a device side is built (a new stream, a resume after the device was
   released, a seek while paused) the sink checks whether another application
   is sounding on that endpoint. If one is, or one was within share_quiet_ms,
   the stream opens in shared mode next to it instead of taking the device
   away; otherwise exclusive. The quiet window keeps a short gap in the other
   application's audio (between two videos, a buffering stall) from flipping
   the stream back and forth.
   others_known = 0 means the session scan itself failed: stay exclusive, the
   same as with the feature off.
   quiet_for_ms is how long ago another application was last heard;
   NEX_AUDIO_NEVER_HEARD when none has been. */
#define NEX_AUDIO_SHARE_QUIET_DEFAULT_MS 3000
#define NEX_AUDIO_SHARE_QUIET_MIN_MS     500
#define NEX_AUDIO_SHARE_QUIET_MAX_MS     60000
#define NEX_AUDIO_NEVER_HEARD            0xFFFFFFFFul
enum { NEX_AUDIO_OUTPUT_EXCLUSIVE = 0, NEX_AUDIO_OUTPUT_SHARED = 1 };
NEX_AUDIO_POLICY_FN int nex_audio_policy_share_quiet_ms(int ms) {
    if (ms <= 0) return NEX_AUDIO_SHARE_QUIET_DEFAULT_MS;
    if (ms < NEX_AUDIO_SHARE_QUIET_MIN_MS) return NEX_AUDIO_SHARE_QUIET_MIN_MS;
    if (ms > NEX_AUDIO_SHARE_QUIET_MAX_MS) return NEX_AUDIO_SHARE_QUIET_MAX_MS;
    return ms;
}
NEX_AUDIO_POLICY_FN int nex_audio_policy_output_mode(int auto_shared, int others_known,
                                                     int others_active,
                                                     unsigned long quiet_for_ms,
                                                     int quiet_ms) {
    if (!auto_shared || !others_known) return NEX_AUDIO_OUTPUT_EXCLUSIVE;
    if (others_active) return NEX_AUDIO_OUTPUT_SHARED;
    if (quiet_for_ms < (unsigned long)quiet_ms) return NEX_AUDIO_OUTPUT_SHARED;
    return NEX_AUDIO_OUTPUT_EXCLUSIVE;
}

/* A session counts as "sounding" when it is Active and its peak meter is above
   this. Active alone is not enough: some programs keep a silent stream open
   the whole time. The threshold is about -80 dBFS. */
#define NEX_AUDIO_OTHER_PEAK_MIN 0.0001f
NEX_AUDIO_POLICY_FN int nex_audio_policy_session_sounding(int active, float peak) {
    return active && peak > NEX_AUDIO_OTHER_PEAK_MIN;
}

/* ---- what the negotiated path does to the samples ------------------------- */
enum {
    NEX_AUDIO_PATH_NATIVE = 0,    /* identical bytes                             */
    NEX_AUDIO_PATH_REPACKED = 1,  /* identical values, other container width     */
    NEX_AUDIO_PATH_DEPTH = 2,     /* same rate, other precision or sample type   */
    NEX_AUDIO_PATH_CONVERTED = 3  /* resampled                                   */
};

NEX_AUDIO_POLICY_FN int nex_audio_policy_path_kind(int in_rate, int in_container, int in_valid,
                                                   int in_float, int out_rate, int out_container,
                                                   int out_valid, int out_float) {
    if (in_rate != out_rate) return NEX_AUDIO_PATH_CONVERTED;
    if (in_float != out_float || in_valid != out_valid) return NEX_AUDIO_PATH_DEPTH;
    return in_container == out_container ? NEX_AUDIO_PATH_NATIVE : NEX_AUDIO_PATH_REPACKED;
}

/* The tag written into the ENGAGED log line and read by the panel. An opening
   fade changes the first samples of every start, so it is part of the tag. */
NEX_AUDIO_POLICY_FN const char *nex_audio_policy_path_tag(int kind, int fade_ms) {
    switch (kind) {
    case NEX_AUDIO_PATH_NATIVE: return fade_ms > 0 ? "[native+fade]" : "[native]";
    case NEX_AUDIO_PATH_REPACKED: return fade_ms > 0 ? "[repacked+fade]" : "[repacked]";
    case NEX_AUDIO_PATH_DEPTH: return "[depth only]";
    default: return "[converted]";
    }
}

/* ---- candidates ----------------------------------------------------------- */

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

typedef struct NexAudioPolicyCandidate {
    int rate;
    int container_bits;
    int valid_bits;
    int is_float;
} NexAudioPolicyCandidate;

/* Ordered search plan. The first primary_count entries are every depth at the
   leading rate (the source rate, or the forced rate). */
typedef struct NexAudioPolicyPlan {
    NexAudioPolicyCandidate items[NEX_AUDIO_POLICY_MAX_CANDIDATES];
    int count;
    int primary_count;
} NexAudioPolicyPlan;

NEX_AUDIO_POLICY_FN int nex_audio_policy_push_rate(int *out, int count, int capacity, int rate) {
    int i;
    if (!out || rate <= 0 || count >= capacity) return count;
    for (i = 0; i < count; i++) if (out[i] == rate) return count;
    out[count] = rate;
    return count + 1;
}

NEX_AUDIO_POLICY_FN int nex_audio_policy_integer_ratio(int a, int b) {
    if (a <= 0 || b <= 0) return 0;
    return (a % b == 0) || (b % a == 0);
}

NEX_AUDIO_POLICY_FN int nex_audio_policy_ratio_factor(int a, int b) {
    if (a <= 0 || b <= 0) return 0;
    return a > b ? a / b : b / a;
}

/* Keep the source rate in slot zero. Among converted rates, integer ratios
   precede non-integer ratios and smaller integer factors precede larger ones. */
NEX_AUDIO_POLICY_FN void nex_audio_policy_prefer_integer_ratios(int *rates, int count,
                                                                int source_rate) {
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

/* Rate order. The source rate always leads. A forced rate and the strict
   modes produce that one rate only. Otherwise: nearest higher pool rates, then
   lower ones from high to low, then the endpoint's own mix rate, which is a
   candidate like any other and never evidence of exclusive support. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_build_rates(const NexAudioPolicyInput *policy,
                                                     int endpoint_mix_rate,
                                                     int *out, int capacity) {
    int count = 0;
    int pool_count = 0;
    const int *pool = nex_audio_policy_rate_pool(&pool_count);
    int i;
    if (!policy || !out || capacity <= 0) return 0;
    count = nex_audio_policy_push_rate(out, count, capacity,
                                       policy->forced_rate > 0 ? policy->forced_rate
                                                               : policy->source_rate);
    if (policy->forced_rate > 0 || policy->hifi || !policy->compatibility) return count;
    for (i = 0; i < pool_count; i++) {
        if (pool[i] > policy->source_rate)
            count = nex_audio_policy_push_rate(out, count, capacity, pool[i]);
    }
    for (i = pool_count - 1; i >= 0; i--) {
        if (pool[i] < policy->source_rate)
            count = nex_audio_policy_push_rate(out, count, capacity, pool[i]);
    }
    if (nex_audio_policy_rate_valid(endpoint_mix_rate))
        count = nex_audio_policy_push_rate(out, count, capacity, endpoint_mix_rate);
    if (policy->source_preference == 1)
        nex_audio_policy_prefer_integer_ratios(out, count, policy->source_rate);
    return count;
}

NEX_AUDIO_POLICY_FN int nex_audio_policy_push_depth(NexAudioPolicyDepth *out, int count,
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

/* Any layout the format check accepts: an integer container of 16/24/32 bits
   holding 1..container valid bits (20-in-24 included), or float32. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_valid_source_depth(const NexAudioPolicyInput *policy) {
    if (!policy) return 0;
    if (policy->source_is_float)
        return policy->source_container_bits == 32 && policy->source_valid_bits == 32;
    return (policy->source_container_bits == 16 ||
            policy->source_container_bits == 24 ||
            policy->source_container_bits == 32) &&
           policy->source_valid_bits >= 1 &&
           policy->source_valid_bits <= policy->source_container_bits;
}

NEX_AUDIO_POLICY_FN int nex_audio_policy_push_requested(NexAudioPolicyDepth *out, int count,
                                                        int capacity, int requested) {
    if (requested == 32) return nex_audio_policy_push_depth(out, count, capacity, 32, 32, 1);
    if (requested == 16 || requested == 24)
        return nex_audio_policy_push_depth(out, count, capacity, requested, requested, 0);
    return count;
}

/* Depth order inside one rate.

   Strict modes: the client representation and nothing else.

   Otherwise:
     1. the bits setting, only when it asks for less than the source carries;
        that is an explicit request to reduce. bits=0 (the default) asks for
        nothing and leaves the source ladder first.
     2. the source ladder, as in AppleMusic-OpenHome's AudioFormatPolicy:
        source width first, then the same valid bits in wider containers, then
        wider declarations. Every step is exact; the extra low bits are zero.
          16 -> 16/16, 16-in-24, 16-in-32, 24/24, 24-in-32, 32/32
          24 -> 24/24, 24-in-32, 32/32
          32 -> 32/32          float -> float32
     3. compatibility fallbacks: the bits setting, float32, 24/24, 24-in-32,
        16/16. These may reduce depth and exist so playback continues on an
        endpoint that refuses everything above. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_build_depths(const NexAudioPolicyInput *policy,
                                                      NexAudioPolicyDepth *out, int capacity) {
    int count = 0;
    int requested;
    int source_ok;
    int source_precision;
    if (!policy || !out || capacity <= 0) return 0;

    source_ok = nex_audio_policy_valid_source_depth(policy);
    if (policy->hifi || !policy->compatibility) {
        if (source_ok) {
            count = nex_audio_policy_push_depth(out, count, capacity,
                                                policy->source_container_bits,
                                                policy->source_valid_bits,
                                                policy->source_is_float);
        }
        return count;
    }

    requested = policy->requested_output_bits;
    source_precision = policy->source_is_float ? 24 : policy->source_valid_bits;
    if (source_ok && (requested == 16 || requested == 24) && requested < source_precision)
        count = nex_audio_policy_push_requested(out, count, capacity, requested);

    if (source_ok) {
        if (policy->source_is_float) {
            count = nex_audio_policy_push_depth(out, count, capacity, 32, 32, 1);
        } else {
            int v = policy->source_valid_bits;
            /* An uncommon precision (e.g. 20 bits) is tried as the client
               wrote it before the exact widenings below. */
            if (v != 16 && v != 24 && v != 32)
                count = nex_audio_policy_push_depth(out, count, capacity,
                                                    policy->source_container_bits, v, 0);
            if (policy->source_valid_bits == 16) {
                count = nex_audio_policy_push_depth(out, count, capacity, 16, 16, 0);
                count = nex_audio_policy_push_depth(out, count, capacity, 24, 16, 0);
                count = nex_audio_policy_push_depth(out, count, capacity, 32, 16, 0);
            }
            if (policy->source_valid_bits <= 24) {
                count = nex_audio_policy_push_depth(out, count, capacity, 24, 24, 0);
                count = nex_audio_policy_push_depth(out, count, capacity, 32, 24, 0);
            }
            count = nex_audio_policy_push_depth(out, count, capacity, 32, 32, 0);
        }
    }

    count = nex_audio_policy_push_requested(out, count, capacity, requested);
    count = nex_audio_policy_push_depth(out, count, capacity, 32, 32, 1);
    count = nex_audio_policy_push_depth(out, count, capacity, 24, 24, 0);
    count = nex_audio_policy_push_depth(out, count, capacity, 32, 24, 0);
    count = nex_audio_policy_push_depth(out, count, capacity, 16, 16, 0);
    return count;
}

/* The single entry point for negotiation: every candidate in the order it
   should be probed. Rate is the outer key, so every depth at the source rate
   is tried before any conversion to another rate. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_build_plan(const NexAudioPolicyInput *policy,
                                                    int endpoint_mix_rate,
                                                    NexAudioPolicyPlan *plan) {
    int rates[NEX_AUDIO_POLICY_MAX_RATES];
    NexAudioPolicyDepth depths[NEX_AUDIO_POLICY_MAX_DEPTHS];
    int nr, nd, ri, di;
    if (!plan) return 0;
    plan->count = 0;
    plan->primary_count = 0;
    if (!policy) return 0;
    nr = nex_audio_policy_build_rates(policy, endpoint_mix_rate, rates,
                                      NEX_AUDIO_POLICY_MAX_RATES);
    nd = nex_audio_policy_build_depths(policy, depths, NEX_AUDIO_POLICY_MAX_DEPTHS);
    for (ri = 0; ri < nr; ri++) {
        for (di = 0; di < nd; di++) {
            NexAudioPolicyCandidate *c = &plan->items[plan->count++];
            c->rate = rates[ri];
            c->container_bits = depths[di].container_bits;
            c->valid_bits = depths[di].valid_bits;
            c->is_float = depths[di].is_float;
        }
        if (ri == 0) plan->primary_count = plan->count;
    }
    return plan->count;
}

/* ---- search --------------------------------------------------------------- */

enum {
    NEX_AUDIO_PROBE_ACCEPTED = 0,
    NEX_AUDIO_PROBE_REJECTED = 1,  /* the endpoint refuses this format          */
    NEX_AUDIO_PROBE_BUSY     = 2,  /* the endpoint is held; says nothing about
                                      the format                                */
    NEX_AUDIO_PROBE_SKIPPED  = 3   /* not probeable here; costs no attempt      */
};

enum {
    NEX_AUDIO_SEARCH_FOUND    = 0,
    NEX_AUDIO_SEARCH_REJECTED = 1, /* every candidate was genuinely refused     */
    NEX_AUDIO_SEARCH_BUSY     = 2, /* nothing worked and the endpoint was held  */
    NEX_AUDIO_SEARCH_LIMIT    = 3  /* probe budget exhausted                    */
};

/* AUDCLNT_E_DEVICE_IN_USE is a statement about the endpoint, not the format:
   the previous stream's teardown has not released it yet. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_classify_failure(unsigned long code) {
    return code == 0x8889000Aul ? NEX_AUDIO_PROBE_BUSY : NEX_AUDIO_PROBE_REJECTED;
}

typedef int (*NexAudioPolicyProbe)(void *context, const NexAudioPolicyCandidate *candidate);

typedef struct NexAudioPolicySearch {
    int index;      /* winning candidate in the plan, or -1                    */
    int tried;      /* probes spent                                            */
    int busy_seen;  /* some probe (or the remembered format) answered busy     */
    int cacheable;  /* the winner is the first candidate the endpoint accepts,
                       so it may be remembered for this input and endpoint     */
} NexAudioPolicySearch;

NEX_AUDIO_POLICY_FN int nex_audio_policy_search_win(NexAudioPolicySearch *out,
                                                    const unsigned char *busy,
                                                    int index, int cached_busy) {
    int k;
    out->index = index;
    out->cacheable = !cached_busy;
    for (k = 0; k < index; k++) if (busy[k]) out->cacheable = 0;
    return NEX_AUDIO_SEARCH_FOUND;
}

/* Walk the plan.

   A busy answer does not move a candidate out of the way: before any rate other
   than the leading one is considered, the leading-rate candidates that answered
   busy are probed once more. If a conversion is still chosen while an earlier
   candidate is unresolved, the result is usable for this stream but must not be
   remembered, so the next stream starts from the source format again.

   cached_busy tells the search that the remembered format for this input was
   just found busy; whatever is chosen now must not replace it. */
NEX_AUDIO_POLICY_FN int nex_audio_policy_search(const NexAudioPolicyPlan *plan,
                                                NexAudioPolicyProbe probe, void *context,
                                                int max_tries, int cached_busy,
                                                NexAudioPolicySearch *out) {
    unsigned char busy[NEX_AUDIO_POLICY_MAX_CANDIDATES];
    NexAudioPolicySearch local;
    int i, j, unresolved = 0;
    if (!out) out = &local;
    out->index = -1;
    out->tried = 0;
    out->busy_seen = cached_busy ? 1 : 0;
    out->cacheable = 0;
    if (!plan || !probe || plan->count <= 0) return NEX_AUDIO_SEARCH_REJECTED;
    for (i = 0; i < plan->count; i++) busy[i] = 0;

    for (i = 0; i <= plan->count; i++) {
        if (i == plan->primary_count) {
            for (j = 0; j < plan->primary_count; j++) {
                int r;
                if (!busy[j]) continue;
                if (out->tried >= max_tries) return NEX_AUDIO_SEARCH_LIMIT;
                r = probe(context, &plan->items[j]);
                if (r == NEX_AUDIO_PROBE_SKIPPED) { busy[j] = 0; continue; }
                out->tried++;
                if (r == NEX_AUDIO_PROBE_ACCEPTED)
                    return nex_audio_policy_search_win(out, busy, j, cached_busy);
                if (r != NEX_AUDIO_PROBE_BUSY) busy[j] = 0;
            }
        }
        if (i == plan->count) break;
        {
            int r;
            if (out->tried >= max_tries) return NEX_AUDIO_SEARCH_LIMIT;
            r = probe(context, &plan->items[i]);
            if (r == NEX_AUDIO_PROBE_SKIPPED) continue;
            out->tried++;
            if (r == NEX_AUDIO_PROBE_ACCEPTED)
                return nex_audio_policy_search_win(out, busy, i, cached_busy);
            if (r == NEX_AUDIO_PROBE_BUSY) { busy[i] = 1; out->busy_seen = 1; }
        }
    }
    for (i = 0; i < plan->count; i++) if (busy[i]) unresolved = 1;
    return (unresolved || cached_busy) ? NEX_AUDIO_SEARCH_BUSY : NEX_AUDIO_SEARCH_REJECTED;
}

#ifdef __cplusplus
}
#endif

#endif
