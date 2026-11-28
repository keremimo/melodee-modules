/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#include "mm_draw.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void mm_cv_init(mm_canvas *c, uint32_t *px, int w, int h, int stride)
{
    c->px = px;
    c->w = w;
    c->h = h;
    c->stride = stride;
    mm_cv_clip(c, 0, 0, 0, 0);
}
void mm_cv_clip(mm_canvas *c, int x, int y, int w, int h)
{
    if (!w) {
        c->cx0 = c->cy0 = 0, c->cx1 = c->w, c->cy1 = c->h;
        return;
    }
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x + w > c->w ? c->w : x + w;
    c->cy1 = y + h > c->h ? c->h : y + h;
}

uint32_t mm_mix(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    for (int s = 0; s < 24; s += 8) {
        float x = (float)(a >> s & 255u), y = (float)(b >> s & 255u);
        out |= (uint32_t)lroundf(x + (y - x) * t) << s;
    }
    return out;
}

uint32_t mm_hue(uint32_t rgb, float degrees)
{
    float r = (float)(rgb >> 16 & 255u) / 255.0f, g = (float)(rgb >> 8 & 255u) / 255.0f;
    float b = (float)(rgb & 255u) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), l = (mx + mn) / 2, s = 0, h = 0, d = mx - mn;
    float q, p, hh, t[3];
    uint32_t out = 0;
    if (d > 1e-6f) {
        s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
        h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h /= 6;
    }
    h = fmodf(h + degrees / 360.0f + 1.0f, 1.0f);
    q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    p = 2 * l - q;
    t[0] = h + 1.0f / 3, t[1] = h, t[2] = h - 1.0f / 3;
    for (int i = 0; i < 3; i++) {
        float x = t[i] < 0 ? t[i] + 1 : t[i] > 1 ? t[i] - 1 : t[i], v;
        hh = x;
        v = hh < 1.0f / 6 ? p + (q - p) * 6 * hh : hh < 0.5f ? q
          : hh < 2.0f / 3 ? p + (q - p) * (2.0f / 3 - hh) * 6 : p;
        out = out << 8 | (uint32_t)lroundf(fminf(1, fmaxf(0, v)) * 255);
    }
    return out;
}

void mm_blend(mm_canvas *c, int x, int y, uint32_t col, uint32_t alpha)
{
    uint32_t *p, d, a;
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1 || !alpha)
        return;
    p = &c->px[y * c->stride + x];
    if (alpha >= 255) {
        *p = col;
        return;
    }
    d = *p;
    a = alpha;
    *p = ((((col >> 16 & 255u) * a + (d >> 16 & 255u) * (255u - a)) / 255u) << 16) |
         ((((col >> 8 & 255u) * a + (d >> 8 & 255u) * (255u - a)) / 255u) << 8) |
         (((col & 255u) * a + (d & 255u) * (255u - a)) / 255u);
}

/* a box's coverage of pixel column/row [i, i+1): its share, 0..1 */
static float cover(float lo, float hi, int i)
{
    float a = lo > (float)i ? lo : (float)i, b = hi < (float)(i + 1) ? hi : (float)(i + 1);
    return b > a ? b - a : 0;
}

void mm_fill(mm_canvas *c, float x, float y, float w, float h, uint32_t col)
{
    int x0 = (int)floorf(x), x1 = (int)ceilf(x + w), y0 = (int)floorf(y), y1 = (int)ceilf(y + h);
    for (int j = y0; j < y1; j++) {
        float cy = cover(y, y + h, j);
        for (int i = x0; i < x1; i++)
            mm_blend(c, i, j, col, (uint32_t)lroundf(255 * cy * cover(x, x + w, i)));
    }
}

/* a rounded box's coverage at a pixel's centre (px, py): soft over one pixel */
static float round_cover(float x, float y, float w, float h, float r, float px, float py)
{
    float qx = fabsf(px - (x + w / 2)) - (w / 2 - r), qy = fabsf(py - (y + h / 2)) - (h / 2 - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;   /* signed distance to the edge */
    return d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d;
}

void mm_round(mm_canvas *c, float x, float y, float w, float h, float r, uint32_t col, uint32_t alpha)
{
    int x0 = (int)floorf(x), x1 = (int)ceilf(x + w), y0 = (int)floorf(y), y1 = (int)ceilf(y + h);
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int j = y0; j < y1; j++) {
        float py = (float)j + 0.5f, cy = cover(y, y + h, j);
        int corner_row = py < y + r + 1 || py > y + h - r - 1;
        for (int i = x0; i < x1; i++) {
            float px = (float)i + 0.5f, k;
            if (corner_row && (px < x + r + 1 || px > x + w - r - 1))
                k = round_cover(x, y, w, h, r, px, py);
            else
                k = cy * cover(x, x + w, i);
            if (k > 0)
                mm_blend(c, i, j, col, (uint32_t)lroundf((float)alpha * k));
        }
    }
}

void mm_ring(mm_canvas *c, float x, float y, float w, float h, float r, uint32_t col)
{
    int x0 = (int)floorf(x), x1 = (int)ceilf(x + w), y0 = (int)floorf(y), y1 = (int)ceilf(y + h);
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            float k = round_cover(x, y, w, h, r, px, py) - round_cover(x + 1, y + 1, w - 2, h - 2, r - 1, px, py);
            if (k > 0)
                mm_blend(c, i, j, col, (uint32_t)lroundf(255 * k));
        }
}

