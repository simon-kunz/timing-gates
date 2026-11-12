/*
 * Crossing detector (design D5). One instance per gate on the tag, per tag
 * on the uplink. Platform-free, host-tested.
 *
 *  1. Binning: received frames go into bins of bin_us (tag 50 ms, uplink 100 ms), mean dBm each.
 *  2. Smoothing: a centred moving average over smooth_us (250 ms) of the non-empty bins.
 *  3. Candidate: the highest smoothed bin at or above the threshold.
 *  4. Confirmation: the smoothed value stays >= drop_db below the candidate for drop_us, or no
 *     frame arrives for silence_us.
 *  5. Time: the vertex of a least-squares parabola through the raw bin means around the
 *     candidate: every bin whose smoothed value is within fit_db of the candidate's, at least
 *     the two neighbours and at most fit_max_us on each side. The window grows with the width of
 *     the peak, so slow passes (broad peaks) are fitted over more bins; whole-dB RSSI makes the
 *     top of a peak flat, which a 3-point fit can't place.
 *  6. Hold: after a confirmation, nothing can create or move a crossing for minint_us after the
 *     crossing time, and until the source has once been below the threshold (or silent). A rider
 *     standing near the gate, or riding off after standing there, gives no second crossing.
 */
#ifndef TLOG_CROSSING_H
#define TLOG_CROSSING_H

#include <stdbool.h>
#include <stdint.h>

#define TLOG_XDET_FIT_MAX 60  /* bins on each side of the candidate the fit may use */
#define TLOG_XDET_RING 72     /* closed bins kept; > TLOG_XDET_FIT_MAX + smoothing half-width */
#define TLOG_XDET_EMPTY INT16_MIN

typedef struct {
    int32_t bin_us;     /* 50 000 on the tag, 100 000 on the uplink */
    int32_t smooth_us;  /* 250 000 */
    int8_t thresh_dbm;  /* -80 */
    uint8_t drop_db;    /* 6 */
    int32_t drop_us;    /* 200 000 */
    int32_t silence_us; /* 300 000 */
    int64_t minint_us;  /* 10 000 000 */
    uint8_t fit_db;     /* 4 */
    int32_t fit_max_us; /* 3 000 000, capped at TLOG_XDET_FIT_MAX bins */
} tlog_xdet_cfg_t;

/* The defaults of design D5 for a given bin width. */
tlog_xdet_cfg_t tlog_xdet_defaults(int32_t bin_us);

typedef struct {
    int64_t t_us;       /* crossing time (parabola vertex), on the caller's clock */
    int64_t confirm_us; /* when it was confirmed */
    float peak_dbm;     /* smoothed RSSI at the candidate */
} tlog_xing_t;

/* A closed bin: raw mean and smoothed value in 1/100 dB, TLOG_XDET_EMPTY if none. */
typedef struct {
    int16_t raw;
    int16_t sm;
} tlog_xbin_t;

typedef struct {
    tlog_xdet_cfg_t cfg;
    int half;              /* smoothing half-width in bins */
    int fit_max;           /* fit half-width limit in bins */
    bool started;
    int64_t cur_bin;       /* index of the bin being filled */
    float cur_sum;
    uint16_t cur_n;
    tlog_xbin_t ring[TLOG_XDET_RING]; /* closed bins, by index modulo TLOG_XDET_RING */
    int64_t closed;        /* index of the newest closed bin */
    int64_t last_frame_us;
    /* candidate, with the bins around it: snap[TLOG_XDET_FIT_MAX + k] is bin cand_bin + k */
    bool cand;
    int64_t cand_bin;
    int16_t cand_s;
    tlog_xbin_t snap[2 * TLOG_XDET_FIT_MAX + 1];
    int drop_bins;
    /* hold */
    bool armed;
    int64_t hold_until_us;
    uint32_t crossings;
} tlog_xdet_t;

void tlog_xdet_init(tlog_xdet_t *d, const tlog_xdet_cfg_t *cfg);
/* A frame heard at t_us (non-decreasing) with rssi dBm. Returns true and fills *out when this
 * frame confirmed a crossing. */
bool tlog_xdet_feed(tlog_xdet_t *d, int64_t t_us, int rssi, tlog_xing_t *out);
/* Call periodically (e.g. every bin) with the current time: closes bins and confirms a candidate
 * after silence_us without frames. Returns true and fills *out on a confirmation. */
bool tlog_xdet_poll(tlog_xdet_t *d, int64_t now_us, tlog_xing_t *out);

#endif
