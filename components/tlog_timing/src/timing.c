#include "tlog/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tlog/result.h"

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put24(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); p[2] = (uint8_t)(v >> 16); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get24(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)p[2] << 16); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | ((uint32_t)get16(p + 2) << 16); }
static uint32_t sat24(uint32_t v) { return v > TLOG_U24_MAX ? TLOG_U24_MAX : v; }

/* ---- frames ---- */

void tlog_gate_encode(const tlog_gate_t *g, uint8_t *out) {
    out[0] = g->role;
    out[1] = g->index;
    out[2] = g->flags;
    out[3] = g->battery;
}

int tlog_gate_decode(const uint8_t *in, size_t len, tlog_gate_t *g) {
    if (len < TLOG_GATE_BODY) return TLOG_ERR_FORMAT;
    if (in[0] < TLOG_ROLE_START || in[0] > TLOG_ROLE_STARTFINISH) return TLOG_ERR_FORMAT;
    g->role = in[0];
    g->index = in[1];
    g->flags = in[2];
    g->battery = in[3];
    return TLOG_OK;
}

void tlog_tag_encode(const tlog_tag_t *t, uint8_t *out) {
    out[0] = t->state;
    out[1] = t->battery;
    out[2] = out[3] = 0;
}

int tlog_tag_decode(const uint8_t *in, size_t len, tlog_tag_t *t) {
    if (len < TLOG_TAG_BODY) return TLOG_ERR_FORMAT;
    t->state = in[0];
    t->battery = in[1];
    return TLOG_OK;
}

void tlog_break_encode(const tlog_break_t *b, uint8_t *out) {
    put16(out, b->break_seq);
    out[2] = b->copy;
    out[3] = b->flags;
    put32(out + 4, b->age_us);
    put32(out + 8, b->nearest);
}

int tlog_break_decode(const uint8_t *in, size_t len, tlog_break_t *b) {
    if (len < TLOG_BREAK_BODY) return TLOG_ERR_FORMAT;
    b->break_seq = get16(in);
    b->copy = in[2];
    b->flags = in[3];
    b->age_us = get32(in + 4);
    b->nearest = get32(in + 8);
    return TLOG_OK;
}

void tlog_tev_encode(const tlog_tev_t *e, uint8_t *out) {
    if (e->kind == TLOG_EV_NONE) {
        memset(out, 0, TLOG_TEV_SIZE);
        return;
    }
    out[0] = e->gate;
    out[1] = (uint8_t)((e->kind << 4) | (e->beam ? 1u : 0u));
    out[2] = e->index;
    put24(out + 3, sat24(e->t_ms));
}

int tlog_tev_decode(const uint8_t *in, tlog_tev_t *e) {
    uint8_t kind = in[1] >> 4;
    if (kind > TLOG_EV_FINISH) return TLOG_ERR_FORMAT;
    e->gate = in[0];
    e->kind = kind;
    e->beam = (in[1] & 1u) != 0;
    e->index = in[2];
    e->t_ms = get24(in + 3);
    return TLOG_OK;
}

/* ---- advert ---- */

void tlog_advert_encode(const tlog_advert_t *a, uint8_t *out) {
    out[0] = (uint8_t)((TLOG_ADVERT_VERSION << 4) | (a->state & 0x0Fu));
    put32(out + 1, a->tag_id);
    out[5] = a->run_counter;
    out[6] = a->n;
    put24(out + 7, sat24(a->run_ms));
    out[10] = a->battery;
    out[11] = a->k;
    tlog_tev_encode(&a->a, out + 12);
    if (a->k <= 1) {
        memset(out + 18, 0, TLOG_TEV_SIZE);
    } else {
        tlog_tev_encode(&a->b, out + 18);
    }
}

int tlog_advert_decode(const uint8_t *in, size_t len, tlog_advert_t *a) {
    if (len < TLOG_ADVERT_SIZE) return TLOG_ERR_FORMAT;
    if ((in[0] >> 4) != TLOG_ADVERT_VERSION) return TLOG_ERR_VERSION;
    a->state = in[0] & 0x0Fu;
    a->tag_id = get32(in + 1);
    a->run_counter = in[5];
    a->n = in[6];
    a->run_ms = get24(in + 7);
    a->battery = in[10];
    a->k = in[11];
    if (tlog_tev_decode(in + 12, &a->a) != TLOG_OK) return TLOG_ERR_FORMAT;
    if (tlog_tev_decode(in + 18, &a->b) != TLOG_OK) return TLOG_ERR_FORMAT;
    return TLOG_OK;
}

/* ---- tag id ---- */

uint32_t tlog_tag_id_from_mac(const uint8_t mac[6]) {
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
}

void tlog_id_hex(uint32_t id, char out[9]) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) out[i] = hex[(id >> (28 - 4 * i)) & 0xFu];
    out[8] = '\0';
}

/* ---- roles ---- */

