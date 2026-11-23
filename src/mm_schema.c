/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#include "mm_schema.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }

/* the string at off in the schema's strings, 0: none or not inside them */
static const char *str_at(const uint8_t *s, const mod_sch_head *h, uint32_t len, uint32_t off)
{
    uint32_t at = h->str + off, end = h->str + h->nstr;
    if (off == MOD_SCH_NONE || end > len || at >= end || !memchr(s + at, 0, end - at))
        return 0;
    return (const char *)s + at;
}

int mm_schema_read(mm_schema *sc, const mm_file *f)
{
    const uint8_t *s = f->schm;
    uint32_t len = f->schm_len, i, k;
    mod_sch_head h;
    memset(sc, 0, sizeof *sc);
    if (!s || !f->patch_len)
        return 0;
    memcpy(&h, s, sizeof h);
    if (h.version != MOD_SCH_VERSION || h.param + (uint32_t)h.nparam * MOD_SCH_PREC > len ||
        h.page + (uint32_t)h.npage * MOD_SCH_GREC > len)
        return -1;
    sc->flags = h.flags;
    sc->param = calloc(h.nparam ? h.nparam : 1u, sizeof *sc->param);
    sc->page = calloc(h.npage ? 4u * h.npage : 1u, sizeof *sc->page);   /* (room for instances: grown below) */
    if (!sc->param || !sc->page) {
        mm_schema_free(sc);
        return -1;
    }
    for (i = 0; i < h.nparam; i++) {
        const uint8_t *b = s + h.param + i * MOD_SCH_PREC;
        mm_sch_param *p = &sc->param[sc->nparam];
        p->id = (uint16_t)le16(b);
        p->kind = b[2];
        p->fmt = b[3];
        p->min = (int16_t)le16(b + 4);
        p->max = (int16_t)le16(b + 6);
        p->def = (int16_t)le16(b + 8);
        p->off = (uint16_t)le16(b + 10);
        p->shift = b[12];
        p->width = b[13];
        p->flags = b[14];
        p->flags2 = b[15];
        p->label = str_at(s, &h, len, le16(b + 16));
        p->name = str_at(s, &h, len, le16(b + 18));
        p->unit = str_at(s, &h, len, le16(b + 22));
        if (p->max < p->min || (sc->nparam && p->id <= sc->param[sc->nparam - 1u].id) ||
            (p->kind == MOD3_SK_FIELD && (!p->width || p->width > 16u || p->shift > 7u ||
                                         p->off + (p->shift + p->width + 7u) / 8u > f->patch_len)) ||
            (p->kind == MOD3_SK_TRACK && p->off >= MOD3_P_COUNT) || p->kind > MOD3_SK_CALL) {
            mm_schema_free(sc);
            return -1;
        }
        if (p->fmt == MOD3_F_ENUM) {             /* the names one after another */
            uint32_t n = (uint32_t)(p->max - p->min + 1), off = le16(b + 20);
            const char *q = str_at(s, &h, len, off);
            if (q) {
                p->names = calloc(n, sizeof *p->names);
                if (!p->names) {
                    mm_schema_free(sc);
                    return -1;
                }
                for (k = 0; k < n && q; k++) {
                    p->names[k] = q;
                    off += (uint32_t)strlen(q) + 1u;
                    q = str_at(s, &h, len, off);
                }
                if (k < n) {
                    free(p->names);
                    p->names = 0;
                }
            }
        }
        sc->nparam++;
    }
    for (i = 0, k = 4u * h.npage; i < h.npage; i++) {
        const uint8_t *b = s + h.page + i * MOD_SCH_GREC;
        const char *title = str_at(s, &h, len, le16(b)), *sec = str_at(s, &h, len, le16(b + 22));
        uint32_t widget = b[2], flags = b[3], ninst = (flags & MOD3_PG_INST) ? b[12] : 1u, chip = b[13], j, c;
        const char *iname = (flags & MOD3_PG_INST) ? str_at(s, &h, len, le16(b + 18)) : 0;
        uint32_t ioff = le16(b + 18);
        if (widget == MOD3_W_STORE || widget == MOD3_W_TOOLS || (flags & MOD3_PG_BANK) || !ninst)
            continue;
        if (title && chip < strlen(title))       /* its short name: the title past the prefix (chip) */
            title += chip;
        for (j = 0; j < ninst; j++) {
            mm_sch_page *g;
            if (sc->npage == k) {                /* (more instances than the room) */
                mm_sch_page *more = realloc(sc->page, 2u * k * sizeof *more);
                if (!more)
                    break;
                sc->page = more;
                k *= 2u;
            }
            g = &sc->page[sc->npage];
            memset(g, 0, sizeof *g);
            snprintf(g->title, sizeof g->title, "%s%s%s", title ? title : "", iname ? " " : "", iname ? iname : "");
            snprintf(g->section, sizeof g->section, "%s", sec ? sec : "");
            g->flags = (uint8_t)flags;
            for (c = 0; c < 4u; c++) {
                uint32_t id = le16(b + 4 + 2 * c);
                g->id[c] = id == MOD_SCH_NONE ? MOD_SCH_NONE : (uint16_t)(id + (int32_t)j * (int8_t)b[14 + c]);
            }
            sc->npage++;
            if (iname) {                         /* the next instance's name */
                ioff += (uint32_t)strlen(iname) + 1u;
                iname = str_at(s, &h, len, ioff);
            }
        }
    }
    if (h.nlen && h.name != MOD_SCH_NONE && (uint32_t)h.name + h.nlen <= f->patch_len)
        sc->name = h.name, sc->nlen = h.nlen;
    for (i = 0; i < h.nrole && h.role + (i + 1u) * MOD_SCH_RREC <= len; i++) {   /* (the SysEx's patterns) */
        const uint8_t *b = s + h.role + i * MOD_SCH_RREC;
        uint32_t at = h.wid + le16(b + 2);
        if (b[0] != MOD3_R_SYSEX)
            continue;
        for (k = 0; k < b[1] && sc->nsysex < MM_SCH_NSYSEX && at + 4u * (k + 1u) <= len; k++) {
            const uint8_t *q = s + at + 4u * k;
            if (q[0] >= 1u && q[0] <= 3u)
                memcpy(sc->sysex[sc->nsysex++], q, 4);
        }
    }
    return 0;
}

