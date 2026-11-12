/* Task 3.2: the crossing detector (design D5) against synthetic passes.
 *
 * Model: RSSI = P0 - 10 n log10(d), P0 = -45 dBm at 1 m, n = 2.5, so the -80 dBm threshold is
 * reached at about 25 m and a gate 2 m from the line peaks at -52.5 dBm. Frames at 100 Hz (the
 * gate's beacon rate as heard by a tag), 10 % lost, +-2 dB uniform noise, rounded to whole dB
 * like the radio reports. Deterministic pseudo-random numbers, so the tests are repeatable. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "tlog/crossing.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define MAX_X 8

static uint32_t rng;
static float urand(void) { /* [0, 1) */
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 16777216.0f;
}

static float model(float lateral_m, float along_m) {
    float d = sqrtf(lateral_m * lateral_m + along_m * along_m);
    if (d < 0.5f) d = 0.5f;
    return -45.0f - 25.0f * log10f(d);
}

typedef struct {
    tlog_xing_t x[MAX_X];
    int n;
} found_t;

/* A scenario gives the along-track position (m) and an extra attenuation (dB) at time t (s),
 * and whether the gate is heard at all. */
typedef struct {
    float lateral;
    float (*pos)(float t);
    float (*fade)(float t);
    int (*heard)(float t);
} scenario_t;

static float v_ms; /* speed for the simple passes */
static float pos_const_speed(float t) { return v_ms * t; }
static float no_fade(float t) { (void)t; return 0; }
static int always(float t) { (void)t; return 1; }

static found_t run_seeded(const scenario_t *sc, float t_from, float t_to, int32_t bin_us, uint32_t seed,
                          float tc);
static found_t run(const scenario_t *sc, float t_from, float t_to, int32_t bin_us) {
    return run_seeded(sc, t_from, t_to, bin_us, 12345, 0.0f);
}

/* tc shifts the whole scenario in time (the pass happens at tc instead of 0). */
static found_t run_seeded(const scenario_t *sc, float t_from, float t_to, int32_t bin_us, uint32_t seed,
                          float tc) {
    found_t f = {0};
    tlog_xdet_cfg_t cfg = tlog_xdet_defaults(bin_us);
    tlog_xdet_t d;
    tlog_xdet_init(&d, &cfg);
    rng = seed;
    /* 100 Hz frames; the receiver polls every 10 ms too */
    for (int64_t us = (int64_t)(t_from * 1e6f); us <= (int64_t)(t_to * 1e6f); us += 10000) {
        float t = (float)us / 1e6f - tc;
        tlog_xing_t x;
        if (sc->heard(t) && urand() >= 0.10f) {
            float r = model(sc->lateral, sc->pos(t)) - sc->fade(t) + (urand() * 4.0f - 2.0f);
            int dbm = (int)lroundf(r);
            if (dbm >= -97 && tlog_xdet_feed(&d, us, dbm, &x) && f.n < MAX_X) f.x[f.n++] = x;
        }
        if (tlog_xdet_poll(&d, us + 1, &x) && f.n < MAX_X) f.x[f.n++] = x;
    }
    return f;
}

/* 50 passes, each with its own noise and a random phase of the pass against the bins. The spec's
 * accuracy target (+-0.1 s repeatable from RSSI, 10-50 km/h) bounds the worst pass. */
