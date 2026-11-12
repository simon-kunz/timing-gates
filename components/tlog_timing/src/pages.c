#include "tlog/pages.h"

#include <string.h>

void tlog_pages_init(tlog_pages_t *p) { memset(p, 0, sizeof(*p)); }

void tlog_pages_event(tlog_pages_t *p, int64_t now_us) {
    p->have_event = true;
    p->event_us = now_us;
    p->cursor = 0;
}

void tlog_pages_next(tlog_pages_t *p, const tlog_run_t *r, int64_t now_us, uint8_t *k, uint32_t *interval_ms) {
    uint8_t n = r->state == TLOG_RUN_IDLE ? 0 : r->n;
    *k = n;
    int64_t since = p->have_event ? now_us - p->event_us : INT64_MAX;
    if (n && since < TLOG_PAGES_FAST_US) {
        *interval_ms = TLOG_PAGES_FAST_MS;
        return;
    }
    if (r->state != TLOG_RUN_FINISHED || n < 2 || since >= TLOG_PAGES_FAST_US + TLOG_PAGES_SUMMARY_US) {
        p->cursor = 0;
        *interval_ms = TLOG_PAGES_SLOW_MS;
        return;
    }
    /* the summary: pairs (n, n-1), (n-2, n-3), ... cover every event the run still keeps */
    uint8_t oldest = n > TLOG_RUN_EVENTS ? (uint8_t)(n - TLOG_RUN_EVENTS + 1) : 1;
    if (p->cursor == 0 || p->cursor > n) p->cursor = n;
    *k = p->cursor;
    p->cursor = (p->cursor >= oldest + 2) ? (uint8_t)(p->cursor - 2) : n;
    *interval_ms = TLOG_PAGES_SUMMARY_MS;
}
