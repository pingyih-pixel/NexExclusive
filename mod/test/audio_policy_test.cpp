#include "../src/audio_policy.h"
#include <cstdio>
#include <string>

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { std::printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; } \
} while (0)

static int depth_index(const NexAudioPolicyDepth *depths, int count,
                       int container_bits, int valid_bits, int is_float) {
    for (int i = 0; i < count; ++i) {
        if (depths[i].container_bits == container_bits &&
            depths[i].valid_bits == valid_bits &&
            depths[i].is_float == is_float) return i;
    }
    return count;
}

static bool has_depth(const NexAudioPolicyDepth *depths, int count,
                      int container_bits, int valid_bits, int is_float) {
    return depth_index(depths, count, container_bits, valid_bits, is_float) < count;
}

static bool depth_is(const NexAudioPolicyDepth &d, int container_bits, int valid_bits,
                     int is_float) {
    return d.container_bits == container_bits && d.valid_bits == valid_bits &&
           d.is_float == is_float;
}

static NexAudioPolicyInput input(int rate, int container, int valid, int is_float,
                                 int requested_bits) {
    NexAudioPolicyInput p{};
    p.source_rate = rate;
    p.source_container_bits = container;
    p.source_valid_bits = valid;
    p.source_is_float = is_float;
    p.requested_output_bits = requested_bits;
    p.compatibility = 1;
    return p;
}

/* ---- depth order ---------------------------------------------------------- */

/* A 16-bit source with the default bits=24 used to probe 24/24 first. The
   source ladder now follows the OpenHome order: source width first, wider
   containers next, wider declarations only after that. */
static void test_16_bit_source_ladder_precedes_higher_setting() {
    NexAudioPolicyInput p = input(44100, 16, 16, 0, 24);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n >= 6);
    CHECK(depth_is(d[0], 16, 16, 0));
    CHECK(depth_is(d[1], 24, 16, 0));
    CHECK(depth_is(d[2], 32, 16, 0));
    CHECK(depth_is(d[3], 24, 24, 0));
    CHECK(depth_is(d[4], 32, 24, 0));
    CHECK(depth_is(d[5], 32, 32, 0));
    std::printf("PASS 16-bit source: source ladder precedes a higher bits setting\n");
}

static void test_24_bit_source_ladder() {
    NexAudioPolicyInput p = input(96000, 24, 24, 0, 24);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n >= 3);
    CHECK(depth_is(d[0], 24, 24, 0));
    CHECK(depth_is(d[1], 32, 24, 0));
    CHECK(depth_is(d[2], 32, 32, 0));
    CHECK(!has_depth(d, n, 24, 16, 0));
    CHECK(!has_depth(d, n, 32, 16, 0));
    /* 16 bit stays available as the last compatibility resort only. */
    CHECK(depth_index(d, n, 16, 16, 0) == n - 1);
    std::printf("PASS 24-bit source: 24/24, 24-in-32, 32/32, reduction last\n");
}

static void test_container_does_not_change_source_precision() {
    NexAudioPolicyInput p = input(44100, 32, 16, 0, 24);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n >= 3);
    CHECK(depth_is(d[0], 16, 16, 0));
    CHECK(depth_is(d[1], 24, 16, 0));
    CHECK(depth_is(d[2], 32, 16, 0));
    std::printf("PASS 16-valid in a 32-bit container is still a 16-bit source\n");
}

/* The bits setting leads only when it asks for less than the source carries. */
static void test_lower_setting_leads() {
    NexAudioPolicyInput p = input(96000, 24, 24, 0, 16);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n >= 2);
    CHECK(depth_is(d[0], 16, 16, 0));
    CHECK(depth_is(d[1], 24, 24, 0));

    p = input(44100, 16, 16, 0, 16);
    n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(depth_is(d[0], 16, 16, 0));
    CHECK(depth_is(d[1], 24, 16, 0));

    p = input(44100, 32, 32, 1, 16);
    n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(depth_is(d[0], 16, 16, 0));
    CHECK(depth_is(d[1], 32, 32, 1));
    std::printf("PASS bits setting leads only when it reduces the source depth\n");
}