static void check_pass_at(float kmh, float lateral, int32_t bin_us, int64_t confirm_limit_us) {
    v_ms = kmh / 3.6f;
    scenario_t sc = {.lateral = lateral, .pos = pos_const_speed, .fade = no_fade, .heard = always};
    float span = 35.0f / v_ms > 8.0f ? 35.0f / v_ms : 8.0f; /* from and to out of range */
    double sq = 0, worst = 0;
    int64_t worst_confirm = 0;
    const int passes = 50;
    for (int i = 0; i < passes; i++) {
        rng = 777u + (uint32_t)i * 7919u;
        float tc = urand() * 0.1f - 0.05f;
        found_t f = run_seeded(&sc, -span, span, bin_us, 1000u + (uint32_t)i, tc);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, f.n, "one crossing per pass");
        int64_t tc_us = (int64_t)llroundf(tc * 1e6f);
        double err = (double)(f.x[0].t_us - tc_us);
        sq += err * err;
        if (fabs(err) > worst) worst = fabs(err);
        TEST_ASSERT_TRUE_MESSAGE(f.x[0].confirm_us > f.x[0].t_us, "confirmed after the peak");
        if (f.x[0].confirm_us - tc_us > worst_confirm) worst_confirm = f.x[0].confirm_us - tc_us;
    }
    double rms = sqrt(sq / passes);
    printf("  %2.0f km/h, gate %.0f m off, %d ms bins: rms %.1f ms, worst %.1f ms, confirmed after at most %.2f s (%d passes)\n",
           kmh, (double)lateral, (int)(bin_us / 1000), rms / 1000, worst / 1000, (double)worst_confirm / 1e6, passes);
    TEST_ASSERT_TRUE_MESSAGE(worst_confirm <= confirm_limit_us, "confirmation delay");
    TEST_ASSERT_TRUE_MESSAGE(worst <= 100000, "every pass within +-0.1 s");
    TEST_ASSERT_TRUE_MESSAGE(rms <= 40000, "rms within 40 ms");
}
static void check_pass(float kmh, int32_t bin_us, int64_t confirm_limit_us) {
    check_pass_at(kmh, 2.0f, bin_us, confirm_limit_us);
}

/* Slow passes confirm later: the signal needs longer to fall 6 dB (15 km/h about 1.1 s, 10 km/h 1.4 s);
 * the spec's "confirmed within 1 s" is the 30 km/h scenario "Pass at speed". */
static void test_pass_15_kmh(void) { check_pass(15, 50000, 1500000); }
static void test_pass_10_kmh(void) { check_pass(10, 50000, 2000000); }
/* Slow uphill split (spec): the gate stands 1 m from the line; confirmed within 3 s. */
static void test_pass_5_kmh_gate_1m(void) { check_pass_at(5, 1.0f, 50000, 3000000); }
static void test_pass_30_kmh(void) { check_pass(30, 50000, 1000000); }
static void test_pass_50_kmh(void) { check_pass(50, 50000, 1000000); }
/* The uplink uses 100 ms bins and hears the tag at 20 Hz; same rules. */
static void test_pass_30_kmh_uplink_bins(void) { check_pass(30, 100000, 1000000); }

/* A 6 dB multipath fade lasting 100 ms on the approach, while the signal is above the threshold. */
static float fade_approach(float t) { return (t >= -0.60f && t < -0.50f) ? 6.0f : 0.0f; }
static void test_fade_on_approach(void) {
    v_ms = 30 / 3.6f;
    scenario_t sc = {.lateral = 2.0f, .pos = pos_const_speed, .fade = fade_approach, .heard = always};
    found_t f = run(&sc, -8.0f, 8.0f, 50000);
    TEST_ASSERT_EQUAL_INT(1, f.n);
    TEST_ASSERT_INT64_WITHIN(50000, 0, f.x[0].t_us);
}

/* Rider passes at 30 km/h, brakes to a stop 15 m past the gate, walks back to stand 3 m from the
 * gate for 60 s, then rides off. One crossing, at the pass. */
static float pos_stop_and_stand(float t) {
    const float v = 30 / 3.6f, a = v * v / (2 * 15.0f); /* stop after 15 m */
    float t_stop = v / a;
    if (t <= 0) return v * t;
    if (t <= t_stop) return v * t - 0.5f * a * t * t;
    if (t <= 10) return 15.0f;
    if (t <= 20) return 15.0f - (15.0f - 2.24f) * (t - 10) / 10; /* walk back to d = 3 m */
    if (t <= 80) return 2.24f;                                   /* sqrt(3^2 - 2^2) */
    return 2.24f + 5.0f * (t - 80);                              /* rides off */
}
static void test_rider_stands_near_gate(void) {
    scenario_t sc = {.lateral = 2.0f, .pos = pos_stop_and_stand, .fade = no_fade, .heard = always};
    found_t f = run(&sc, -8.0f, 100.0f, 50000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, f.n, "standing or riding off makes no second crossing");
    TEST_ASSERT_INT64_WITHIN(50000, 0, f.x[0].t_us);
    TEST_ASSERT_TRUE(f.x[0].confirm_us <= 1000000);
}

