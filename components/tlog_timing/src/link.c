#include "tlog/link.h"

#include "tlog/result.h"

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void tlog_link_hdr_encode(const tlog_link_hdr_t *h, uint8_t *out) {
    out[0] = TLOG_LINK_MAGIC;
    out[1] = TLOG_LINK_VERSION;
    out[2] = h->kind;
    out[3] = h->flags;
    put32(out + 4, h->unit);
    put32(out + 8, h->packet_seq);
}

int tlog_link_hdr_decode(const uint8_t *in, size_t len, tlog_link_hdr_t *h) {
    if (len < TLOG_LINK_HEADER_SIZE || in[0] != TLOG_LINK_MAGIC) return TLOG_ERR_FORMAT;
    if (in[1] != TLOG_LINK_VERSION) return TLOG_ERR_VERSION;
    h->kind = in[2];
    h->flags = in[3];
    h->unit = get32(in + 4);
    h->packet_seq = get32(in + 8);
    return TLOG_OK;
}
