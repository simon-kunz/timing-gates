/*
 * Run state machine (design D6; spec crossing-detection, Run timing).
 * Platform-free, host-tested.
 *
 * Crossings tagged with the gate's role drive idle -> running -> finished:
 *  - start gate: starts a run (t0); during a run it restarts it (a new run, counter + 1)
 *  - split gate during a run: split n, n = the gate's split index
 *  - finish gate during a run: finish, the run is finished
 *  - start-finish gate: the first crossing starts a run, every later one is lap n
 *  - a split or finish outside a run gives no event
 * A beam-timed crossing replaces an RSSI-timed event of the same gate within 1.5 s: the event
 * keeps its number and gets the new time and the beam flag (replacing a start moves t0). An RSSI
 * crossing within 1.5 s of a beam-timed event of the same gate is the same pass and is dropped.
 * A run with no crossing for 30 min is abandoned (back to idle).
 * Event times are ms since t0 (u24, saturated); all clocks are the tag's µs clock.
 */
#ifndef TLOG_RUN_H
#define TLOG_RUN_H

#include <stdbool.h>
#include <stdint.h>

#include "tlog/timing.h"

#define TLOG_RUN_EVENTS 64                    /* events kept for the summary */
#define TLOG_RUN_REPLACE_US 1500000           /* beam replaces RSSI within this */
#define TLOG_RUN_ABANDON_US (30LL * 60 * 1000000) /* no crossing for 30 min */

typedef struct {
    uint8_t state;   /* TLOG_RUN_* */
    uint8_t counter; /* run counter, + 1 per run, wraps */
    int64_t t0_us;
    int64_t last_us; /* latest crossing of the run */
    uint8_t n;       /* events so far, numbered 1..n (saturates at 255) */
    uint8_t laps;
    struct {
        tlog_tev_t ev;
        int64_t t_us;
    } ev[TLOG_RUN_EVENTS]; /* event k at ev[(k - 1) % TLOG_RUN_EVENTS] */
} tlog_run_t;

/* What a crossing did. */
enum {
    TLOG_RUN_NONE = 0,     /* no event (split or finish outside a run, or before t0) */
    TLOG_RUN_NEW = 1,      /* a new event, out->event_seq == n */
    TLOG_RUN_REPLACED = 2, /* an earlier event got the beam time; same event_seq */
};

/* counter: the last run counter used (persisted by the caller), the next run gets counter + 1. */
void tlog_run_init(tlog_run_t *r, uint8_t counter);

/* A crossing of gate `gate_id` (its GATE beacon `gate`) at t_us. Fills *out for NEW and REPLACED. */
int tlog_run_crossing(tlog_run_t *r, uint8_t gate_id, const tlog_gate_t *gate, int64_t t_us, bool beam,
                      tlog_crossing_t *out);

/* Call periodically; abandons a run with no crossing for 30 min. Returns true when it did. */
bool tlog_run_tick(tlog_run_t *r, int64_t now_us);

/* Event k (1..n) of the current run, if still kept. */
bool tlog_run_event(const tlog_run_t *r, uint8_t k, tlog_tev_t *ev);

/* Run clock: ms since t0, saturated at TLOG_U24_MAX; 0 when idle. */
uint32_t tlog_run_clock_ms(const tlog_run_t *r, int64_t now_us);

/* Whether the tag runs the RSSI detector for this gate. Not for a start gate with a working
 * barrier: a rider waiting next to it would produce a crossing; its runs start from BREAKs. */
bool tlog_run_rssi_allowed(const tlog_gate_t *gate);

#endif
