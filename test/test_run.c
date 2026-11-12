/* Task 3.3: the run state machine (design D6; spec crossing-detection,
 * Run timing). */
#include <math.h>
#include <stdlib.h>

#include "tlog/crossing.h"
#include "tlog/run.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define S(x) ((int64_t)((x) * 1e6))

static const tlog_gate_t START = {TLOG_ROLE_START, 0, 0, 100};
static const tlog_gate_t SPLIT1 = {TLOG_ROLE_SPLIT, 1, 0, 100};
static const tlog_gate_t SPLIT2 = {TLOG_ROLE_SPLIT, 2, 0, 100};
static const tlog_gate_t FINISH = {TLOG_ROLE_FINISH, 0, 0, 100};
static const tlog_gate_t SF = {TLOG_ROLE_STARTFINISH, 0, 0, 100};

static void check_event(const tlog_run_t *r, uint8_t k, uint8_t gate, uint8_t kind, uint8_t index, uint32_t t_ms,
                        bool beam) {
    tlog_tev_t e;
    TEST_ASSERT_TRUE(tlog_run_event(r, k, &e));
    TEST_ASSERT_EQUAL_UINT8(gate, e.gate);
    TEST_ASSERT_EQUAL_UINT8(kind, e.kind);
    TEST_ASSERT_EQUAL_UINT8(index, e.index);
    TEST_ASSERT_EQUAL_UINT32(t_ms, e.t_ms);
    TEST_ASSERT_EQUAL(beam, e.beam);
}

/* spec "DH training run" */
static void test_dh_run(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 6);
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_IDLE, r.state);
    TEST_ASSERT_EQUAL_UINT32(0, tlog_run_clock_ms(&r, S(5)));

    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 1, &START, S(10), true, &c));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_RUNNING, r.state);
    TEST_ASSERT_EQUAL_UINT8(7, r.counter);
    TEST_ASSERT_EQUAL_UINT8(1, c.event_seq);
    TEST_ASSERT_EQUAL_UINT8(TLOG_EV_START, c.ev.kind);
    TEST_ASSERT_EQUAL_INT64(S(10), c.crossing_us);

    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 2, &SPLIT1, S(40.1234), false, &c));
    TEST_ASSERT_EQUAL_UINT8(2, c.event_seq);
    TEST_ASSERT_EQUAL_UINT32(30123, c.ev.t_ms);
    TEST_ASSERT_EQUAL_UINT32(45000, tlog_run_clock_ms(&r, S(55)));

    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 3, &FINISH, S(100), true, &c));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_FINISHED, r.state);
    TEST_ASSERT_EQUAL_UINT8(3, r.n);

    check_event(&r, 1, 1, TLOG_EV_START, 0, 0, true);
    check_event(&r, 2, 2, TLOG_EV_SPLIT, 1, 30123, false);
    check_event(&r, 3, 3, TLOG_EV_FINISH, 0, 90000, true);
    tlog_tev_t e;
    TEST_ASSERT_FALSE(tlog_run_event(&r, 0, &e));
    TEST_ASSERT_FALSE(tlog_run_event(&r, 4, &e));

    /* after the finish, a split or finish crossing gives nothing */
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 2, &SPLIT1, S(130), false, &c));
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 3, &FINISH, S(140), false, &c));
    TEST_ASSERT_EQUAL_UINT8(3, r.n);
    /* the next start begins run 8 */
    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 1, &START, S(300), true, &c));
    TEST_ASSERT_EQUAL_UINT8(8, r.counter);
    TEST_ASSERT_EQUAL_UINT8(1, r.n);
}

/* Splits keep the gate's split index, in whatever order they come. */
static void test_split_index_from_gate(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 1, &START, S(0), true, &c);
    tlog_run_crossing(&r, 5, &SPLIT2, S(20), false, &c);
    TEST_ASSERT_EQUAL_UINT8(2, c.ev.index);
    TEST_ASSERT_EQUAL_UINT8(5, c.ev.gate);
}

