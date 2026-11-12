/* Frame codecs (task 1.1), advert codec, tag id and read-stream lines (1.2). */
#include <string.h>

#include "tlog/result.h"
#include "tlog/link.h"
#include "tlog/timing.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- 1.1 frames ---- */

static size_t frame(uint8_t kind, uint32_t unit, uint32_t seq, uint8_t *buf) {
    tlog_link_hdr_t h = {.kind = kind, .unit = unit, .packet_seq = seq};
    tlog_link_hdr_encode(&h, buf);
    return TLOG_LINK_HEADER_SIZE;
}

static void test_frame_sizes(void) {
    TEST_ASSERT_EQUAL(16, TLOG_LINK_HEADER_SIZE + TLOG_GATE_BODY);
    TEST_ASSERT_EQUAL(16, TLOG_LINK_HEADER_SIZE + TLOG_TAG_BODY);
    TEST_ASSERT_EQUAL(24, TLOG_LINK_HEADER_SIZE + TLOG_BREAK_BODY);
    TEST_ASSERT_EQUAL(6, TLOG_LINK_GATE);
    TEST_ASSERT_EQUAL(8, TLOG_LINK_BREAK);
}

static void test_gate_round_trip(void) {
    uint8_t buf[16];
    size_t o = frame(TLOG_LINK_GATE, 3, 99, buf);
    tlog_gate_t g = {.role = TLOG_ROLE_SPLIT, .index = 2, .flags = TLOG_GATE_F_BARRIER | TLOG_GATE_F_FAULT,
                     .battery = TLOG_BATTERY_UNKNOWN}, d;
    tlog_gate_encode(&g, buf + o);
    tlog_link_hdr_t h;
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_link_hdr_decode(buf, sizeof(buf), &h));
    TEST_ASSERT_EQUAL_UINT8(TLOG_LINK_GATE, h.kind);
    TEST_ASSERT_EQUAL_UINT32(3, h.unit);
    TEST_ASSERT_EQUAL_UINT32(99, h.packet_seq);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_gate_decode(buf + o, sizeof(buf) - o, &d));
    TEST_ASSERT_EQUAL_UINT8(TLOG_ROLE_SPLIT, d.role);
    TEST_ASSERT_EQUAL_UINT8(2, d.index);
    TEST_ASSERT_EQUAL_UINT8(TLOG_GATE_F_BARRIER | TLOG_GATE_F_FAULT, d.flags);
    TEST_ASSERT_EQUAL_UINT8(255, d.battery);
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_gate_decode(buf + o, TLOG_GATE_BODY - 1, &d));
    buf[o] = 0; /* no role */
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_gate_decode(buf + o, TLOG_GATE_BODY, &d));
    buf[o] = 5;
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_gate_decode(buf + o, TLOG_GATE_BODY, &d));
}

static void test_tag_round_trip(void) {
    uint8_t buf[TLOG_TAG_BODY];
    tlog_tag_t t = {.state = TLOG_RUN_RUNNING, .battery = 87}, d;
    tlog_tag_encode(&t, buf);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_tag_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_RUNNING, d.state);
    TEST_ASSERT_EQUAL_UINT8(87, d.battery);
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_tag_decode(buf, 3, &d));
}

static void test_break_round_trip(void) {
    uint8_t buf[TLOG_BREAK_BODY];
    tlog_break_t b = {.break_seq = 0xBEEF, .copy = 4, .flags = 0, .age_us = 48123, .nearest = 0x1A2B3C4Du}, d;
    tlog_break_encode(&b, buf);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_break_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT16(0xBEEF, d.break_seq);
    TEST_ASSERT_EQUAL_UINT8(4, d.copy);
    TEST_ASSERT_EQUAL_UINT32(48123, d.age_us);
    TEST_ASSERT_EQUAL_UINT32(0x1A2B3C4Du, d.nearest);
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_break_decode(buf, TLOG_BREAK_BODY - 1, &d));
}

/* ---- 1.2 advert ---- */

static tlog_advert_t sample_advert(void) {
    tlog_advert_t a = {.state = TLOG_RUN_RUNNING, .tag_id = 0x1A2B3C4Du, .run_counter = 17, .n = 3,
                       .run_ms = 61000, .battery = 76, .k = 3,
                       .a = {.gate = 3, .kind = TLOG_EV_FINISH, .beam = true, .index = 0, .t_ms = 60250},
                       .b = {.gate = 2, .kind = TLOG_EV_SPLIT, .beam = false, .index = 1, .t_ms = 42315}};
    return a;
}

