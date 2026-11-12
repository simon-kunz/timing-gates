#include "radio.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "radio";
static const uint8_t BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

esp_err_t radio_start(esp_now_recv_cb_t recv_cb, esp_now_send_cb_t send_cb, uint32_t *espnow_version) {
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE), TAG, "channel");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "ps");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "espnow init");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(recv_cb), TAG, "recv cb");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(send_cb), TAG, "send cb");
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, BCAST, 6);
    p.channel = RADIO_CHANNEL;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&p), TAG, "bcast peer");
    uint32_t v = 1;
    if (esp_now_get_version(&v) != ESP_OK) v = 1;
    if (espnow_version) *espnow_version = v;
    ESP_LOGI(TAG, "ESP-NOW v%lu on channel %d", (unsigned long)v, RADIO_CHANNEL);
    return ESP_OK;
}
