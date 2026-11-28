/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The plug-in's page in Melodee, 320 x 218 under Melodee's B1-B3: a faceplate. Each synth has its own enamel panel,
 * printed in Michroma, with knob caps over K1-K3.
 *
 *   SYNTH   the synths as panels in a rack, the lit one pulled out, a light on the one playing; Push plays the lit one
 *           (its first sound) and opens SOUNDS
 *   SOUNDS  the synth's sounds in a program window, the lit one in its middle: its factory sounds, then each SysEx
 *           file's (the bank's name at the top right); Push loads the lit one, SHIFT + encoder jumps a bank
 *   then the synth's pages, as the FM-1 has them: four values each (its pages of more split in fours), K1-K3 the first
 *   three, the encoder the fourth; a picture of what they shape (an envelope, a filter, an LFO), else the sound itself
 *
 * Minus / Plus walk the pages. On SYNTH and SOUNDS K1-K3 turn the sound's cutoff, resonance and release (those it
 * has). SHIFT + encoder on a page steps the value a pot set last, by one. A value the screen sets goes to the audio
 * thread (value_edit); the screen counts on from it until the plug-in's own copy has it. */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mm_draw.h"
#include "mm_plugin.h"
#include "mm_screen.h"

#define SW 320
#define SH 218
#define ROWS 5                                   /* SOUNDS' window */
#define NO_ID MELODEE_SCREEN_NO_CONTROL

enum { V_SYNTH, V_SOUNDS, V_PAGES };
enum { PIC_SCOPE, PIC_ENV, PIC_FILTER, PIC_LFO };

typedef struct sc_page {
    char title[CLAP_NAME_SIZE];
    clap_id ids[4];
    uint8_t n, pic;
} sc_page;

typedef struct sc_bank {
    char name[256];
    char path[4096];                             /* a SysEx file; "" the module file's factory sounds */
    uint32_t n;
    char (*names)[24];
} sc_bank;

typedef struct sc_row {
    int16_t bank;
    int32_t k;                                   /* its place in the bank */
} sc_row;

struct mm_screen {
    const mm_kind *kind;                         /* the synth its pages and sounds are */
    sc_page *page;
    uint32_t npage;
    sc_bank *bank;
    uint32_t nbank;
    sc_row *row;
    uint32_t nrow;
    clap_id tone[3];                             /* K1-K3 on SYNTH and SOUNDS */
    uint32_t view;
    int32_t tile, tile_top;                      /* SYNTH's lit tile, its first row of tiles shown */
    int32_t at, top;                             /* SOUNDS' lit row, its first row shown */
    char seen_path[4096];                        /* the sound loaded when it last looked: the list follows a new one */
    int32_t seen_index;
    clap_id ed_id;                               /* the value it set last, and to what (counted on from for a while) */
    int32_t ed_v;
    double ed_t;
    uint32_t drawn;                              /* what it drew last (a hash), and whether it drew at all */
    int drew;
};

static mm_plug *plug_at(const void *plugin) { return (mm_plug *)((const clap_plugin_t *)plugin)->plugin_data; }
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}
static int has_word(const char *s, const char *w) { return strstr(s, w) != 0; }

