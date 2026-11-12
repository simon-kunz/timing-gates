#include "console.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "beacons.h"
#include "settings.h"
#include "tagrun.h"
#include "tlog/result.h"
#include "tlog/timing.h"

#define CON_LINE 256

static const char *HELP =
    "commands:\n"
    "  role gate|tag|uplink\n"
    "  gate <id> start|split <n>|finish|startfinish\n"
    "  barrier on|off        stream on|off\n"
    "  thresh <dBm>          minint <s>\n"
    "  wifi <ssid> <pass>    server <url>    reader <id>    token <t>|none\n"
    "  hear [on|off]         trace [on|off]        rate 1|6\n"
    "  xings                 xdump <n>             mute <s>\n"
    "  break [<tag id>|none] (test BREAK from a gate)\n"
    "  show                  help\n";

void console_show(void) {
    settings_t s;
    settings_load(&s);
    uint8_t mac[6];
    settings_mac(mac);
    char id[9], gr[16];
    tlog_id_hex(settings_tag_id(), id);
    tlog_role_format(settings_gate_role(&s), s.gate_set ? s.gate_index : 0, gr, sizeof(gr));
    printf("role     %s\n", settings_role_name(s.role));
    printf("tag id   %s\n", id);
    printf("mac      %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("gate     %u %s%s\n", settings_gate_id(&s), gr, s.gate_set ? "" : " (unconfigured)");
    printf("barrier  %s\n", s.barrier ? "on" : "off");
    printf("stream   %s\n", s.stream ? "on" : "off");
    printf("thresh   %d dBm\n", s.thresh_dbm);
    printf("minint   %u s\n", s.minint_s);
    printf("wifi     %s%s\n", s.ssid[0] ? s.ssid : "none", s.pass[0] ? " (password set)" : "");
    printf("server   %s\n", s.server[0] ? s.server : "none");
    printf("reader   %s\n", s.reader[0] ? s.reader : "none");
    printf("token    %s\n", s.token[0] ? "set" : "none");
    printf("rate     %d Mbps%s\n", beacons_rate(), beacons_rate() == 6 ? "" : " (range test: set back with rate 6)");
    if (s.role == UNIT_GATE && !s.gate_set) printf("this gate is unconfigured: set it with gate <id> <role>\n");
}

static bool on_off(const char *w, bool *out) {
    if (!strcmp(w, "on")) *out = true;
    else if (!strcmp(w, "off")) *out = false;
    else return false;
    return true;
}

static bool copy_word(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (!n || n >= cap) return false;
    memcpy(dst, src, n + 1);
    return true;
}

static void restart(void) {
    printf("restarting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void handle(char *line) {
    /* split into the command and the rest */
    char *cmd = line, *rest = line;
    while (*rest && !isspace((unsigned char)*rest)) rest++;
    if (*rest) *rest++ = 0;
    while (isspace((unsigned char)*rest)) rest++;
    size_t rl = strlen(rest);
    while (rl && isspace((unsigned char)rest[rl - 1])) rest[--rl] = 0;

    settings_t s;
    settings_load(&s);
    bool save = true;
    char w1[130], w2[130];

    if (!strcmp(cmd, "show")) {
        console_show();
        return;
    } else if (!strcmp(cmd, "hear")) {
        bool v = !beacons_hearing();
        if (*rest && !on_off(rest, &v)) {
            printf("hear: on or off\n");
            return;
        }
        beacons_hear(v);
        printf("hear %s\n", v ? "on" : "off");
        return;
    } else if (!strcmp(cmd, "trace")) {
        bool v = !beacons_tracing();
        if (*rest && !on_off(rest, &v)) {
            printf("trace: on or off\n");
            return;
        }
        if (v) printf("trace on\n");
        beacons_trace(v);
        return;
    } else if (!strcmp(cmd, "rate")) {
        if (!*rest) {
            printf("rate %d Mbps\n", beacons_rate());
            return;
        }
        int r = atoi(rest);
        if (!beacons_set_rate(r)) printf("rate: 1 or 6 (Mbps, stored)\n");
        else printf("rate %d Mbps, stored\n", r);
        return;
    } else if (!strcmp(cmd, "break")) {
        uint32_t nearest = 0;
        if (*rest && strcmp(rest, "none")) {
            char *end;
            nearest = (uint32_t)strtoul(rest, &end, 16);
            if (*end || !nearest) {
                printf("break: <tag id, 8 hex digits> or none\n");
                return;
            }
        }
        if (!beacons_test_break(nearest)) printf("break: gate role only\n");
        return;
    } else if (!strcmp(cmd, "mute")) {
        int v = atoi(rest);
        if (v < 1 || v > 600) {
            printf("mute: 1 to 600 s\n");
            return;
        }
        beacons_mute(v);
        printf("mute %d s\n", v);
        return;
    } else if (!strcmp(cmd, "xings")) {
        tagrun_print_log();
        return;
    } else if (!strcmp(cmd, "xdump")) {
        tagrun_dump(atoi(rest));
        return;
    } else if (!strcmp(cmd, "help")) {
        fputs(HELP, stdout);
        return;
    } else if (!strcmp(cmd, "role")) {
        int r = settings_role_parse(rest);
        if (r < 0) {
            printf("role: gate, tag or uplink\n");
            return;
        }
        if (r == s.role) {
            printf("role is already %s\n", rest);
            return;
        }
        s.role = (uint8_t)r;
        if (!settings_save(&s)) return;
        printf("role %s saved\n", rest);
        restart();
        return;
    } else if (!strcmp(cmd, "gate")) {
        char *end;
        long id = strtol(rest, &end, 10);
        while (isspace((unsigned char)*end)) end++;
        uint8_t role, index;
        if (end == rest || id < 1 || id > 255 || tlog_role_parse(end, strlen(end), &role, &index) != TLOG_OK) {
            printf("gate: <id 1-255> start|split <1-10>|finish|startfinish\n");
            return;
        }
        s.gate_set = true;
        s.gate_id = (uint8_t)id;
        s.gate_role = role;
        s.gate_index = index;
    } else if (!strcmp(cmd, "barrier") || !strcmp(cmd, "stream")) {
        bool v;
        if (!on_off(rest, &v)) {
            printf("%s: on or off\n", cmd);
            return;
        }
        if (cmd[0] == 'b') s.barrier = v;
        else s.stream = v;
    } else if (!strcmp(cmd, "thresh")) {
        char *end;
        long v = strtol(rest, &end, 10);
        if (end == rest || *end || v < -100 || v > -30) {
            printf("thresh: -100 to -30 dBm\n");
            return;
        }
        s.thresh_dbm = (int8_t)v;
    } else if (!strcmp(cmd, "minint")) {
        char *end;
        long v = strtol(rest, &end, 10);
        if (end == rest || *end || v < 1 || v > 600) {
            printf("minint: 1 to 600 s\n");
            return;
        }
        s.minint_s = (uint16_t)v;
    } else if (!strcmp(cmd, "wifi")) {
        if (sscanf(rest, "%32s %63s", w1, w2) != 2 || strlen(w2) < 8) {
            printf("wifi: <ssid> <password, 8-63 characters> (no spaces)\n");
            return;
        }
        strcpy(s.ssid, w1);
        strcpy(s.pass, w2);
    } else if (!strcmp(cmd, "server")) {
        if (strncmp(rest, "http://", 7) && strncmp(rest, "https://", 8)) {
            printf("server: http(s)://... (up to %u characters)\n", (unsigned)sizeof(s.server) - 1);
            return;
        }
        if (!copy_word(s.server, sizeof(s.server), rest)) {
            printf("server: too long\n");
            return;
        }
    } else if (!strcmp(cmd, "reader")) {
        if (!copy_word(s.reader, sizeof(s.reader), rest)) {
            printf("reader: 1-%u characters\n", (unsigned)sizeof(s.reader) - 1);
            return;
        }
    } else if (!strcmp(cmd, "token")) {
        if (!strcmp(rest, "none")) s.token[0] = 0;
        else if (!copy_word(s.token, sizeof(s.token), rest)) {
            printf("token: 1-%u characters, or none\n", (unsigned)sizeof(s.token) - 1);
            return;
        }
    } else {
        printf("unknown command: %s (help lists them)\n", cmd);
        save = false;
    }
    if (save) {
        if (!settings_save(&s)) {
            printf("could not save\n");
            return;
        }
        printf("saved\n");
        /* the beacons carry these, and they are read at start */
        if (s.role == UNIT_GATE && (!strcmp(cmd, "gate") || !strcmp(cmd, "barrier") || !strcmp(cmd, "stream"))) restart();
        if (s.role == UNIT_TAG && (!strcmp(cmd, "thresh") || !strcmp(cmd, "minint"))) restart();
    }
}

static void console_task(void *arg) {
    (void)arg;
    static char line[CON_LINE];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t buf[32];
        int got = usb_serial_jtag_ll_rxfifo_data_available() ? usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf)) : 0;
        if (!got) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (int i = 0; i < got; i++) {
            char ch = (char)buf[i];
            if (ch != '\n' && ch != '\r') {
                if (n < sizeof(line) - 1) line[n++] = ch;
                else overflow = true;
                continue;
            }
            if (!n) continue;
            line[n] = 0;
            n = 0;
            if (overflow) printf("line too long\n");
            else handle(line);
            overflow = false;
        }
    }
}

void console_start(void) { xTaskCreate(console_task, "console", 4096, NULL, 2, NULL); }