void mm_disc(mm_canvas *c, float cx, float cy, float r, uint32_t col, uint32_t alpha)
{
    for (int j = (int)floorf(cy - r - 1); j <= (int)ceilf(cy + r + 1); j++)
        for (int i = (int)floorf(cx - r - 1); i <= (int)ceilf(cx + r + 1); i++) {
            float dx = (float)i + 0.5f - cx, dy = (float)j + 0.5f - cy, d = sqrtf(dx * dx + dy * dy) - r;
            float k = d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d;
            if (k > 0)
                mm_blend(c, i, j, col, (uint32_t)lroundf((float)alpha * k));
        }
}

/* a segment's distance from (px, py) */
static float seg_dist(float x0, float y0, float x1, float y1, float px, float py)
{
    float vx = x1 - x0, vy = y1 - y0, wx = px - x0, wy = py - y0, l = vx * vx + vy * vy;
    float t = l > 0 ? (wx * vx + wy * vy) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    wx = px - (x0 + t * vx), wy = py - (y0 + t * vy);
    return sqrtf(wx * wx + wy * wy);
}

void mm_line(mm_canvas *c, float x0, float y0, float x1, float y1, float width, uint32_t col, uint32_t alpha)
{
    float hw = width / 2;
    int i0 = (int)floorf(fminf(x0, x1) - hw - 1), i1 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int j0 = (int)floorf(fminf(y0, y1) - hw - 1), j1 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            float d = seg_dist(x0, y0, x1, y1, (float)i + 0.5f, (float)j + 0.5f) - hw;
            float k = d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d;
            if (k > 0)
                mm_blend(c, i, j, col, (uint32_t)lroundf((float)alpha * k));
        }
}

/* the curve as one shape: each pixel near it takes the most any segment covers it, once (no darker joints) */
void mm_curve(mm_canvas *c, const float *xs, const float *ys, int n, float width, uint32_t col, float base,
              uint32_t fill_alpha)
{
    float hw = width / 2, minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int bx, by, bw, bh;
    uint8_t *cov;
    if (n < 2)
        return;
    for (int k = 0; k < n; k++) {
        minx = fminf(minx, xs[k]), maxx = fmaxf(maxx, xs[k]);
        miny = fminf(miny, ys[k]), maxy = fmaxf(maxy, ys[k]);
    }
    if (fill_alpha) {                            /* (under it: column by column, xs rising) */
        for (int k = 0; k + 1 < n; k++)
            for (int i = (int)floorf(xs[k]); i < (int)ceilf(xs[k + 1]); i++) {
                float t = xs[k + 1] > xs[k] ? ((float)i + 0.5f - xs[k]) / (xs[k + 1] - xs[k]) : 0, top;
                if (t < 0 || t > 1)
                    continue;
                top = ys[k] + (ys[k + 1] - ys[k]) * t;
                for (int j = (int)floorf(top); j < (int)ceilf(base); j++)
                    mm_blend(c, i, j, col, (uint32_t)lroundf((float)fill_alpha * cover(top, base, j)));
            }
    }
    if (width <= 0)
        return;
    bx = (int)floorf(minx - hw - 1), by = (int)floorf(miny - hw - 1);
    bw = (int)ceilf(maxx + hw + 1) - bx + 1, bh = (int)ceilf(maxy + hw + 1) - by + 1;
    if (!(cov = calloc((size_t)bw * (size_t)bh, 1)))
        return;
    for (int s = 0; s + 1 < n; s++) {
        int i0 = (int)floorf(fminf(xs[s], xs[s + 1]) - hw - 1), i1 = (int)ceilf(fmaxf(xs[s], xs[s + 1]) + hw + 1);
        int j0 = (int)floorf(fminf(ys[s], ys[s + 1]) - hw - 1), j1 = (int)ceilf(fmaxf(ys[s], ys[s + 1]) + hw + 1);
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                float d = seg_dist(xs[s], ys[s], xs[s + 1], ys[s + 1], (float)i + 0.5f, (float)j + 0.5f) - hw;
                uint8_t k = (uint8_t)(d <= -0.5f ? 255 : d >= 0.5f ? 0 : lroundf(255 * (0.5f - d)));
                uint8_t *at = &cov[(size_t)(j - by) * (size_t)bw + (size_t)(i - bx)];
                if (k > *at)
                    *at = k;
            }
    }
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++)
            if (cov[(size_t)j * (size_t)bw + (size_t)i])
                mm_blend(c, bx + i, by + j, col, cov[(size_t)j * (size_t)bw + (size_t)i]);
    free(cov);
}

static const mm_glyph *glyph(const mm_face *f, const char **s)
{
    unsigned char ch = (unsigned char)**s;
    if (ch == 0xC2 && (unsigned char)(*s)[1] == 0xB7) {   /* the middle dot, UTF-8 */
        (*s)++;
        return &f->g[95];
    }
    return ch >= 32 && ch <= 126 ? &f->g[ch - 32] : &f->g['?' - 32];
}

float mm_text(mm_canvas *c, const mm_face *f, float x, float y, const char *s, uint32_t col, float tracking)
{
    float pen = x;
    int by = (int)lroundf(y);
    for (; *s; s++) {
        const mm_glyph *g = glyph(f, &s);
        int gx = (int)lroundf(pen) + g->x, gy = by + g->y;
        const uint8_t *b = f->bits + g->at;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++)
                if (b[j * g->w + i])
                    mm_blend(c, gx + i, gy + j, col, b[j * g->w + i]);
        pen += (float)g->adv / 64.0f + (s[1] ? tracking : 0);
    }
    return pen - x;
}

float mm_text_w(const mm_face *f, const char *s, float tracking)
{
    float w = 0;
    for (; *s; s++)
        w += (float)glyph(f, &s)->adv / 64.0f + (s[1] ? tracking : 0);
    return w;
}

float mm_mid(const mm_face *f, float y, float h) { return roundf(y + (h + (float)f->cap) / 2); }