int mm_schema_sysex(const mm_schema *s, const uint8_t *f, uint32_t n)
{
    for (uint32_t i = 0; i < s->nsysex; i++)
        if (n >= s->sysex[i][0] + 2u && f[0] == 0xF0u && !memcmp(f + 1, s->sysex[i] + 1, s->sysex[i][0]))
            return 1;
    return 0;
}

uint32_t mm_sysex_file(mm_part *t, const mm_schema *s, const uint8_t *data, uint32_t n, mm_sx_take take, void *ctx,
                       char *msg, uint32_t cap)
{
    uint32_t got = 0, i = 0, j;
    char m[64];
    if (msg && cap)
        msg[0] = 0;
    while (i < n) {
        if (data[i] != 0xF0u) {
            i++;
            continue;
        }
        for (j = i + 1u; j < n && data[j] < 0x80u; j++)
            ;
        if (j < n && data[j] == 0xF7u) {
            if (mm_schema_sysex(s, data + i, j + 1u - i)) {
                got += mm_sysex(t, data + i, j + 1u - i, take, ctx, m, sizeof m);
                if (m[0] && msg && cap)
                    snprintf(msg, cap, "%s", m);
            }
            j++;
        }
        i = j;                                   /* (a frame another status broke: from that status on) */
    }
    return got;
}

void mm_schema_free(mm_schema *s)
{
    for (uint32_t i = 0; s->param && i < s->nparam; i++)
        free(s->param[i].names);
    free(s->param);
    free(s->page);
    memset(s, 0, sizeof *s);
}

