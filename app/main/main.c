/*
 * Timing unit (design D1): one app, three roles chosen in NVS: gate, tag or uplink.
 * Done: settings and console (task 1.3), ID beacons and `hear` (2.1), crossings and run timing on
 * the tag (3.x), BREAK handling on the tag (4.3), BLE adverts (5.x, in progress). Still to do: the
 * barrier ISR on the gate (4.2), the read stream (7.4), the uplink role (7.6) and the setup AP (1.5).
 */
#include <stdio.h>

#include "beacons.h"
#include "console.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "settings.h"
#include "tagrun.h"
#include "tlog/timing.h"

static const char *TAG = "timing";

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    settings_t s;
    settings_load(&s);
    char id[9];
    tlog_id_hex(settings_tag_id(), id);
    printf("\ntiming %s, role %s, tag id %s\n", esp_app_get_description()->version, settings_role_name(s.role), id);
    console_show();
    console_start();

    if (s.role == UNIT_GATE && !s.gate_set) ESP_LOGW(TAG, "unconfigured gate: start-finish, id %u", settings_gate_id(&s));
    if (s.role == UNIT_UPLINK) ESP_LOGI(TAG, "uplink role: follows in task 7.6");
    else {
        if (s.role == UNIT_TAG) tagrun_start(&s); /* before the radio, so no frame finds it missing */
        beacons_start(&s);
    }
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
