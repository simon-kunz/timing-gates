#include "tlog/crossing.h"

#include <math.h>
#include <string.h>

tlog_xdet_cfg_t tlog_xdet_defaults(int32_t bin_us) {
    tlog_xdet_cfg_t c = {
        .bin_us = bin_us,
        .smooth_us = 250000,
        .thresh_dbm = -80,
        .drop_db = 6,
        .drop_us = 200000,
        .silence_us = 300000,
        .minint_us = 10000000,
        .fit_db = 4,
        .fit_max_us = 3000000,
    };
    return c;
}

static const tlog_xbin_t EMPTY_BIN = {TLOG_XDET_EMPTY, TLOG_XDET_EMPTY};

static int64_t floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

static tlog_xbin_t *slot(tlog_xdet_t *d, int64_t i) {
    int64_t m = i % (int64_t)TLOG_XDET_RING;
    if (m < 0) m += TLOG_XDET_RING;
    return &d->ring[m];
}

/* A closed bin still in the ring, or an empty one. */
static tlog_xbin_t bin_at(tlog_xdet_t *d, int64_t i) {
    if (i > d->closed || d->closed - i >= (int64_t)TLOG_XDET_RING) return EMPTY_BIN;
    return *slot(d, i);
}

static int16_t smooth(tlog_xdet_t *d, int64_t c) {
    int32_t acc = 0;
    int n = 0;
    for (int64_t i = c - d->half; i <= c + d->half; i++) {
        int16_t r = bin_at(d, i).raw;
        if (r != TLOG_XDET_EMPTY) {
            acc += r;
            n++;
        }
    }
    return n ? (int16_t)(acc / n) : TLOG_XDET_EMPTY;
}

void tlog_xdet_init(tlog_xdet_t *d, const tlog_xdet_cfg_t *cfg) {
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
    int bins = (int)((cfg->smooth_us + cfg->bin_us / 2) / cfg->bin_us); /* window in bins */
    if (bins < 1) bins = 1;
    d->half = bins / 2; /* 250/50 = 5 -> 2; 250/100 = 2.5 -> 3 -> 1 */
    if (d->half > 4) d->half = 4;
    d->fit_max = (int)(cfg->fit_max_us / cfg->bin_us);
    if (d->fit_max < 1) d->fit_max = 1;
    if (d->fit_max > TLOG_XDET_FIT_MAX) d->fit_max = TLOG_XDET_FIT_MAX;
    if (d->fit_max > TLOG_XDET_RING - d->half - 1) d->fit_max = TLOG_XDET_RING - d->half - 1;
    for (int i = 0; i < TLOG_XDET_RING; i++) d->ring[i] = EMPTY_BIN;
    d->armed = true;
    d->hold_until_us = INT64_MIN;
}

static void set_candidate(tlog_xdet_t *d, int64_t c, int16_t s) {
    d->cand = true;
    d->cand_bin = c;
    d->cand_s = s;
    d->drop_bins = 0;
    for (int k = -TLOG_XDET_FIT_MAX; k <= TLOG_XDET_FIT_MAX; k++) {
        d->snap[TLOG_XDET_FIT_MAX + k] = (k <= 0 && -k <= d->fit_max) ? bin_at(d, c + k) : EMPTY_BIN;
    }
}

/* Least-squares parabola y = a x^2 + b x + c over the points; returns the vertex x, or 0 if the
 * fit is not a maximum or has fewer than 3 points. */
static double vertex(const double *x, const double *y, int n) {
    if (n < 3) return 0;
    double s0 = n, s1 = 0, s2 = 0, s3 = 0, s4 = 0, t0 = 0, t1 = 0, t2 = 0;
    for (int i = 0; i < n; i++) {
        double xi = x[i], x2 = xi * xi;
        s1 += xi; s2 += x2; s3 += x2 * xi; s4 += x2 * x2;
        t0 += y[i]; t1 += xi * y[i]; t2 += x2 * y[i];
    }
    /* normal equations: [s4 s3 s2; s3 s2 s1; s2 s1 s0] [a b c] = [t2 t1 t0], Cramer's rule */
    double det = s4 * (s2 * s0 - s1 * s1) - s3 * (s3 * s0 - s1 * s2) + s2 * (s3 * s1 - s2 * s2);
    if (fabs(det) < 1e-12) return 0;
    double da = t2 * (s2 * s0 - s1 * s1) - s3 * (t1 * s0 - s1 * t0) + s2 * (t1 * s1 - s2 * t0);
    double db = s4 * (t1 * s0 - t0 * s1) - t2 * (s3 * s0 - s1 * s2) + s2 * (s3 * t0 - t1 * s2);
    double a = da / det, b = db / det;
    if (a >= 0) return 0;
    return -b / (2 * a);
}