const mm_sch_param *mm_schema_find(const mm_schema *s, uint32_t id)
{
    uint32_t lo = 0, hi = s->nparam;
    while (lo < hi) {                            /* (ids ascending) */
        uint32_t mid = (lo + hi) / 2u;
        if (s->param[mid].id == id)
            return &s->param[mid];
        if (s->param[mid].id < id)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return 0;
}

static int32_t field_get(const uint8_t *patch, const mm_sch_param *p)
{
    uint32_t w = 0, nb = (p->shift + p->width + 7u) / 8u, i;
    for (i = 0; i < nb; i++)
        w |= (uint32_t)patch[p->off + i] << (8u * i);
    w = (w >> p->shift) & ((1u << p->width) - 1u);
    if ((p->flags & MOD3_SF_SIGNED) && (w >> (p->width - 1u)))
        return (int32_t)w - (int32_t)(1u << p->width);
    return (int32_t)w;
}
static void field_put(uint8_t *patch, const mm_sch_param *p, int32_t v)
{
    uint32_t nb = (p->shift + p->width + 7u) / 8u, w = 0, m = ((1u << p->width) - 1u) << p->shift, i;
    for (i = 0; i < nb; i++)
        w |= (uint32_t)patch[p->off + i] << (8u * i);
    w = (w & ~m) | (((uint32_t)v << p->shift) & m);
    for (i = 0; i < nb; i++)
        patch[p->off + i] = (uint8_t)(w >> (8u * i));
}

/* the module's descriptor of p now (MOD3_SF_DESC), 0 none */
static const mod_param_v3 *desc_of(mm_part *t, const mm_sch_param *p)
{
    if (!(p->flags & MOD3_SF_DESC) || !t->e->desc)
        return 0;
    mm_enter(t);
    return t->e->desc(&t->h, MOD3_DESC_ID + p->id);
}

int32_t mm_sch_get(mm_part *t, const uint8_t *patch, const mm_sch_param *p)
{
    switch (p->kind) {
    case MOD3_SK_FIELD:
        return field_get(patch, p);
    case MOD3_SK_TRACK:
        return t->h.p[p->off];
    default:
        if (!t->e->pget)
            return 0;
        mm_enter(t);
        return t->e->pget(&t->h, patch, p->id);
    }
}

int mm_sch_put(mm_part *t, uint8_t *patch, const mm_sch_param *p, int32_t v)
{
    const mod_param_v3 *d = desc_of(t, p);
    int32_t lo = d ? d->min : p->min, hi = d ? d->max : p->max, old;
    v = v < lo ? lo : v > hi ? hi : v;
    switch (p->kind) {
    case MOD3_SK_FIELD:
        old = field_get(patch, p);
        field_put(patch, p, v);
        return old != v;
    case MOD3_SK_TRACK:
        if (t->h.p[p->off] == v)
            return 0;
        t->h.p[p->off] = (int16_t)v;
        return 1;
    default:
        if (!t->e->pput)
            return 0;
        mm_enter(t);
        return t->e->pput(&t->h, patch, p->id, v) != 0;
    }
}

void mm_sch_text(mm_part *t, const uint8_t *patch, const mm_sch_param *p, int32_t v, char *buf, uint32_t cap)
{
    const mod_param_v3 *d;
    if ((p->flags & MOD3_SF_FORMAT) && t->e->pformat) {   /* the module's own text */
        char val[8] = "";
        const char *unit = 0;
        mm_enter(t);
        t->e->pformat(&t->h, patch, p->id, v, val, &unit);
        val[7] = 0;
        snprintf(buf, cap, unit && *unit ? "%s %s" : "%s", val, unit ? unit : "");
        return;
    }
    if ((d = desc_of(t, p))) {                   /* its descriptor now: its format and names */
        mm_format(d->fmt, v, d->min, d->max, d->names, d->unit, buf, cap);
        return;
    }
    mm_format(p->fmt, v, p->min, p->max, (const char *const *)p->names, p->unit, buf, cap);
}
