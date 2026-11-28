/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Drawing into the screen's pixels (0x00RRGGBB, as melodee_screen.h gives them): fills with round corners, lines and
 * discs with soft edges, and the faceplates' letters (mm_font.h). Coordinates in pixels, fractions allowed
 * where an edge is soft. */
#pragma once
#include <stdint.h>
#include "mm_font.h"

typedef struct mm_canvas {
    uint32_t *px;
    int w, h, stride;
    int cx0, cy0, cx1, cy1;                      /* the clip: x in cx0..cx1-1, y in cy0..cy1-1 */
} mm_canvas;

void mm_cv_init(mm_canvas *c, uint32_t *px, int w, int h, int stride);
void mm_cv_clip(mm_canvas *c, int x, int y, int w, int h);   /* (within the canvas; w = 0: the whole canvas) */
uint32_t mm_mix(uint32_t a, uint32_t b, float t);            /* a to b: t 0..1 */
uint32_t mm_hue(uint32_t rgb, float degrees);                /* the same lightness and saturation, the hue turned */
void mm_blend(mm_canvas *c, int x, int y, uint32_t col, uint32_t alpha);   /* alpha 0..255 */
void mm_fill(mm_canvas *c, float x, float y, float w, float h, uint32_t col);
void mm_round(mm_canvas *c, float x, float y, float w, float h, float r, uint32_t col, uint32_t alpha);
void mm_ring(mm_canvas *c, float x, float y, float w, float h, float r, uint32_t col);   /* a 1 px outline */
void mm_disc(mm_canvas *c, float cx, float cy, float r, uint32_t col, uint32_t alpha);
void mm_line(mm_canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t col, uint32_t alpha);
/* a polyline through n points (xs, ys), and with fill_alpha > 0 the area between it and y = base under it */
void mm_curve(mm_canvas *c, const float *xs, const float *ys, int n, float width, uint32_t col, float base,
              uint32_t fill_alpha);

/* text: tracking in pixels between letters; x the left, y the baseline. Its width */
float mm_text(mm_canvas *c, const mm_face *f, float x, float y, const char *s, uint32_t col, float tracking);
float mm_text_w(const mm_face *f, const char *s, float tracking);
/* .. its baseline for capitals centred in a box from y, h tall */
float mm_mid(const mm_face *f, float y, float h);