/* spec "XC laps": four crossings of a start-finish gate are a start and three laps. */
static void test_xc_laps(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    const double t[] = {5, 605.5, 1190.25, 1800};
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 9, &SF, S(t[i]), false, &c));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_RUNNING, r.state);
    TEST_ASSERT_EQUAL_UINT8(4, r.n);
    TEST_ASSERT_EQUAL_UINT8(1, r.counter);
    check_event(&r, 1, 9, TLOG_EV_START, 0, 0, false);
    check_event(&r, 2, 9, TLOG_EV_LAP, 1, 600500, false);
    check_event(&r, 3, 9, TLOG_EV_LAP, 2, 1185250, false);
    check_event(&r, 4, 9, TLOG_EV_LAP, 3, 1795000, false);
    /* lap time = difference of consecutive events */
    tlog_tev_t a, b;
    tlog_run_event(&r, 3, &a);
    tlog_run_event(&r, 4, &b);
    TEST_ASSERT_EQUAL_UINT32(609750, b.t_ms - a.t_ms);
}

/* spec "Split without start" */
static void test_split_without_start(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 2, &SPLIT1, S(10), false, &c));
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 3, &FINISH, S(20), false, &c));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_IDLE, r.state);
    TEST_ASSERT_EQUAL_UINT8(0, r.n);
    TEST_ASSERT_EQUAL_UINT8(0, r.counter);
}

/* A start gate during a run restarts it: a new run with its own t0 and counter. */
static void test_restart_on_start_gate(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 3);
    tlog_run_crossing(&r, 1, &START, S(10), true, &c);
    tlog_run_crossing(&r, 2, &SPLIT1, S(30), false, &c);
    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 1, &START, S(200), true, &c));
    TEST_ASSERT_EQUAL_UINT8(5, r.counter);
    TEST_ASSERT_EQUAL_UINT8(1, r.n);
    TEST_ASSERT_EQUAL_UINT8(1, c.event_seq);
    TEST_ASSERT_EQUAL_UINT32(0, c.ev.t_ms);
    tlog_run_crossing(&r, 3, &FINISH, S(260), true, &c);
    TEST_ASSERT_EQUAL_UINT8(2, c.event_seq);
    TEST_ASSERT_EQUAL_UINT32(60000, c.ev.t_ms);
}

/* 30 min without a crossing abandons the run. */
static void test_abandonment(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 1, &START, S(10), true, &c);
    tlog_run_crossing(&r, 2, &SPLIT1, S(70), false, &c);
    TEST_ASSERT_FALSE(tlog_run_tick(&r, S(70 + 29 * 60)));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_RUNNING, r.state);
    TEST_ASSERT_TRUE(tlog_run_tick(&r, S(70 + 30 * 60)));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_IDLE, r.state);
    TEST_ASSERT_EQUAL_UINT8(1, r.counter); /* the abandoned run keeps its number */
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 3, &FINISH, S(70 + 31 * 60), false, &c));
    TEST_ASSERT_FALSE(tlog_run_tick(&r, S(10000))); /* idle: nothing to abandon */
    /* a finished run is not abandoned */
    tlog_run_crossing(&r, 1, &START, S(20000), true, &c);
    tlog_run_crossing(&r, 3, &FINISH, S(20100), true, &c);
    TEST_ASSERT_FALSE(tlog_run_tick(&r, S(20100 + 3600)));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_FINISHED, r.state);
}

/* D6: a beam crossing replaces an RSSI event of the same gate within 1.5 s. */
static void test_beam_replaces_rssi(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 1, &START, S(10), true, &c);
    tlog_run_crossing(&r, 3, &FINISH, S(100.08), false, &c);
    TEST_ASSERT_EQUAL(TLOG_RUN_REPLACED, tlog_run_crossing(&r, 3, &FINISH, S(100.01), true, &c));
    TEST_ASSERT_EQUAL_UINT8(2, c.event_seq);
    TEST_ASSERT_TRUE(c.ev.beam);
    TEST_ASSERT_EQUAL_UINT32(90010, c.ev.t_ms);
    TEST_ASSERT_EQUAL_UINT8(2, r.n);
    check_event(&r, 2, 3, TLOG_EV_FINISH, 0, 90010, true);
    /* a second beam crossing doesn't replace a beam event; after the finish it gives nothing */
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 3, &FINISH, S(100.5), true, &c));
}