static void test_float_source() {
    NexAudioPolicyInput p = input(48000, 32, 32, 1, 24);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n >= 2);
    CHECK(depth_is(d[0], 32, 32, 1));
    CHECK(has_depth(d, n, 24, 24, 0));
    CHECK(!has_depth(d, n, 32, 32, 0));
    std::printf("PASS float source: float32 first, integer fallbacks after\n");
}

static void test_strict_source() {
    NexAudioPolicyInput p = input(44100, 32, 24, 0, 16);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    p.compatibility = 0;
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n == 1 && depth_is(d[0], 32, 24, 0));
    p.compatibility = 1;
    p.hifi = 1;
    n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n == 1 && depth_is(d[0], 32, 24, 0));
    std::printf("PASS strict policy keeps the client representation only\n");
}

static void test_depth_candidates_are_unique_and_legal() {
    const int valids[] = {16, 24, 32};
    const int requested[] = {16, 24, 32};
    for (int v : valids) for (int r : requested) {
        NexAudioPolicyInput p = input(44100, v, v, 0, r);
        NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
        int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
        CHECK(n > 0 && n <= NEX_AUDIO_POLICY_MAX_DEPTHS);
        for (int i = 0; i < n; ++i) {
            CHECK(nex_audio_policy_format_valid(44100, 2, d[i].container_bits,
                                                d[i].valid_bits, d[i].is_float,
                                                2 * d[i].container_bits / 8));
            for (int j = 0; j < i; ++j)
                CHECK(!depth_is(d[j], d[i].container_bits, d[i].valid_bits, d[i].is_float));
        }
    }
    std::printf("PASS depth candidates are unique and legal for every source/setting\n");
}

/* ---- rate order ----------------------------------------------------------- */

static void test_rate_order() {
    NexAudioPolicyInput p = input(88200, 24, 24, 0, 24);
    int r[NEX_AUDIO_POLICY_MAX_RATES]{};
    int n = nex_audio_policy_build_rates(&p, 48000, r, NEX_AUDIO_POLICY_MAX_RATES);
    const int bandwidth[] = {88200, 96000, 176400, 192000, 352800, 384000, 48000, 44100};
    CHECK(n == 8);
    for (int i = 0; i < 8 && i < n; ++i) CHECK(r[i] == bandwidth[i]);

    p.source_preference = 1;
    n = nex_audio_policy_build_rates(&p, 48000, r, NEX_AUDIO_POLICY_MAX_RATES);
    CHECK(n == 8);
    CHECK(r[0] == 88200);
    CHECK(r[1] == 176400 || r[1] == 44100);
    CHECK(r[2] == 176400 || r[2] == 44100);
    CHECK(r[3] == 352800);
    std::printf("PASS rate order: source first, higher before lower, integer ratios on request\n");
}

static void test_rate_pool_is_the_legal_range() {
    int n = 0;
    const int *pool = nex_audio_policy_rate_pool(&n);
    CHECK(n == 8);
    for (int i = 0; i < n; ++i) {
        CHECK(pool[i] >= NEX_AUDIO_POLICY_MIN_RATE && pool[i] <= NEX_AUDIO_POLICY_MAX_RATE);
        if (i) CHECK(pool[i] > pool[i - 1]);
    }
    CHECK(pool[0] == 44100 && pool[n - 1] == 384000);
    std::printf("PASS rate pool is ascending and inside the legal range\n");
}

