/* Task 4.3: BREAK handling on the tag (spec crossing-detection, "Crossing from
 * a BREAK event"), and the beam taking over the RSSI crossing in the run. */
#include "tlog/brk.h"
#include "tlog/run.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define ME 0x0A17D5C4u
#define OTHER 0x1A2B3C4Du
#define S(x) ((int64_t)((x) * 1e6))

static tlog_break_t copy(uint16_t seq, uint8_t k, uint32_t age_us, uint32_t nearest) {
    tlog_break_t b = {seq, k, 0, age_us, nearest};
    return b;
}

/* A gate breaks at t_break and sends copy k at t_break + 1 ms + k * 10 ms; air delay 0.4 ms. */
static int deliver(tlog_brk_t *b, uint8_t gate, uint16_t seq, int k, int64_t t_break, uint32_t nearest, int64_t *out) {
    int64_t sent = t_break + 1000 + k * 10000;
    tlog_break_t c = copy(seq, (uint8_t)k, (uint32_t)(sent - t_break), nearest);
    return tlog_brk_copy(b, gate, &c, sent + 400, out);
}

static void test_names_this_tag(void) {
    tlog_brk_t b;
    tlog_brk_init(&b, ME, -80);
    int64_t t = 0;
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_MINE, deliver(&b, 3, 7, 0, S(100), ME, &t));
    TEST_ASSERT_INT64_WITHIN(500, S(100), t); /* off by the air delay only */
}

/* spec "BREAK for another tag" */
static void test_names_another_tag(void) {
    tlog_brk_t b;
    tlog_brk_init(&b, ME, -80);
    tlog_brk_rssi(&b, 3, -50, S(99.9)); /* even right next to the gate */
    int64_t t = 0;
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_OTHER, deliver(&b, 3, 7, 0, S(100), OTHER, &t));
    for (int k = 1; k < 5; k++) TEST_ASSERT_EQUAL_INT(TLOG_BRK_DUPLICATE, deliver(&b, 3, 7, k, S(100), OTHER, &t));
}

static void test_names_none(void) {
    tlog_brk_t b;
    tlog_brk_init(&b, ME, -80);
    int64_t t = 0;
    /* never heard the gate strongly */
    tlog_brk_rssi(&b, 3, -88, S(99.5));
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_FAR, deliver(&b, 3, 1, 0, S(100), 0, &t));
    /* heard it at the threshold 1.2 s before */
    tlog_brk_rssi(&b, 3, -80, S(118.8));
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_MINE, deliver(&b, 3, 2, 0, S(120), 0, &t));
    /* heard it strongly, but 2 s before */
    tlog_brk_rssi(&b, 3, -60, S(138));
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_FAR, deliver(&b, 3, 3, 0, S(140), 0, &t));
    /* another gate's strong signal doesn't count for this one */
    tlog_brk_rssi(&b, 4, -40, S(159.9));
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_FAR, deliver(&b, 3, 4, 0, S(160), 0, &t));
}

/* spec "BREAK copies lost": any single copy gives the same, correct break time. */
static void test_four_of_five_copies_lost(void) {
    for (int k = 0; k < 5; k++) {
        tlog_brk_t b;
        tlog_brk_init(&b, ME, -80);
        int64_t t = 0;
        TEST_ASSERT_EQUAL_INT(TLOG_BRK_MINE, deliver(&b, 3, 9, k, S(50), ME, &t));
        TEST_ASSERT_INT64_WITHIN(500, S(50), t);
    }
}

/* All five copies: one crossing. */
static void test_copies_give_one_crossing(void) {
    tlog_brk_t b;
    tlog_brk_init(&b, ME, -80);
    int64_t t = 0;
    int mine = 0;
    for (int k = 0; k < 5; k++)
        if (deliver(&b, 3, 11, k, S(70), ME, &t) == TLOG_BRK_MINE) mine++;
    TEST_ASSERT_EQUAL_INT(1, mine);
    /* the same sequence number from another gate is another break */
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_MINE, deliver(&b, 4, 11, 0, S(70.2), ME, &t));
    /* and from the same gate much later (the u16 wrapped) too */
    TEST_ASSERT_EQUAL_INT(TLOG_BRK_MINE, deliver(&b, 3, 11, 0, S(200), ME, &t));
}

static const tlog_gate_t START_B = {TLOG_ROLE_START, 0, TLOG_GATE_F_BARRIER, 100};
static const tlog_gate_t SPLIT_1 = {TLOG_ROLE_SPLIT, 1, 0, 100};
static const tlog_gate_t FINISH_B = {TLOG_ROLE_FINISH, 0, TLOG_GATE_F_BARRIER, 100};

/* spec "Beam time replaces RSSI time", BREAK first: the RSSI crossing confirmed 0.7 s later is
 * the same pass and adds nothing. */
static void test_rssi_after_beam_is_dropped(void) {
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 1, &START_B, S(10), true, &c);
    tlog_run_crossing(&r, 2, &SPLIT_1, S(40), false, &c);
    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 3, &FINISH_B, S(100.000), true, &c));
    TEST_ASSERT_EQUAL(TLOG_RUN_NONE, tlog_run_crossing(&r, 3, &FINISH_B, S(100.04), false, &c));
    TEST_ASSERT_EQUAL_UINT8(3, r.n);
    tlog_tev_t e;
    tlog_run_event(&r, 3, &e);
    TEST_ASSERT_TRUE(e.beam);
    TEST_ASSERT_EQUAL_UINT32(90000, e.t_ms);
}

/* On a start-finish gate with a barrier, both orders give one lap per pass. */
static void test_xc_lap_beam_and_rssi(void) {
    static const tlog_gate_t SF_B = {TLOG_ROLE_STARTFINISH, 0, TLOG_GATE_F_BARRIER, 100};
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 9, &SF_B, S(0), true, &c);
    tlog_run_crossing(&r, 9, &SF_B, S(0.05), false, &c);  /* RSSI after the beam */
    tlog_run_crossing(&r, 9, &SF_B, S(600.03), false, &c); /* RSSI first ... */
    TEST_ASSERT_EQUAL(TLOG_RUN_REPLACED, tlog_run_crossing(&r, 9, &SF_B, S(600), true, &c)); /* ... beam */
    TEST_ASSERT_EQUAL_UINT8(2, r.n);
    tlog_tev_t e;
    tlog_run_event(&r, 2, &e);
    TEST_ASSERT_EQUAL_UINT8(TLOG_EV_LAP, e.kind);
    TEST_ASSERT_EQUAL_UINT8(1, e.index);
    TEST_ASSERT_TRUE(e.beam);
    TEST_ASSERT_EQUAL_UINT32(600000, e.t_ms);
}

/* A real later pass of the same gate (beyond 1.5 s) still counts. */
static void test_rssi_beyond_window_counts(void) {
    static const tlog_gate_t SF = {TLOG_ROLE_STARTFINISH, 0, TLOG_GATE_F_BARRIER, 100};
    tlog_run_t r;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_run_crossing(&r, 9, &SF, S(0), true, &c);
    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 9, &SF, S(1.6), false, &c));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_names_this_tag);
    RUN_TEST(test_names_another_tag);
    RUN_TEST(test_names_none);
    RUN_TEST(test_four_of_five_copies_lost);
    RUN_TEST(test_copies_give_one_crossing);
    RUN_TEST(test_rssi_after_beam_is_dropped);
    RUN_TEST(test_xc_lap_beam_and_rssi);
    RUN_TEST(test_rssi_beyond_window_counts);
    return UNITY_END();
}