static void test_beam_too_late_or_other_gate(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 1, &START, S(10), true, &c);
    tlog_run_crossing(&r, 2, &SPLIT1, S(40), false, &c);
    /* another gate's beam at the same moment is its own event */
    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 4, &SPLIT2, S(40.2), true, &c));
    /* the same gate's beam 1.6 s later is a new split */
    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 2, &SPLIT1, S(41.6), true, &c));
    TEST_ASSERT_EQUAL_UINT8(4, r.n);
}

/* Replacing the start moves t0; later events keep their absolute times. */
static void test_beam_replaces_start(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 1, &START, S(10.3), false, &c);
    tlog_run_crossing(&r, 2, &SPLIT1, S(11.0), false, &c); /* a split right after the start */
    TEST_ASSERT_EQUAL(TLOG_RUN_REPLACED, tlog_run_crossing(&r, 1, &START, S(10.0), true, &c));
    TEST_ASSERT_EQUAL_UINT8(1, c.event_seq);
    TEST_ASSERT_EQUAL_UINT32(0, c.ev.t_ms);
    TEST_ASSERT_EQUAL_UINT8(1, r.counter);
    check_event(&r, 2, 2, TLOG_EV_SPLIT, 1, 1000, false);
    TEST_ASSERT_EQUAL_UINT32(5000, tlog_run_clock_ms(&r, S(15)));
}

/* A crossing from before t0 (a late confirmation) is not part of the run. */
static void test_crossing_before_t0(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 1, &START, S(10), true, &c);
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 2, &SPLIT1, S(9), false, &c));
    TEST_ASSERT_EQUAL_UINT8(1, r.n);
}

static void test_counter_wraps_and_clock_saturates(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 255);
    tlog_run_crossing(&r, 9, &SF, S(0), false, &c);
    TEST_ASSERT_EQUAL_UINT8(0, r.counter);
    tlog_run_crossing(&r, 9, &SF, S(5 * 3600), false, &c); /* 5 h > 4.6 h */
    TEST_ASSERT_EQUAL_UINT32(TLOG_U24_MAX, c.ev.t_ms);
    TEST_ASSERT_EQUAL_UINT32(TLOG_U24_MAX, tlog_run_clock_ms(&r, S(6 * 3600)));
}

/* The last 64 events are kept; event numbers keep counting. */
static void test_event_ring(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    for (int i = 0; i < 70; i++) tlog_run_crossing(&r, 9, &SF, S(i * 20.0), false, &c);
    TEST_ASSERT_EQUAL_UINT8(70, r.n);
    TEST_ASSERT_EQUAL_UINT8(70, c.event_seq);
    tlog_tev_t e;
    TEST_ASSERT_FALSE(tlog_run_event(&r, 6, &e));
    TEST_ASSERT_TRUE(tlog_run_event(&r, 7, &e));
    TEST_ASSERT_EQUAL_UINT8(6, e.index); /* event 7 = lap 6 */
    check_event(&r, 70, 9, TLOG_EV_LAP, 69, 69 * 20000, false);
}

static void test_rssi_allowed(void) {
    tlog_gate_t g = START;
    TEST_ASSERT_TRUE(tlog_run_rssi_allowed(&g));
    g.flags = TLOG_GATE_F_BARRIER;
    TEST_ASSERT_FALSE(tlog_run_rssi_allowed(&g));
    g.flags = TLOG_GATE_F_BARRIER | TLOG_GATE_F_FAULT; /* broken barrier: fall back to RSSI */
    TEST_ASSERT_TRUE(tlog_run_rssi_allowed(&g));
    g = FINISH;
    g.flags = TLOG_GATE_F_BARRIER;
    TEST_ASSERT_TRUE(tlog_run_rssi_allowed(&g)); /* a finish with a barrier still has RSSI as backup */
    g = SF;
    g.flags = TLOG_GATE_F_BARRIER;
    TEST_ASSERT_TRUE(tlog_run_rssi_allowed(&g));
}

