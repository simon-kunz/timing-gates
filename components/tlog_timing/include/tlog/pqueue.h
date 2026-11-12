/*
 * Passage queue of the uplink (design D10; spec gate-uplink, Offline queue).
 * Platform-free; the flash is reached through tlog_flash_t, so the host tests run on RAM.
 *
 * Layout: the partition is a ring of erase sectors, each a run of 16-byte records:
 *   header   'H' | 0 | seq u16 | first serial u32 | acked u32 | crc32     (slot 0 of a sector)
 *   passage  'P' | gate u8 | epoch_ms u48 | epc u32 | crc32
 *   ack      'A' | 0 0 0 0 0 0 0 | acked u32 | crc32
 * (multi-byte fields little-endian; crc32 over bytes 0-11; an erased slot is all 0xFF.)
 * Passages get serial numbers in append order; "acked" means every passage with a lower serial was
 * accepted by the server. Records are only appended. When the write sector is full, the next
 * sector in the ring is erased and started, which is allowed only if all its passages are acked;
 * otherwise the queue is full. A 64 kB partition of 4 kB sectors holds 16 x 255 records, so more
 * than 3800 unsent passages.
 *
 * After a restart, tlog_pq_open rebuilds the state from the records: a torn record (bad CRC) is
 * skipped, a sector with a torn header is treated as free. An ack lost in a crash makes the
 * uplink post a few passages again, with the same EPC and time (the server drops them).
 */
#ifndef TLOG_PQUEUE_H
#define TLOG_PQUEUE_H

#include <stdbool.h>
#include <stdint.h>

#define TLOG_PQ_REC 16u
#define TLOG_PQ_MAX_SECTORS 32u

typedef struct {
    void *ctx;
    /* each returns 0 on success */
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, uint32_t len); /* can only clear bits */
    int (*erase)(void *ctx, uint32_t off);                                /* one sector at off */
    uint32_t size;   /* partition size, a multiple of sector */
    uint32_t sector; /* erase sector size, a multiple of TLOG_PQ_REC */
} tlog_flash_t;

typedef struct {
    uint32_t epc;      /* tag id */
    uint64_t epoch_ms; /* passage time, < 2^48 */
    uint8_t gate;      /* gate id the passage was heard on */
} tlog_passage_t;

typedef struct {
    tlog_flash_t fl;
    uint16_t nsec;  /* sectors in the ring */
    uint16_t slots; /* records per sector */
    uint16_t cur;   /* write sector */
    uint16_t head;  /* next free slot in cur */
    uint32_t next_serial;
    uint32_t acked;
    struct {
        bool live;
        uint16_t seq;
        uint32_t first;  /* serial of its first passage */
        uint16_t count;  /* passages in it */
    } sec[TLOG_PQ_MAX_SECTORS];
} tlog_pq_t;

/* Rebuilds the queue from flash (formats it if empty). TLOG_ERR_ARG for a bad geometry
 * (2..TLOG_PQ_MAX_SECTORS sectors), TLOG_ERR_IO on a flash error. */
int tlog_pq_open(tlog_pq_t *q, const tlog_flash_t *fl);

/* Stores a passage. TLOG_ERR_NOSPACE when the ring is full of unacked passages. */
int tlog_pq_append(tlog_pq_t *q, const tlog_passage_t *p);

/* Passages not yet acked. */
uint32_t tlog_pq_pending(const tlog_pq_t *q);

/* The i-th pending passage (0 = oldest), in append order. TLOG_ERR_ARG if i >= pending. */
int tlog_pq_peek(const tlog_pq_t *q, uint32_t i, tlog_passage_t *p);

/* The server accepted the n oldest pending passages. The batcher posts a prefix of the queue, so
 * the ack is a count. The ack holds in RAM even if it can't be stored (TLOG_ERR_NOSPACE). */
int tlog_pq_ack(tlog_pq_t *q, uint32_t n);

#endif