static void test_single_rate_modes() {
    NexAudioPolicyInput p = input(44100, 16, 16, 0, 24);
    int r[NEX_AUDIO_POLICY_MAX_RATES]{};
    p.forced_rate = 96000;
    CHECK(nex_audio_policy_build_rates(&p, 48000, r, NEX_AUDIO_POLICY_MAX_RATES) == 1);
    CHECK(r[0] == 96000);
    p.forced_rate = 0;
    p.hifi = 1;
    CHECK(nex_audio_policy_build_rates(&p, 48000, r, NEX_AUDIO_POLICY_MAX_RATES) == 1);
    CHECK(r[0] == 44100);
    p.hifi = 0;
    p.compatibility = 0;
    CHECK(nex_audio_policy_build_rates(&p, 48000, r, NEX_AUDIO_POLICY_MAX_RATES) == 1);
    CHECK(r[0] == 44100);
    std::printf("PASS forced rate and strict modes produce one rate\n");
}

static void test_unusual_mix_rate_is_a_late_candidate() {
    NexAudioPolicyInput p = input(44100, 16, 16, 0, 24);
    int r[NEX_AUDIO_POLICY_MAX_RATES]{};
    int n = nex_audio_policy_build_rates(&p, 32000, r, NEX_AUDIO_POLICY_MAX_RATES);
    CHECK(n == 9);
    CHECK(r[0] == 44100);
    CHECK(r[n - 1] == 32000);
    std::printf("PASS endpoint mix rate is a candidate, never the leader\n");
}

/* ---- unified plan --------------------------------------------------------- */

static void test_plan_is_rate_major() {
    NexAudioPolicyInput p = input(44100, 16, 16, 0, 24);
    NexAudioPolicyPlan plan{};
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int nd = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(nex_audio_policy_build_plan(&p, 48000, &plan) == plan.count);
    CHECK(plan.count == 8 * nd);
    CHECK(plan.primary_count == nd);
    for (int i = 0; i < plan.primary_count; ++i) {
        CHECK(plan.items[i].rate == 44100);
        CHECK(plan.items[i].container_bits == d[i].container_bits);
        CHECK(plan.items[i].valid_bits == d[i].valid_bits);
    }
    CHECK(plan.items[0].container_bits == 16 && plan.items[0].valid_bits == 16);
    CHECK(plan.items[plan.primary_count].rate == 48000);
    std::printf("PASS plan: every depth at the source rate precedes any other rate\n");
}

static void test_plan_strict_is_one_candidate() {
    NexAudioPolicyInput p = input(96000, 32, 24, 0, 16);
    NexAudioPolicyPlan plan{};
    p.hifi = 1;
    nex_audio_policy_build_plan(&p, 48000, &plan);
    CHECK(plan.count == 1 && plan.primary_count == 1);
    CHECK(plan.items[0].rate == 96000 && plan.items[0].container_bits == 32 &&
          plan.items[0].valid_bits == 24 && !plan.items[0].is_float);
    std::printf("PASS plan: strict native is exactly the client format\n");
}

/* ---- search over a fake endpoint ------------------------------------------ */

struct FakeEndpoint {
    int accept_rate[4];
    int accept_container[4];
    int accept_valid[4];
    int accept_count;
    int busy_rate;       /* candidates at this rate answer busy ...            */
    int busy_calls;      /* ... for this many probes, then answer normally     */
    int skip_rate;       /* candidates at this rate are not probeable          */
    int calls;
};

static int fake_probe(void *ctx, const NexAudioPolicyCandidate *c) {
    FakeEndpoint *e = static_cast<FakeEndpoint *>(ctx);
    if (e->skip_rate && c->rate == e->skip_rate) return NEX_AUDIO_PROBE_SKIPPED;
    e->calls++;
    if (e->busy_rate && c->rate == e->busy_rate && e->busy_calls > 0) {
        e->busy_calls--;
        return NEX_AUDIO_PROBE_BUSY;
    }
    for (int i = 0; i < e->accept_count; ++i) {
        if (e->accept_rate[i] == c->rate && e->accept_container[i] == c->container_bits &&
            e->accept_valid[i] == c->valid_bits && !c->is_float)
            return NEX_AUDIO_PROBE_ACCEPTED;
    }
    return NEX_AUDIO_PROBE_REJECTED;
}

