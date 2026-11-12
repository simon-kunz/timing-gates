/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), same as zlib.crc32. */
#ifndef TLOG_CRC32_H
#define TLOG_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Start with crc = 0; feed data in pieces; the running value is the final CRC. */
uint32_t tlog_crc32_update(uint32_t crc, const void *data, size_t len);

static inline uint32_t tlog_crc32(const void *data, size_t len) { return tlog_crc32_update(0, data, len); }

#endif
