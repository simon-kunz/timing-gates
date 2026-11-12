#include "tlog/crc32.h"

static uint32_t table[256];
static int table_ready;

static void make_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    table_ready = 1;
}

uint32_t tlog_crc32_update(uint32_t crc, const void *data, size_t len) {
    if (!table_ready) make_table();
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) crc = table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
