/* Task 7.5: the uplink's passage queue (design D10; spec gate-uplink,
 * Offline queue) on a RAM flash stub with NOR semantics: erase sets 0xFF, a write can only clear
 * bits, and a write can be cut off after a byte budget to model a power loss. */
#include <string.h>

#include "tlog/result.h"
#include "tlog/pqueue.h"
#include "unity.h"

#define SECTOR 4096u
#define SIZE (64u * 1024u)

static uint8_t mem[SIZE];
static long budget = -1; /* bytes until the "power" fails; -1 = never */
static int dead;
static int set_bits; /* a write tried to turn a 0 into a 1 */
static unsigned erases[SIZE / SECTOR];

static int f_read(void *ctx, uint32_t off, void *buf, uint32_t len) {
    (void)ctx;
    if (dead || off + len > SIZE) return -1;
    memcpy(buf, mem + off, len);
    return 0;
}
static int f_write(void *ctx, uint32_t off, const void *buf, uint32_t len) {
    (void)ctx;
    if (dead || off + len > SIZE) return -1;
    const uint8_t *b = buf;
    for (uint32_t i = 0; i < len; i++) {
        if (budget == 0) {
            dead = 1;
            return -1;
        }
        if (budget > 0) budget--;
        if (~mem[off + i] & b[i]) set_bits = 1;
        mem[off + i] &= b[i];
    }
    return 0;
}
static int f_erase(void *ctx, uint32_t off) {
    (void)ctx;
    if (dead || off % SECTOR) return -1;
    memset(mem + off, 0xFF, SECTOR);
    erases[off / SECTOR]++;
    return 0;
}
static const tlog_flash_t FL = {NULL, f_read, f_write, f_erase, SIZE, SECTOR};

void setUp(void) {
    memset(mem, 0xFF, sizeof(mem));
    memset(erases, 0, sizeof(erases));
    budget = -1;
    dead = 0;
    set_bits = 0;
}
void tearDown(void) { TEST_ASSERT_FALSE_MESSAGE(set_bits, "a write tried to set bits"); }

static tlog_passage_t P(uint32_t i) {
    tlog_passage_t p = {0x1A2B0000u + (i % 80), 1793880000000ull + 1000ull * i, (uint8_t)(1 + i % 3)};
    return p;
}

static void reopen(tlog_pq_t *q) {
    dead = 0;
    budget = -1;
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_open(q, &FL));
}

static void expect_pending(tlog_pq_t *q, uint32_t first_i, uint32_t n) {
    TEST_ASSERT_EQUAL_UINT32(n, tlog_pq_pending(q));
    for (uint32_t k = 0; k < n; k++) {
        tlog_passage_t got, want = P(first_i + k);
        TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_peek(q, k, &got));
        TEST_ASSERT_EQUAL_HEX32(want.epc, got.epc);
        TEST_ASSERT_EQUAL_UINT64(want.epoch_ms, got.epoch_ms);
        TEST_ASSERT_EQUAL_UINT8(want.gate, got.gate);
    }
    tlog_passage_t x;
    TEST_ASSERT_EQUAL_INT(TLOG_ERR_ARG, tlog_pq_peek(q, n, &x));
}

static void test_geometry(void) {
    tlog_pq_t q;
    tlog_flash_t f = FL;
    f.size = SECTOR; /* one sector can't rotate */
    TEST_ASSERT_EQUAL_INT(TLOG_ERR_ARG, tlog_pq_open(&q, &f));
    f = FL;
    f.sector = 1000;
    TEST_ASSERT_EQUAL_INT(TLOG_ERR_ARG, tlog_pq_open(&q, &f));
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_open(&q, &FL));
    TEST_ASSERT_EQUAL_UINT32(0, tlog_pq_pending(&q));
}

