/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The modules play at 44.1 kHz (MOD3_RATE); the host may run at another rate. A windowed-sinc interpolator (Kaiser,
 * MM_RS_Z taps each side, MM_RS_P phases with linear steps between them) reads the 44.1 kHz stream as the host's
 * frames ask for it. At 44.1 kHz it passes the samples through as they are. */
#pragma once
#include <stdint.h>

#define MM_RS_Z 16u
#define MM_RS_P 256u
#define MM_RS_BUF 512u

typedef struct mm_rs {
    double step;                                 /* input samples a frame: 44100 / host rate */
    double pos;                                  /* the next frame's place in buf */
    uint32_t n;                                  /* samples held in buf */
    int bypass;
    float buf[2][MM_RS_BUF];
    float coef[MM_RS_P + 1u][2u * MM_RS_Z];
} mm_rs;

/* the source: the next input sample (both sides) */
typedef void (*mm_rs_pull)(void *ctx, float *l, float *r);

void mm_rs_init(mm_rs *s, double in_rate, double out_rate);
void mm_rs_reset(mm_rs *s);
uint32_t mm_rs_latency(const mm_rs *s);          /* output frames */
void mm_rs_run(mm_rs *s, mm_rs_pull pull, void *ctx, float *l, float *r, uint32_t frames);