static void test_advert_layout(void) {
    uint8_t buf[TLOG_ADVERT_SIZE];
    tlog_advert_t a = sample_advert(), d;
    tlog_advert_encode(&a, buf);
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[0]); /* version 1, running */
    TEST_ASSERT_EQUAL_HEX8(0x4D, buf[1]); /* tag id, little-endian */
    TEST_ASSERT_EQUAL_HEX8(0x1A, buf[4]);
    TEST_ASSERT_EQUAL_UINT8(17, buf[5]);
    TEST_ASSERT_EQUAL_UINT8(3, buf[6]);
    TEST_ASSERT_EQUAL_HEX8(61000 & 0xFF, buf[7]);
    TEST_ASSERT_EQUAL_HEX8((61000 >> 16) & 0xFF, buf[9]);
    TEST_ASSERT_EQUAL_UINT8(76, buf[10]);
    TEST_ASSERT_EQUAL_UINT8(3, buf[11]);
    TEST_ASSERT_EQUAL_UINT8(3, buf[12]);    /* A: gate */
    TEST_ASSERT_EQUAL_HEX8(0x41, buf[13]);  /* A: finish, beam */
    TEST_ASSERT_EQUAL_UINT8(2, buf[18]);    /* B: gate */
    TEST_ASSERT_EQUAL_HEX8(0x20, buf[19]);  /* B: split, no beam */
    TEST_ASSERT_EQUAL_UINT8(1, buf[20]);    /* B: index */

    TEST_ASSERT_EQUAL(TLOG_OK, tlog_advert_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_RUNNING, d.state);
    TEST_ASSERT_EQUAL_UINT32(0x1A2B3C4Du, d.tag_id);
    TEST_ASSERT_EQUAL_UINT8(17, d.run_counter);
    TEST_ASSERT_EQUAL_UINT8(3, d.n);
    TEST_ASSERT_EQUAL_UINT32(61000, d.run_ms);
    TEST_ASSERT_EQUAL_UINT8(3, d.k);
    TEST_ASSERT_EQUAL_UINT8(TLOG_EV_FINISH, d.a.kind);
    TEST_ASSERT_TRUE(d.a.beam);
    TEST_ASSERT_EQUAL_UINT32(60250, d.a.t_ms);
    TEST_ASSERT_EQUAL_UINT8(TLOG_EV_SPLIT, d.b.kind);
    TEST_ASSERT_FALSE(d.b.beam);
    TEST_ASSERT_EQUAL_UINT8(1, d.b.index);
    TEST_ASSERT_EQUAL_UINT32(42315, d.b.t_ms);

    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_advert_decode(buf, TLOG_ADVERT_SIZE - 1, &d));
    buf[0] = 0x21; /* version 2 */
    TEST_ASSERT_EQUAL(TLOG_ERR_VERSION, tlog_advert_decode(buf, sizeof(buf), &d));
}

static void test_advert_run_clock_range(void) {
    uint8_t buf[TLOG_ADVERT_SIZE];
    tlog_advert_t a = sample_advert(), d;
    a.run_ms = 0;
    tlog_advert_encode(&a, buf);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_advert_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT32(0, d.run_ms);
    a.run_ms = 4u * 3600000u + 36u * 60000u; /* 4.6 h = 16 560 000 ms, still fits the u24 */
    a.a.t_ms = a.run_ms - 1;
    tlog_advert_encode(&a, buf);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_advert_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT32(16560000u, d.run_ms);
    TEST_ASSERT_EQUAL_UINT32(16559999u, d.a.t_ms);
    a.run_ms = 20000000u; /* beyond the u24: saturates */
    tlog_advert_encode(&a, buf);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_advert_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT32(TLOG_U24_MAX, d.run_ms);
}

