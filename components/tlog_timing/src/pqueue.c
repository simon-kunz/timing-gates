#include "tlog/pqueue.h"

#include <string.h>

#include "tlog/crc32.h"
#include "tlog/result.h"

enum { T_HEADER = 'H', T_PASSAGE = 'P', T_ACK = 'A' };

static void put32(uint8_t *b, uint32_t v) {
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static void seal(uint8_t *r) { put32(r + 12, tlog_crc32(r, 12)); }
static bool valid(const uint8_t *r) { return get32(r + 12) == tlog_crc32(r, 12); }
static bool erased(const uint8_t *r) {
    for (unsigned i = 0; i < TLOG_PQ_REC; i++)
        if (r[i] != 0xFF) return false;
    return true;
}

static uint32_t off(const tlog_pq_t *q, uint16_t sec, uint16_t slot) {
    return (uint32_t)sec * q->fl.sector + (uint32_t)slot * TLOG_PQ_REC;
}

static int rd(const tlog_pq_t *q, uint16_t sec, uint16_t slot, uint8_t *r) {
    return q->fl.read(q->fl.ctx, off(q, sec, slot), r, TLOG_PQ_REC) ? TLOG_ERR_IO : TLOG_OK;
}

/* Erases the next sector of the ring and starts it, if its passages are all acked. */
static int advance(tlog_pq_t *q) {
    uint16_t nx = (uint16_t)((q->cur + 1) % q->nsec);
    if (q->sec[nx].live && q->sec[nx].first + q->sec[nx].count > q->acked) return TLOG_ERR_NOSPACE;
    if (q->fl.erase(q->fl.ctx, off(q, nx, 0))) return TLOG_ERR_IO;
    q->sec[nx].live = false;
    uint16_t seq = (uint16_t)(q->sec[q->cur].seq + 1);
    uint8_t r[TLOG_PQ_REC] = {T_HEADER, 0};
    r[2] = (uint8_t)seq;
    r[3] = (uint8_t)(seq >> 8);
    put32(r + 4, q->next_serial);
    put32(r + 8, q->acked);
    seal(r);
    if (q->fl.write(q->fl.ctx, off(q, nx, 0), r, TLOG_PQ_REC)) return TLOG_ERR_IO;
    q->sec[nx].live = true;
    q->sec[nx].seq = seq;
    q->sec[nx].first = q->next_serial;
    q->sec[nx].count = 0;
    q->cur = nx;
    q->head = 1;
    return TLOG_OK;
}

static int put(tlog_pq_t *q, uint8_t *r) {
    if (q->head >= q->slots) {
        int e = advance(q);
        if (e) return e;
    }
    seal(r);
    if (q->fl.write(q->fl.ctx, off(q, q->cur, q->head), r, TLOG_PQ_REC)) return TLOG_ERR_IO;
    q->head++;
    return TLOG_OK;
}

int tlog_pq_open(tlog_pq_t *q, const tlog_flash_t *fl) {
    memset(q, 0, sizeof(*q));
    if (!fl || !fl->sector || fl->sector % TLOG_PQ_REC || fl->size % fl->sector) return TLOG_ERR_ARG;
    uint32_t n = fl->size / fl->sector;
    if (n < 2 || n > TLOG_PQ_MAX_SECTORS || fl->sector / TLOG_PQ_REC < 2 || fl->sector / TLOG_PQ_REC > 0xFFFF)
        return TLOG_ERR_ARG;
    q->fl = *fl;
    q->nsec = (uint16_t)n;
    q->slots = (uint16_t)(fl->sector / TLOG_PQ_REC);

    uint8_t r[TLOG_PQ_REC];
    bool any = false;
    uint16_t newest = 0, newest_seq = 0;
    uint32_t acked = 0;
    for (uint16_t s = 0; s < q->nsec; s++) {
        if (rd(q, s, 0, r)) return TLOG_ERR_IO;
        if (!valid(r) || r[0] != T_HEADER) continue; /* free, or a torn header: erased on entry */
        q->sec[s].live = true;
        q->sec[s].seq = (uint16_t)(r[2] | r[3] << 8);
        q->sec[s].first = get32(r + 4);
        if (get32(r + 8) > acked) acked = get32(r + 8);
        /* the newest sector: live sectors are a contiguous run of the ring ending at it */
        if (!any || (int16_t)(q->sec[s].seq - newest_seq) > 0) {
            newest = s;
            newest_seq = q->sec[s].seq;
        }
        any = true;
    }
    if (!any) {
        /* empty: start sector 0 as if coming from the last one */
        q->cur = (uint16_t)(q->nsec - 1);
        q->sec[q->cur].seq = 0xFFFF;
        return advance(q);
    }
    /* drop live sectors that aren't part of the run ending at the newest (stale leftovers) */
    for (uint16_t s = 0; s < q->nsec; s++) {
        if (!q->sec[s].live) continue;
        uint16_t back = (uint16_t)((newest + q->nsec - s) % q->nsec);
        if ((uint16_t)(newest_seq - q->sec[s].seq) != back) q->sec[s].live = false;
    }
    uint32_t oldest_first = q->sec[newest].first;
    for (uint16_t s = 0; s < q->nsec; s++) {
        if (!q->sec[s].live) continue;
        if (q->sec[s].first < oldest_first) oldest_first = q->sec[s].first;
        uint16_t last = 0;
        for (uint16_t k = 1; k < q->slots; k++) {
            if (rd(q, s, k, r)) return TLOG_ERR_IO;
            if (erased(r)) continue;
            last = k;
            if (!valid(r)) continue; /* torn */
            if (r[0] == T_PASSAGE) q->sec[s].count++;
            else if (r[0] == T_ACK && get32(r + 8) > acked) acked = get32(r + 8);
        }
        if (s == newest) q->head = (uint16_t)(last + 1);
    }
    q->cur = newest;
    q->next_serial = q->sec[newest].first + q->sec[newest].count;
    if (oldest_first > acked) acked = oldest_first; /* passages of erased sectors were acked */
    if (acked > q->next_serial) acked = q->next_serial;
    q->acked = acked;
    return TLOG_OK;
}

int tlog_pq_append(tlog_pq_t *q, const tlog_passage_t *p) {
    uint8_t r[TLOG_PQ_REC] = {T_PASSAGE, p->gate};
    for (int i = 0; i < 6; i++) r[2 + i] = (uint8_t)(p->epoch_ms >> (8 * i));
    put32(r + 8, p->epc);
    int e = put(q, r);
    if (e) return e;
    q->sec[q->cur].count++;
    q->next_serial++;
    return TLOG_OK;
}

uint32_t tlog_pq_pending(const tlog_pq_t *q) { return q->next_serial - q->acked; }

int tlog_pq_peek(const tlog_pq_t *q, uint32_t i, tlog_passage_t *p) {
    if (i >= tlog_pq_pending(q)) return TLOG_ERR_ARG;
    uint32_t serial = q->acked + i;
    for (uint16_t s = 0; s < q->nsec; s++) {
        if (!q->sec[s].live || serial < q->sec[s].first || serial >= q->sec[s].first + q->sec[s].count) continue;
        uint32_t want = serial - q->sec[s].first, seen = 0;
        uint8_t r[TLOG_PQ_REC];
        for (uint16_t k = 1; k < q->slots; k++) {
            if (rd(q, s, k, r)) return TLOG_ERR_IO;
            if (!valid(r) || r[0] != T_PASSAGE) continue;
            if (seen++ < want) continue;
            p->gate = r[1];
            p->epoch_ms = 0;
            for (int b = 0; b < 6; b++) p->epoch_ms |= (uint64_t)r[2 + b] << (8 * b);
            p->epc = get32(r + 8);
            return TLOG_OK;
        }
    }
    return TLOG_ERR_FORMAT; /* the index and the flash disagree */
}

int tlog_pq_ack(tlog_pq_t *q, uint32_t n) {
    if (n > tlog_pq_pending(q)) return TLOG_ERR_ARG;
    if (n == 0) return TLOG_OK;
    q->acked += n;
    uint8_t r[TLOG_PQ_REC] = {T_ACK};
    put32(r + 8, q->acked);
    return put(q, r);
}
