/*
 * ID beacons (task 2.1; spec gate-beacon "Gate ID beacon", crossing-detection
 * "Tag ID beacon"): a gate broadcasts GATE frames at 100 Hz, a tag TAG frames at 20 Hz, over ESP-NOW
 * on RADIO_CHANNEL at 6 Mbps OFDM (design D11). Both count what they hear, per sender, for the
 * `hear` console command.
 */
#ifndef BEACONS_H
#define BEACONS_H

#include <stdbool.h>

#include "settings.h"

/* Starts the radio and, for the gate and tag roles, the beacons. */
void beacons_start(const settings_t *s);
/* Prints, once a second, every unit heard in that second: id, frames per second, mean RSSI,
 * sequence gaps and the PHY rate. */
void beacons_hear(bool on);
bool beacons_hearing(void);
/* RSSI trace (task 3.1): one $TGR line per GATE frame heard, with the gate id and the receive time
 * on this unit's µs clock. Off prints how many frames were traced and dropped. */
void beacons_trace(bool on);
/* Sending rate for the range test (task 2.2): 6 (the default, OFDM) or 1 Mbps. Stored, so a unit
 * on a power bank keeps it; set it back to 6 after the test. */
bool beacons_set_rate(int mbps);
/* Test: no beacons for this many seconds (a gate out of range, or a pass's silence). */
void beacons_mute(int seconds);
/* Test: a BREAK burst as the barrier will send it (task 4.2); nearest 0 = names no tag. */
bool beacons_test_break(uint32_t nearest);
int beacons_rate(void);
bool beacons_tracing(void);

#endif