/* ---- what a synth shows: its pages in fours, its banks of sounds, K1-K3's three on the lists */
static void pages_free(struct mm_screen *s)
{
    for (uint32_t b = 0; b < s->nbank; b++)
        free(s->bank[b].names);
    free(s->bank);
    free(s->page);
    free(s->row);
    s->bank = 0, s->page = 0, s->row = 0;
    s->nbank = s->npage = s->nrow = 0;
}
static uint8_t page_picture(const sc_page *g)
{
    int atk = 0, dec = 0, cut = 0, rate = 0, wave = 0;
    for (uint32_t i = 0; i < g->n; i++) {
        const mm_param *p = param_find(g->ids[i]);
        if (!p)
            continue;
        atk |= has_word(p->name, "ATTACK"), dec |= has_word(p->name, "DECAY") || has_word(p->name, "RELEASE");
        cut |= has_word(p->name, "CUTOFF"), rate |= has_word(p->name, "RATE");
        wave |= has_word(p->name, "WAVE") || has_word(p->name, "SHAPE");
    }
    return atk && dec ? PIC_ENV : cut ? PIC_FILTER : rate && wave ? PIC_LFO : PIC_SCOPE;
}
static void page_add(struct mm_screen *s, const char *title, const clap_id *ids, uint32_t n)
{
    sc_page *g = &s->page[s->npage++];
    memset(g, 0, sizeof *g);
    snprintf(g->title, sizeof g->title, "%s", title);
    for (uint32_t i = 0; i < n && i < 4u; i++)
        g->ids[g->n++] = ids[i];
    g->pic = page_picture(g);
}
static void pages_make(struct mm_screen *s, const mm_kind *k)
{
    s->page = calloc(2u * k->npage + 1u, sizeof *s->page);
    if (!s->page)
        return;
    for (uint32_t i = 0; i < k->npage; i++) {
        const mm_page *g = &gpg[k->page0 + i];
        clap_id ids[CLAP_REMOTE_CONTROLS_COUNT];
        uint32_t n = 0;
        size_t tl = strlen(g->name);
        char more[CLAP_NAME_SIZE];
        for (uint32_t c = 0; c < CLAP_REMOTE_CONTROLS_COUNT; c++)
            if (g->ids[c] != CLAP_INVALID_ID)
                ids[n++] = g->ids[c];
        if (n == 8u && tl >= 3 && !strcmp(g->name + tl - 3, "LFO")) {   /* (its SYNC beside RATE, WAVE, FADE) */
            clap_id sync = ids[7];
            memmove(ids + 4, ids + 3, 4 * sizeof *ids);
            ids[3] = sync;
        }
        page_add(s, g->name, ids, n);
        if (n > 4u) {                            /* the rest: an envelope's or an LFO's depths, a page's second four */
            snprintf(more, sizeof more, "%s %s", g->name,
                     tl >= 3 && (!strcmp(g->name + tl - 3, "ENV") || !strcmp(g->name + tl - 3, "LFO")) ? "DEPTH" : "2");
            page_add(s, more, ids + 4, n - 4u);
        }
    }
}
typedef struct sc_take {
    sc_bank *b;
    const mm_schema *sc;
    uint32_t cap;
} sc_take;
static void bank_take(void *ctx, const uint8_t *patch, uint32_t len)
{
    sc_take *t = ctx;
    if (t->b->n == t->cap) {
        void *more = realloc(t->b->names, (t->cap = t->cap ? 2u * t->cap : 64u) * sizeof *t->b->names);
        if (!more)
            return;
        t->b->names = more;
    }
    patch_name(t->sc, patch, len, t->b->names[t->b->n], sizeof t->b->names[0]);
    if (!t->b->names[t->b->n][0])
        snprintf(t->b->names[t->b->n], sizeof t->b->names[0], "SOUND %u", (unsigned)t->b->n + 1u);
    t->b->n++;
}
/* a SysEx file's sounds: those a copy of the synth's module offers when given the file (as preset discovery) */
static void bank_read(sc_bank *b, const mm_kind *k)
{
    uint32_t n = 0;
    uint8_t *data = syx_read(b->path, &n);
    mm_image img;
    mm_part *t;
    sc_take take = {b, &k->sch, 0};
    if (!data)
        return;
    if (!mm_image_load(&img, k->file.aa64, k->file.aa64_len) && (t = calloc(1, sizeof *t))) {
        if (!mm_part_open(t, img.desc, &img.eng)) {
            if (img.eng.sysex)
                mm_sysex_file(t, &k->sch, data, n, bank_take, &take, 0, 0);
            mm_part_close(t);
        }
        free(t);
    }
    mm_image_free(&img);
    free(data);
}
static void banks_make(struct mm_screen *s, const mm_kind *k)
{
    char *file[512];
    uint32_t nfile = 0, cap;
    DIR *dp = sx_dir[0] && k->sch.nsysex && k->img.eng.sysex ? opendir(sx_dir) : 0;
    struct dirent *de;
    while (dp && (de = readdir(dp)) && nfile < 512u) {
        size_t n = strlen(de->d_name);
        if (de->d_name[0] != '.' && n > 4 && !strcasecmp(de->d_name + n - 4, ".syx") &&
            (file[nfile] = strdup(de->d_name)))
            nfile++;
    }
    if (dp)
        closedir(dp);
    qsort(file, nfile, sizeof file[0], name_order);
    s->bank = calloc(nfile + 1u, sizeof *s->bank);
    if (s->bank && k->file.npresets) {           /* the module file's own */
        sc_bank *b = &s->bank[s->nbank++];
        snprintf(b->name, sizeof b->name, "FACTORY");
        b->names = calloc(k->file.npresets, sizeof *b->names);
        for (uint32_t i = 0; b->names && i < k->file.npresets; i++)
            if (!mm_file_sound(&k->file, i, b->names[b->n], 0, 0, 0, 0, 0))
                b->n++;
            else
                break;
    }
    for (uint32_t f = 0; f < nfile; f++) {
        uint32_t n = 0;
        uint8_t *data;
        sc_bank *b;
        if (s->bank && strlen(sx_dir) + 1u + strlen(file[f]) < sizeof s->bank[0].path) {
            b = &s->bank[s->nbank];
            strcpy(b->path, sx_dir);             /* (its file in the SysEx folder) */
            strcat(b->path, "/");
            strcat(b->path, file[f]);
            if ((data = syx_read(b->path, &n)) && syx_owner(data, n) == k) {
                snprintf(b->name, sizeof b->name, "%.*s", (int)strlen(file[f]) - 4, file[f]);
                bank_read(b, k);
                if (b->n)
                    s->nbank++;
                else
                    free(b->names), b->names = 0;
            }
            free(data);
        }
        free(file[f]);
    }
    cap = 0;
    for (uint32_t b = 0; b < s->nbank; b++)
        cap += s->bank[b].n;
    s->row = calloc(cap ? cap : 1u, sizeof *s->row);
    for (uint32_t b = 0; s->row && b < s->nbank; b++) {
        for (uint32_t i = 0; i < s->bank[b].n; i++)
            s->row[s->nrow++] = (sc_row){(int16_t)b, (int32_t)i};
    }
}
/* K1-K3 on the lists: the sound's cutoff, resonance and (amp) release, those it has; else its first page's */
static int usable(const mm_param *p, const mm_kind *k) { return p && p->kind == k && p->max > p->min; }
static void tone_make(struct mm_screen *s, const mm_kind *k)
{
    const mm_param *found[3] = {0, 0, 0};
    for (uint32_t i = k->param0; i < k->param0 + k->nparam; i++) {
        const mm_param *p = &gp[i];
        if (!usable(p, k))
            continue;
        if (!found[0] && has_word(p->name, "CUTOFF"))
            found[0] = p;
        if (!found[1] && (has_word(p->name, "RESO") || has_word(p->name, "EMPHASIS")))
            found[1] = p;
        if (has_word(p->name, "RELEASE") && !has_word(p->name, "FILTER") &&
            (!found[2] || !strcmp(p->name, "RELEASE") || has_word(p->name, "LOUD") ||
             (strcmp(found[2]->name, "RELEASE") && !has_word(found[2]->name, "LOUD"))))
            found[2] = p;                        /* (the part's own, LOUD's, else the last) */
    }
    for (uint32_t g = 0; g < s->npage; g++)      /* (those missing: from the first pages) */
        for (uint32_t i = 0; i < s->page[g].n; i++) {
            const mm_param *p = param_find(s->page[g].ids[i]);
            int used = 0;
            for (int j = 0; j < 3; j++)
                used |= found[j] == p;
            for (int j = 0; j < 3 && !used; j++)
                if (!found[j] && usable(p, k)) {
                    found[j] = p;
                    used = 1;
                }
        }
    for (int j = 0; j < 3; j++)
        s->tone[j] = found[j] ? found[j]->id : NO_ID;
}

/* the list's place of the sound loaded, -1 none */
static int32_t row_of_loaded(const struct mm_screen *s, const mm_plug *pl)
{
    for (uint32_t r = 0; r < s->nrow; r++) {
        const sc_bank *b = &s->bank[s->row[r].bank];
        if (s->row[r].k == pl->cur_index && s->row[r].k >= 0 &&
            (b->path[0] ? !strcmp(b->path, pl->cur_path) : !strcmp(s->kind->path, pl->cur_path)))
            return (int32_t)r;
    }
    return -1;
}
static int32_t first_sound(const struct mm_screen *s)
{
    for (uint32_t r = 0; r < s->nrow; r++)
        if (s->row[r].k >= 0)
            return (int32_t)r;
    return 0;
}
static void list_show(struct mm_screen *s)       /* the lit row in view, a row of room round it where there is one */
{
    int32_t max_top = (int32_t)s->nrow - ROWS;
    if (s->at - 1 < s->top)
        s->top = s->at - 1;
    if (s->at + 2 > s->top + ROWS)
        s->top = s->at + 2 - ROWS;
    if (s->top > max_top)
        s->top = max_top;
    if (s->top < 0)
        s->top = 0;
}

/* the screen as the plug-in stands: its synth's pages and sounds made again when it plays another, the list on the
 * sound loaded when that changed */
