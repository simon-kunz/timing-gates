/*
 * BLE adverts of the tag (task 5.1; design D7): NimBLE, legacy scannable
 * non-connectable advertising (ADV_SCAN_IND). Advert data: flags, then manufacturer data 0xFFFF
 * with the 24-byte payload (31 bytes in all); scan response: the name TG-<tag id>.
 */
#ifndef ADV_H
#define ADV_H

#include <stdint.h>

void adv_start(uint32_t tag_id);
/* New payload and advertising interval; restarts advertising only if the interval changed. */
void adv_update(const uint8_t payload[24], uint32_t interval_ms);

#endif