static void test_append_peek_ack(void) {
    tlog_pq_t q;
    reopen(&q);
    for (uint32_t i = 0; i < 10; i++) {
        tlog_passage_t p = P(i);
        TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_append(&q, &p));
    }
    expect_pending(&q, 0, 10);
    TEST_ASSERT_EQUAL_INT(TLOG_ERR_ARG, tlog_pq_ack(&q, 11));
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_ack(&q, 4));
    expect_pending(&q, 4, 6);
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_ack(&q, 0));
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_ack(&q, 6));
    TEST_ASSERT_EQUAL_UINT32(0, tlog_pq_pending(&q));
}

/* spec "Offline queue": kept across a restart, in order. */
static void test_survives_restart(void) {
    tlog_pq_t q;
    reopen(&q);
    for (uint32_t i = 0; i < 30; i++) {
        tlog_passage_t p = P(i);
        tlog_pq_append(&q, &p);
    }
    tlog_pq_ack(&q, 12);
    reopen(&q);
    expect_pending(&q, 12, 18);
    tlog_passage_t p = P(30);
    tlog_pq_append(&q, &p);
    reopen(&q);
    expect_pending(&q, 12, 19);
}

/* Restart mid-flush: a batch was posted but the uplink lost power before storing the ack. The
 * batch is posted again with the same EPCs and times. */
static void test_restart_mid_flush(void) {
    tlog_pq_t q;
    reopen(&q);
    for (uint32_t i = 0; i < 20; i++) {
        tlog_passage_t p = P(i);
        tlog_pq_append(&q, &p);
    }
    tlog_pq_ack(&q, 5);  /* first batch stored */
    budget = 3;          /* the second batch's ack is cut off mid-record */
    TEST_ASSERT_NOT_EQUAL(TLOG_OK, tlog_pq_ack(&q, 5));
    reopen(&q);
    expect_pending(&q, 5, 15); /* the second batch again */
    tlog_pq_ack(&q, 5);
    reopen(&q);
    expect_pending(&q, 10, 10);
}

/* A passage torn by a power loss is skipped; the ones before it survive and appending goes on. */
static void test_torn_passage(void) {
    tlog_pq_t q;
    reopen(&q);
    for (uint32_t i = 0; i < 7; i++) {
        tlog_passage_t p = P(i);
        tlog_pq_append(&q, &p);
    }
    budget = 9;
    tlog_passage_t t = P(99);
    TEST_ASSERT_NOT_EQUAL(TLOG_OK, tlog_pq_append(&q, &t));
    reopen(&q);
    expect_pending(&q, 0, 7);
    for (uint32_t i = 7; i < 10; i++) {
        tlog_passage_t p = P(i);
        TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_append(&q, &p));
    }
    reopen(&q);
    expect_pending(&q, 0, 10);
}

/* spec: at least 2000 unsent passages, across sector boundaries, in order, after a restart. */
static void test_capacity_while_offline(void) {
    tlog_pq_t q;
    reopen(&q);
    uint32_t n = 0;
    for (;;) {
        tlog_passage_t p = P(n);
        int e = tlog_pq_append(&q, &p);
        if (e == TLOG_ERR_NOSPACE) break;
        TEST_ASSERT_EQUAL_INT(TLOG_OK, e);
        n++;
    }
    TEST_ASSERT_TRUE_MESSAGE(n >= 2000, "holds 2000 unsent passages");
    TEST_ASSERT_EQUAL_UINT32(15 * 255 + 255, n); /* every sector, minus one header each */
    reopen(&q);
    expect_pending(&q, 0, n);
    /* the network is back: acking the oldest sector makes room again */
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_ack(&q, 300));
    tlog_passage_t p = P(n);
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_append(&q, &p));
    reopen(&q);
    expect_pending(&q, 300, n - 300 + 1);
}

/* An outage starting when the log is nearly full of acked records still keeps 2000+: acked
 * sectors are reclaimed one by one (compaction). */