static struct mm_screen *screen_of(mm_plug *pl)
{
    struct mm_screen *s = pl->scr;
    if (!s) {
        if (!(s = pl->scr = calloc(1, sizeof *s)))
            return 0;
        s->view = V_SOUNDS;                      /* (it opens on the sounds) */
    }
    if (s->kind != pl->kind) {
        pages_free(s);
        s->kind = pl->kind;
        pages_make(s, pl->kind);
        banks_make(s, pl->kind);
        tone_make(s, pl->kind);
        if (s->view >= V_PAGES + s->npage)
            s->view = V_SOUNDS;
        for (uint32_t i = 0; i < nkinds; i++)
            if (&kinds[i] == pl->kind)
                s->tile = (int32_t)i;
        s->seen_path[0] = 0;
        s->seen_index = -2;
        s->at = first_sound(s);
    }
    if (s->seen_index != pl->cur_index || strcmp(s->seen_path, pl->cur_path)) {
        int32_t r = row_of_loaded(s, pl);
        snprintf(s->seen_path, sizeof s->seen_path, "%s", pl->cur_path);
        s->seen_index = pl->cur_index;
        if (r >= 0)
            s->at = r;
        list_show(s);
    }
    return s;
}

/* ---- drawing: a faceplate. Each synth an enamel panel of its own colour (from its module ID: kinds_enamel), its
 * lettering printed on it (Michroma), knob caps over K1-K3 with their scales, the track's colour on their pointers */
typedef struct sc_enamel {
    uint32_t panel, ink;                         /* the enamel, its printing */
} sc_enamel;
static const sc_enamel ENAMELS[] = {
    {0x2E4A66, 0xECE8DC}, {0x2F5E58, 0xECE8DC}, {0x3A3D42, 0xECE8DC}, {0x6A2E2A, 0xECE8DC},
    {0xD9CFB8, 0x2A2622}, {0x4B5532, 0xECE8DC}, {0x4A3550, 0xECE8DC}, {0x8A6A2A, 0xF4EEDF},
};
#define NENAMEL (sizeof ENAMELS / sizeof ENAMELS[0])
static uint32_t fnv(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}
/* a synth's enamel: its module ID's, the next free one when a synth before it (by name) has that already */
static const sc_enamel *enamel_of(const mm_kind *k)
{
    uint32_t used = 0, e = 0;
    for (uint32_t i = 0; i < nkinds; i++) {
        e = fnv(kinds[i].file.id, 8) % NENAMEL;
        for (uint32_t tries = 0; tries < NENAMEL && (used >> e & 1u); tries++)
            e = (e + 1u) % NENAMEL;
        used |= 1u << e;
        if (&kinds[i] == k)
            break;
    }
    return &ENAMELS[e];
}

typedef struct sc_look {
    const sc_enamel *en;
    uint32_t dim, faint;                         /* the printing, lighter (the enamel through it) */
    uint32_t track, shade[3];                    /* the track's colour, K1-K3's pointers */
} sc_look;
static sc_look look_of(const mm_kind *k, uint32_t colour)
{
    sc_look l;
    l.en = enamel_of(k);
    l.dim = mm_mix(l.en->panel, l.en->ink, 0.72f);
    l.faint = mm_mix(l.en->panel, l.en->ink, 0.45f);
    l.track = colour & 0xFFFFFFu;
    if (!l.track)
        l.track = 0xFF7A5C;
    l.shade[0] = l.track;
    l.shade[1] = mm_hue(l.track, -24);           /* (as Melodee turns a track's knob shades round its hue) */
    l.shade[2] = mm_hue(l.track, 18);
    return l;
}
#define F11 (&MM_FACE_M11)
#define F13 (&MM_FACE_M13)
#define F15 (&MM_FACE_M15)
#define F18 (&MM_FACE_M18)
#define TRACK11 0.4f                             /* (the printing's letter spacing) */
#define COLS 3
static const float COL_X[COLS] = {53.0f, 160.0f, 267.0f};   /* K1-K3, as the pots sit under the screen */
static const float COL_W[COLS] = {78.0f, 100.0f, 78.0f};    /* .. their printing's room (the outer two: to the edge) */
#define LABEL_Y 144.0f
#define KNOB_Y 174.0f
#define VALUE_Y 212.0f

static void trim(char *t)
{
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ')
        t[--n] = 0;
}
static void upper(char *t)
{
    for (; *t; t++)
        *t = (char)toupper((unsigned char)*t);
}
static float text_c(mm_canvas *c, const mm_face *f, float cx, float y, const char *s, uint32_t col, float tr)
{
    float w = mm_text_w(f, s, tr);
    mm_text(c, f, roundf(cx - w / 2), y, s, col, tr);
    return w;
}
/* p's label on page `title`, fitting w: its name without the page's ("FILTER 1 CUTOFF" on FILTER 1: CUTOFF), else as
 * it is, else the FM-1's knob label; *two: none fits on one line, the shortest split in two at a space */
static void label_of(const mm_param *p, const char *title, float w, char *buf, size_t cap, int *two)
{
    const char *cand[3] = {0, p->name, p->sp && p->sp->label ? p->sp->label : 0};
    size_t tl = title ? strlen(title) : 0;
    const char *best = p->name;
    *two = 0;
    if (tl && !strncmp(p->name, title, tl) && p->name[tl] == ' ' && p->name[tl + 1])
        cand[0] = p->name + tl + 1;
    for (int i = 0; i < 3; i++) {
        if (!cand[i])
            continue;
        snprintf(buf, cap, "%s", cand[i]);
        upper(buf);
        if (mm_text_w(F11, buf, TRACK11) <= w)
            return;
        if (strlen(cand[i]) < strlen(best))
            best = cand[i];
    }
    snprintf(buf, cap, "%s", best);
    upper(buf);
    *two = strchr(buf, ' ') != 0;
}
static void label_draw(mm_canvas *c, float cx, float base, float w, const char *t, int two, uint32_t col)
{
    char a[CLAP_NAME_SIZE], *sp;
    if (!two) {
        text_c(c, F11, cx, base, t, col, mm_text_w(F11, t, TRACK11) <= w ? TRACK11 : 0);
        return;
    }
    snprintf(a, sizeof a, "%s", t);              /* (two lines: split at the space nearest the middle) */
    sp = 0;
    for (char *q = strchr(a, ' '); q; q = strchr(q + 1, ' '))
        if (!sp || labs((long)(q - a) - (long)strlen(a) / 2) < labs((long)(sp - a) - (long)strlen(a) / 2))
            sp = q;
    *sp = 0;
    text_c(c, F11, cx, base - 11, a, col, 0);
    text_c(c, F11, cx, base, sp + 1, col, 0);
}