static NexAudioPolicyPlan plan_44k_16() {
    NexAudioPolicyInput p = input(44100, 16, 16, 0, 24);
    NexAudioPolicyPlan plan{};
    nex_audio_policy_build_plan(&p, 48000, &plan);
    return plan;
}

static void test_search_takes_source_format() {
    NexAudioPolicyPlan plan = plan_44k_16();
    FakeEndpoint e{{44100, 48000}, {16, 24}, {16, 24}, 2, 0, 0, 0, 0};
    NexAudioPolicySearch s{};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &e, 64, 0, &s) == NEX_AUDIO_SEARCH_FOUND);
    CHECK(s.index == 0 && s.tried == 1 && s.cacheable == 1 && s.busy_seen == 0);
    std::printf("PASS search: source format wins in one probe and is remembered\n");
}

static void test_search_genuine_rejection_moves_rate() {
    NexAudioPolicyPlan plan = plan_44k_16();
    FakeEndpoint e{{48000}, {24}, {24}, 1, 0, 0, 0, 0};
    NexAudioPolicySearch s{};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &e, 64, 0, &s) == NEX_AUDIO_SEARCH_FOUND);
    CHECK(plan.items[s.index].rate == 48000 && plan.items[s.index].container_bits == 24);
    CHECK(s.cacheable == 1 && s.busy_seen == 0);
    std::printf("PASS search: a real refusal of the source rate is a cacheable conversion\n");
}

/* The 20:53 case: the endpoint takes 44.1/16 but the previous stream still
   holds it while the source-rate candidates are probed. */
static void test_search_busy_source_rate_is_retried_before_converting() {
    NexAudioPolicyPlan plan = plan_44k_16();
    FakeEndpoint e{{44100, 48000}, {16, 24}, {16, 24}, 2, 44100, plan.primary_count, 0, 0};
    NexAudioPolicySearch s{};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &e, 64, 0, &s) == NEX_AUDIO_SEARCH_FOUND);
    CHECK(plan.items[s.index].rate == 44100);
    CHECK(s.index == 0);
    CHECK(s.busy_seen == 1);
    CHECK(s.cacheable == 1);
    std::printf("PASS search: a busy source rate is retried and still wins\n");
}

static void test_search_busy_result_is_not_remembered() {
    NexAudioPolicyPlan plan = plan_44k_16();
    FakeEndpoint e{{44100, 48000}, {16, 24}, {16, 24}, 2, 44100, 1000, 0, 0};
    NexAudioPolicySearch s{};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &e, 64, 0, &s) == NEX_AUDIO_SEARCH_FOUND);
    CHECK(plan.items[s.index].rate == 48000);
    CHECK(s.busy_seen == 1);
    CHECK(s.cacheable == 0);
    std::printf("PASS search: a conversion chosen past a busy candidate is not remembered\n");
}

static void test_search_busy_cached_format_is_not_replaced() {
    NexAudioPolicyPlan plan = plan_44k_16();
    FakeEndpoint e{{44100}, {16}, {16}, 1, 0, 0, 0, 0};
    NexAudioPolicySearch s{};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &e, 64, 1, &s) == NEX_AUDIO_SEARCH_FOUND);
    CHECK(s.cacheable == 0);
    std::printf("PASS search: a busy remembered format keeps its place\n");
}

static void test_search_all_busy_reports_busy() {
    NexAudioPolicyInput p = input(44100, 16, 16, 0, 24);
    p.hifi = 1;
    NexAudioPolicyPlan plan{};
    nex_audio_policy_build_plan(&p, 48000, &plan);
    FakeEndpoint e{{44100}, {16}, {16}, 1, 44100, 1000, 0, 0};
    NexAudioPolicySearch s{};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &e, 64, 0, &s) == NEX_AUDIO_SEARCH_BUSY);
    CHECK(s.index < 0);

    FakeEndpoint none{{0}, {0}, {0}, 0, 0, 0, 0, 0};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &none, 64, 0, &s) ==
          NEX_AUDIO_SEARCH_REJECTED);
    std::printf("PASS search: occupied and unsupported are reported differently\n");
}