size_t tlog_role_format(uint8_t role, uint8_t index, char *out, size_t cap) {
    int n;
    switch (role) {
    case TLOG_ROLE_START: n = snprintf(out, cap, "start"); break;
    case TLOG_ROLE_SPLIT: n = snprintf(out, cap, "split%u", (unsigned)index); break;
    case TLOG_ROLE_FINISH: n = snprintf(out, cap, "finish"); break;
    case TLOG_ROLE_STARTFINISH: n = snprintf(out, cap, "startfinish"); break;
    default: n = snprintf(out, cap, "?"); break;
    }
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

static bool word_is(const char *s, size_t len, const char *w) {
    return strlen(w) == len && memcmp(s, w, len) == 0;
}

int tlog_role_parse(const char *s, size_t len, uint8_t *role, uint8_t *index) {
    if (word_is(s, len, "start")) { *role = TLOG_ROLE_START; *index = 0; return TLOG_OK; }
    if (word_is(s, len, "finish")) { *role = TLOG_ROLE_FINISH; *index = 0; return TLOG_OK; }
    if (word_is(s, len, "startfinish")) { *role = TLOG_ROLE_STARTFINISH; *index = 0; return TLOG_OK; }
    if (len > 5 && memcmp(s, "split", 5) == 0) {
        unsigned v = 0;
        size_t i = 5;
        if (s[i] == ' ') i++;
        if (i >= len || len - i > 2) return TLOG_ERR_FORMAT;
        for (; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') return TLOG_ERR_FORMAT;
            v = v * 10 + (unsigned)(s[i] - '0');
        }
        if (v < 1 || v > 10) return TLOG_ERR_FORMAT;
        *role = TLOG_ROLE_SPLIT;
        *index = (uint8_t)v;
        return TLOG_OK;
    }
    return TLOG_ERR_FORMAT;
}

/* ---- read stream lines ---- */

uint8_t tlog_line_xor(const char *p, size_t len) {
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) x ^= (uint8_t)p[i];
    return x;
}

static size_t finish_line(char *out, size_t cap, int n) {
    if (n < 0 || (size_t)n + 4 > cap) return 0; /* room for "*hh" and the NUL */
    uint8_t x = tlog_line_xor(out + 1, (size_t)n - 1);
    int m = snprintf(out + n, cap - (size_t)n, "*%02X", x);
    if (m != 3) return 0;
    return (size_t)n + 3;
}

size_t tlog_tgr_format(const tlog_tgr_t *r, char *out, size_t cap) {
    char id[9];
    tlog_id_hex(r->id, id);
    int n = snprintf(out, cap, "$TGR,%s,%d,%lld", id, (int)r->rssi, (long long)r->us);
    return finish_line(out, cap, n);
}

size_t tlog_tgs_format(const tlog_tgs_t *s, char *out, size_t cap) {
    char role[16];
    if (tlog_role_format(s->role, s->index, role, sizeof(role)) == 0) return 0;
    int n = snprintf(out, cap, "$TGS,%u,%s,%lld,%lu", (unsigned)s->gate, role, (long long)s->us,
                     (unsigned long)s->drops);
    return finish_line(out, cap, n);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Parses a decimal integer filling the whole field [s, s+len). */
static bool parse_int(const char *s, size_t len, int64_t lo, int64_t hi, int64_t *v) {
    if (len == 0 || len > 20) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; if (len == 1) return false; }
    int64_t acc = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (acc > (INT64_MAX - 9) / 10) return false;
        acc = acc * 10 + (s[i] - '0');
    }
    if (neg) acc = -acc;
    if (acc < lo || acc > hi) return false;
    *v = acc;
    return true;
}

int tlog_line_parse(const char *line, size_t len, tlog_tgr_t *r, tlog_tgs_t *s) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (len < 8 || line[0] != '$') return TLOG_ERR_FORMAT;
    const char *star = memchr(line, '*', len);
    if (!star || (size_t)(line + len - star) != 3) return TLOG_ERR_CRC;
    int h = hexval(star[1]), l = hexval(star[2]);
    if (h < 0 || l < 0) return TLOG_ERR_CRC;
    if (tlog_line_xor(line + 1, (size_t)(star - line - 1)) != (uint8_t)(h * 16 + l)) return TLOG_ERR_CRC;

    /* split the body into fields */
    const char *f[6];
    size_t fl[6];
    size_t nf = 0;
    const char *p = line + 1;
    while (p <= star && nf < 6) {
        const char *e = p;
        while (e < star && *e != ',') e++;
        f[nf] = p;
        fl[nf] = (size_t)(e - p);
        nf++;
        p = e + 1;
    }
    if (p <= star) return TLOG_ERR_FORMAT; /* more than 6 fields */

    int64_t v;
    if (word_is(f[0], fl[0], "TGR")) {
        if (nf != 4 || fl[1] != 8) return TLOG_ERR_FORMAT;
        uint32_t id = 0;
        for (size_t i = 0; i < 8; i++) {
            int d = hexval(f[1][i]);
            if (d < 0) return TLOG_ERR_FORMAT;
            id = (id << 4) | (uint32_t)d;
        }
        r->id = id;
        if (!parse_int(f[2], fl[2], -128, 127, &v)) return TLOG_ERR_FORMAT;
        r->rssi = (int8_t)v;
        if (!parse_int(f[3], fl[3], INT64_MIN + 1, INT64_MAX, &v)) return TLOG_ERR_FORMAT;
        r->us = v;
        return TLOG_LINE_TGR;
    }
    if (word_is(f[0], fl[0], "TGS")) {
        if (nf != 5) return TLOG_ERR_FORMAT;
        if (!parse_int(f[1], fl[1], 0, 255, &v)) return TLOG_ERR_FORMAT;
        s->gate = (uint8_t)v;
        if (tlog_role_parse(f[2], fl[2], &s->role, &s->index) != TLOG_OK) return TLOG_ERR_FORMAT;
        if (!parse_int(f[3], fl[3], INT64_MIN + 1, INT64_MAX, &v)) return TLOG_ERR_FORMAT;
        s->us = v;
        if (!parse_int(f[4], fl[4], 0, UINT32_MAX, &v)) return TLOG_ERR_FORMAT;
        s->drops = (uint32_t)v;
        return TLOG_LINE_TGS;
    }
    return TLOG_ERR_FORMAT;
}
