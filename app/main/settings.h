/*
 * Settings of a timing unit (D1, D8), stored in NVS. The one module that reads
 * and writes them, shared by the USB console and, later, the setup page (D8a).
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

enum { UNIT_GATE = 0, UNIT_TAG = 1, UNIT_UPLINK = 2 };

typedef struct {
    uint8_t role;        /* UNIT_*; default gate */
    bool gate_set;       /* gate id and role configured; otherwise "unconfigured" */
    uint8_t gate_id;     /* 1-255 */
    uint8_t gate_role;   /* TLOG_ROLE_* */
    uint8_t gate_index;  /* split index 1-10, else 0 */
    bool barrier;
    bool stream;
    int8_t thresh_dbm;   /* crossing threshold, default -80 */
    uint16_t minint_s;   /* minimum interval, default 10 */
    char ssid[33];
    char pass[64];
    char server[128];
    char reader[33];
    char token[129];
} settings_t;

/* Loads the stored settings, or the defaults where none are stored. */
void settings_load(settings_t *s);
/* Stores all settings. Returns false if NVS refused. */
bool settings_save(const settings_t *s);

/* The unit's Wi-Fi MAC and its tag id (D7a: low 4 bytes, as 8 upper-case hex digits). */
void settings_mac(uint8_t mac[6]);
uint32_t settings_tag_id(void);

/* The gate id and role the beacons carry: the configured ones, or for an unconfigured gate
 * start-finish with an id derived from the MAC (spec "Unconfigured gate"). */
uint8_t settings_gate_id(const settings_t *s);
uint8_t settings_gate_role(const settings_t *s);

const char *settings_role_name(uint8_t role);
/* Parses gate, tag or uplink; returns -1 otherwise. */
int settings_role_parse(const char *word);

#endif
