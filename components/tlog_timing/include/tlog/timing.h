/*
 * Timing gates: frame bodies, BLE advert codec, read-stream lines and the tag id
 * (design D3, D7, D7a, D9). Platform-free, host-tested.
 *
 * Frames use the header in tlog/link.h with kinds 6-8; this file encodes and decodes the body
 * after the 12-byte header. All integers little-endian.
 *   GATE (6)      role u8 | index u8 | flags u8 | battery u8                              16 B
 *   TAG (7)       state u8 | battery u8 | reserved u16                                    16 B
 *   BREAK (8)     break_seq u16 | copy u8 | flags u8 | age_us u32 | nearest u32           24 B
 * The sizes on the right are whole frames, header included. The header's `unit` is the gate id
 * for a gate and the tag id for a tag; its packet_seq is the beacon sequence number.
 */
#ifndef TLOG_TIMING_H
#define TLOG_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TLOG_GATE_BODY 4u
#define TLOG_TAG_BODY 4u
#define TLOG_BREAK_BODY 12u
#define TLOG_TEV_SIZE 6u      /* one event in an advert */
#define TLOG_ADVERT_SIZE 24u  /* manufacturer data payload after the company id */
#define TLOG_ADVERT_COMPANY 0xFFFFu
#define TLOG_ADVERT_VERSION 1u
#define TLOG_U24_MAX 0xFFFFFFu /* ms: about 4.66 h */

/* Gate roles (GATE beacon, console, read stream). */
enum {
    TLOG_ROLE_START = 1,
    TLOG_ROLE_SPLIT = 2,
    TLOG_ROLE_FINISH = 3,
    TLOG_ROLE_STARTFINISH = 4,
};

#define TLOG_GATE_F_BARRIER 0x01u      /* a light barrier is configured */
#define TLOG_GATE_F_FAULT 0x02u        /* the barrier is in fault (blocked >= 10 s) */
#define TLOG_GATE_F_UNCONFIGURED 0x04u /* id and role are defaults */
#define TLOG_BATTERY_UNKNOWN 255u

/* Run states (TAG beacon, advert). */
enum {
    TLOG_RUN_IDLE = 0,
    TLOG_RUN_RUNNING = 1,
    TLOG_RUN_FINISHED = 2,
};

/* Event kinds (advert). */
enum {
    TLOG_EV_NONE = 0,
    TLOG_EV_START = 1,
    TLOG_EV_SPLIT = 2,
    TLOG_EV_LAP = 3,
    TLOG_EV_FINISH = 4,
};

typedef struct {
    uint8_t role;    /* TLOG_ROLE_* */
    uint8_t index;   /* split index 1-10, else 0 */
    uint8_t flags;   /* TLOG_GATE_F_* */
    uint8_t battery; /* %, TLOG_BATTERY_UNKNOWN */
} tlog_gate_t;

typedef struct {
    uint8_t state;   /* TLOG_RUN_* */
    uint8_t battery;
} tlog_tag_t;

typedef struct {
    uint16_t break_seq;
    uint8_t copy;     /* 0..4 */
    uint8_t flags;    /* reserved, 0 */
    uint32_t age_us;  /* send time - break time */
    uint32_t nearest; /* tag id heard strongest within 1 s of the break, 0 = none */
} tlog_break_t;

/* One run event, 6 bytes on the air: gate u8 | kind u8 (high nibble kind, bit 0 beam) |
 * index u8 | t_ms u24. */
typedef struct {
    uint8_t gate;  /* gate id */
    uint8_t kind;  /* TLOG_EV_* */
    bool beam;     /* beam-timed */
    uint8_t index; /* split n or lap n, else 0 */
    uint32_t t_ms; /* time since t0, saturated at TLOG_U24_MAX */
} tlog_tev_t;

/* A run event with its crossing time on the tag's clock (output of the run state machine). */
typedef struct {
    tlog_tev_t ev;
    int64_t crossing_us; /* tag's µs clock */
    uint8_t event_seq;   /* event number within the run */
} tlog_crossing_t;

/* Encoders write exactly the body size. Decoders return TLOG_OK, or TLOG_ERR_FORMAT for a body
 * shorter than its size or an invalid role / kind. */
