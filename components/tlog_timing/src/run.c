#include "tlog/run.h"

#include <string.h>

void tlog_run_init(tlog_run_t *r, uint8_t counter) {
    memset(r, 0, sizeof(*r));
    r->state = TLOG_RUN_IDLE;
    r->counter = counter;
}

static uint32_t since_t0_ms(const tlog_run_t *r, int64_t t_us) {
    int64_t dt = t_us - r->t0_us;
    if (dt <= 0) return 0;
    int64_t ms = (dt + 500) / 1000;
    return ms > TLOG_U24_MAX ? TLOG_U24_MAX : (uint32_t)ms;
}

static void fill(const tlog_run_t *r, uint8_t k, tlog_crossing_t *out) {
    const tlog_tev_t *e = &r->ev[(k - 1) % TLOG_RUN_EVENTS].ev;
    out->ev = *e;
    out->crossing_us = r->ev[(k - 1) % TLOG_RUN_EVENTS].t_us;
    out->event_seq = k;
}

static int add(tlog_run_t *r, uint8_t gate_id, uint8_t kind, uint8_t index, int64_t t_us, bool beam,
               tlog_crossing_t *out) {
    if (r->n == 255) return TLOG_RUN_NONE;
    r->n++;
    tlog_tev_t *e = &r->ev[(r->n - 1) % TLOG_RUN_EVENTS].ev;
    e->gate = gate_id;
    e->kind = kind;
    e->beam = beam;
    e->index = index;
    e->t_ms = since_t0_ms(r, t_us);
    r->ev[(r->n - 1) % TLOG_RUN_EVENTS].t_us = t_us;
    r->last_us = t_us;
    if (out) fill(r, r->n, out);
    return TLOG_RUN_NEW;
}

static int start(tlog_run_t *r, uint8_t gate_id, int64_t t_us, bool beam, tlog_crossing_t *out) {
    uint8_t counter = (uint8_t)(r->counter + 1);
    tlog_run_init(r, counter);
    r->state = TLOG_RUN_RUNNING;
    r->t0_us = t_us;
    return add(r, gate_id, TLOG_EV_START, 0, t_us, beam, out);
}

/* The latest RSSI-timed event of this gate within the replace window, or 0. */
static uint8_t replaceable(const tlog_run_t *r, uint8_t gate_id, int64_t t_us) {
    uint8_t oldest = r->n > TLOG_RUN_EVENTS ? (uint8_t)(r->n - TLOG_RUN_EVENTS + 1) : 1;
    for (int k = r->n; k >= oldest && k >= 1; k--) {
        const tlog_tev_t *e = &r->ev[(k - 1) % TLOG_RUN_EVENTS].ev;
        int64_t et = r->ev[(k - 1) % TLOG_RUN_EVENTS].t_us;
        int64_t d = t_us - et;
        if (d < 0) d = -d;
        if (d > TLOG_RUN_REPLACE_US) {
            if (et < t_us) break; /* older events are further away */
            continue;
        }
        if (e->gate == gate_id && !e->beam) return (uint8_t)k;
    }
    return 0;
}

/* Whether this gate already has a beam-timed event within the replace window of t_us. */
static bool beam_near(const tlog_run_t *r, uint8_t gate_id, int64_t t_us) {
    uint8_t oldest = r->n > TLOG_RUN_EVENTS ? (uint8_t)(r->n - TLOG_RUN_EVENTS + 1) : 1;
    for (int k = r->n; k >= oldest && k >= 1; k--) {
        const tlog_tev_t *e = &r->ev[(k - 1) % TLOG_RUN_EVENTS].ev;
        int64_t d = t_us - r->ev[(k - 1) % TLOG_RUN_EVENTS].t_us;
        if (d < 0) d = -d;
        if (e->gate == gate_id && e->beam && d <= TLOG_RUN_REPLACE_US) return true;
    }
    return false;
}

int tlog_run_crossing(tlog_run_t *r, uint8_t gate_id, const tlog_gate_t *gate, int64_t t_us, bool beam,
                      tlog_crossing_t *out) {
    /* the RSSI crossing of a pass the beam already timed (it is confirmed later than the BREAK) */
    if (!beam && r->state != TLOG_RUN_IDLE && beam_near(r, gate_id, t_us)) return TLOG_RUN_NONE;
    if (beam && r->state != TLOG_RUN_IDLE) {
        uint8_t k = replaceable(r, gate_id, t_us);
        if (k) {
            tlog_tev_t *e = &r->ev[(k - 1) % TLOG_RUN_EVENTS].ev;
            r->ev[(k - 1) % TLOG_RUN_EVENTS].t_us = t_us;
            e->beam = true;
            if (k == 1 && e->kind == TLOG_EV_START) {
                /* the start moves; later events keep their absolute times */
                r->t0_us = t_us;
                for (int j = 1; j <= r->n; j++) {
                    if (r->n - j >= TLOG_RUN_EVENTS) continue;
                    r->ev[(j - 1) % TLOG_RUN_EVENTS].ev.t_ms = since_t0_ms(r, r->ev[(j - 1) % TLOG_RUN_EVENTS].t_us);
                }
            } else {
                e->t_ms = since_t0_ms(r, t_us);
            }
            if (t_us > r->last_us) r->last_us = t_us;
            if (out) fill(r, k, out);
            return TLOG_RUN_REPLACED;
        }
    }

    bool running = r->state == TLOG_RUN_RUNNING;
    if (running && t_us < r->t0_us) return TLOG_RUN_NONE; /* from before this run */
    switch (gate->role) {
    case TLOG_ROLE_START:
        return start(r, gate_id, t_us, beam, out);
    case TLOG_ROLE_STARTFINISH:
        if (!running) return start(r, gate_id, t_us, beam, out);
        r->laps++;
        return add(r, gate_id, TLOG_EV_LAP, r->laps, t_us, beam, out);
    case TLOG_ROLE_SPLIT:
        if (!running) return TLOG_RUN_NONE;
        return add(r, gate_id, TLOG_EV_SPLIT, gate->index, t_us, beam, out);
    case TLOG_ROLE_FINISH:
        if (!running) return TLOG_RUN_NONE;
        r->state = TLOG_RUN_FINISHED;
        return add(r, gate_id, TLOG_EV_FINISH, 0, t_us, beam, out);
    default:
        return TLOG_RUN_NONE;
    }
}

bool tlog_run_tick(tlog_run_t *r, int64_t now_us) {
    if (r->state != TLOG_RUN_RUNNING || now_us - r->last_us < TLOG_RUN_ABANDON_US) return false;
    uint8_t counter = r->counter;
    tlog_run_init(r, counter);
    return true;
}

bool tlog_run_event(const tlog_run_t *r, uint8_t k, tlog_tev_t *ev) {
    if (k < 1 || k > r->n || r->n - k >= TLOG_RUN_EVENTS) return false;
    *ev = r->ev[(k - 1) % TLOG_RUN_EVENTS].ev;
    return true;
}

uint32_t tlog_run_clock_ms(const tlog_run_t *r, int64_t now_us) {
    if (r->state == TLOG_RUN_IDLE) return 0;
    return since_t0_ms(r, now_us);
}

bool tlog_run_rssi_allowed(const tlog_gate_t *gate) {
    bool barrier_ok = (gate->flags & TLOG_GATE_F_BARRIER) && !(gate->flags & TLOG_GATE_F_FAULT);
    return !(gate->role == TLOG_ROLE_START && barrier_ok);
}
