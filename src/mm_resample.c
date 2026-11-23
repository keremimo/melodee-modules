/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#include "mm_resample.h"
#include <math.h>
#include <string.h>

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 40; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

void mm_rs_init(mm_rs *s, double in_rate, double out_rate)
{
    const double beta = 8.0;
    double fc = 0.92 * (out_rate < in_rate ? out_rate / in_rate : 1.0);   /* of the input's Nyquist */
    double i0b = bessel_i0(beta);
    memset(s, 0, sizeof *s);
    s->bypass = fabs(in_rate - out_rate) < 0.5;
    s->step = in_rate / out_rate;
    for (uint32_t p = 0; p <= MM_RS_P; p++) {
        double f = (double)p / MM_RS_P;
        for (uint32_t j = 0; j < 2u * MM_RS_Z; j++) {
            double x = f - ((double)j - (double)MM_RS_Z + 1.0);   /* t - tap, taps -Z+1 .. Z */
            double u = x / (double)MM_RS_Z, w, sinc;
            w = fabs(u) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - u * u)) / i0b;
            sinc = fabs(x) < 1e-12 ? 1.0 : sin(M_PI * fc * x) / (M_PI * fc * x);
            s->coef[p][j] = (float)(fc * sinc * w);
        }
    }
    mm_rs_reset(s);
}

void mm_rs_reset(mm_rs *s)
{
    memset(s->buf, 0, sizeof s->buf);
    s->n = MM_RS_Z;                              /* zeros before the first sample */
    s->pos = (double)MM_RS_Z - 1.0;
}

uint32_t mm_rs_latency(const mm_rs *s)
{
    return s->bypass ? 0u : (uint32_t)lround((double)MM_RS_Z / s->step);
}

void mm_rs_run(mm_rs *s, mm_rs_pull pull, void *ctx, float *l, float *r, uint32_t frames)
{
    if (s->bypass) {
        for (uint32_t i = 0; i < frames; i++)
            pull(ctx, &l[i], &r[i]);
        return;
    }
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t i0 = (uint32_t)s->pos, need = i0 + MM_RS_Z + 1u;
        double fp;
        uint32_t p;
        float a, accl = 0.0f, accr = 0.0f;
        const float *c0, *c1, *xl, *xr;
        if (need > MM_RS_BUF) {                  /* move what is still needed to the start */
            uint32_t drop = i0 + 1u - MM_RS_Z;
            memmove(s->buf[0], s->buf[0] + drop, (s->n - drop) * sizeof(float));
            memmove(s->buf[1], s->buf[1] + drop, (s->n - drop) * sizeof(float));
            s->n -= drop;
            s->pos -= drop;
            i0 -= drop;
            need -= drop;
        }
        while (s->n < need) {
            pull(ctx, &s->buf[0][s->n], &s->buf[1][s->n]);
            s->n++;
        }
        fp = (s->pos - i0) * MM_RS_P;
        p = (uint32_t)fp;
        a = (float)(fp - p);
        c0 = s->coef[p];
        c1 = s->coef[p + 1u];
        xl = s->buf[0] + i0 + 1u - MM_RS_Z;
        xr = s->buf[1] + i0 + 1u - MM_RS_Z;
        for (uint32_t j = 0; j < 2u * MM_RS_Z; j++) {
            float c = c0[j] + a * (c1[j] - c0[j]);
            accl += c * xl[j];
            accr += c * xr[j];
        }
        l[i] = accl;
        r[i] = accr;
        s->pos += s->step;
    }
}
