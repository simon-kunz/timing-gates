#include "adv.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "tlog/timing.h"

static const char *TAG = "adv";

static uint8_t s_own_addr;
static volatile bool s_synced;
static char s_name[12]; /* TG-XXXXXXXX */
static uint8_t s_data[31];
static uint32_t s_interval_ms, s_running_ms;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void build(const uint8_t payload[24]) {
    uint8_t *d = s_data;
    *d++ = 2; /* flags */
    *d++ = BLE_HS_ADV_TYPE_FLAGS;
    *d++ = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    *d++ = 27; /* manufacturer data: type, company, payload */
    *d++ = BLE_HS_ADV_TYPE_MFG_DATA;
    *d++ = (uint8_t)(TLOG_ADVERT_COMPANY & 0xFF);
    *d++ = (uint8_t)(TLOG_ADVERT_COMPANY >> 8);
    memcpy(d, payload, TLOG_ADVERT_SIZE);
}

static void restart(uint32_t interval_ms) {
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    uint8_t rsp[2 + sizeof(s_name)];
    size_t nl = strlen(s_name);
    rsp[0] = (uint8_t)(nl + 1);
    rsp[1] = BLE_HS_ADV_TYPE_COMP_NAME;
    memcpy(rsp + 2, s_name, nl);
    int rc = ble_gap_adv_set_data(s_data, sizeof(s_data));
    if (!rc) rc = ble_gap_adv_rsp_set_data(rsp, (int)(nl + 2));
    uint16_t itvl = (uint16_t)(interval_ms * 1000 / 625); /* units of 0.625 ms */
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_NON, .disc_mode = BLE_GAP_DISC_MODE_GEN, .itvl_min = itvl, .itvl_max = itvl};
    if (!rc) rc = ble_gap_adv_start(s_own_addr, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
    if (rc) ESP_LOGE(TAG, "advertising failed: %d", rc);
    else s_running_ms = interval_ms;
}

void adv_update(const uint8_t payload[24], uint32_t interval_ms) {
    portENTER_CRITICAL(&s_lock);
    build(payload);
    s_interval_ms = interval_ms;
    portEXIT_CRITICAL(&s_lock);
    if (!s_synced) return;
    if (!ble_gap_adv_active() || interval_ms != s_running_ms) restart(interval_ms);
    else if (ble_gap_adv_set_data(s_data, sizeof(s_data))) ESP_LOGW(TAG, "advert update failed");
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr);
    s_synced = true;
    restart(s_interval_ms ? s_interval_ms : 1000);
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset: %d", reason); }

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void adv_start(uint32_t tag_id) {
    char id[9];
    tlog_id_hex(tag_id, id);
    snprintf(s_name, sizeof(s_name), "TG-%s", id);
    uint8_t empty[TLOG_ADVERT_SIZE] = {0};
    tlog_advert_t a = {.state = TLOG_RUN_IDLE, .tag_id = tag_id, .battery = TLOG_BATTERY_UNKNOWN};
    tlog_advert_encode(&a, empty);
    build(empty);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE adverts as %s", s_name);
}
