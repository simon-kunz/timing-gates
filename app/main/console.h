/*
 * USB console of a timing unit (design D8): one command per line on the USB-Serial-JTAG port,
 * read by polling.
 *   role gate|tag|uplink                       stores the role and restarts
 *   gate <id> start|split <n>|finish|startfinish
 *   barrier on|off, stream on|off              (gate)
 *   thresh <dBm>, minint <s>                   (tag, uplink)
 *   wifi <ssid> <pass>, server <url>, reader <id>, token <t>|none   (uplink)
 *   hear [on|off]                              once a second: every unit heard, rate, RSSI, gaps
 *   trace [on|off]                             $TGR line per GATE frame heard (gate id, RSSI, µs)
 *   rate [1|6]                                 sending rate in Mbps, stored (range test, task 2.2)
 *   xings                                      tag: the last crossings and their run events
 *   xdump <n>                                  tag: raw RSSI around crossing n as $TGR lines
 *   break [<tag>|none]                         test: a gate sends a BREAK burst naming that tag
 *   mute <s>                                   test: no beacons for s seconds
 *   show                                       role, tag id, MAC and every setting
 *   help
 */
#ifndef CONSOLE_H
#define CONSOLE_H

void console_start(void);
/* Prints the unit's identity and settings (also at boot). */
void console_show(void);

#endif
