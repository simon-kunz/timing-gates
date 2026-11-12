#include "tlog/brk.h"

#include <string.h>

void tlog_brk_init(tlog_brk_t *b, uint32_t tag_id, int8_t thresh_dbm) {
    memset(b, 0, sizeof(*b));
    b->tag_id = tag_id;
    b->thresh_dbm = thresh_dbm;
}

void tlog_brk_rssi(tlog_brk_t *b, uint8_t gate, int rssi, int64_t rx_us) {
    if (rssi < b->thresh_dbm) return;
    int free_i = -1, oldest = 0;
    for (int i = 0; i < TLOG_BRK_GATES; i++) {
        if (b->g[i].used && b->g[i].gate == gate) {
            b->g[i].strong_us = rx_us;
            return;
        }
        if (!b->g[i].used && free_i < 0) free_i = i;
        if (b->g[i].strong_us < b->g[oldest].strong_us) oldest = i;
    }
    int i = free_i >= 0 ? free_i : oldest;
    b->g[i].used = true;
    b->g[i].gate = gate;
    b->g[i].strong_us = rx_us;
}

static bool heard_strongly(const tlog_brk_t *b, uint8_t gate, int64_t break_us) {
    for (int i = 0; i < TLOG_BRK_GATES; i++) {
        if (!b->g[i].used || b->g[i].gate != gate) continue;
        int64_t d = b->g[i].strong_us - break_us;
        if (d < 0) d = -d;
        return d <= TLOG_BRK_WINDOW_US;
    }
    return false;
}

int tlog_brk_copy(tlog_brk_t *b, uint8_t gate, const tlog_break_t *brk, int64_t rx_us, int64_t *break_us) {
    for (int i = 0; i < TLOG_BRK_SEEN; i++) {
        /* copies come within 50 ms; a sequence number seen more than 10 s ago is a new break (wrap) */
        if (b->seen[i].used && b->seen[i].gate == gate && b->seen[i].seq == brk->break_seq &&
            rx_us - b->seen[i].rx_us < 10000000)
            return TLOG_BRK_DUPLICATE;
    }
    b->seen[b->next_seen].used = true;
    b->seen[b->next_seen].gate = gate;
    b->seen[b->next_seen].seq = brk->break_seq;
    b->seen[b->next_seen].rx_us = rx_us;
    b->next_seen = (uint8_t)((b->next_seen + 1) % TLOG_BRK_SEEN);

    int64_t t = rx_us - (int64_t)brk->age_us;
    if (brk->nearest && brk->nearest != b->tag_id) return TLOG_BRK_OTHER;
    if (!brk->nearest && !heard_strongly(b, gate, t)) return TLOG_BRK_FAR;
    *break_us = t;
    return TLOG_BRK_MINE;
}
