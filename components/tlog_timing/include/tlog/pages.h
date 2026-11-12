/*
 * Advert pages (design D7; spec crossing-events, "Run summary after a
 * finish"). Platform-free, host-tested. An advert carries the event pair (k, k-1); this decides k
 * for each advert and how long until the next one:
 *  - for 5 s after each event (new or replaced): k = n, every 100 ms
 *  - running, or idle: k = n, every 1 s
 *  - finished, after those 5 s and for 2 min: the summary, k = n, n-2, n-4, ... down to 1 or 2
 *    (as far as the run still keeps events), every 500 ms, round and round
 *  - finished, after the summary: k = n, every 1 s
 * A new run ends the summary: its start is an event, and the run is no longer finished.
 */
#ifndef TLOG_PAGES_H
#define TLOG_PAGES_H

#include <stdbool.h>
#include <stdint.h>

#include "tlog/run.h"

#define TLOG_PAGES_FAST_US 5000000LL      /* 10 Hz after an event */
#define TLOG_PAGES_SUMMARY_US 120000000LL /* then the summary, for 2 min */
#define TLOG_PAGES_FAST_MS 100u
#define TLOG_PAGES_SUMMARY_MS 500u
#define TLOG_PAGES_SLOW_MS 1000u

typedef struct {
    bool have_event;
    int64_t event_us; /* the latest event */
    uint8_t cursor;   /* next k of the summary, 0 = start from n */
} tlog_pages_t;

void tlog_pages_init(tlog_pages_t *p);
/* A run event happened (NEW or REPLACED from tlog_run_crossing) at now_us. */
void tlog_pages_event(tlog_pages_t *p, int64_t now_us);
/* The page for an advert sent at now_us: *k (0 when the run has no events) and the interval
 * until the next advert. */
void tlog_pages_next(tlog_pages_t *p, const tlog_run_t *r, int64_t now_us, uint8_t *k, uint32_t *interval_ms);

#endif