static void test_search_limit_and_skips() {
    NexAudioPolicyPlan plan = plan_44k_16();
    FakeEndpoint e{{48000}, {24}, {24}, 1, 0, 0, 0, 0};
    NexAudioPolicySearch s{};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &e, 3, 0, &s) == NEX_AUDIO_SEARCH_LIMIT);
    CHECK(s.tried == 3 && e.calls == 3);

    FakeEndpoint skip{{48000, 96000}, {24, 24}, {24, 24}, 2, 0, 0, 48000, 0};
    CHECK(nex_audio_policy_search(&plan, fake_probe, &skip, 64, 0, &s) == NEX_AUDIO_SEARCH_FOUND);
    CHECK(plan.items[s.index].rate != 48000);
    CHECK(s.tried == skip.calls);
    std::printf("PASS search: probe budget is honoured; skipped candidates cost nothing\n");
}

static void test_failure_classification() {
    CHECK(nex_audio_policy_classify_failure(0x8889000Aul) == NEX_AUDIO_PROBE_BUSY);
    CHECK(nex_audio_policy_classify_failure(0x88890008ul) == NEX_AUDIO_PROBE_REJECTED);
    CHECK(nex_audio_policy_classify_failure(0x80004005ul) == NEX_AUDIO_PROBE_REJECTED);
    std::printf("PASS device-in-use is not a format refusal\n");
}

/* ---- config, format legality, dither -------------------------------------- */

static void test_config_defaults_and_normalization() {
    NexAudioPolicyConfig c = nex_audio_policy_config_defaults();
    CHECK(c.out_bits == 0 && c.force_rate == 0 && c.out_frames == 0 && c.compat == 1 &&
          c.dither == 2 && c.fade_ms == 0 && c.src_prefer == 0 && c.hifi == 0);

    c.out_bits = 20; c.force_rate = 5; c.out_frames = 99999; c.dither = 7;
    c.fade_ms = 500; c.src_prefer = 3; c.compat = 9;
    nex_audio_policy_normalize_config(&c);
    CHECK(c.out_bits == 0 && c.force_rate == 0 && c.out_frames == 0 && c.dither == 2 &&
          c.fade_ms == 0 && c.src_prefer == 0 && c.compat == 1);

    c = nex_audio_policy_config_defaults();
    c.force_rate = 400000;
    nex_audio_policy_normalize_config(&c);
    CHECK(c.force_rate == 0);
    c.force_rate = 96000; c.out_bits = 32; c.out_frames = 4096;
    nex_audio_policy_normalize_config(&c);
    CHECK(c.force_rate == 96000 && c.out_bits == 32 && c.out_frames == 4096);

    c.hifi = 1;
    nex_audio_policy_normalize_config(&c);
    CHECK(c.force_rate == 0 && c.compat == 0 && c.dither == 0 && c.fade_ms == 0);
    std::printf("PASS config: defaults, range repair, strict native overrides\n");
}