static float norm(const mm_param *p, int32_t v)
{
    return p->max > p->min ? (float)(v - p->min) / (float)(p->max - p->min) : 0;
}
static float knob_angle(float n) { return (-135.0f + 270.0f * n) * 3.14159265f / 180.0f; }
/* a knob cap from above: its scale's dots (r: the cap's size), the skirt, the cap, its pointer; a pot short of its
 * value: the pointer faint and a ring on the scale where the pot sits */
static void knob(mm_canvas *c, const sc_look *lk, float cx, float cy, float r, float n, uint32_t pointer, int pot,
                 int pickup)
{
    float dots = r * 1.25f, a = knob_angle(n < 0 ? 0 : n > 1 ? 1 : n);
    for (int i = 0; i < 11; i++) {
        float d = knob_angle((float)i / 10.0f);
        mm_disc(c, cx + dots * sinf(d), cy - dots * cosf(d), r > 15 ? 1.1f : 0.9f, lk->en->ink, 180);
    }
    mm_disc(c, cx, cy, r, 0x15171A, 255);
    mm_disc(c, cx, cy, r * 0.725f, 0x2C3036, 255);
    mm_disc(c, cx, cy - r * 0.07f, r * 0.58f, 0x383D44, 255);
    mm_line(c, cx + sinf(a) * r * 0.25f, cy - cosf(a) * r * 0.25f, cx + sinf(a) * r * 0.65f, cy - cosf(a) * r * 0.65f,
            r > 15 ? 2.4f : 2.0f, pointer, pickup ? 110 : 255);
    if (pickup && pot >= 0) {
        float p = knob_angle((float)pot / 127.0f);
        mm_disc(c, cx + dots * sinf(p), cy - dots * cosf(p), 2.7f, lk->en->ink, 255);
        mm_disc(c, cx + dots * sinf(p), cy - dots * cosf(p), 1.5f, lk->en->panel, 255);
    }
}
static void check_mark(mm_canvas *c, float x, float y, uint32_t col)   /* 10 x 8 */
{
    mm_line(c, x, y + 4, x + 3.5f, y + 7.5f, 2, col, 255);
    mm_line(c, x + 3.5f, y + 7.5f, x + 10, y + 0.5f, 2, col, 255);
}
static void wheel_mark(mm_canvas *c, float cx, float cy, uint32_t col, uint32_t ground)
{                                                /* the encoder, as Melodee's Wheel glyph: a ring, a dot near its top */
    mm_disc(c, cx, cy, 5.0f, col, 255);
    mm_disc(c, cx, cy, 3.7f, ground, 255);
    mm_disc(c, cx, cy - 2.1f, 1.1f, col, 255);
}
static void sound_mark(mm_canvas *c, float x, float y, uint32_t col)   /* Melodee's Sound glyph, 10 x 10: five bars */
{
    static const float H[5] = {2, 7, 4.5f, 10, 2};
    for (int i = 0; i < 5; i++)
        mm_round(c, x + (float)i * 2.2f, y + 5 - H[i] / 2, 1.3f, H[i], 0.6f, col, 255);
}
static void screw(mm_canvas *c, float cx, float cy)
{
    mm_disc(c, cx, cy, 2.8f, 0xA6B3C0, 255);
    mm_line(c, cx - 1.6f, cy + 1.0f, cx + 1.6f, cy - 1.0f, 0.9f, 0x5E6873, 255);
}
/* a window cut in the panel: dark glass, shaded at its top, lit at its lower edge */
static void window(mm_canvas *c, float x, float y, float w, float h)
{
    mm_round(c, x, y, w, h + 1, 3, 0xFFFFFF, 34);
    mm_round(c, x, y, w, h, 3, 0x0B0F13, 255);
    mm_fill(c, x + 2, y, w - 4, 1, 0x000000);
    mm_fill(c, x + 2, y + 1, w - 4, 1, 0x05080A);
}

/* the band at the panel's top: screws, the synth's name, the sound's name (a dot once changed), what is on the right */
static void band(mm_canvas *c, mm_plug *pl, struct mm_screen *s, const sc_look *lk, const char *sound,
                 const char *right)
{
    char name[MOD3_NAME_MAX + 1];
    float x = 18, rx, rw = right ? mm_text_w(F11, right, TRACK11) : 0;
    snprintf(name, sizeof name, "%s", pl->kind->name);
    upper(name);
    screw(c, 9, 10);
    screw(c, 311, 10);
    x += mm_text(c, F15, x, 17, name, lk->en->ink, 1.0f) + 10;
    rx = SW - 18 - rw;
    if (right)
        mm_text(c, F11, rx, 15, right, lk->dim, TRACK11);
    if (sound && sound[0]) {                     /* (the sound's name where it fits, else the rule alone) */
        float w = mm_text_w(F11, sound, 0);
        if (x + w + (atomic_load(&pl->edited) ? 10 : 0) <= rx - 10) {
            x += mm_text(c, F11, x, 15, sound, lk->dim, 0) + 4;
            if (atomic_load(&pl->edited))
                mm_disc(c, x + 2, 11.5f, 2.3f, lk->track, 255);
            x += 10;
        }
    }
    if (rx - 10 - x > 12)
        mm_fill(c, x, 10, rx - 10 - x, 1, lk->faint);
    (void)s;
}
static const char *loaded_name(mm_plug *pl, struct mm_screen *s)
{
    int32_t r = s->seen_index >= 0 ? row_of_loaded(s, pl) : -1;
    return r >= 0 ? s->bank[s->row[r].bank].names[s->row[r].k] : "";
}

/* K1-K3: a label, a knob cap, the value, each over its pot */
static void knobs(mm_canvas *c, mm_plug *pl, const clap_id *ids, const char *title, const sc_look *lk,
                  const melodee_screen_frame_t *fr)
{
    for (int k = 0; k < COLS; k++) {
        const mm_param *p = ids[k] == NO_ID ? 0 : param_find(ids[k]);
        char lab[CLAP_NAME_SIZE], val[64];
        int two, pickup = fr->pots[k] >= 0 && !fr->pots_live[k];
        int32_t v;
        if (!p || p->kind != pl->kind)
            continue;                            /* (a pot with no job: no knob) */
        v = value_get(pl, p);
        label_of(p, title, COL_W[k], lab, sizeof lab, &two);
        label_draw(c, COL_X[k], LABEL_Y, COL_W[k], lab, two, lk->en->ink);
        knob(c, lk, COL_X[k], KNOB_Y, 20, norm(p, v), lk->shade[k], fr->pots[k], pickup);
        value_text(pl, p, v, val, sizeof val);
        trim(val);
        text_c(c, F11, COL_X[k], VALUE_Y, val, pickup ? lk->faint : lk->dim,
               mm_text_w(F11, val, TRACK11) <= COL_W[k] ? TRACK11 : 0);
    }
}

