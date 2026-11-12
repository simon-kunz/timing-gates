#include "beacons.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "freertos/task.h"
#include "tlog/result.h"
#include "tlog/link.h"
#include "radio.h"
#include "tagrun.h"
#include "tlog/timing.h"

static const char *TAG = "beacons";
static const uint8_t BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

#define GATE_PERIOD_US 10000 /* 100 Hz */
#define TAG_PERIOD_US 50000  /* 20 Hz */
#define MAX_HEARD 16

static uint8_t s_frame[TLOG_LINK_HEADER_SIZE + TLOG_GATE_BODY];
static tlog_link_hdr_t s_hdr;
static tlog_gate_t s_gate;
static tlog_tag_t s_tag;
static uint32_t s_send_fail;

/* What was heard in the current second, per sender. */
typedef struct {
    bool used;
    uint8_t kind;  /* TLOG_LINK_GATE / TLOG_LINK_TAG */
    uint32_t unit; /* gate id or tag id */
    uint8_t role, index, flags;
    uint32_t n;
    int32_t rssi_sum;
    bool have_seq;
    uint32_t last_seq;
    uint32_t gaps;     /* missing sequence numbers */
    uint32_t backward; /* repeated or older sequence numbers */
    uint8_t rate;      /* rx_ctrl rate of the last frame */
} heard_t;

static heard_t s_heard[MAX_HEARD];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_hear;

/* RSSI trace (task 3.1): every GATE frame as a $TGR line with the gate id, stamped on arrival. */
typedef struct {
    uint32_t gate;
    int8_t rssi;
    int64_t us;
} trace_t;
static QueueHandle_t s_trace_q;
static volatile bool s_trace;
static uint32_t s_trace_n, s_trace_drop;

static volatile int64_t s_mute_until;

static void send_beacon(void *arg) {
    (void)arg;
    if (esp_timer_get_time() < s_mute_until) return;
    s_hdr.packet_seq++;
    tlog_link_hdr_encode(&s_hdr, s_frame);
    if (s_hdr.kind == TLOG_LINK_GATE) tlog_gate_encode(&s_gate, s_frame + TLOG_LINK_HEADER_SIZE);
    else tlog_tag_encode(&s_tag, s_frame + TLOG_LINK_HEADER_SIZE);
    if (esp_now_send(BCAST, s_frame, sizeof(s_frame)) != ESP_OK) s_send_fail++;
}

static void on_send(const uint8_t *mac, esp_now_send_status_t status) {
    (void)mac;
    (void)status;
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    int64_t rx_us = esp_timer_get_time(); /* design D4: stamped first */
    tlog_link_hdr_t h;
    if (tlog_link_hdr_decode(data, (size_t)len, &h) != TLOG_OK) return;
    if (h.kind == TLOG_LINK_BREAK) {
        tlog_break_t b;
        if (s_hdr.kind == TLOG_LINK_TAG &&
            tlog_break_decode(data + TLOG_LINK_HEADER_SIZE, (size_t)len - TLOG_LINK_HEADER_SIZE, &b) == TLOG_OK)
            tagrun_break((uint8_t)h.unit, &b, rx_us);
        return;
    }
    if (h.kind != TLOG_LINK_GATE && h.kind != TLOG_LINK_TAG) return;
    const uint8_t *body = data + TLOG_LINK_HEADER_SIZE;
    size_t blen = (size_t)len - TLOG_LINK_HEADER_SIZE;
    tlog_gate_t g = {0};
    if (h.kind == TLOG_LINK_GATE && tlog_gate_decode(body, blen, &g) != TLOG_OK) return;
    if (h.kind == TLOG_LINK_TAG) {
        tlog_tag_t t;
        if (tlog_tag_decode(body, blen, &t) != TLOG_OK) return;
    }
    int rssi = info->rx_ctrl->rssi;
    uint8_t rate = info->rx_ctrl->rate;
    if (h.kind == TLOG_LINK_GATE && s_hdr.kind == TLOG_LINK_TAG) tagrun_gate_frame((uint8_t)h.unit, &g, rssi, rx_us);
    if (s_trace && h.kind == TLOG_LINK_GATE) {
        trace_t t = {h.unit, (int8_t)rssi, rx_us};
        if (xQueueSend(s_trace_q, &t, 0) == pdTRUE) s_trace_n++;
        else s_trace_drop++;
    }
    portENTER_CRITICAL(&s_lock);
    heard_t *e = NULL;
    for (int i = 0; i < MAX_HEARD && !e; i++)
        if (s_heard[i].used && s_heard[i].kind == h.kind && s_heard[i].unit == h.unit) e = &s_heard[i];
    for (int i = 0; i < MAX_HEARD && !e; i++) {
        if (s_heard[i].used) continue;
        e = &s_heard[i];
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->kind = h.kind;
        e->unit = h.unit;
    }
    if (e) {
        if (e->have_seq) {
            if (h.packet_seq > e->last_seq) e->gaps += h.packet_seq - e->last_seq - 1;
            else e->backward++;
        }
        e->have_seq = true;
        e->last_seq = h.packet_seq;
        e->role = g.role;
        e->index = g.index;
        e->flags = g.flags;
        e->n++;
        e->rssi_sum += rssi;
        e->rate = rate;
    }
    portEXIT_CRITICAL(&s_lock);
}

