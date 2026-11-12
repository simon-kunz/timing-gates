/*
 * ESP-NOW frame header of the timing units (design D3). Platform-free, host-tested.
 * Every frame:
 *   magic u8 | version u8 | kind u8 | flags u8 | unit u32 | packet_seq u32
 * then the body of its kind (tlog/timing.h). `unit` is the gate id for a gate and the tag id for
 * a tag; packet_seq is the sender's frame counter. All integers little-endian. Frames with another
 * magic are rejected (TLOG_ERR_FORMAT), frames of another version too (TLOG_ERR_VERSION).
 */
#ifndef TLOG_LINK_H
#define TLOG_LINK_H

#include <stddef.h>
#include <stdint.h>

#define TLOG_LINK_MAGIC 0xA7u
#define TLOG_LINK_VERSION 1u
#define TLOG_LINK_HEADER_SIZE 12u

enum {
    TLOG_LINK_GATE = 6,  /* gate ID beacon, 100 Hz */
    TLOG_LINK_TAG = 7,   /* tag ID beacon, 20 Hz */
    TLOG_LINK_BREAK = 8, /* light barrier break, 5 copies */
};

typedef struct {
    uint8_t kind;  /* TLOG_LINK_* */
    uint8_t flags; /* reserved, 0 */
    uint32_t unit;
    uint32_t packet_seq;
} tlog_link_hdr_t;

void tlog_link_hdr_encode(const tlog_link_hdr_t *h, uint8_t *out);
/* TLOG_OK, TLOG_ERR_FORMAT (short or wrong magic) or TLOG_ERR_VERSION. */
int tlog_link_hdr_decode(const uint8_t *in, size_t len, tlog_link_hdr_t *h);

#endif