static void test_advert_first_event_has_empty_b(void) {
    uint8_t buf[TLOG_ADVERT_SIZE];
    tlog_advert_t a = sample_advert(), d;
    a.n = 1;
    a.k = 1;
    a.a = (tlog_tev_t){.gate = 1, .kind = TLOG_EV_START, .beam = true, .t_ms = 0};
    /* whatever is in b, k = 1 sends B as zeros */
    tlog_advert_encode(&a, buf);
    for (int i = 18; i < 24; i++) TEST_ASSERT_EQUAL_HEX8(0, buf[i]);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_advert_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT8(TLOG_EV_START, d.a.kind);
    TEST_ASSERT_EQUAL_UINT8(TLOG_EV_NONE, d.b.kind);
}

static void test_advert_idle_without_events(void) {
    uint8_t buf[TLOG_ADVERT_SIZE];
    tlog_advert_t a = {.state = TLOG_RUN_IDLE, .tag_id = 5, .battery = TLOG_BATTERY_UNKNOWN}, d;
    tlog_advert_encode(&a, buf);
    for (int i = 11; i < 24; i++) TEST_ASSERT_EQUAL_HEX8(0, buf[i]);
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_advert_decode(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT8(TLOG_RUN_IDLE, d.state);
    TEST_ASSERT_EQUAL_UINT8(TLOG_EV_NONE, d.a.kind);
}

/* ---- 1.2 tag id ---- */

static void test_tag_id_from_mac(void) {
    const uint8_t mac[6] = {0x34, 0x85, 0x1A, 0x2B, 0x3C, 0x4D};
    uint32_t id = tlog_tag_id_from_mac(mac);
    TEST_ASSERT_EQUAL_HEX32(0x1A2B3C4Du, id);
    char s[9];
    tlog_id_hex(id, s);
    TEST_ASSERT_EQUAL_STRING("1A2B3C4D", s);
    const uint8_t mac2[6] = {0xFF, 0xFF, 0x00, 0x0a, 0xbc, 0x01};
    tlog_id_hex(tlog_tag_id_from_mac(mac2), s);
    TEST_ASSERT_EQUAL_STRING("000ABC01", s); /* 8 digits, upper case, leading zeros */
}

/* ---- 1.2 read-stream lines ---- */

static void test_tgr_line(void) {
    char line[TLOG_LINE_MAX];
    tlog_tgr_t r = {.id = 0x1A2B3C4Du, .rssi = -67, .us = 123456789}, rd;
    tlog_tgs_t sd;
    size_t n = tlog_tgr_format(&r, line, sizeof(line));
    TEST_ASSERT_EQUAL(strlen(line), n);
    TEST_ASSERT_EQUAL_STRING_LEN("$TGR,1A2B3C4D,-67,123456789*", line, 28);
    /* checksum is the XOR between $ and * */
    uint8_t x = tlog_line_xor(line + 1, n - 4);
    char hx[3];
    snprintf(hx, sizeof(hx), "%02X", x);
    TEST_ASSERT_EQUAL_STRING(hx, line + n - 2);
    TEST_ASSERT_EQUAL(TLOG_LINE_TGR, tlog_line_parse(line, n, &rd, &sd));
    TEST_ASSERT_EQUAL_HEX32(r.id, rd.id);
    TEST_ASSERT_EQUAL_INT8(-67, rd.rssi);
    TEST_ASSERT_EQUAL_INT64(123456789, rd.us);
    /* with a line ending, and lower-case hex in the checksum */
    char crlf[TLOG_LINE_MAX];
    snprintf(crlf, sizeof(crlf), "%s\r\n", line);
    TEST_ASSERT_EQUAL(TLOG_LINE_TGR, tlog_line_parse(crlf, strlen(crlf), &rd, &sd));
    TEST_ASSERT_EQUAL(0, tlog_tgr_format(&r, line, 20)); /* too small */
}

static void test_tgs_line(void) {
    char line[TLOG_LINE_MAX];
    tlog_tgs_t s = {.gate = 3, .role = TLOG_ROLE_SPLIT, .index = 2, .us = 5000000, .drops = 12}, sd;
    tlog_tgr_t rd;
    size_t n = tlog_tgs_format(&s, line, sizeof(line));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING_LEN("$TGS,3,split2,5000000,12*", line, 25);
    TEST_ASSERT_EQUAL(TLOG_LINE_TGS, tlog_line_parse(line, n, &rd, &sd));
    TEST_ASSERT_EQUAL_UINT8(3, sd.gate);
    TEST_ASSERT_EQUAL_UINT8(TLOG_ROLE_SPLIT, sd.role);
    TEST_ASSERT_EQUAL_UINT8(2, sd.index);
    TEST_ASSERT_EQUAL_INT64(5000000, sd.us);
    TEST_ASSERT_EQUAL_UINT32(12, sd.drops);
    s.role = TLOG_ROLE_STARTFINISH;
    n = tlog_tgs_format(&s, line, sizeof(line));
    TEST_ASSERT_EQUAL(TLOG_LINE_TGS, tlog_line_parse(line, n, &rd, &sd));
    TEST_ASSERT_EQUAL_UINT8(TLOG_ROLE_STARTFINISH, sd.role);
}

static void test_bad_and_missing_checksums(void) {
    char line[TLOG_LINE_MAX];
    tlog_tgr_t r = {.id = 0xDEADBEEFu, .rssi = -80, .us = 1}, rd;
    tlog_tgs_t sd;
    size_t n = tlog_tgr_format(&r, line, sizeof(line));
    /* wrong checksum */
    line[n - 1] = line[n - 1] == '0' ? '1' : '0';
    TEST_ASSERT_EQUAL(TLOG_ERR_CRC, tlog_line_parse(line, n, &rd, &sd));
    /* a corrupted field with the old checksum */
    n = tlog_tgr_format(&r, line, sizeof(line));
    line[6] = 'F'; /* DEADBEEF -> DFADBEEF */
    TEST_ASSERT_EQUAL(TLOG_ERR_CRC, tlog_line_parse(line, n, &rd, &sd));
    /* missing checksum */
    const char *nock = "$TGR,DEADBEEF,-80,1";
    TEST_ASSERT_EQUAL(TLOG_ERR_CRC, tlog_line_parse(nock, strlen(nock), &rd, &sd));
    /* truncated checksum */
    n = tlog_tgr_format(&r, line, sizeof(line));
    TEST_ASSERT_EQUAL(TLOG_ERR_CRC, tlog_line_parse(line, n - 1, &rd, &sd));
    /* ROM boot noise and unknown sentences */
    const char *boot = "ESP-ROM:esp32c3-api1-20210207";
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_line_parse(boot, strlen(boot), &rd, &sd));
    char gga[TLOG_LINE_MAX] = "$GPGGA,1,2";
    size_t g = strlen(gga);
    snprintf(gga + g, sizeof(gga) - g, "*%02X", tlog_line_xor(gga + 1, g - 1));
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_line_parse(gga, strlen(gga), &rd, &sd));
    /* valid checksum but a bad field */
    char bad[TLOG_LINE_MAX] = "$TGR,XYZ,-80,1";
    size_t b = strlen(bad);
    snprintf(bad + b, sizeof(bad) - b, "*%02X", tlog_line_xor(bad + 1, b - 1));
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_line_parse(bad, strlen(bad), &rd, &sd));
}