/* SYNTH: the synths as panels in a rack, the lit one pulled out (its maker, its sounds), a light on the one playing */
static void draw_rack(mm_canvas *c, mm_plug *pl, struct mm_screen *s, const sc_look *lk)
{
    const int32_t shown = 5;
    float y = 4;
    (void)lk;
    mm_fill(c, 0, 0, SW, SH, 0x111316);
    for (int side = 0; side < 2; side++) {       /* the rails, their holes */
        float x = side ? SW - 10 : 0;
        mm_fill(c, x, 0, 10, SH, 0x24272B);
        for (float hy = 12; hy < SH; hy += 36)
            mm_round(c, x + 3, hy, 4, 7, 2, 0x0B0C0E, 255);
    }
    if (s->tile < s->tile_top)
        s->tile_top = s->tile;
    if (s->tile >= s->tile_top + shown)
        s->tile_top = s->tile - shown + 1;
    for (int32_t i = s->tile_top; i < (int32_t)nkinds && i < s->tile_top + shown; i++) {
        const mm_kind *k = &kinds[i];
        const sc_enamel *en = enamel_of(k);
        uint32_t dim = mm_mix(en->panel, en->ink, 0.75f), faint = mm_mix(en->panel, en->ink, 0.4f);
        int lit = i == s->tile;
        float x = lit ? 8 : 14, w = lit ? 304 : 292, h = lit ? 52 : 30, nx;
        char name[MOD3_NAME_MAX + 1], count[16], vendor[MOD3_NAME_MAX + 1];
        snprintf(name, sizeof name, "%s", k->name);
        upper(name);
        snprintf(count, sizeof count, "%u", (unsigned)k->file.npresets);
        snprintf(vendor, sizeof vendor, "%s", k->file.vendor);
        upper(vendor);
        if (lit)
            mm_round(c, x, y + 3, w, h + 2, 3, 0x000000, 150);   /* (its shadow: pulled out) */
        mm_round(c, x, y, w, h, 2, en->panel, 255);
        mm_fill(c, x + 2, y, w - 4, 1, mm_mix(en->panel, 0xFFFFFF, 0.14f));
        if (lit) {
            float cw = mm_text_w(F11, count, TRACK11);
            screw(c, x + 7, y + 7), screw(c, x + w - 7, y + 7);
            screw(c, x + 7, y + h - 7), screw(c, x + w - 7, y + h - 7);
            nx = x + 16 + mm_text(c, F18, x + 16, y + 23, name, en->ink, 1.0f) + 10;
            mm_fill(c, nx, y + 15, x + w - 16 - nx, 1, faint);
            mm_text(c, F11, x + 16, y + 42, vendor, dim, TRACK11);
            mm_text(c, F11, x + w - 16 - cw, y + 42, count, dim, TRACK11);
            sound_mark(c, x + w - 16 - cw - 16, y + 33, dim);
        } else {
            float rx = x + w - 12 - (k == pl->kind ? 14 : 0), cw = mm_text_w(F11, count, TRACK11);
            nx = x + 12 + mm_text(c, F13, x + 12, y + 20, name, en->ink, 0.8f) + 10;
            mm_text(c, F11, rx - cw, y + 19, count, dim, TRACK11);
            sound_mark(c, rx - cw - 16, y + 10, dim);
            if (rx - cw - 26 > nx)
                mm_fill(c, nx, y + 15, rx - cw - 26 - nx, 1, faint);
        }
        if (k == pl->kind) {                     /* (the one playing: a light) */
            mm_disc(c, x + w - (lit ? 26 : 12) - 3, y + (lit ? 18 : h / 2), 4.6f, 0x000000, 90);
            mm_disc(c, x + w - (lit ? 26 : 12) - 3, y + (lit ? 18 : h / 2), 3.2f, lk->track, 255);
        }
        y += h + 4;
    }
}

/* SOUNDS: the program window, the lit sound in its middle, two either side; a line where a bank ends */
static void draw_sounds(mm_canvas *c, mm_plug *pl, struct mm_screen *s, const sc_look *lk)
{
    const float wx = 12, wy = 26, ww = 296, wh = 102, mid = wy + wh / 2;
    int32_t loaded = row_of_loaded(s, pl);
    window(c, wx, wy, ww, wh);
    for (int d = -2; d <= 2 && s->nrow; d++) {
        int32_t r = s->at + d;
        const sc_row *w;
        const char *name;
        float y = mid + (float)d * 19 + (d < 0 ? -2 : d > 0 ? 2 : 0);
        if (r < 0 || r >= (int32_t)s->nrow)
            continue;
        w = &s->row[r];
        name = s->bank[w->bank].names[w->k];
        if (d > -2 && r > 0 && s->row[r - 1].bank != w->bank)   /* (a bank ends above it) */
            mm_fill(c, wx + 12, y - (d == 0 ? 12.5f : 10.5f), ww - 24, 1, 0x2A3440);
        if (!d) {
            const mm_face *f = mm_text_w(F13, name, 0) <= ww - 60 ? F13 : F11;
            {
                float tx[3] = {wx + 9, wx + 16, wx + 9}, ty[3] = {y - 5.5f, y - 0.5f, y + 4.5f};
                for (float yy = ty[0]; yy <= ty[2]; yy += 0.5f) {   /* (a filled triangle) */
                    float t = yy < ty[1] ? (yy - ty[0]) / (ty[1] - ty[0]) : (ty[2] - yy) / (ty[2] - ty[1]);
                    mm_fill(c, tx[0], yy, (tx[1] - tx[0]) * t, 0.5f, lk->track);
                }
            }
            mm_text(c, f, wx + 22, y + (f == F13 ? 5 : 4), name, 0xFFFFFF, 0);
            if (r == loaded)
                check_mark(c, wx + ww - 22, y - 4.5f, lk->track);
        } else {
            uint32_t col = d == -1 || d == 1 ? 0x8C9AA8 : 0x5C6773;
            mm_text(c, F11, wx + 22, y + 4, name, col, 0);
            if (r == loaded)
                check_mark(c, wx + ww - 22, y - 4.5f, mm_mix(0x0B0F13, lk->track, 0.6f));
        }
    }
    if (!s->nrow)
        text_c(c, F11, SW / 2.0f, mid + 4, "NO SOUNDS", 0x5C6773, TRACK11);
}

