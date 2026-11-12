#include "tagrun.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "adv.h"
#include "tlog/brk.h"
#include "tlog/crossing.h"
#include "tlog/result.h"
#include "tlog/link.h"
#include "tlog/pages.h"
#include "tlog/run.h"

static const char *TAG = "tagrun";

#define BIN_US 50000
#define MAX_GATES 8
#define LOG_N 32
#define SNAPS 4
#define SNAP_MAX 1600             /* samples: 16 s at 100 Hz */
#define RING_N 3072               /* recent samples of every gate, for the snapshots */
#define SNAP_BEFORE_US 8000000LL  /* snapshot from 8 s before the crossing */
#define SNAP_AFTER_US 1000000LL   /* to 1 s after its confirmation */

typedef struct {
    uint8_t id;
    bool is_break;
    tlog_gate_t g;     /* gate frame */
    tlog_break_t brk;  /* BREAK copy */
    int rssi;
    int64_t us;
} sample_t;

typedef struct {
    bool used;
    uint8_t id;
    tlog_gate_t g; /* from its latest beacon */
    tlog_xdet_t det;
} gate_t;

typedef struct {
    uint32_t n;      /* crossing number since boot, 1.. */
    uint8_t gate;
    uint8_t role, index;
    tlog_xing_t x;
    int result;      /* TLOG_RUN_* */
    bool beam;
    tlog_crossing_t ev;
    uint8_t run;     /* run counter */
} logent_t;

typedef struct {
    uint32_t us_lo;
    uint8_t gate;
    int8_t rssi;
} raw_t;

typedef struct {
    uint32_t n; /* crossing number, 0 = empty */
    uint8_t gate;
    int64_t base_us; /* crossing time; sample times are relative to it */
    uint16_t count;
    raw_t s[SNAP_MAX];
} snap_t;

static QueueHandle_t s_q;
static gate_t s_gates[MAX_GATES];
static tlog_xdet_cfg_t s_cfg;
static tlog_run_t s_run;
static tlog_brk_t s_brk;
static tlog_pages_t s_pages;
static int64_t s_next_adv_us;
static logent_t s_log[LOG_N];
static uint32_t s_nx;
static raw_t s_ring[RING_N];
static uint32_t s_ring_n; /* samples written, ever */
static snap_t s_snap[SNAPS];
static struct {
    uint32_t n;
    uint8_t gate;
    int64_t base_us, until_us;
} s_pending; /* a snapshot waiting for its "after" part */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t s_tag_id;
static uint32_t s_qdrop;

static uint8_t load_counter(void) {
    nvs_handle_t h;
    uint8_t c = 0;
    if (nvs_open("timing", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "runctr", &c);
        nvs_close(h);
    }
    return c;
}