static void test_role_names(void) {
    uint8_t role, index;
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_role_parse("split 10", 8, &role, &index));
    TEST_ASSERT_EQUAL_UINT8(TLOG_ROLE_SPLIT, role);
    TEST_ASSERT_EQUAL_UINT8(10, index);
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_role_parse("split0", 6, &role, &index));
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_role_parse("split11", 7, &role, &index));
    TEST_ASSERT_EQUAL(TLOG_ERR_FORMAT, tlog_role_parse("split", 5, &role, &index));
    TEST_ASSERT_EQUAL(TLOG_OK, tlog_role_parse("finish", 6, &role, &index));
    TEST_ASSERT_EQUAL_UINT8(TLOG_ROLE_FINISH, role);
    char out[16];
    TEST_ASSERT_EQUAL(6, tlog_role_format(TLOG_ROLE_SPLIT, 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("split3", out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_frame_sizes);
    RUN_TEST(test_gate_round_trip);
    RUN_TEST(test_tag_round_trip);
    RUN_TEST(test_break_round_trip);
    RUN_TEST(test_advert_layout);
    RUN_TEST(test_advert_run_clock_range);
    RUN_TEST(test_advert_first_event_has_empty_b);
    RUN_TEST(test_advert_idle_without_events);
    RUN_TEST(test_tag_id_from_mac);
    RUN_TEST(test_tgr_line);
    RUN_TEST(test_tgs_line);
    RUN_TEST(test_bad_and_missing_checksums);
    RUN_TEST(test_role_names);
    return UNITY_END();
}
