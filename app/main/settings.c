#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "tlog/timing.h"

static const char *TAG = "settings";

#define NS "timing"
#define KEY "cfg"
#define VERSION 1

/* Stored as one blob: a version byte, then the struct. A blob of another size or version is
 * ignored, so a firmware with a changed struct starts from the defaults. */
typedef struct {
    uint8_t version;
    settings_t s;
} blob_t;

static void defaults(settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->role = UNIT_GATE;
    s->thresh_dbm = -80;
    s->minint_s = 10;
}

void settings_load(settings_t *s) {
    defaults(s);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    blob_t b;
    size_t n = sizeof(b);
    esp_err_t err = nvs_get_blob(h, KEY, &b, &n);
    nvs_close(h);
    if (err != ESP_OK) return;
    if (n != sizeof(b) || b.version != VERSION) {
        ESP_LOGW(TAG, "stored settings have another layout, using defaults");
        return;
    }
    *s = b.s;
    /* strings are always terminated */
    s->ssid[sizeof(s->ssid) - 1] = s->pass[sizeof(s->pass) - 1] = 0;
    s->server[sizeof(s->server) - 1] = s->reader[sizeof(s->reader) - 1] = s->token[sizeof(s->token) - 1] = 0;
}

bool settings_save(const settings_t *s) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    blob_t b = {.version = VERSION, .s = *s};
    esp_err_t err = nvs_set_blob(h, KEY, &b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

void settings_mac(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_WIFI_STA); }

uint32_t settings_tag_id(void) {
    uint8_t mac[6];
    settings_mac(mac);
    return tlog_tag_id_from_mac(mac);
}

uint8_t settings_gate_id(const settings_t *s) {
    if (s->gate_set) return s->gate_id;
    uint8_t mac[6];
    settings_mac(mac);
    return mac[5] ? mac[5] : 1; /* 1-255 */
}

uint8_t settings_gate_role(const settings_t *s) { return s->gate_set ? s->gate_role : TLOG_ROLE_STARTFINISH; }

const char *settings_role_name(uint8_t role) {
    switch (role) {
    case UNIT_GATE: return "gate";
    case UNIT_TAG: return "tag";
    case UNIT_UPLINK: return "uplink";
    default: return "?";
    }
}

int settings_role_parse(const char *w) {
    if (!strcmp(w, "gate")) return UNIT_GATE;
    if (!strcmp(w, "tag")) return UNIT_TAG;
    if (!strcmp(w, "uplink")) return UNIT_UPLINK;
    return -1;
}