static void test_format_legality() {
    CHECK(nex_audio_policy_format_valid(44100, 2, 16, 16, 0, 4));
    CHECK(nex_audio_policy_format_valid(192000, 2, 24, 24, 0, 6));
    CHECK(nex_audio_policy_format_valid(96000, 6, 32, 24, 0, 24));
    CHECK(nex_audio_policy_format_valid(96000, 2, 32, 32, 1, 8));
    CHECK(nex_audio_policy_format_valid(8000, 1, 16, 16, 0, 2));
    CHECK(nex_audio_policy_format_valid(384000, 8, 32, 32, 0, 32));
    CHECK(!nex_audio_policy_format_valid(7999, 2, 16, 16, 0, 4));
    CHECK(!nex_audio_policy_format_valid(384001, 2, 16, 16, 0, 4));
    CHECK(!nex_audio_policy_format_valid(44100, 0, 16, 16, 0, 0));
    CHECK(!nex_audio_policy_format_valid(44100, 9, 16, 16, 0, 18));
    CHECK(!nex_audio_policy_format_valid(44100, 2, 20, 20, 0, 5));
    CHECK(!nex_audio_policy_format_valid(44100, 2, 16, 20, 0, 4));
    CHECK(!nex_audio_policy_format_valid(44100, 2, 16, 0, 0, 4));
    CHECK(!nex_audio_policy_format_valid(44100, 2, 16, 16, 0, 6));
    CHECK(!nex_audio_policy_format_valid(44100, 2, 16, 16, 1, 4));
    CHECK(!nex_audio_policy_format_valid(44100, 2, 24, 24, 1, 6));
    std::printf("PASS format legality: rate, channels, container, valid bits, float\n");
}

static void test_dither() {
    CHECK(nex_audio_policy_dither_selected(0, 24, 0, 16, 2, 0));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 0, 16, 2, 5));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 0, 24, 2, 0));
    CHECK(!nex_audio_policy_dither_selected(0, 16, 0, 24, 2, 0));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 1, 16, 2, 0));
    CHECK(nex_audio_policy_dither_selected(0, 24, 0, 24, 1, 5));
    CHECK(!nex_audio_policy_dither_selected(0, 24, 0, 16, 0, 0));
    CHECK(!nex_audio_policy_dither_selected(1, 32, 0, 24, 2, 0));
    CHECK(nex_audio_policy_dither_selected(1, 32, 0, 16, 2, 0));
    std::printf("PASS dither: auto needs an integer depth reduction and the fade off\n");
}

/* ---- items 7, 4, 1, 6 ------------------------------------------------------ */
static void test_20_bit_strict_keeps_client_layout() {
    NexAudioPolicyInput p = input(44100, 24, 20, 0, 0);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    p.compatibility = 0;
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n == 1 && depth_is(d[0], 24, 20, 0));
    p = input(96000, 32, 20, 0, 0);
    p.hifi = 1;
    n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n == 1 && depth_is(d[0], 32, 20, 0));
    std::printf("PASS 20-bit source: strict modes still offer the client layout\n");
}

static void test_20_bit_compat_source_first() {
    NexAudioPolicyInput p = input(44100, 32, 20, 0, 0);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n >= 3);
    CHECK(depth_is(d[0], 32, 20, 0));
    CHECK(depth_is(d[1], 24, 24, 0));
    CHECK(depth_is(d[2], 32, 24, 0));
    std::printf("PASS 20-bit source: client layout first, then exact 24-bit widenings\n");
}

/* bits=0 is "follow the source": no reduction is requested by default. */
static void test_bits_zero_follows_source() {
    NexAudioPolicyInput p = input(44100, 32, 32, 0, 0);
    NexAudioPolicyDepth d[NEX_AUDIO_POLICY_MAX_DEPTHS]{};
    int n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(n >= 1 && depth_is(d[0], 32, 32, 0));
    p = input(44100, 24, 24, 0, 0);
    n = nex_audio_policy_build_depths(&p, d, NEX_AUDIO_POLICY_MAX_DEPTHS);
    CHECK(depth_is(d[0], 24, 24, 0));
    NexAudioPolicyConfig c = nex_audio_policy_config_defaults();
    CHECK(c.out_bits == 0);
    c.out_bits = 0;
    nex_audio_policy_normalize_config(&c);
    CHECK(c.out_bits == 0);
    c.out_bits = 16;
    nex_audio_policy_normalize_config(&c);
    CHECK(c.out_bits == 16);
    std::printf("PASS bits=0 follows the source and is the default\n");
}