/* ---- a page's picture, printed on the panel (x, y, w, h: its room) */
static int slot_of(const sc_page *g, const char *word)
{
    for (int i = 0; i < g->n; i++) {
        const mm_param *p = param_find(g->ids[i]);
        if (p && has_word(p->name, word))
            return i;
    }
    return -1;
}
static float slot_norm(mm_plug *pl, const sc_page *g, int i, float none)
{
    const mm_param *p = i < 0 ? 0 : param_find(g->ids[i]);
    return p ? norm(p, value_get(pl, p)) : none;
}
static uint32_t slot_col(const sc_look *lk, int i) { return i >= 0 && i < 3 ? lk->shade[i] : lk->en->ink; }
static void pic_env(mm_canvas *c, mm_plug *pl, const sc_page *g, const sc_look *lk, float x, float y, float w, float h)
{
    int ia = slot_of(g, "ATTACK"), id = slot_of(g, "DECAY"), is = slot_of(g, "SUSTAIN"), ir = slot_of(g, "RELEASE");
    float a = slot_norm(pl, g, ia, 0), d = slot_norm(pl, g, id, 0.3f), su = slot_norm(pl, g, is, 0),
          r = slot_norm(pl, g, ir, 0);
    float wa = w * (0.03f + 0.25f * a), wd = w * (0.03f + 0.25f * d), wr = w * (0.03f + 0.25f * r);
    float ws = w - wa - wd - wr - w * 0.02f, b = y + h, t = y + 4;
    float xs[5] = {x, x + wa, x + wa + wd, x + wa + wd + ws, x + wa + wd + ws + wr};
    float ys[5] = {b, t, t + (b - t) * (1 - su), t + (b - t) * (1 - su), b};
    int seg[3] = {ia, id, ir >= 0 ? ir : is};
    if (is < 0 && ir < 0)                        /* (an AD envelope: down to nothing) */
        ys[2] = ys[3] = b, xs[3] = xs[2] + 1, xs[4] = xs[3] + 1;
    mm_fill(c, x, b, w, 1, lk->faint);
    mm_curve(c, xs, ys, 5, 1.6f, lk->en->ink, 0, 0);
    for (int k = 1; k < 4; k++)                  /* (where each knob's part ends: its pointer's colour) */
        mm_disc(c, xs[k], ys[k], 2.8f, slot_col(lk, seg[k - 1]), 255);
}
static void pic_filter(mm_canvas *c, mm_plug *pl, const sc_page *g, const sc_look *lk, float x, float y, float w,
                       float h)
{
    int ic = slot_of(g, "CUTOFF"), iq = slot_of(g, "RESO"), kind = 0;
    float cut = slot_norm(pl, g, ic, 0.5f), q, fc, xs[120], ys[120], px;
    if (iq < 0)
        iq = slot_of(g, "EMPHASIS");
    q = 0.7f + 9.0f * slot_norm(pl, g, iq, 0);
    for (int i = 0; i < g->n; i++) {             /* the filter's type, from a named value ("LP", "BAND", "HP") */
        const mm_param *p = param_find(g->ids[i]);
        char t[64];
        if (p && p->fmt == MOD3_F_ENUM && (has_word(p->name, "TYPE") || has_word(p->name, "MODE") ||
                                           has_word(p->name, "OUTPUT"))) {
            value_text(pl, p, value_get(pl, p), t, sizeof t);
            kind = has_word(t, "HP") || has_word(t, "HIGH") ? 1 : has_word(t, "BAND") || has_word(t, "BP") ? 2 : 0;
            break;
        }
    }
    fc = 20.0f * powf(1000.0f, cut);
    for (int i = 0; i < 120; i++) {
        float f = 20.0f * powf(1000.0f, (float)i / 119.0f), r = f / fc, re = 1 - r * r, im = r / q;
        float mag = 1.0f / sqrtf(re * re + im * im), db;
        mag *= kind == 1 ? r * r : kind == 2 ? im : 1;
        db = 20.0f * log10f(mag > 1e-6f ? mag : 1e-6f);
        db = db < -36 ? -36 : db > 22 ? 22 : db;
        xs[i] = x + w * (float)i / 119.0f;
        ys[i] = y + h * (22 - db) / 58.0f;
    }
    px = x + w * cut;
    mm_fill(c, x, y + h, w, 1, lk->faint);
    for (float dy = y; dy < y + h; dy += 5)      /* (the cutoff: dashed) */
        mm_fill(c, px, dy, 1, 2, lk->faint);
    mm_curve(c, xs, ys, 120, 1.6f, lk->en->ink, 0, 0);
    {
        float pr = 1.0f / q, db = kind == 2 ? 0 : 20.0f * log10f(1.0f / pr);
        db = db < -36 ? -36 : db > 22 ? 22 : db;
        mm_disc(c, px, y + h * (22 - db) / 58.0f, 3.2f, slot_col(lk, iq >= 0 ? iq : ic), 255);
    }
}
static float wave_at(int shape, float ph)        /* -1..1 at phase 0..1 */
{
    static const float RND[8] = {0.6f, -0.3f, 0.9f, -0.8f, 0.1f, -0.5f, 0.75f, -0.1f};
    switch (shape) {
    case 1: return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    case 2: return 2 * ph - 1;
    case 3: return ph < 0.5f ? 1 : -1;
    case 4: return RND[(int)(ph * 8) & 7];
    default: return sinf(6.2831853f * ph);
    }
}
static void pic_lfo(mm_canvas *c, mm_plug *pl, const sc_page *g, const sc_look *lk, float x, float y, float w, float h)
{
    int ir = slot_of(g, "RATE"), iw = slot_of(g, "WAVE"), shape = 0;
    float cycles, xs[160], ys[160];
    if (iw < 0)
        iw = slot_of(g, "SHAPE");
    if (iw >= 0) {
        const mm_param *p = param_find(g->ids[iw]);
        char t[64];
        value_text(pl, p, value_get(pl, p), t, sizeof t);
        shape = has_word(t, "TRI") ? 1 : has_word(t, "SAW") || has_word(t, "RAMP") ? 2 :
                has_word(t, "SQ") || has_word(t, "PUL") ? 3 :
                has_word(t, "S&H") || has_word(t, "S/H") || has_word(t, "RND") || has_word(t, "RAN") ? 4 : 0;
    }
    cycles = 1.0f + 4.0f * slot_norm(pl, g, ir, 0.3f);
    mm_fill(c, x, y + h / 2, w, 1, lk->faint);
    for (int i = 0; i < 160; i++) {
        float ph = fmodf(cycles * (float)i / 159.0f, 1.0f);
        xs[i] = x + w * (float)i / 159.0f;
        ys[i] = y + h / 2 - wave_at(shape, ph) * (h / 2 - 3);
    }
    mm_curve(c, xs, ys, 160, 1.6f, lk->en->ink, 0, 0);
}
/* the sound itself, in a window: the module's last samples (15 ms) from a rising zero crossing, its level made to fill
 * the room, a graticule; only that while the part is quiet */
