/* Task 5.2: advert page scheduling (design D7; spec crossing-events, "Run
 * summary after a finish"). */
#include <string.h>

#include "tlog/pages.h"
#include "tlog/run.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define S(x) ((int64_t)((x) * 1e6))

static const tlog_gate_t START = {TLOG_ROLE_START, 0, 0, 100};
static const tlog_gate_t FINISH = {TLOG_ROLE_FINISH, 0, 0, 100};

/* A finished DH run of n events (start, n - 2 splits, finish), the finish at t_finish. */
static void dh_run(tlog_run_t *r, tlog_pages_t *p, int n, double t_finish) {
    tlog_crossing_t c;
    tlog_run_init(r, 0);
    tlog_pages_init(p);
    tlog_run_crossing(r, 1, &START, S(t_finish - 100), true, &c);
    tlog_pages_event(p, S(t_finish - 100));
    for (int i = 0; i < n - 2; i++) {
        tlog_gate_t g = {TLOG_ROLE_SPLIT, (uint8_t)(1 + i % 10), 0, 100};
        double t = t_finish - 90 + i;
        tlog_run_crossing(r, (uint8_t)(10 + i), &g, S(t), false, &c);
        tlog_pages_event(p, S(t));
    }
    tlog_run_crossing(r, 2, &FINISH, S(t_finish), true, &c);
    tlog_pages_event(p, S(t_finish));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_FINISHED, r->state);
    TEST_ASSERT_EQUAL_UINT8(n, r->n);
}

/* Steps the advert schedule from t0 for secs; checks the three phases after a finish. */
static void check_finished(int n) {
    tlog_run_t r;
    tlog_pages_t p;
    const double tf = 1000;
    dh_run(&r, &p, n, tf);
    int64_t t = S(tf);
    bool seen[256] = {false};
    int summary_pages = 0, cycle = (n + 1) / 2;
    int64_t complete_at = -1;
    uint8_t prev = 0;
    while (t < S(tf + 130)) {
        uint8_t k;
        uint32_t iv;
        tlog_pages_next(&p, &r, t, &k, &iv);
        double since = (double)(t - S(tf)) / 1e6;
        if (since < 5) {
            TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_FAST_MS, iv);
            TEST_ASSERT_EQUAL_UINT8(n, k);
        } else if (since < 125) {
            TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_SUMMARY_MS, iv);
            TEST_ASSERT_TRUE(k >= 1 && k <= n);
            if (summary_pages) TEST_ASSERT_TRUE_MESSAGE(k == prev - 2 || k == n, "pages step down by two");
            seen[k] = true;
            if (k > 1) seen[k - 1] = true;
            summary_pages++;
            if (complete_at < 0) {
                bool all = true;
                for (int i = 1; i <= n; i++) all = all && seen[i];
                if (all) complete_at = t;
            }
            prev = k;
        } else {
            TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_SLOW_MS, iv);
            TEST_ASSERT_EQUAL_UINT8(n, k);
        }
        t += (int64_t)iv * 1000;
    }
    TEST_ASSERT_TRUE_MESSAGE(complete_at > 0, "the summary covers every event");
    /* the whole run within one cycle of pages (spec: a 5-event run within 1.5 s) */
    TEST_ASSERT_TRUE(complete_at - S(tf + 5) <= (int64_t)(cycle - 1) * 500000 + 1);
    TEST_ASSERT_INT_WITHIN(2, 240, summary_pages); /* 2 min at 500 ms */
}

static void test_summary_2_events(void) { check_finished(2); }
static void test_summary_5_events(void) { check_finished(5); }
static void test_summary_12_events(void) { check_finished(12); }