static void test_path_kind_labels() {
    CHECK(nex_audio_policy_path_kind(44100, 16, 16, 0, 44100, 16, 16, 0) == NEX_AUDIO_PATH_NATIVE);
    CHECK(nex_audio_policy_path_kind(44100, 32, 16, 0, 44100, 16, 16, 0) == NEX_AUDIO_PATH_REPACKED);
    CHECK(nex_audio_policy_path_kind(44100, 16, 16, 0, 44100, 24, 16, 0) == NEX_AUDIO_PATH_REPACKED);
    CHECK(nex_audio_policy_path_kind(44100, 16, 16, 0, 44100, 24, 24, 0) == NEX_AUDIO_PATH_DEPTH);
    CHECK(nex_audio_policy_path_kind(44100, 32, 32, 1, 44100, 32, 32, 0) == NEX_AUDIO_PATH_DEPTH);
    CHECK(nex_audio_policy_path_kind(44100, 16, 16, 0, 48000, 16, 16, 0) == NEX_AUDIO_PATH_CONVERTED);
    CHECK(std::string(nex_audio_policy_path_tag(NEX_AUDIO_PATH_NATIVE, 0)) == "[native]");
    CHECK(std::string(nex_audio_policy_path_tag(NEX_AUDIO_PATH_NATIVE, 5)) == "[native+fade]");
    CHECK(std::string(nex_audio_policy_path_tag(NEX_AUDIO_PATH_REPACKED, 0)) == "[repacked]");
    CHECK(std::string(nex_audio_policy_path_tag(NEX_AUDIO_PATH_REPACKED, 3)) == "[repacked+fade]");
    CHECK(std::string(nex_audio_policy_path_tag(NEX_AUDIO_PATH_DEPTH, 5)) == "[depth only]");
    CHECK(std::string(nex_audio_policy_path_tag(NEX_AUDIO_PATH_CONVERTED, 0)) == "[converted]");
    std::printf("PASS path labels: native, repacked, depth only, converted; fade is never hidden\n");
}

/* ---- Orion 1+2: start with silence, longer across clock families ----------- */
static void test_rate_family_and_prime() {
    CHECK(nex_audio_policy_rate_family(44100) == 44100);
    CHECK(nex_audio_policy_rate_family(88200) == 44100);
    CHECK(nex_audio_policy_rate_family(176400) == 44100);
    CHECK(nex_audio_policy_rate_family(352800) == 44100);
    CHECK(nex_audio_policy_rate_family(48000) == 48000);
    CHECK(nex_audio_policy_rate_family(96000) == 48000);
    CHECK(nex_audio_policy_rate_family(192000) == 48000);
    CHECK(nex_audio_policy_rate_family(32000) == 48000);
    CHECK(nex_audio_policy_rate_family(0) == 0);
    /* one device buffer of silence on every fresh start */
    CHECK(nex_audio_policy_prime_frames(44100, 44100, 4410, 1, 200) == 4410);
    CHECK(nex_audio_policy_prime_frames(0, 44100, 4410, 1, 200) == 4410);       /* first ever */
    CHECK(nex_audio_policy_prime_frames(48000, 96000, 9600, 1, 200) == 9600);   /* same family */
    /* plus the resync time when the DAC has to change crystal */
    CHECK(nex_audio_policy_prime_frames(48000, 44100, 4410, 1, 200) == 4410 + 8820);
    CHECK(nex_audio_policy_prime_frames(44100, 192000, 19200, 1, 200) == 19200 + 38400);
    CHECK(nex_audio_policy_prime_frames(44100, 48000, 4800, 0, 200) == 9600);   /* prime off, resync on */
    CHECK(nex_audio_policy_prime_frames(44100, 48000, 4800, 1, 0) == 4800);     /* resync off */
    CHECK(nex_audio_policy_prime_frames(44100, 48000, 4800, 0, 0) == 0);
    std::printf("PASS prime: one buffer of silence per fresh start, +resync across clock families\n");
}

