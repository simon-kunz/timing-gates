/*
 * Tag role: crossing detection and run timing (task 3.4; design D5, D6).
 * Gate frames go from the radio callback into a queue; a task feeds one crossing detector per gate
 * (50 ms bins), polls them, and drives the run state machine. Crossings and run events are printed
 * on the console as they happen and kept in RAM for later:
 *   xings      the last 32 crossings with their run events
 *   xdump <n>  the raw RSSI of crossing n (8 s before it to 1 s after its confirmation) as $TGR
 *              lines, for test vectors; the last 4 crossings are kept
 * So a ride needs no laptop on the bike: plug the tag in afterwards.
 */
#ifndef TAGRUN_H
#define TAGRUN_H

#include <stdint.h>

#include "settings.h"
#include "tlog/timing.h"

void tagrun_start(const settings_t *s);
/* From the ESP-NOW receive callback: a GATE frame of gate `id` heard at us with rssi. */
void tagrun_gate_frame(uint8_t id, const tlog_gate_t *g, int rssi, int64_t us);
void tagrun_print_log(void);
/* From the ESP-NOW receive callback: a BREAK copy from gate `id`, received at us. */
void tagrun_break(uint8_t id, const tlog_break_t *b, int64_t us);
void tagrun_dump(int n);

#endif
