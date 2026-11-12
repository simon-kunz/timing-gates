/* Wi-Fi in station mode on a fixed channel, power save off, ESP-NOW on top. A unit that hears the
 * gates must never join a Wi-Fi network: that would move its channel. */
#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>

#include "esp_err.h"
#include "esp_now.h"

#define RADIO_CHANNEL 1

/* Initialises Wi-Fi and ESP-NOW once (NVS must be up) and registers the callbacks; adds the
 * broadcast peer. *espnow_version gets the ESP-NOW version. */
esp_err_t radio_start(esp_now_recv_cb_t recv_cb, esp_now_send_cb_t send_cb, uint32_t *espnow_version);

#endif