static const char *rate_name(uint8_t r) {
    switch (r) {
    case 0x00: return "1M";
    case 0x01: return "2M";
    case 0x02: return "5.5M";
    case 0x03: return "11M";
    case 0x0B: return "6M";
    case 0x0F: return "9M";
    case 0x0A: return "12M";
    case 0x0E: return "18M";
    case 0x09: return "24M";
    case 0x0D: return "36M";
    case 0x08: return "48M";
    case 0x0C: return "54M";
    default: return "?";
    }
}

/* Every second: print (when hearing) and clear the counts. The last sequence number is kept, so a
 * gap across the second boundary is still counted. */
static void hear_task(void *arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    heard_t snap[MAX_HEARD];
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        portENTER_CRITICAL(&s_lock);
        memcpy(snap, s_heard, sizeof(snap));
        for (int i = 0; i < MAX_HEARD; i++) {
            s_heard[i].n = 0;
            s_heard[i].rssi_sum = 0;
            s_heard[i].gaps = 0;
            s_heard[i].backward = 0;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!s_hear) continue;
        int64_t t = esp_timer_get_time() / 1000;
        bool any = false;
        for (int i = 0; i < MAX_HEARD; i++) {
            const heard_t *e = &snap[i];
            if (!e->n) continue;
            any = true;
            char who[24];
            if (e->kind == TLOG_LINK_GATE) {
                char r[16];
                tlog_role_format(e->role, e->index, r, sizeof(r));
                snprintf(who, sizeof(who), "gate %" PRIu32 " %s", e->unit, r);
            } else {
                char id[9];
                tlog_id_hex(e->unit, id);
                snprintf(who, sizeof(who), "tag %s", id);
            }
            printf("hear %" PRId64 " %-20s %3" PRIu32 "/s rssi %4" PRId32 " gaps %" PRIu32 " back %" PRIu32 " seq %" PRIu32
                   " rate %s%s\n",
                   t, who, e->n, e->rssi_sum / (int32_t)e->n, e->gaps, e->backward, e->last_seq, rate_name(e->rate),
                   (e->kind == TLOG_LINK_GATE && (e->flags & TLOG_GATE_F_UNCONFIGURED)) ? " unconfigured" : "");
        }
        if (!any) printf("hear %" PRId64 " nothing\n", t);
        if (s_send_fail) {
            printf("hear %" PRId64 " %" PRIu32 " sends failed\n", t, s_send_fail);
            s_send_fail = 0;
        }
    }
}

static void trace_task(void *arg) {
    (void)arg;
    trace_t t;
    char line[TLOG_LINE_MAX];
    for (;;) {
        if (xQueueReceive(s_trace_q, &t, portMAX_DELAY) != pdTRUE) continue;
        tlog_tgr_t r = {t.gate, t.rssi, t.us};
        size_t n = tlog_tgr_format(&r, line, sizeof(line) - 2);
        line[n++] = '\r';
        line[n++] = '\n';
        fwrite(line, 1, n, stdout);
    }
}