#define SCOPE_SPAN 660u
static void pic_scope(mm_canvas *c, mm_plug *pl, const sc_look *lk, float x, float y, float w, float h)
{
    uint32_t at = atomic_load_explicit(&pl->scope_at, memory_order_acquire), start = at - MM_SCOPE + 1u, from = start;
    float peak = 0, gain, xs[200], ys[200];
    window(c, x, y, w, h);
    x += 4, y += 4, w -= 8, h -= 8;
    for (int i = 1; i < 8; i++)
        for (float j = y; j < y + h; j += 3)
            mm_blend(c, (int)(x + w * (float)i / 8), (int)j, 0x1C252F, 255);
    for (int i = 1; i < 4; i++)
        for (float k = x; k < x + w; k += 3)
            mm_blend(c, (int)k, (int)(y + h * (float)i / 4), i == 2 ? 0x2E3A47 : 0x1C252F, 255);
    for (uint32_t i = 0; i + 1u < MM_SCOPE - SCOPE_SPAN; i++) {
        float a = pl->scope[(start + i) & (MM_SCOPE - 1u)], b = pl->scope[(start + i + 1u) & (MM_SCOPE - 1u)];
        if (a <= 0 && b > 0) {
            from = start + i;
            break;
        }
    }
    for (uint32_t i = 0; i < SCOPE_SPAN; i++) {
        float v = fabsf(pl->scope[(from + i) & (MM_SCOPE - 1u)]);
        peak = v > peak ? v : peak;
    }
    if (peak < 1e-4f)
        return;
    gain = 0.85f / (peak > 0.05f ? peak : 0.05f);
    for (int i = 0; i < 200; i++) {
        float v = pl->scope[(from + (uint32_t)i * SCOPE_SPAN / 200u) & (MM_SCOPE - 1u)] * gain;
        v = v < -1 ? -1 : v > 1 ? 1 : v;
        xs[i] = x + w * (float)i / 199.0f;
        ys[i] = y + h / 2 - v * (h / 2 - 2);
    }
    mm_curve(c, xs, ys, 200, 1.6f, lk->track, 0, 0);
}

/* a page: a section printed round it with the page's name in its line, its picture, the encoder's value as a small
 * knob at its right, K1-K3 */
static void draw_page(mm_canvas *c, mm_plug *pl, const sc_page *g, const sc_look *lk, const melodee_screen_frame_t *fr)
{
    const float bx = 10, by = 27, bw = 300, bh = 186;
    float tw = mm_text_w(F11, g->title, TRACK11), pw = g->n > 3 ? 180 : 276;
    mm_ring(c, bx, by, bw, bh, 4, lk->faint);
    mm_fill(c, bx + 10, by - 1, tw + 10, 3, lk->en->panel);
    mm_text(c, F11, bx + 15, by + 4, g->title, lk->en->ink, TRACK11);
    switch (g->pic) {
    case PIC_ENV: pic_env(c, pl, g, lk, 24, 44, pw - 4, 76); break;
    case PIC_FILTER: pic_filter(c, pl, g, lk, 22, 42, pw, 80); break;
    case PIC_LFO: pic_lfo(c, pl, g, lk, 24, 42, pw - 4, 80); break;
    default: pic_scope(c, pl, lk, 22, 40, pw, 84); break;
    }
    if (g->n > 3) {                              /* the encoder's: its mark and name, a small cap, its value */
        const mm_param *p = param_find(g->ids[3]);
        if (p && p->kind == pl->kind) {
            const float cx = 256;
            int32_t v = value_get(pl, p);
            char lab[CLAP_NAME_SIZE], val[64];
            int two;
            float lw;
            label_of(p, g->title, 78, lab, sizeof lab, &two);
            if (two) {
                label_draw(c, cx, 56, 92, lab, 1, lk->en->ink);
                wheel_mark(c, cx - 46, 40, lk->en->ink, lk->en->panel);
            } else {
                lw = mm_text_w(F11, lab, TRACK11);
                wheel_mark(c, cx - lw / 2 - 4, 52, lk->en->ink, lk->en->panel);
                mm_text(c, F11, roundf(cx - lw / 2 + 5), 56, lab, lk->en->ink, TRACK11);
            }
            knob(c, lk, cx, 84, 12.5f, norm(p, v), lk->en->ink, -1, 0);
            value_text(pl, p, v, val, sizeof val);
            trim(val);
            text_c(c, F11, cx, 118, val, lk->dim, mm_text_w(F11, val, TRACK11) <= 92 ? TRACK11 : 0);
        }
    }
    knobs(c, pl, g->ids, g->title, lk, fr);
}

/* what the page shows, as a hash: nothing new, no draw */
static uint32_t hash_in(uint32_t h, const void *d, size_t n)
{
    const uint8_t *b = d;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}
static uint32_t shown(mm_plug *pl, struct mm_screen *s, const melodee_screen_frame_t *fr)
{
    uint32_t h = 2166136261u;
    int32_t e = atomic_load(&pl->edited);
    const clap_id *ids = s->view >= V_PAGES ? s->page[s->view - V_PAGES].ids : s->tone;
    uint32_t n = s->view >= V_PAGES ? s->page[s->view - V_PAGES].n : 3u;
    h = hash_in(h, &pl->kind, sizeof pl->kind);
    h = hash_in(h, &s->view, sizeof s->view);
    h = hash_in(h, &s->tile, sizeof s->tile);
    h = hash_in(h, &s->at, sizeof s->at);
    h = hash_in(h, &s->top, sizeof s->top);
    h = hash_in(h, &pl->cur_index, sizeof pl->cur_index);
    h = hash_in(h, pl->cur_path, strlen(pl->cur_path));
    h = hash_in(h, &e, sizeof e);
    h = hash_in(h, &fr->colour, sizeof fr->colour);
    h = hash_in(h, fr->pots, sizeof fr->pots);
    h = hash_in(h, fr->pots_live, sizeof fr->pots_live);
    for (uint32_t i = 0; i < n; i++) {
        const mm_param *p = ids[i] == NO_ID ? 0 : param_find(ids[i]);
        int32_t v = p ? value_get(pl, p) : 0;
        h = hash_in(h, &v, sizeof v);
        if (p && p->fmt == MOD3_F_ENUM) {        /* (a value's text may follow another's: a page's pictures) */
            char t[64];
            value_text(pl, p, v, t, sizeof t);
            h = hash_in(h, t, strlen(t));
        }
    }
    if (s->view >= V_PAGES && s->page[s->view - V_PAGES].pic == PIC_SCOPE) {
        uint32_t at = atomic_load(&pl->scope_at);
        h = hash_in(h, &at, sizeof at);
    }
    return h;
}