/* Rider stops right after the pass and stands 3 m from the gate (within 6 dB of the peak): the
 * crossing can only be confirmed when the rider leaves, but it keeps the time of the pass. */
static float pos_stand_at_3m(float t) {
    if (t <= 0) return 30 / 3.6f * t;
    if (t <= 0.5f) return 2.24f * t / 0.5f;
    if (t <= 60) return 2.24f;
    return 2.24f + 5.0f * (t - 60);
}
static void test_standing_within_6db_keeps_pass_time(void) {
    scenario_t sc = {.lateral = 2.0f, .pos = pos_stand_at_3m, .fade = no_fade, .heard = always};
    found_t f = run(&sc, -8.0f, 80.0f, 50000);
    TEST_ASSERT_EQUAL_INT(1, f.n);
    TEST_ASSERT_INT64_WITHIN(60000, 0, f.x[0].t_us);
}

/* Two laps 60 s apart: the hold re-arms once the rider was out of range. */
static float pos_two_laps(float t) {
    const float v = 30 / 3.6f;
    return t < 30 ? v * t : v * (t - 60);
}
static void test_two_laps(void) {
    scenario_t sc = {.lateral = 2.0f, .pos = pos_two_laps, .fade = no_fade, .heard = always};
    found_t f = run(&sc, -8.0f, 68.0f, 50000);
    TEST_ASSERT_EQUAL_INT(2, f.n);
    TEST_ASSERT_INT64_WITHIN(50000, 0, f.x[0].t_us);
    TEST_ASSERT_INT64_WITHIN(50000, 60000000, f.x[1].t_us);
}

/* The gate goes silent 100 ms after the peak (e.g. the rider turns behind a rock): confirmed by
 * the 300 ms silence rule. */
static int heard_until_100ms(float t) { return t < 0.10f; }
static void test_confirm_on_silence(void) {
    v_ms = 30 / 3.6f;
    scenario_t sc = {.lateral = 2.0f, .pos = pos_const_speed, .fade = no_fade, .heard = heard_until_100ms};
    found_t f = run(&sc, -8.0f, 3.0f, 50000);
    TEST_ASSERT_EQUAL_INT(1, f.n);
    TEST_ASSERT_INT64_WITHIN(60000, 0, f.x[0].t_us);
    TEST_ASSERT_TRUE(f.x[0].confirm_us <= 500000);
}

/* A gate 40 m off the trail never reaches -80 dBm. */
static void test_gate_far_off(void) {
    v_ms = 30 / 3.6f;
    scenario_t sc = {.lateral = 40.0f, .pos = pos_const_speed, .fade = no_fade, .heard = always};
    found_t f = run(&sc, -10.0f, 10.0f, 50000);
    TEST_ASSERT_EQUAL_INT(0, f.n);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pass_5_kmh_gate_1m);
    RUN_TEST(test_pass_10_kmh);
    RUN_TEST(test_pass_15_kmh);
    RUN_TEST(test_pass_30_kmh);
    RUN_TEST(test_pass_50_kmh);
    RUN_TEST(test_pass_30_kmh_uplink_bins);
    RUN_TEST(test_fade_on_approach);
    RUN_TEST(test_rider_stands_near_gate);
    RUN_TEST(test_standing_within_6db_keeps_pass_time);
    RUN_TEST(test_two_laps);
    RUN_TEST(test_confirm_on_silence);
    RUN_TEST(test_gate_far_off);
    return UNITY_END();
}
