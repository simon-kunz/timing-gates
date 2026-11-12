/*
 * BREAK handling on the tag (task 4.3; spec crossing-detection, "Crossing from
 * a BREAK event"). Platform-free, host-tested.
 *
 * A gate sends each BREAK 5 times within 50 ms; every copy carries the same break sequence number
 * and its own age, so any one copy gives the break time (receive time - age). The tag takes a break
 * as its own crossing of that gate if:
 *  - the BREAK names this tag (the gate heard it strongest within 1 s of the break), or
 *  - it names no tag and this tag heard that gate at or above the crossing threshold within 1.5 s
 *    of the break.
 * A BREAK naming another tag is ignored. Only the first copy of a break counts; later copies of the
 * same break (gate, sequence number) are ignored, whatever the first one decided.
 * Replacing an RSSI crossing with the beam time is the run state machine's job (tlog/run.h).
 */
#ifndef TLOG_BRK_H
#define TLOG_BRK_H

#include <stdbool.h>
#include <stdint.h>

#include "tlog/timing.h"

#define TLOG_BRK_GATES 8         /* gates whose RSSI is tracked */
#define TLOG_BRK_SEEN 16         /* recent breaks remembered for deduplication */
#define TLOG_BRK_WINDOW_US 1500000

typedef struct {
    uint32_t tag_id;
    int8_t thresh_dbm;
    struct {
        bool used;
        uint8_t gate;
        int64_t strong_us; /* last frame at or above the threshold */
    } g[TLOG_BRK_GATES];
    struct {
        bool used;
        uint8_t gate;
        uint16_t seq;
        int64_t rx_us;
    } seen[TLOG_BRK_SEEN];
    uint8_t next_seen;
} tlog_brk_t;

enum {
    TLOG_BRK_MINE = 1,      /* a crossing: *break_us is its time */
    TLOG_BRK_DUPLICATE = 0, /* a later copy of a break already handled */
    TLOG_BRK_OTHER = -1,    /* names another tag */
    TLOG_BRK_FAR = -2,      /* names no tag, and this tag didn't hear the gate strongly enough */
};

void tlog_brk_init(tlog_brk_t *b, uint32_t tag_id, int8_t thresh_dbm);
/* Every frame of a gate as heard (rssi in dBm at rx_us). */
void tlog_brk_rssi(tlog_brk_t *b, uint8_t gate, int rssi, int64_t rx_us);
/* A BREAK copy from `gate`, received at rx_us. Returns TLOG_BRK_*. */
int tlog_brk_copy(tlog_brk_t *b, uint8_t gate, const tlog_break_t *brk, int64_t rx_us, int64_t *break_us);

#endif