/* Test BREAK (until the barrier ISR of task 4.2): 5 copies 10 ms apart, each with its own age. */
static uint16_t s_break_seq;
bool beacons_test_break(uint32_t nearest) {
    if (s_hdr.kind != TLOG_LINK_GATE) return false;
    int64_t t_break = esp_timer_get_time();
    s_break_seq++;
    static uint32_t pkt;
    for (uint8_t k = 0; k < 5; k++) {
        uint8_t f[TLOG_LINK_HEADER_SIZE + TLOG_BREAK_BODY];
        tlog_link_hdr_t h = {TLOG_LINK_BREAK, 0, s_hdr.unit, ++pkt};
        tlog_break_t b = {s_break_seq, k, 0, 0, nearest};
        b.age_us = (uint32_t)(esp_timer_get_time() - t_break);
        tlog_link_hdr_encode(&h, f);
        tlog_break_encode(&b, f + TLOG_LINK_HEADER_SIZE);
        esp_now_send(BCAST, f, sizeof(f));
        if (k < 4) vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("break seq %u sent 5 times, names %s\n", s_break_seq, nearest ? "a tag" : "no tag");
    return true;
}

void beacons_mute(int seconds) { s_mute_until = esp_timer_get_time() + (int64_t)seconds * 1000000; }

void beacons_trace(bool on) {
    if (on) {
        s_trace_n = s_trace_drop = 0;
        s_trace = true;
        return;
    }
    s_trace = false;
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the queue drain */
    fflush(stdout);
    printf("trace off: %" PRIu32 " frames, %" PRIu32 " dropped\n", s_trace_n, s_trace_drop);
}
bool beacons_tracing(void) { return s_trace; }

static int stored_rate(void) {
    nvs_handle_t h;
    uint8_t r = 6;
    if (nvs_open("timing", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "rate", &r);
        nvs_close(h);
    }
    return r == 1 ? 1 : 6;
}

static void store_rate(int mbps) {
    nvs_handle_t h;
    if (nvs_open("timing", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "rate", (uint8_t)mbps) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static bool apply_rate(int mbps) {
    esp_now_rate_config_t rc = {.phymode = WIFI_PHY_MODE_11G, .rate = WIFI_PHY_RATE_6M, .ersu = false, .dcm = false};
    if (mbps == 1) {
        rc.phymode = WIFI_PHY_MODE_11B;
        rc.rate = WIFI_PHY_RATE_1M_L;
    } else if (mbps != 6) {
        return false;
    }
    esp_err_t err = esp_now_set_peer_rate_config(BCAST, &rc);
    if (err != ESP_OK) ESP_LOGE(TAG, "rate config failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static int s_rate = 6;
static bool s_started;
bool beacons_set_rate(int mbps) {
    if ((mbps != 1 && mbps != 6) || !apply_rate(mbps)) return false;
    store_rate(mbps);
    s_rate = mbps;
    return true;
}
int beacons_rate(void) { return s_started ? s_rate : stored_rate(); }

void beacons_hear(bool on) { s_hear = on; }
bool beacons_hearing(void) { return s_hear; }

void beacons_start(const settings_t *s) {
    uint32_t ver = 0;
    ESP_ERROR_CHECK(radio_start(on_recv, on_send, &ver));
    s_rate = stored_rate();
    apply_rate(s_rate);
    s_started = true;
    xTaskCreate(hear_task, "hear", 3072, NULL, 3, NULL);
    s_trace_q = xQueueCreate(512, sizeof(trace_t));
    xTaskCreate(trace_task, "trace", 3072, NULL, 4, NULL);

    int64_t period = 0;
    if (s->role == UNIT_GATE) {
        s_hdr.kind = TLOG_LINK_GATE;
        s_hdr.unit = settings_gate_id(s);
        s_gate.role = settings_gate_role(s);
        s_gate.index = s->gate_set ? s->gate_index : 0;
        s_gate.flags = (s->barrier ? TLOG_GATE_F_BARRIER : 0) | (s->gate_set ? 0 : TLOG_GATE_F_UNCONFIGURED);
        s_gate.battery = TLOG_BATTERY_UNKNOWN;
        period = GATE_PERIOD_US;
    } else if (s->role == UNIT_TAG) {
        s_hdr.kind = TLOG_LINK_TAG;
        s_hdr.unit = settings_tag_id();
        s_tag.state = TLOG_RUN_IDLE;
        s_tag.battery = TLOG_BATTERY_UNKNOWN;
        period = TAG_PERIOD_US;
    } else {
        return;
    }
    s_hdr.flags = 0;
    s_hdr.packet_seq = 0;
    const esp_timer_create_args_t ta = {.callback = send_beacon, .name = "beacon", .dispatch_method = ESP_TIMER_TASK};
    esp_timer_handle_t tm;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &tm));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tm, period));
    ESP_LOGI(TAG, "%s beacons at %d Hz, %d Mbps, channel %d", s->role == UNIT_GATE ? "GATE" : "TAG",
             (int)(1000000 / period), s_rate, RADIO_CHANNEL);
}