/* spec "DH start from standstill": the rider walks to the start hut, waits 30 s with the tag 1 m
 * from a start gate with a barrier, then rides off through the beam. The detector isn't fed for
 * this gate, so the waiting gives no crossing; the run starts at the break time. Fed anyway, the
 * same signal does give crossings, which is why the rule exists. */
static uint32_t rng = 4242;
static float urand(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 8) / 16777216.0f;
}
static float along(float t) { /* m along the trail, gate at 0, 1 m to the side */
    if (t < 10) return -20.0f + 2.0f * t; /* walks in */
    if (t < 40) return 0.0f;              /* waits */
    return 5.0f * (t - 40);               /* rides off */
}
static int standstill_crossings(bool apply_rule, tlog_run_t *r) {
    tlog_gate_t g = START;
    g.flags = TLOG_GATE_F_BARRIER;
    tlog_xdet_cfg_t cfg = tlog_xdet_defaults(50000);
    tlog_xdet_t d;
    tlog_xdet_init(&d, &cfg);
    tlog_run_init(r, 0);
    rng = 4242;
    int found = 0;
    tlog_crossing_t c;
    for (int64_t us = 0; us <= S(50); us += 10000) {
        float t = (float)us / 1e6f;
        tlog_xing_t x;
        bool allowed = !apply_rule || tlog_run_rssi_allowed(&g);
        if (urand() >= 0.1f) {
            float a = along(t), dist = sqrtf(1.0f + a * a);
            int dbm = (int)lroundf(-45.0f - 25.0f * log10f(dist) + urand() * 4.0f - 2.0f);
            if (allowed && tlog_xdet_feed(&d, us, dbm, &x)) {
                found++;
                tlog_run_crossing(r, 1, &g, x.t_us, false, &c);
            }
        }
        if (allowed && tlog_xdet_poll(&d, us + 1, &x)) {
            found++;
            tlog_run_crossing(r, 1, &g, x.t_us, false, &c);
        }
        if (us == S(40.2)) tlog_run_crossing(r, 1, &g, S(40.2) - 1500, true, &c); /* BREAK, 1.5 ms old */
    }
    return found;
}
static void test_dh_start_from_standstill(void) {
    tlog_run_t r;
    TEST_ASSERT_EQUAL_INT(0, standstill_crossings(true, &r));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_RUNNING, r.state);
    TEST_ASSERT_EQUAL_UINT8(1, r.n);
    TEST_ASSERT_EQUAL_UINT8(1, r.counter);
    TEST_ASSERT_EQUAL_INT64(S(40.2) - 1500, r.t0_us);
    check_event(&r, 1, 1, TLOG_EV_START, 0, 0, true);

    TEST_ASSERT_TRUE_MESSAGE(standstill_crossings(false, &r) >= 1, "without the rule the wait gives a crossing");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_dh_run);
    RUN_TEST(test_split_index_from_gate);
    RUN_TEST(test_xc_laps);
    RUN_TEST(test_split_without_start);
    RUN_TEST(test_restart_on_start_gate);
    RUN_TEST(test_abandonment);
    RUN_TEST(test_beam_replaces_rssi);
    RUN_TEST(test_beam_too_late_or_other_gate);
    RUN_TEST(test_beam_replaces_start);
    RUN_TEST(test_crossing_before_t0);
    RUN_TEST(test_counter_wraps_and_clock_saturates);
    RUN_TEST(test_event_ring);
    RUN_TEST(test_rssi_allowed);
    RUN_TEST(test_dh_start_from_standstill);
    return UNITY_END();
}