static void save_counter(uint8_t c) {
    nvs_handle_t h;
    if (nvs_open("timing", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "runctr", c) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void tagrun_gate_frame(uint8_t id, const tlog_gate_t *g, int rssi, int64_t us) {
    if (!s_q) return;
    sample_t sm = {.id = id, .g = *g, .rssi = rssi, .us = us};
    if (xQueueSend(s_q, &sm, 0) != pdTRUE) s_qdrop++;
}

void tagrun_break(uint8_t id, const tlog_break_t *b, int64_t us) {
    if (!s_q) return;
    sample_t sm = {.id = id, .is_break = true, .brk = *b, .us = us};
    if (xQueueSend(s_q, &sm, 0) != pdTRUE) s_qdrop++;
}

static gate_t *gate_for(uint8_t id) {
    for (int i = 0; i < MAX_GATES; i++)
        if (s_gates[i].used && s_gates[i].id == id) return &s_gates[i];
    for (int i = 0; i < MAX_GATES; i++) {
        if (s_gates[i].used) continue;
        s_gates[i].used = true;
        s_gates[i].id = id;
        tlog_xdet_init(&s_gates[i].det, &s_cfg);
        return &s_gates[i];
    }
    return NULL;
}

static const char *kind_name(uint8_t k) {
    switch (k) {
    case TLOG_EV_START: return "start";
    case TLOG_EV_SPLIT: return "split";
    case TLOG_EV_LAP: return "lap";
    case TLOG_EV_FINISH: return "finish";
    default: return "-";
    }
}

static void print_entry(const logent_t *e) {
    char r[16];
    tlog_role_format(e->role, e->index, r, sizeof(r));
    if (e->beam)
        printf("xing %" PRIu32 " gate %u %s at %.4f s, beam (BREAK heard +%.3f s)", e->n, e->gate, r,
               (double)e->x.t_us / 1e6, (double)(e->x.confirm_us - e->x.t_us) / 1e6);
    else
        printf("xing %" PRIu32 " gate %u %s at %.3f s, peak %.1f dBm, confirmed +%.2f s", e->n, e->gate, r,
               (double)e->x.t_us / 1e6, (double)e->x.peak_dbm, (double)(e->x.confirm_us - e->x.t_us) / 1e6);
    if (e->result == TLOG_RUN_NONE) {
        printf(", no run event\n");
        return;
    }
    printf(" -> run %u event %u %s", e->run, e->ev.event_seq, kind_name(e->ev.ev.kind));
    if (e->ev.ev.index) printf(" %u", e->ev.ev.index);
    printf(" %.3f s%s%s\n", (double)e->ev.ev.t_ms / 1e3, e->ev.ev.beam ? " beam" : "",
           e->result == TLOG_RUN_REPLACED ? " (replaced)" : "");
}

static void take_snapshot(void);

static void on_crossing(gate_t *gt, const tlog_xing_t *x, bool beam) {
    if (s_pending.n) take_snapshot(); /* the previous one, cut short */
    logent_t e = {0};
    e.n = ++s_nx;
    e.gate = gt->id;
    e.role = gt->g.role;
    e.index = gt->g.index;
    e.x = *x;
    e.beam = beam;
    uint8_t before = s_run.counter;
    e.result = tlog_run_crossing(&s_run, gt->id, &gt->g, x->t_us, beam, &e.ev);
    e.run = s_run.counter;
    if (s_run.counter != before) save_counter(s_run.counter);
    if (e.result != TLOG_RUN_NONE) {
        int64_t now = esp_timer_get_time();
        tlog_pages_event(&s_pages, now);
        s_next_adv_us = now; /* the new event goes out with the next advert, now */
    }
    s_log[(e.n - 1) % LOG_N] = e;
    print_entry(&e);
    s_pending.n = e.n;
    s_pending.gate = gt->id;
    s_pending.base_us = x->t_us;
    s_pending.until_us = x->confirm_us + SNAP_AFTER_US;
}

/* Copies the recent samples of the pending crossing's gate into a snapshot slot. */
static void take_snapshot(void) {
    snap_t *sn = &s_snap[(s_pending.n - 1) % SNAPS];
    portENTER_CRITICAL(&s_lock);
    sn->n = 0;
    portEXIT_CRITICAL(&s_lock);
    uint16_t c = 0;
    uint32_t first = s_ring_n > RING_N ? s_ring_n - RING_N : 0;
    for (uint32_t i = first; i < s_ring_n && c < SNAP_MAX; i++) {
        const raw_t *r = &s_ring[i % RING_N];
        if (r->gate != s_pending.gate) continue;
        int64_t us = s_pending.base_us + (int32_t)(r->us_lo - (uint32_t)s_pending.base_us);
        if (us < s_pending.base_us - SNAP_BEFORE_US || us > s_pending.until_us) continue;
        sn->s[c++] = *r;
    }
    sn->count = c;
    sn->gate = s_pending.gate;
    sn->base_us = s_pending.base_us;
    portENTER_CRITICAL(&s_lock);
    sn->n = s_pending.n;
    portEXIT_CRITICAL(&s_lock);
    s_pending.n = 0;
}

static void run_task(void *arg) {
    (void)arg;
    int64_t last_poll = 0, last_tick = 0;
    for (;;) {
        sample_t sm;
        if (xQueueReceive(s_q, &sm, pdMS_TO_TICKS(10)) == pdTRUE) {
            gate_t *gt = gate_for(sm.id);
            if (gt && sm.is_break) {
                int64_t t = 0;
                int res = tlog_brk_copy(&s_brk, sm.id, &sm.brk, sm.us, &t);
                if (res == TLOG_BRK_MINE) {
                    tlog_xing_t x = {t, sm.us, 0};
                    on_crossing(gt, &x, true);
                } else if (res != TLOG_BRK_DUPLICATE) {
                    printf("break gate %u seq %u: %s\n", sm.id, sm.brk.break_seq,
                           res == TLOG_BRK_OTHER ? "for another tag" : "names no tag and this tag was not near");
                }
            } else if (gt) {
                s_ring[s_ring_n % RING_N] = (raw_t){(uint32_t)sm.us, sm.id, (int8_t)sm.rssi};
                s_ring_n++;
                gt->g = sm.g;
                tlog_brk_rssi(&s_brk, sm.id, sm.rssi, sm.us);
                tlog_xing_t x;
                if (tlog_run_rssi_allowed(&gt->g) && tlog_xdet_feed(&gt->det, sm.us, sm.rssi, &x)) on_crossing(gt, &x, false);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - last_poll >= 10000) {
            last_poll = now;
            for (int i = 0; i < MAX_GATES; i++) {
                tlog_xing_t x;
                if (s_gates[i].used && tlog_xdet_poll(&s_gates[i].det, now, &x)) on_crossing(&s_gates[i], &x, false);
            }
            if (s_pending.n && now >= s_pending.until_us) take_snapshot();
            if (now >= s_next_adv_us) {
                uint8_t k;
                uint32_t iv;
                tlog_pages_next(&s_pages, &s_run, now, &k, &iv);
                tlog_advert_t a = {.state = s_run.state, .tag_id = s_tag_id, .run_counter = s_run.counter,
                                   .n = s_run.state == TLOG_RUN_IDLE ? 0 : s_run.n,
                                   .run_ms = tlog_run_clock_ms(&s_run, now), .battery = TLOG_BATTERY_UNKNOWN, .k = k};
                if (k) tlog_run_event(&s_run, k, &a.a);
                if (k > 1) tlog_run_event(&s_run, (uint8_t)(k - 1), &a.b);
                uint8_t payload[TLOG_ADVERT_SIZE];
                tlog_advert_encode(&a, payload);
                adv_update(payload, iv);
                s_next_adv_us = now + (int64_t)iv * 1000;
            }
        }
        if (now - last_tick >= 1000000) {
            last_tick = now;
            if (tlog_run_tick(&s_run, now)) printf("run %u abandoned (30 min without a crossing)\n", s_run.counter);
            if (s_qdrop) {
                printf("tagrun: %" PRIu32 " gate frames dropped (queue full)\n", s_qdrop);
                s_qdrop = 0;
            }
        }
    }
}

void tagrun_print_log(void) {
    uint32_t n = s_nx;
    if (!n) {
        printf("no crossings since boot\n");
        return;
    }
    uint32_t from = n > LOG_N ? n - LOG_N + 1 : 1;
    for (uint32_t k = from; k <= n; k++) print_entry(&s_log[(k - 1) % LOG_N]);
    char r[8] = "idle";
    if (s_run.state == TLOG_RUN_RUNNING) strcpy(r, "running");
    if (s_run.state == TLOG_RUN_FINISHED) strcpy(r, "done");
    printf("run %u %s, %u events; snapshots for xdump:", s_run.counter, r, s_run.n);
    for (int i = 0; i < SNAPS; i++)
        if (s_snap[i].n) printf(" %" PRIu32, s_snap[i].n);
    printf("\n");
}

void tagrun_dump(int n) {
    snap_t *sn = NULL;
    for (int i = 0; i < SNAPS; i++)
        if (s_snap[i].n && (int)s_snap[i].n == n) sn = &s_snap[i];
    if (!sn) {
        printf("xdump: crossing %d has no snapshot (xings lists them)\n", n);
        return;
    }
    char line[TLOG_LINE_MAX];
    for (uint16_t i = 0; i < sn->count; i++) {
        const raw_t *r = &sn->s[i];
        tlog_tgr_t t = {r->gate, r->rssi, sn->base_us + (int32_t)(r->us_lo - (uint32_t)sn->base_us)};
        size_t len = tlog_tgr_format(&t, line, sizeof(line));
        printf("%.*s\r\n", (int)len, line);
        if (i % 64 == 63) vTaskDelay(1);
    }
    printf("xdump end: crossing %d, %u samples\n", n, sn->count);
}

void tagrun_start(const settings_t *s) {
    s_cfg = tlog_xdet_defaults(BIN_US);
    s_cfg.thresh_dbm = s->thresh_dbm;
    s_cfg.minint_us = (int64_t)s->minint_s * 1000000;
    tlog_run_init(&s_run, load_counter());
    s_tag_id = settings_tag_id();
    tlog_brk_init(&s_brk, s_tag_id, s->thresh_dbm);
    tlog_pages_init(&s_pages);
    adv_start(s_tag_id);
    s_q = xQueueCreate(256, sizeof(sample_t));
    xTaskCreate(run_task, "tagrun", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "crossings: threshold %d dBm, minimum interval %u s, last run %u", s->thresh_dbm, s->minint_s,
             s_run.counter);
}