static bool s_draw(const void *plugin, const melodee_screen_frame_t *fr)
{
    mm_plug *pl = plug_at(plugin);
    struct mm_screen *s;
    mm_canvas c;
    sc_look lk;
    uint32_t h;
    if (fr->area != MELODEE_SCREEN_PAGE || fr->width < SW || fr->height < SH || !(s = screen_of(pl)))
        return false;                            /* (an instrument: never a strip) */
    atomic_store(&pl->screen_on, 1);
    h = shown(pl, s, fr);
    if (s->drew && !fr->redraw_all && h == s->drawn)
        return false;
    s->drawn = h;
    s->drew = 1;
    lk = look_of(pl->kind, fr->colour);
    mm_cv_init(&c, fr->pixels, (int)fr->width, (int)fr->height, (int)fr->stride);
    if (s->view == V_SYNTH) {
        draw_rack(&c, pl, s, &lk);
        return true;
    }
    mm_fill(&c, 0, 0, (float)fr->width, (float)fr->height, lk.en->panel);
    mm_fill(&c, 0, 0, (float)fr->width, 1, mm_mix(lk.en->panel, 0xFFFFFF, 0.12f));
    if (s->view == V_SOUNDS) {
        char bank[256] = "";
        if (s->nrow) {
            snprintf(bank, sizeof bank, "%s", s->bank[s->row[s->at].bank].name);
            upper(bank);
            if (mm_text_w(F11, bank, TRACK11) > 150)
                bank[0] = 0;
        }
        band(&c, pl, s, &lk, 0, bank);
        draw_sounds(&c, pl, s, &lk);
        knobs(&c, pl, s->tone, 0, &lk, fr);
    } else {
        char place[24];
        snprintf(place, sizeof place, "%u/%u", (unsigned)s->view + 1u, (unsigned)(V_PAGES + s->npage));
        band(&c, pl, s, &lk, loaded_name(pl, s), place);
        draw_page(&c, pl, &s->page[s->view - V_PAGES], &lk, fr);
    }
    return true;
}

/* ---- inputs */
static void step_value(mm_plug *pl, struct mm_screen *s, const mm_param *p, int32_t by)
{
    double t = now_s();
    int32_t v = s->ed_id == p->id && t - s->ed_t < 0.4 ? s->ed_v : value_get(pl, p);
    v += by;
    v = v < p->min ? p->min : v > p->max ? p->max : v;
    s->ed_id = p->id, s->ed_v = v, s->ed_t = t;
    value_edit(pl, p, v);
}
static int sound_pick(mm_plug *pl, struct mm_screen *s, int32_t r)
{
    const sc_row *w = &s->row[r];
    const sc_bank *b = &s->bank[w->bank];
    return w->k >= 0 && !sound_load(pl, s->kind, b->path[0] ? b->path : 0, (uint32_t)w->k);
}
static int list_move(struct mm_screen *s, int32_t by, int bank)
{
    int32_t r = s->at;
    if (!s->nrow)
        return 0;
    while (by) {
        int32_t d = by > 0 ? 1 : -1, n = r;
        do {                                     /* (the next sound; with SHIFT the next bank's first) */
            n += d;
        } while (n >= 0 && n < (int32_t)s->nrow &&
                 (s->row[n].k < 0 || (bank && s->row[n].bank == s->row[r].bank)));
        if (n < 0 || n >= (int32_t)s->nrow)
            break;
        if (bank && d < 0)                       /* (back to that bank's first) */
            while (n > 0 && s->row[n - 1].k >= 0 && s->row[n - 1].bank == s->row[n].bank)
                n--;
        r = n;
        by -= d;
    }
    if (r == s->at)
        return 0;
    s->at = r;
    list_show(s);
    return 1;
}
static bool s_input(const void *plugin, const melodee_screen_input_t *in)
{
    mm_plug *pl = plug_at(plugin);
    struct mm_screen *s = screen_of(pl);
    int used = 0;
    if (!s)
        return false;
    switch (in->kind) {
    case MELODEE_SCREEN_STEP: {
        int32_t v = (int32_t)s->view + in->value, last = (int32_t)(V_PAGES + s->npage) - 1;
        v = v < 0 ? 0 : v > last ? last : v;
        used = v != (int32_t)s->view;
        s->view = (uint32_t)v;
        break;
    }
    case MELODEE_SCREEN_WHEEL:
        if (s->view == V_SYNTH) {
            int32_t t = s->tile + in->value;
            t = t < 0 ? 0 : t >= (int32_t)nkinds ? (int32_t)nkinds - 1 : t;
            used = t != s->tile;
            s->tile = t;
        } else if (s->view == V_SOUNDS) {
            used = list_move(s, in->value, in->shift != 0);
        } else {
            const sc_page *g = &s->page[s->view - V_PAGES];
            const mm_param *p = 0;
            if (in->shift || g->n < 4) {         /* the value a pot set last, on this page (else its first) */
                clap_id last = atomic_load(&pl->last_set);
                for (uint32_t i = 0; i < g->n && i < 3u; i++)
                    if (g->ids[i] == last)
                        p = param_find(last);
                if (!p && g->n < 4 && g->n)
                    p = param_find(g->ids[0]);
            } else {
                p = param_find(g->ids[3]);
            }
            if (p && p->kind == pl->kind) {
                step_value(pl, s, p, in->value);
                used = 1;
            }
        }
        break;
    case MELODEE_SCREEN_PUSH:
        if (s->view == V_SYNTH && s->tile < (int32_t)nkinds) {
            const mm_kind *k = &kinds[s->tile];
            if (k != pl->kind)
                sound_load(pl, k, 0, k->file.npresets ? k->file.first : 0);
            screen_of(pl);
            s->view = V_SOUNDS;
            used = 1;
        } else if (s->view == V_SOUNDS && s->nrow) {
            used = sound_pick(pl, s, s->at);
        }
        break;
    default:
        break;
    }
    if (used && pl->screen_host)
        pl->screen_host->redraw(pl->host, 0);
    return used != 0;
}

static void s_controls(const void *plugin, uint32_t ids[3])
{
    mm_plug *pl = plug_at(plugin);
    struct mm_screen *s = screen_of(pl);
    for (int k = 0; k < 3; k++) {
        const sc_page *g = s && s->view >= V_PAGES ? &s->page[s->view - V_PAGES] : 0;
        ids[k] = !s ? NO_ID : g ? (k < g->n ? g->ids[k] : NO_ID) : s->tone[k];
    }
}

const melodee_screen_t mm_screen_ext = {s_draw, s_input, s_controls};

void mm_screen_free(struct mm_screen *s)
{
    if (!s)
        return;
    pages_free(s);
    free(s);
}