void tlog_gate_encode(const tlog_gate_t *g, uint8_t *out);
int tlog_gate_decode(const uint8_t *in, size_t len, tlog_gate_t *g);
void tlog_tag_encode(const tlog_tag_t *t, uint8_t *out);
int tlog_tag_decode(const uint8_t *in, size_t len, tlog_tag_t *t);
void tlog_break_encode(const tlog_break_t *b, uint8_t *out);
int tlog_break_decode(const uint8_t *in, size_t len, tlog_break_t *b);

/* A single event (TLOG_TEV_SIZE bytes). An all-zero event decodes as kind TLOG_EV_NONE. */
void tlog_tev_encode(const tlog_tev_t *e, uint8_t *out);
int tlog_tev_decode(const uint8_t *in, tlog_tev_t *e);

/* ---- BLE advert (D7), version 1, 24 bytes ----
 *  0     version (high nibble) | run state (low nibble)
 *  1-4   tag id u32
 *  5     run counter
 *  6     event count n
 *  7-9   run clock, ms since t0 (u24)
 *  10    battery %
 *  11    number k of event A
 *  12-17 event A (= event k)
 *  18-23 event B (= event k-1, all zero if k <= 1)
 */
typedef struct {
    uint8_t state;       /* TLOG_RUN_* */
    uint32_t tag_id;
    uint8_t run_counter;
    uint8_t n;
    uint32_t run_ms;     /* saturated at TLOG_U24_MAX */
    uint8_t battery;
    uint8_t k;
    tlog_tev_t a, b;
} tlog_advert_t;

void tlog_advert_encode(const tlog_advert_t *a, uint8_t *out);
/* TLOG_ERR_FORMAT if shorter than 24 bytes, TLOG_ERR_VERSION for another version. */
int tlog_advert_decode(const uint8_t *in, size_t len, tlog_advert_t *a);

/* ---- Tag id (D7a): the low 4 bytes of the Wi-Fi MAC, as 8 upper-case hex digits ---- */
uint32_t tlog_tag_id_from_mac(const uint8_t mac[6]);
/* Writes 8 hex digits and a NUL into out (9 bytes). */
void tlog_id_hex(uint32_t id, char out[9]);

/* ---- Read stream lines (D9) ----
 *   $TGR,<id 8 hex>,<rssi>,<us>*hh                 one frame heard (gate: tag id; tag trace: gate id)
 *   $TGS,<gate id>,<role>,<us>,<drops>*hh          gate status, once a second
 * hh is the XOR of the characters between '$' and '*', as two upper-case hex digits (NMEA).
 * Encoders write the line without a line ending and return its length, or 0 if cap is too small.
 * <role> is start, split<n>, finish or startfinish.
 */
#define TLOG_LINE_MAX 64u

typedef struct {
    uint32_t id;
    int8_t rssi;
    int64_t us;
} tlog_tgr_t;

typedef struct {
    uint8_t gate;
    uint8_t role;  /* TLOG_ROLE_* */
    uint8_t index; /* split n */
    int64_t us;
    uint32_t drops;
} tlog_tgs_t;

enum {
    TLOG_LINE_TGR = 1,
    TLOG_LINE_TGS = 2,
};

size_t tlog_tgr_format(const tlog_tgr_t *r, char *out, size_t cap);
size_t tlog_tgs_format(const tlog_tgs_t *s, char *out, size_t cap);
/* Parses one line (a trailing CR/LF is ignored). Returns TLOG_LINE_TGR or TLOG_LINE_TGS and fills
 * the matching struct; TLOG_ERR_CRC for a missing or wrong checksum, TLOG_ERR_FORMAT for anything
 * else (unknown sentence, bad field). */
int tlog_line_parse(const char *line, size_t len, tlog_tgr_t *r, tlog_tgs_t *s);
/* The NMEA XOR over [p, p+len). */
uint8_t tlog_line_xor(const char *p, size_t len);

/* Role names shared by the console and the stream: "start", "split<n>", "finish", "startfinish".
 * tlog_role_format returns the length written (cap >= 12 suffices), tlog_role_parse TLOG_OK or
 * TLOG_ERR_FORMAT (split index must be 1-10). */
size_t tlog_role_format(uint8_t role, uint8_t index, char *out, size_t cap);
int tlog_role_parse(const char *s, size_t len, uint8_t *role, uint8_t *index);

#endif