static void test_capacity_after_long_use(void) {
    tlog_pq_t q;
    reopen(&q);
    uint32_t i = 0;
    for (; i < 3900; i++) { /* a race: every passage posted and acked in its own batch */
        tlog_passage_t p = P(i);
        TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_append(&q, &p));
        TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_ack(&q, 1));
    }
    uint32_t start = i;
    for (;; i++) { /* the hotspot goes away */
        tlog_passage_t p = P(i);
        if (tlog_pq_append(&q, &p) == TLOG_ERR_NOSPACE) break;
    }
    TEST_ASSERT_TRUE_MESSAGE(i - start >= 2000, "2000 unsent after long use");
    reopen(&q);
    expect_pending(&q, start, i - start);
}

/* Many races: never full while acked, the ring wears evenly, ordering holds. */
static void test_compaction_and_wear(void) {
    tlog_pq_t q;
    reopen(&q);
    uint32_t next = 0, acked = 0;
    for (int round = 0; round < 400; round++) {
        for (int k = 0; k < 37; k++) { /* passages arrive */
            tlog_passage_t p = P(next++);
            TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_append(&q, &p));
        }
        uint32_t batch = 5 + round % 20; /* the server takes some */
        if (batch > next - acked) batch = next - acked;
        TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_ack(&q, batch));
        acked += batch;
        if (next - acked > 1500) { /* catch up */
            tlog_pq_ack(&q, next - acked - 100);
            acked = next - 100;
        }
        if (round % 50 == 0) reopen(&q);
    }
    reopen(&q);
    expect_pending(&q, acked, next - acked);
    unsigned lo = ~0u, hi = 0;
    for (unsigned s = 0; s < SIZE / SECTOR; s++) {
        if (erases[s] < lo) lo = erases[s];
        if (erases[s] > hi) hi = erases[s];
    }
    TEST_ASSERT_TRUE_MESSAGE(hi - lo <= 1, "sectors wear evenly");
    TEST_ASSERT_TRUE(lo >= 1);
}

/* Power lost after erasing the next sector but before its header: nothing is lost. */
static void test_crash_while_starting_a_sector(void) {
    tlog_pq_t q;
    reopen(&q);
    for (uint32_t i = 0; i < 255; i++) { /* fill sector 0: header + 255 passages */
        tlog_passage_t p = P(i);
        TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_append(&q, &p));
    }
    budget = 4; /* the next append starts sector 1; its header is cut */
    tlog_passage_t p = P(255);
    TEST_ASSERT_NOT_EQUAL(TLOG_OK, tlog_pq_append(&q, &p));
    TEST_ASSERT_EQUAL_UINT(1, erases[1]);
    reopen(&q);
    expect_pending(&q, 0, 255);
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_append(&q, &p));
    reopen(&q);
    expect_pending(&q, 0, 256);
}

/* Passage times use 48 bits; tag ids all 32. */
static void test_field_ranges(void) {
    tlog_pq_t q;
    reopen(&q);
    tlog_passage_t p = {0xFFFFFFFFu, (1ull << 48) - 1, 255}, got;
    tlog_pq_append(&q, &p);
    reopen(&q);
    TEST_ASSERT_EQUAL_INT(TLOG_OK, tlog_pq_peek(&q, 0, &got));
    TEST_ASSERT_EQUAL_HEX32(p.epc, got.epc);
    TEST_ASSERT_EQUAL_UINT64(p.epoch_ms, got.epoch_ms);
    TEST_ASSERT_EQUAL_UINT8(255, got.gate);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_geometry);
    RUN_TEST(test_append_peek_ack);
    RUN_TEST(test_survives_restart);
    RUN_TEST(test_restart_mid_flush);
    RUN_TEST(test_torn_passage);
    RUN_TEST(test_capacity_while_offline);
    RUN_TEST(test_capacity_after_long_use);
    RUN_TEST(test_compaction_and_wear);
    RUN_TEST(test_crash_while_starting_a_sector);
    RUN_TEST(test_field_ranges);
    return UNITY_END();
}