/* ---- Orion 4: the EOS wait is bounded by what is actually buffered ---------- */
static void test_eos_wait_bound() {
    CHECK(nex_audio_policy_eos_wait_ms(0, 48000) == 300);              /* floor */
    CHECK(nex_audio_policy_eos_wait_ms(4800, 48000) == 350);           /* 100 ms + margin */
    CHECK(nex_audio_policy_eos_wait_ms(48000, 48000) == 1250);         /* 1 s + margin */
    CHECK(nex_audio_policy_eos_wait_ms(192000ull * 60, 192000) == 5000); /* capped */
    CHECK(nex_audio_policy_eos_wait_ms(4800, 0) == 5000);              /* unknown rate */
    std::printf("PASS eos wait: buffered audio + 250 ms, between 300 ms and 5 s\n");
}

/* ---- Orion 6: event mode only where one period can be completed ----------- */
static void test_event_mode_fits() {
    CHECK(nex_audio_policy_event_fits(44100, 44100, 4410, 32768, 544));    /* native */
    CHECK(nex_audio_policy_event_fits(44100, 96000, 9600, 32768, 544));    /* upsampling */
    CHECK(nex_audio_policy_event_fits(192000, 96000, 9600, 32768, 544));   /* 2:1 down */
    CHECK(!nex_audio_policy_event_fits(384000, 96000, 9600, 32768, 544));  /* 38400 in > cap */
    CHECK(!nex_audio_policy_event_fits(0, 48000, 4800, 32768, 544));
    std::printf("PASS event mode: used only when a full period fits the resampler history\n");
}

/* ---- Orion 7: the optional dedicated core ------------------------------------ */
static void test_pick_core() {
    /* hybrid: 2 P-cores (class 1, SMT pairs) then 4 E-cores (class 0) */
    NexAudioCoreInfo hybrid[6] = {
        {1, 0x3}, {1, 0xC}, {0, 0x10}, {0, 0x20}, {0, 0x40}, {0, 0x80},
    };
    CHECK(nex_audio_policy_pick_core(hybrid, 6) == 1);       /* fast core, not CPU 0 */
    NexAudioCoreInfo flat[4] = { {0, 0x1}, {0, 0x2}, {0, 0x4}, {0, 0x8} };
    CHECK(nex_audio_policy_pick_core(flat, 4) == 3);         /* last core */
    NexAudioCoreInfo one[1] = { {0, 0x1} };
    CHECK(nex_audio_policy_pick_core(one, 1) == -1);         /* nothing to spare */
    NexAudioCoreInfo e_first[3] = { {0, 0x1}, {1, 0x2}, {0, 0x4} };
    CHECK(nex_audio_policy_pick_core(e_first, 3) == 1);
    std::printf("PASS dedicated core: fastest class, never the core holding CPU 0\n");
}

int main() {
    test_16_bit_source_ladder_precedes_higher_setting();
    test_24_bit_source_ladder();
    test_container_does_not_change_source_precision();
    test_lower_setting_leads();
    test_float_source();
    test_strict_source();
    test_depth_candidates_are_unique_and_legal();
    test_rate_order();
    test_rate_pool_is_the_legal_range();
    test_single_rate_modes();
    test_unusual_mix_rate_is_a_late_candidate();
    test_plan_is_rate_major();
    test_plan_strict_is_one_candidate();
    test_search_takes_source_format();
    test_search_genuine_rejection_moves_rate();
    test_search_busy_source_rate_is_retried_before_converting();
    test_search_busy_result_is_not_remembered();
    test_search_busy_cached_format_is_not_replaced();
    test_search_all_busy_reports_busy();
    test_search_limit_and_skips();
    test_failure_classification();
    test_config_defaults_and_normalization();
    test_format_legality();
    test_dither();
    test_20_bit_strict_keeps_client_layout();
    test_20_bit_compat_source_first();
    test_bits_zero_follows_source();
    test_path_kind_labels();
    test_rate_family_and_prime();
    test_eos_wait_bound();
    test_event_mode_fits();
    test_pick_core();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