static void confirm(tlog_xdet_t *d, int64_t now_us, tlog_xing_t *out) {
    const int F = TLOG_XDET_FIT_MAX;
    int32_t floor_s = (int32_t)d->cand_s - 100 * (int32_t)d->cfg.fit_db;
    int lo = 0, hi = 0;
    while (lo > -d->fit_max) {
        tlog_xbin_t b = d->snap[F + lo - 1];
        if (lo > -1 || (b.sm != TLOG_XDET_EMPTY && b.sm >= floor_s)) lo--; else break;
    }
    while (hi < d->fit_max) {
        tlog_xbin_t b = d->snap[F + hi + 1];
        if (hi < 1 || (b.sm != TLOG_XDET_EMPTY && b.sm >= floor_s)) hi++; else break;
    }
    double xs[2 * TLOG_XDET_FIT_MAX + 1], ys[2 * TLOG_XDET_FIT_MAX + 1];
    int n = 0;
    for (int k = lo; k <= hi; k++) {
        int16_t r = d->snap[F + k].raw;
        if (r == TLOG_XDET_EMPTY) continue;
        xs[n] = k;
        ys[n] = r / 100.0;
        n++;
    }
    double off = vertex(xs, ys, n);
    if (off < lo) off = lo;
    if (off > hi) off = hi;
    double t = ((double)d->cand_bin + 0.5 + off) * (double)d->cfg.bin_us;
    out->t_us = (int64_t)llround(t);
    out->confirm_us = now_us;
    out->peak_dbm = d->cand_s / 100.0f;
    d->crossings++;
    d->cand = false;
    d->armed = false;
    d->hold_until_us = out->t_us + d->cfg.minint_us;
}

/* Decides on bin c, whose smoothing window is complete. */
static bool evaluate(tlog_xdet_t *d, int64_t c, int64_t now_us, tlog_xing_t *out) {
    int16_t s = smooth(d, c);
    slot(d, c)->sm = s;
    bool empty = s == TLOG_XDET_EMPTY;
    bool low = empty || s < 100 * (int32_t)d->cfg.thresh_dbm;
    if (!d->armed) {
        if ((c + 1) * (int64_t)d->cfg.bin_us >= d->hold_until_us && low) d->armed = true;
        return false;
    }
    if (d->cand) {
        int64_t k = c - d->cand_bin;
        if (k > 0 && k <= d->fit_max) d->snap[TLOG_XDET_FIT_MAX + k] = *slot(d, c);
        if (!empty && s > d->cand_s) {
            set_candidate(d, c, s);
        } else if (empty || s <= (int32_t)d->cand_s - 100 * (int32_t)d->cfg.drop_db) {
            d->drop_bins++;
            if ((int64_t)d->drop_bins * d->cfg.bin_us >= d->cfg.drop_us) {
                confirm(d, now_us, out);
                return true;
            }
        } else {
            d->drop_bins = 0;
        }
    } else if (!low) {
        set_candidate(d, c, s);
    }
    return false;
}

/* Closes every bin before `target`. */
static bool close_through(tlog_xdet_t *d, int64_t target, int64_t now_us, tlog_xing_t *out) {
    bool hit = false;
    int64_t limit = (int64_t)TLOG_XDET_RING + 2 * d->half + 2;
    int64_t steps = 0;
    while (d->cur_bin < target) {
        if (steps++ >= limit) {
            /* a long silence: everything in the ring is empty by now; jump ahead */
            for (int i = 0; i < TLOG_XDET_RING; i++) d->ring[i] = EMPTY_BIN;
            d->cur_bin = target;
            d->closed = target - 1;
            d->cur_sum = 0;
            d->cur_n = 0;
            if (!d->armed && target * (int64_t)d->cfg.bin_us >= d->hold_until_us) d->armed = true;
            break;
        }
        tlog_xbin_t b = EMPTY_BIN;
        if (d->cur_n) b.raw = (int16_t)lroundf(100.0f * d->cur_sum / (float)d->cur_n);
        *slot(d, d->cur_bin) = b;
        d->closed = d->cur_bin;
        d->cur_sum = 0;
        d->cur_n = 0;
        d->cur_bin++;
        if (!hit && evaluate(d, d->closed - d->half, now_us, out)) hit = true;
    }
    return hit;
}

static bool silence(tlog_xdet_t *d, int64_t now_us, tlog_xing_t *out) {
    if (d->cand && now_us - d->last_frame_us >= d->cfg.silence_us) {
        confirm(d, now_us, out);
        return true;
    }
    return false;
}

bool tlog_xdet_feed(tlog_xdet_t *d, int64_t t_us, int rssi, tlog_xing_t *out) {
    int64_t b = floordiv(t_us, d->cfg.bin_us);
    bool hit = false;
    if (!d->started) {
        d->started = true;
        d->cur_bin = b;
        d->closed = b - 1;
    } else {
        hit = silence(d, t_us, out);
        if (close_through(d, b, t_us, out)) hit = true;
    }
    if (b == d->cur_bin) { /* a frame older than the current bin is dropped */
        d->cur_sum += (float)rssi;
        d->cur_n++;
    }
    d->last_frame_us = t_us;
    return hit;
}

bool tlog_xdet_poll(tlog_xdet_t *d, int64_t now_us, tlog_xing_t *out) {
    if (!d->started) return false;
    bool hit = silence(d, now_us, out);
    if (close_through(d, floordiv(now_us, d->cfg.bin_us), now_us, out)) hit = true;
    return hit;
}