/* A run with one event (just started): the latest pair, fast then slow; no summary. */
static void test_one_event_running(void) {
    tlog_run_t r;
    tlog_pages_t p;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_pages_init(&p);
    uint8_t k;
    uint32_t iv;
    tlog_pages_next(&p, &r, S(1), &k, &iv); /* idle, nothing yet */
    TEST_ASSERT_EQUAL_UINT8(0, k);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_SLOW_MS, iv);
    tlog_run_crossing(&r, 1, &START, S(10), true, &c);
    tlog_pages_event(&p, S(10));
    tlog_pages_next(&p, &r, S(10.1), &k, &iv);
    TEST_ASSERT_EQUAL_UINT8(1, k);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_FAST_MS, iv);
    tlog_pages_next(&p, &r, S(15.1), &k, &iv);
    TEST_ASSERT_EQUAL_UINT8(1, k);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_SLOW_MS, iv);
}

/* spec "Next run starts": a new start during the summary ends it. */
static void test_summary_interrupted(void) {
    tlog_run_t r;
    tlog_pages_t p;
    dh_run(&r, &p, 5, 1000);
    uint8_t k;
    uint32_t iv;
    tlog_pages_next(&p, &r, S(1030), &k, &iv);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_SUMMARY_MS, iv);
    tlog_crossing_t c;
    TEST_ASSERT_EQUAL(TLOG_RUN_NEW, tlog_run_crossing(&r, 1, &START, S(1030.2), true, &c));
    tlog_pages_event(&p, S(1030.2));
    tlog_pages_next(&p, &r, S(1030.3), &k, &iv);
    TEST_ASSERT_EQUAL_UINT8(1, k);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_FAST_MS, iv);
    tlog_pages_next(&p, &r, S(1040), &k, &iv); /* the new run is running: no summary */
    TEST_ASSERT_EQUAL_UINT8(1, k);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_SLOW_MS, iv);
}

/* A beam replacement of the finish is an event too: back to 10 Hz, then the summary from the top. */
static void test_replaced_finish_restarts_fast(void) {
    tlog_run_t r;
    tlog_pages_t p;
    dh_run(&r, &p, 5, 1000);
    uint8_t k;
    uint32_t iv;
    tlog_pages_next(&p, &r, S(1006), &k, &iv);
    tlog_pages_next(&p, &r, S(1006.5), &k, &iv);
    TEST_ASSERT_EQUAL_UINT8(3, k);
    tlog_pages_event(&p, S(1007));
    tlog_pages_next(&p, &r, S(1007.1), &k, &iv);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_FAST_MS, iv);
    tlog_pages_next(&p, &r, S(1012.1), &k, &iv);
    TEST_ASSERT_EQUAL_UINT8(5, k);
    TEST_ASSERT_EQUAL_UINT32(TLOG_PAGES_SUMMARY_MS, iv);
}

/* More events than the run keeps: the summary covers the kept ones, from n down. */
static void test_summary_beyond_ring(void) {
    tlog_run_t r;
    tlog_pages_t p;
    tlog_crossing_t c;
    tlog_run_init(&r, 0);
    tlog_pages_init(&p);
    tlog_gate_t sp = {TLOG_ROLE_SPLIT, 1, 0, 100};
    tlog_run_crossing(&r, 1, &START, S(0), true, &c);
    for (int i = 0; i < 70; i++) tlog_run_crossing(&r, 2, &sp, S(1 + i * 2), false, &c);
    tlog_run_crossing(&r, 3, &FINISH, S(200), true, &c);
    tlog_pages_event(&p, S(200));
    TEST_ASSERT_EQUAL_UINT8(72, r.n);
    bool seen[256] = {false};
    for (int i = 0; i < 40; i++) {
        uint8_t k;
        uint32_t iv;
        tlog_pages_next(&p, &r, S(206 + i * 0.5), &k, &iv);
        tlog_tev_t e;
        TEST_ASSERT_TRUE_MESSAGE(tlog_run_event(&r, k, &e), "every page's event A is still kept");
        seen[k] = seen[k - 1] = true;
    }
    for (int i = 9; i <= 72; i++) TEST_ASSERT_TRUE(seen[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_summary_2_events);
    RUN_TEST(test_summary_5_events);
    RUN_TEST(test_summary_12_events);
    RUN_TEST(test_one_event_running);
    RUN_TEST(test_summary_interrupted);
    RUN_TEST(test_replaced_finish_restarts_fast);
    RUN_TEST(test_summary_beyond_ring);
    return UNITY_END();
}
