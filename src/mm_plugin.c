/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The CLAP side: one plug-in that plays the synth module files (.mmod) in the Modules folder beside it. Its presets
 * pick the synth: preset discovery gives each module file a location (its factory sounds), and each SysEx file (.syx)
 * in the SysEx folder beside the plug-in a location under the synth whose module takes it, named "<synth>/<file>"
 * (the synth's own then "<synth>/Factory"). In Melodee's PRESETS that is a folder for each synth, with a folder for
 * each of its SysEx files in it. Loading a sound of another synth loads that synth.
 * An instance maps the code of the synth it plays (mm_image.h), a copy of its own. Another synth's code is mapped on
 * the main thread and given to the audio thread at its next block, which gives the old one back to be freed.
 * Its parameters are every synth's, as a CLAP plug-in's list stays the same while it runs (ids: the synth's tag
 * above MM_TAG_SHIFT, the synth's own id below), with no module (group), so a host shows the pages: those of the
 * synth it plays (remote controls: the schema's, then the part's). Its state: the synth (its module's ID), the part's
 * values and patch. Notes come as CLAP notes or MIDI; the wheels and the pedal as MIDI; pressure as MIDI (poly or
 * channel) or CLAP's pressure expression.
 * The module plays at 44.1 kHz (mm_resample.h at other rates). */
#include <clap/clap.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm_engine.h"
#include "mm_file.h"
#include "mm_image.h"
#include "mm_resample.h"
#include "mm_schema.h"
#include "mm_plugin.h"
#include "mm_screen.h"
#include "mm_watch.h"
#include <pthread.h>

#define MM_PLUGIN_ID "studio.ellic.melodee-modules"
#define MM_VENDOR "Ellic Studio"
#define MM_VERSION "0.2.0"
#define MM_P_OWN0 1000u
#define MM_P_BEND (MM_P_OWN0 + MM_OWN_BEND)      /* the wheel's range (semitones) */
#define MM_P_VEL (MM_P_OWN0 + MM_OWN_VEL)        /* how far the level follows velocity */
#define MM_P_ATFLT (MM_P_OWN0 + MM_OWN_ATFLT)    /* pressure (each key's) to the filter */
static const int16_t OWN_DEF[MM_NOWN] = {2, MM_VEL_DEF, MM_ATFLT_DEF};
/* Melodee's synths whose own envelopes follow velocity already (SID's chip, FM6's operators' key sensitivity), by ID:
 * VEL starts at 0 there, so they play velocity as the FM-1 plays them */
static const char *const VEL_OWN[] = {"8757cdd57f9158d4" /* SID */, "918284c33909e71a" /* FM6 */};
static int32_t *own_of(mm_part *t, uint32_t k)
{
    return k == MM_OWN_BEND ? &t->bend_semis : k == MM_OWN_VEL ? &t->vel_amp : &t->at_flt;
}
#define MM_STATE_MAGIC 0x444F4D4Du               /* "MMOD" */
#define MM_STATE_VERSION 3u                      /* 3: VEL and AT FLT after the head (2: none, the defaults) */
#define MM_SCH_ID0 0x10000u                     /* a schema parameter's own id: this + its schema id */
#define MM_TAG_SHIFT 18u                         /* a parameter's CLAP id: its synth's tag << this | its own id */
#define MM_TAG_MAX 0x3FFEu                       /* (tags 1..this: no id is CLAP_INVALID_ID) */

mm_kind *kinds;
uint32_t nkinds;

static int kind_order(const void *a, const void *b)
{
    return strcmp(((const mm_kind *)a)->name, ((const mm_kind *)b)->name);
}
static void kind_fill(mm_kind *k)
{
    for (int i = 0; i < 8; i++)                  /* the ID as mod_build.py --check prints it */
        snprintf(k->idhex + 2 * i, 3, "%02x", k->file.id[7 - i]);
    snprintf(k->name, sizeof k->name, "%s", k->file.name);
}
static const mm_kind *kind_by_hex(const char *hex, size_t n)
{
    for (uint32_t i = 0; i < nkinds; i++)
        if (n == strlen(kinds[i].idhex) && !strncmp(hex, kinds[i].idhex, n))
            return &kinds[i];
    return 0;
}
static uint32_t id_hash(const uint8_t *id)       /* (FNV-1a) */
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++)
        h = (h ^ id[i]) * 16777619u;
    return h;
}

/* every .mmod in dir (the Modules folder beside the plug-in): one synth each, the newest file of a module, those
 * whose code maps */
static void kinds_scan(const char *dir)
{
    DIR *dp = opendir(dir);
    struct dirent *de;
    uint32_t cap = 0;
    if (!dp)
        return;
    while ((de = readdir(dp))) {
        char path[4096];
        size_t n = strlen(de->d_name);
        mm_file f;
        const char *err;
        uint32_t i;
        if (de->d_name[0] == '.' || n < 6 || strcasecmp(de->d_name + n - 5, ".mmod"))
            continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, de->d_name) >= (int)sizeof path)
            continue;                            /* (a path too long) */
        if ((err = mm_file_read(&f, path))) {
            fprintf(stderr, "melodee-modules: %s: %s\n", de->d_name, err);
            continue;
        }
        for (i = 0; i < nkinds && memcmp(kinds[i].file.id, f.id, 8); i++)
            ;
        if (i < nkinds) {                        /* the same module twice: the newer version */
            if (f.head.version > kinds[i].file.head.version) {
                mm_file_free(&kinds[i].file);
                kinds[i].file = f;
                snprintf(kinds[i].path, sizeof kinds[i].path, "%s", path);
            } else {
                mm_file_free(&f);
            }
            continue;
        }
        if (nkinds == cap) {
            mm_kind *more = realloc(kinds, (cap = cap ? 2u * cap : 8u) * sizeof *kinds);
            if (!more) {
                mm_file_free(&f);
                break;
            }
            kinds = more;
        }
        memset(&kinds[nkinds], 0, sizeof kinds[nkinds]);
        kinds[nkinds].file = f;
        snprintf(kinds[nkinds++].path, sizeof kinds[0].path, "%s", path);
    }
    closedir(dp);
    for (uint32_t i = 0; i < nkinds;) {
        mm_kind *k = &kinds[i];
        const char *err;
        kind_fill(k);
        memcpy(k->own_def, OWN_DEF, sizeof k->own_def);
        for (uint32_t v = 0; v < sizeof VEL_OWN / sizeof VEL_OWN[0]; v++)
            if (!strcmp(k->idhex, VEL_OWN[v]))
                k->own_def[MM_OWN_VEL] = 0;
        if (mm_schema_read(&k->sch, &k->file))
            fprintf(stderr, "melodee-modules: %s: a schema this plug-in cannot read (E1..E8 only)\n", k->name);
        if ((err = mm_image_load(&k->img, k->file.aa64, k->file.aa64_len))) {
            fprintf(stderr, "melodee-modules: %s: %s\n", k->name, err);
            mm_image_free(&k->img);
            mm_schema_free(&k->sch);
            mm_file_free(&k->file);
            kinds[i] = kinds[--nkinds];
            continue;
        }
        if (!realpath(k->path, k->real))
            snprintf(k->real, sizeof k->real, "%s", k->path);
        i++;
    }
    qsort(kinds, nkinds, sizeof *kinds, kind_order);
    for (uint32_t i = 0; i < nkinds; i++) {      /* (a tag another synth has: the next free one) */
        uint32_t t = id_hash(kinds[i].file.id) % MM_TAG_MAX + 1u, used = 1;
        while (used) {
            used = 0;
            for (uint32_t k = 0; k < i && !used; k++)
                used = kinds[k].tag == t;
            if (used)
                t = t % MM_TAG_MAX + 1u;
        }
        kinds[i].tag = t;
    }
}

/* ---- the parameters and pages of every synth */


mm_param *gp;                                    /* the synths' parameters, synth by synth */
uint32_t ngp;
static uint32_t cgp;
mm_page *gpg;                                    /* .. their pages */
static uint32_t ngpg, cgpg;
static uint32_t *gsort;                          /* gp's places by id */

/* the last word of a name made long where Melodee's pages look for an envelope or a filter picture ("RES": "RESO") */
static void long_words(char *name, size_t cap)
{
    static const char *const MAP[][2] = {{"CUT", "CUTOFF"}, {"RES", "RESO"}, {"EMPH", "EMPHASIS"}, {"ATK", "ATTACK"},
                                         {"DEC", "DECAY"}, {"SUS", "SUSTAIN"}, {"REL", "RELEASE"}};
    char *w = strrchr(name, ' ');
    w = w ? w + 1 : name;
    for (uint32_t i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (!strcmp(w, MAP[i][0]) && (size_t)(w - name) + strlen(MAP[i][1]) < cap) {
            strcpy(w, MAP[i][1]);
            return;
        }
}

const mm_param *param_find(clap_id id)
{
    uint32_t lo = 0, hi = ngp;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;
        if (gp[gsort[mid]].id < id)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < ngp && gp[gsort[lo]].id == id ? &gp[gsort[lo]] : 0;
}
static int kind_has(const mm_kind *k, uint32_t local)   /* (while its tables are made) */
{
    for (uint32_t i = k->param0; i < ngp; i++)
        if (gp[i].local == local)
            return 1;
    return 0;
}
static mm_param *param_add(mm_kind *k, uint32_t local, uint8_t src, const char *name, uint8_t fmt, int16_t min,
                           int16_t max, int16_t def, const char *const *names, const char *unit)
{
    mm_param *p;
    if (kind_has(k, local) || ngp >= cgp)
        return 0;
    p = &gp[ngp++];
    k->nparam++;
    memset(p, 0, sizeof *p);
    p->id = k->tag << MM_TAG_SHIFT | local;
    p->local = local;
    p->kind = k;
    p->src = src;
    snprintf(p->name, sizeof p->name, "%s", name);
    long_words(p->name, sizeof p->name);
    p->fmt = fmt;
    p->min = min;
    p->max = max;
    p->def = def;
    p->names = names;
    p->unit = unit;
    return p;
}
/* a schema parameter (once): its long name, else its label after its page's title (a second page's "1B": "1"), or
 * the label alone where it names the title already ("RING A" on RING, "OSC 1>F2" on TO F2) */
static void param_add_sch(mm_kind *k, const mm_sch_param *sp, const char *title)
{
    char name[CLAP_NAME_SIZE], pre[48] = "";
    const char *label = sp->label ? sp->label : "?", *last;
    size_t n;
    mm_param *p;
    if (title) {
        snprintf(pre, sizeof pre, "%s", title);
        n = strlen(pre);
        if (n >= 2 && pre[n - 1] >= 'A' && pre[n - 1] <= 'Z' && pre[n - 2] >= '0' && pre[n - 2] <= '9')
            pre[n - 1] = 0;
    }
    last = strrchr(pre, ' ');
    last = last ? last + 1 : pre;
    if (sp->name)
        snprintf(name, sizeof name, "%s", sp->name);
    else if (!*pre || strstr(label, last))
        snprintf(name, sizeof name, "%s", label);
    else
        snprintf(name, sizeof name, "%s %s", pre, label);
    p = param_add(k, MM_SCH_ID0 + sp->id, MM_SRC_SCH, name, sp->fmt, sp->min, sp->max, sp->def,
                  (const char *const *)sp->names, sp->unit);
    if (p)
        p->sp = sp;
}
static const mm_common *common_find(uint32_t id)
{
    for (uint32_t i = 0; i < MM_NCOMMON; i++)
        if (MM_COMMON[i].id == id)
            return &MM_COMMON[i];
    return 0;
}
static void page_add(mm_kind *k, const char *name, const uint32_t *locals, uint32_t n)
{
    mm_page *g;
    if (!n || ngpg >= cgpg)
        return;
    g = &gpg[ngpg++];
    k->npage++;
    snprintf(g->name, sizeof g->name, "%s", name);
    for (uint32_t i = 0; i < CLAP_REMOTE_CONTROLS_COUNT; i++)
        g->ids[i] = i < n ? k->tag << MM_TAG_SHIFT | locals[i] : CLAP_INVALID_ID;
}
static int sch_shown(const mm_sch_param *sp) { return sp && !(sp->flags & MOD3_SF_HIDDEN) && sp->max > sp->min; }

/* the part's own parameters and pages after the schema's whose names a schema's has ("LFO", "LFO RATE"): "TRACK "
 * before them, as the FM-1 shows the track's LFO after the synth's own */
static void rename_clashes(const mm_kind *k, uint32_t first_part, uint32_t first_page)
{
    char t[CLAP_NAME_SIZE];
    for (uint32_t i = first_part; i < ngp; i++)
        for (uint32_t j = k->param0; j < first_part; j++)
            if (!strcmp(gp[i].name, gp[j].name)) {
                if (snprintf(t, sizeof t, "TRACK %s", gp[i].name) < (int)sizeof t)
                    memcpy(gp[i].name, t, sizeof t);
                break;
            }
    for (uint32_t i = first_page; i < ngpg; i++)
        for (uint32_t j = k->page0; j < first_page; j++)
            if (!strcmp(gpg[i].name, gpg[j].name)) {
                if (snprintf(t, sizeof t, "TRACK %s", gpg[i].name) < (int)sizeof t)
                    memcpy(gpg[i].name, t, sizeof t);
                break;
            }
}

/* a synth's parameters and pages: the schema's pages (four knobs each, as on the FM-1), its shown parameters on no
 * page four to a page (MORE), then the engine's two EDIT pages, the envelope, the LFO and the voice, each of the part's
 * own left out where the schema shows it (its TRACK parameters), the envelope where the synth has its own (ownenv, a
 * schema's ENV pages), E1..E8 where the schema says they are no panel */
static uint32_t kind_params_cap(const mm_kind *k) { return k->sch.nparam + 8u + MM_NCOMMON + MM_NOWN; }
static uint32_t kind_pages_cap(const mm_kind *k) { return k->sch.npage + (k->sch.nparam + 3u) / 4u + 2u + 4u + 2u; }
static void params_make(mm_kind *k)
{
    const mod_engine_v3 *e = &k->img.eng;
    const mm_schema *sc = &k->sch;
    static const uint32_t ENV[] = {MM_P_ATK, MM_P_DEC, MM_P_SUS, MM_P_REL, MM_P_ED_FLT, MM_P_ED_PIT, MM_P_ED_SHP};
    static const uint32_t LFO[] = {MM_P_LRATE, MM_P_LWAVE, MM_P_LFADE, MM_P_LD_PIT, MM_P_LD_FLT, MM_P_LD_SHP,
                                   MM_P_LD_AMP, MM_P_LSYNC};
    static const uint32_t LFO2[] = {MM_P_LPHASE, MM_P_LTRIG, MM_P_LPOL};
    static const uint32_t VOICE[] = {MM_P_VOICE, MM_P_GLIDE, MM_P_GLMODE, MM_P_DETUNE, MM_P_PRIO, MM_P_ALLOC,
                                     MM_P_LEVEL};
    static const struct { const char *name; const uint32_t *ids; uint32_t n; } GROUPS[] = {
        {"ENV", ENV, 7}, {"LFO", LFO, 8}, {"LFO 2", LFO2, 3}, {"VOICE", VOICE, 7}};
    uint32_t ids[CLAP_REMOTE_CONTROLS_COUNT];
    uint8_t covered[MOD3_P_COUNT] = {0};
    int own_env = e->ownenv != 0, own_lfo = 0, edit = !(sc->flags & MOD_SCH_F_NOEDIT);
    uint32_t i, n, first_more, first_part, first_page;
    k->param0 = ngp;
    k->page0 = ngpg;
    for (i = 0; i < sc->nparam; i++)
        if (sc->param[i].kind == MOD3_SK_TRACK)
            covered[sc->param[i].off] = 1;
    for (i = 0; i < sc->npage; i++) {            /* the schema's pages */
        const mm_sch_page *g = &sc->page[i];
        own_env |= (g->flags & MOD3_PG_ENV) != 0;
        own_lfo |= (g->flags & MOD3_PG_LFO) != 0;
        n = 0;
        for (uint32_t c = 0; c < 4u; c++) {
            const mm_sch_param *sp = g->id[c] == MOD_SCH_NONE ? 0 : mm_schema_find(sc, g->id[c]);
            if (!sch_shown(sp))
                continue;
            param_add_sch(k, sp, g->title);
            ids[n++] = MM_SCH_ID0 + sp->id;
        }
        page_add(k, g->title, ids, n);
    }
    first_more = ngp;
    for (i = 0; i < sc->nparam; i++)             /* (shown, on no page) */
        if (sch_shown(&sc->param[i]))
            param_add_sch(k, &sc->param[i], 0);
    for (i = first_more; i < ngp; i += n) {
        char title[CLAP_NAME_SIZE];
        n = ngp - i < 4u ? ngp - i : 4u;
        for (uint32_t c = 0; c < n; c++)
            ids[c] = gp[i + c].local;
        if (i == first_more)
            snprintf(title, sizeof title, "MORE");
        else
            snprintf(title, sizeof title, "MORE %u", (unsigned)((i - first_more) / 4u + 1u));
        page_add(k, title, ids, n);
    }
    first_part = ngp;
    first_page = ngpg;
    for (uint32_t pg = 0; edit && pg < 2u; pg++) {   /* EDIT 1 (E1..E4), EDIT 2 (E5..E8) */
        const char *title = e->page_title[pg] ? e->page_title[pg] : pg ? "EDIT 2" : "EDIT 1";
        n = 0;
        for (uint32_t c = pg * 4u; c < pg * 4u + 4u; c++) {
            const mod_param_v3 *d = &e->edit[c];
            if (covered[MM_P_E0 + c] || !d->label || !strcmp(d->label, "-") || d->max <= d->min)
                continue;
            if (param_add(k, MM_P_E0 + c, MM_SRC_PART, d->label, d->fmt, d->min, d->max, d->def, d->names, d->unit))
                ids[n++] = MM_P_E0 + c;
        }
        page_add(k, title, ids, n);
    }
    for (uint32_t g = 0; g < sizeof GROUPS / sizeof GROUPS[0]; g++) {
        /* (a synth with its own LFO pages: the part's after them as TRACK LFO, as the FM-1's LFO button has them) */
        const char *pre = own_lfo && (g == 1u || g == 2u) ? "TRACK " : "";
        char page[CLAP_NAME_SIZE], label[CLAP_NAME_SIZE];
        if (g == 0u && own_env)
            continue;
        snprintf(page, sizeof page, "%s%s", pre, GROUPS[g].name);
        n = 0;
        for (i = 0; i < GROUPS[g].n; i++) {
            const mm_common *c = common_find(GROUPS[g].ids[i]);
            if (!c || covered[c->id])
                continue;
            snprintf(label, sizeof label, "%s%s", pre, c->label);
            if (param_add(k, c->id, MM_SRC_PART, label, c->fmt, c->min, c->max, c->def, c->names, 0))
                ids[n++] = c->id;
        }
        if (g == 3u && param_add(k, MM_P_BEND, MM_SRC_OWN, "BEND", MOD3_F_SEMI, 0, 24, k->own_def[MM_OWN_BEND], 0, 0))
            ids[n++] = MM_P_BEND;
        page_add(k, page, ids, n);
    }
    n = 0;                                       /* TOUCH: velocity to the level, pressure to the filter */
    if (param_add(k, MM_P_VEL, MM_SRC_OWN, "VEL", MOD3_F_PCT, 0, 127, k->own_def[MM_OWN_VEL], 0, 0))
        ids[n++] = MM_P_VEL;
    if (param_add(k, MM_P_ATFLT, MM_SRC_OWN, "AT FLT", MOD3_F_BIPCT, -64, 63, k->own_def[MM_OWN_ATFLT], 0, 0))
        ids[n++] = MM_P_ATFLT;
    page_add(k, "TOUCH", ids, n);
    rename_clashes(k, first_part, first_page);
}
static int by_id(const void *a, const void *b)
{
    clap_id x = gp[*(const uint32_t *)a].id, y = gp[*(const uint32_t *)b].id;
    return x < y ? -1 : x > y;
}
static int tables_make(void)
{
    cgp = cgpg = 0;
    for (uint32_t i = 0; i < nkinds; i++)
        cgp += kind_params_cap(&kinds[i]), cgpg += kind_pages_cap(&kinds[i]);
    gp = calloc(cgp ? cgp : 1u, sizeof *gp);
    gpg = calloc(cgpg ? cgpg : 1u, sizeof *gpg);
    gsort = calloc(cgp ? cgp : 1u, sizeof *gsort);
    if (!gp || !gpg || !gsort)
        return -1;
    for (uint32_t i = 0; i < nkinds; i++)
        params_make(&kinds[i]);
    for (uint32_t i = 0; i < ngp; i++)
        gsort[i] = i;
    qsort(gsort, ngp, sizeof *gsort, by_id);
    return 0;
}

/* ---- a plug-in */


static mm_plug *plug_of(const clap_plugin_t *p) { return (mm_plug *)p->plugin_data; }

static mm_slot *slot_open(const mm_kind *k)
{
    mm_slot *s = calloc(1, sizeof *s);
    const char *err;
    if (!s)
        return 0;
    s->kind = k;
    if ((err = mm_image_load(&s->img, k->file.aa64, k->file.aa64_len)) ||
        mm_part_open(&s->part, s->img.desc, &s->img.eng)) {
        fprintf(stderr, "melodee-modules: %s: %s\n", k->name, err ? err : "the module refused the host");
        mm_image_free(&s->img);
        free(s);
        return 0;
    }
    for (uint32_t i = 0; i < MM_NOWN; i++)       /* (the plug-in's own, as the synth starts them) */
        *own_of(&s->part, i) = k->own_def[i];
    return s;
}
static void slot_free(mm_slot *s)
{
    if (!s)
        return;
    mm_part_close(&s->part);
    mm_image_free(&s->img);
    free(s);
}

/* ---- values: the main thread's (shadow, shadow_patch) and the audio's (the live part); another synth's: its
 * default, set to nothing */
static int16_t value_of(const mm_param *p, double v)
{
    long x = lround(v);
    return (int16_t)(x < p->min ? p->min : x > p->max ? p->max : x);
}
/* (main thread) */
int32_t value_get(mm_plug *pl, const mm_param *p)
{
    if (p->kind != pl->kind)
        return p->def;
    if (p->src == MM_SRC_OWN)
        return pl->shadow_own[p->local - MM_P_OWN0];
    if (p->src == MM_SRC_PART)
        return pl->shadow[p->local];
    if (p->sp->kind == MOD3_SK_TRACK)
        return pl->shadow[p->sp->off];
    return mm_sch_get(&pl->slot->part, pl->shadow_patch, p->sp);
}
void value_text(mm_plug *pl, const mm_param *p, int32_t v, char *buf, uint32_t cap)
{
    if (p->src == MM_SRC_SCH && p->kind == pl->kind)
        mm_sch_text(&pl->slot->part, pl->shadow_patch, p->sp, v, buf, cap);
    else
        mm_format(p->fmt, v, p->min, p->max, p->names, p->unit, buf, cap);
}
/* (the audio thread, or no audio running) set as the FM-1's pages do: the patch written, the module told of an edit
 * that changed it (or one it asks to hear of), the main thread's copies after it (while they are this synth's) */
static void value_set(mm_plug *pl, const mm_param *p, int32_t v)
{
    mm_part *t = &pl->live->part;
    const mm_sch_param *sp = p->sp;
    int changed, mine = p->kind == atomic_load(&pl->want);
    if (p->kind != pl->live->kind)
        return;                                  /* (another synth's: none plays it) */
    if (p->src == MM_SRC_OWN) {
        *own_of(t, p->local - MM_P_OWN0) = (int16_t)v;
        if (mine)
            pl->shadow_own[p->local - MM_P_OWN0] = (int16_t)v;
        return;
    }
    if (p->src == MM_SRC_PART) {
        t->h.p[p->local] = (int16_t)v;
        if (mine)
            pl->shadow[p->local] = (int16_t)v;
        return;
    }
    changed = mm_sch_put(t, t->patch, sp, v);
    if (changed || (sp->flags & MOD3_SF_NOTIFY))
        mm_patch_note(t, MOD3_PATCH_EDIT, sp->id);
    if (mine) {                                  /* (a TRACK parameter, or what the module set) */
        memcpy(pl->shadow, t->h.p, sizeof pl->shadow);
        memcpy(pl->shadow_patch, t->patch, p->kind->file.patch_len);
    }
    if (sp->kind == MOD3_SK_CALL || (sp->flags & MOD3_SF_NOTIFY)) {   /* (it may have moved others) */
        atomic_store(&pl->rescan, 1);
        if (pl->host->request_callback)
            pl->host->request_callback(pl->host);
    }
}

/* the main thread replaced the sound (a preset, a state): the audio takes it at its next block (once it plays that
 * synth), or now when it is not running */
static void sound_put(mm_plug *pl)
{
    mm_part *t = &pl->live->part;
    const mm_file *f = &pl->live->kind->file;
    memcpy(t->h.p, pl->shadow, sizeof pl->shadow);
    for (uint32_t k = 0; k < MM_NOWN; k++)
        *own_of(t, k) = pl->shadow_own[k];
    mm_kill(t);                                  /* a new sound: its notes stop */
    if (f->patch_len) {                          /* (the module may set the part's own values from it, correct it) */
        mm_patch_set(t, pl->shadow_patch, f->patch_len, MOD3_PATCH_NEW);
        memcpy(pl->shadow, t->h.p, sizeof pl->shadow);
        memcpy(pl->shadow_patch, t->patch, f->patch_len);
        atomic_store(&pl->rescan, 1);
        if (pl->host->request_callback)
            pl->host->request_callback(pl->host);
    }
}
static void sound_commit(mm_plug *pl)
{
    if (!atomic_load(&pl->active)) {
        sound_put(pl);
        return;
    }
    atomic_store(&pl->pending, 1);
    if (pl->host->request_process)
        pl->host->request_process(pl->host);
}
/* (the audio thread) a new synth the main thread made (once it took back the last old one), then a new sound */
static void sound_take(mm_plug *pl)
{
    mm_slot *n;
    if (!atomic_load(&pl->old) && atomic_load(&pl->next) && (n = atomic_exchange(&pl->next, 0))) {
        atomic_store(&pl->old, pl->live);
        pl->live = n;
        pl->tpos = MM_CTL;
        if (pl->host->request_callback)
            pl->host->request_callback(pl->host);
    }
    if (atomic_load(&pl->pending) && pl->live->kind == atomic_load(&pl->want) && atomic_exchange(&pl->pending, 0))
        sound_put(pl);
}
/* (the main thread, no audio running) the synth made last plays, the old ones freed, the sound taken */
static void slot_settle(mm_plug *pl)
{
    mm_slot *n = atomic_exchange(&pl->next, 0);
    if (n) {
        slot_free(pl->live);
        pl->live = n;
        pl->tpos = MM_CTL;
    }
    slot_free(atomic_exchange(&pl->old, 0));
    if (atomic_exchange(&pl->pending, 0))
        sound_put(pl);
}
/* (main thread) s, another synth's slot, the one to play: now when no audio runs, else at the audio's next block;
 * the host told its pages changed */
static void synth_post(mm_plug *pl, mm_slot *s)
{
    pl->kind = s->kind;
    pl->slot = s;
    atomic_store(&pl->want, s->kind);
    slot_free(atomic_exchange(&pl->old, 0));
    if (!atomic_load(&pl->active)) {
        slot_free(atomic_exchange(&pl->next, 0));
        slot_free(pl->live);
        pl->live = s;
    } else {
        slot_free(atomic_exchange(&pl->next, s));   /* (one the audio did not take yet: it never played) */
        if (pl->host->request_process)
            pl->host->request_process(pl->host);
    }
    if (pl->host_rc)
        pl->host_rc->changed(pl->host);
}

/* a sound of the synth it plays: the part's own defaults, then a common block (c; 0: none), as the device loads a
 * sound; its patch (n bytes) over the schema's default patch */
static void sound_apply(mm_plug *pl, const uint8_t *c, const uint8_t *patch, uint32_t n)
{
    const mm_file *f = &pl->kind->file;
    if (c)
        mm_preset(&pl->kind->img.eng, pl->shadow, (const int8_t *)c, c + 8, (int8_t)c[12], c[13]);
    else
        mm_defaults(&pl->kind->img.eng, pl->shadow);
    memcpy(pl->shadow_own, pl->kind->own_def, sizeof pl->shadow_own);
    atomic_store(&pl->edited, 0);
    memset(pl->shadow_patch, 0, sizeof pl->shadow_patch);
    if (f->patch_len) {
        memcpy(pl->shadow_patch, f->patch_def, f->patch_len);
        memcpy(pl->shadow_patch, patch, n < f->patch_len ? n : f->patch_len);
    }
    sound_commit(pl);
}
/* factory sound k of synth m: the synth loaded when it is another. 0, else -1 */
static int factory_load(mm_plug *pl, const mm_kind *m, uint32_t k)
{
    uint8_t c[MM_PST_COMMON], patch[MOD3_PATCH_MAX];
    uint32_t n = 0;
    mm_slot *s;
    if (k >= m->file.npresets || mm_file_sound(&m->file, k, 0, 0, c, patch, sizeof patch, &n))
        return -1;
    if (m != pl->kind) {
        if (!(s = slot_open(m)))
            return -1;
        synth_post(pl, s);
    }
    sound_apply(pl, c, patch, n);
    return 0;
}

/* ---- SysEx files: the .syx files in the SysEx folder beside the plug-in, each given to a module as the FM-1 gives it
 * SysEx that came; the sounds it offers for user slots are presets (load key "<module ID>/<index>", its offers in the
 * file in order) */
#define MM_SYX_MAX (1u << 20)                    /* (a file, at most: a DX7 bank is 4 KB, a PPG bank 10 KB) */
#define MM_MAX_SYX 512u
char sx_dir[4096];                        /* (init's) */
uint8_t *syx_read(const char *path, uint32_t *n)   /* the whole file, 0: none or too big */
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = 0;
    long len = -1;
    if (!f)
        return 0;
    if (!fseek(f, 0, SEEK_END) && (len = ftell(f)) > 0 && len <= (long)MM_SYX_MAX && !fseek(f, 0, SEEK_SET) &&
        (b = malloc((size_t)len)) && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = 0;
    }
    fclose(f);
    *n = b ? (uint32_t)len : 0;
    return b;
}
/* the synth a SysEx file is for: the first (by name) whose module answers a frame in it, 0 none */
const mm_kind *syx_owner(const uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0; i < nkinds; i++) {
        const mm_kind *k = &kinds[i];
        if (!k->sch.nsysex || !k->img.eng.sysex)
            continue;
        for (uint32_t at = 0; at < n; at++)
            if (data[at] == 0xF0u && mm_schema_sysex(&k->sch, data + at, n - at))
                return k;
    }
    return 0;
}
/* a sound's name in its patch (its schema's), printable, the spaces after it left out: "" none */
void patch_name(const mm_schema *sc, const uint8_t *patch, uint32_t len, char *buf, uint32_t cap)
{
    uint32_t k = 0;
    for (uint32_t i = 0; sc->nlen && i < sc->nlen && sc->name + i < len && k + 1u < cap; i++)
        buf[k++] = patch[sc->name + i] < 32u || patch[sc->name + i] > 126u ? ' ' : (char)patch[sc->name + i];
    while (k && buf[k - 1u] == ' ')
        k--;
    buf[k] = 0;
}
/* key "<module ID>/<k>": its synth and k. 0, else -1 */
static int key_read(const char *key, const mm_kind **m, uint32_t *k)
{
    const char *slash = key ? strchr(key, '/') : 0;
    char *end;
    unsigned long v;
    if (!slash || !(*m = kind_by_hex(key, (size_t)(slash - key))))
        return -1;
    v = strtoul(slash + 1, &end, 10);
    if (end == slash + 1 || *end || v > UINT32_MAX)
        return -1;
    *k = (uint32_t)v;
    return 0;
}
/* the sound wanted of a file (the sounds its module offers, in order) */
typedef struct sx_pick {
    uint32_t want, n, len;
    uint8_t patch[MOD3_PATCH_MAX];
} sx_pick;
static void sx_pick_take(void *ctx, const uint8_t *patch, uint32_t len)
{
    sx_pick *p = ctx;
    if (p->n++ == p->want)
        memcpy(p->patch, patch, p->len = len < MOD3_PATCH_MAX ? len : MOD3_PATCH_MAX);
}
/* sound k of SysEx file path that synth m's module offers: given to the part's module (its new slot's for another
 * synth), the synth loaded when it is another. 0, else -1 */
static int sx_load(mm_plug *pl, const mm_kind *m, const char *path, uint32_t k)
{
    uint8_t c[MM_PST_COMMON], first[MOD3_PATCH_MAX], *data;
    uint32_t n = 0, flen = 0;
    sx_pick p = {0};
    mm_slot *s;
    p.want = k;
    if (!m->sch.nsysex || !m->img.eng.sysex || !(data = syx_read(path, &n)))
        return -1;
    if (!(s = m == pl->kind ? pl->slot : slot_open(m))) {
        free(data);
        return -1;
    }
    mm_sysex_file(&s->part, &m->sch, data, n, sx_pick_take, &p, 0, 0);   /* (as the FM-1: beside the audio) */
    free(data);
    if (p.n <= p.want) {
        if (s != pl->slot)
            slot_free(s);
        return -1;
    }
    if (s != pl->slot)
        synth_post(pl, s);
    /* the part's own values: the file's first sound's (its INIT), as a user slot's on the device start from */
    sound_apply(pl, m->file.npresets && !mm_file_sound(&m->file, 0, 0, 0, c, first, sizeof first, &flen) ? c : 0,
                p.patch, p.len);
    return 0;
}

/* the sound loaded: its file and place, the host told its values (and, when the plug-in loaded it itself, which) */
static void sound_loaded(mm_plug *pl, const char *path, int32_t k, int tell)
{
    const clap_host_preset_load_t *hp;
    snprintf(pl->cur_path, sizeof pl->cur_path, "%s", path ? path : "");
    pl->cur_index = path ? k : -1;
    atomic_store(&pl->edited, 0);
    if (pl->host_params)
        pl->host_params->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    if (tell && path && ((hp = pl->host->get_extension(pl->host, CLAP_EXT_PRESET_LOAD)) ||
                         (hp = pl->host->get_extension(pl->host, CLAP_EXT_PRESET_LOAD_COMPAT)))) {
        char key[40];
        snprintf(key, sizeof key, "%s/%d", pl->kind->idhex, (int)k);
        hp->loaded(pl->host, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path, key);
    }
}
int sound_load(mm_plug *pl, const mm_kind *m, const char *syx, uint32_t k)
{
    slot_free(atomic_exchange(&pl->old, 0));
    if (syx ? sx_load(pl, m, syx, k) : factory_load(pl, m, k))
        return -1;
    sound_loaded(pl, syx ? syx : m->path, (int32_t)k, 1);
    return 0;
}

/* ---- values the screen sets: through a ring to the audio thread, which sets them as the host's (value_set), then has
 * the host read the values again (the main thread's next callback) and the screen draw */
void value_edit(mm_plug *pl, const mm_param *p, int32_t v)
{
    uint32_t w = atomic_load(&pl->edit_w);
    if (p->kind != pl->kind)
        return;
    v = v < p->min ? p->min : v > p->max ? p->max : v;
    if (!atomic_load(&pl->active)) {             /* (no audio: now) */
        slot_settle(pl);
        value_set(pl, p, v);
        atomic_store(&pl->edited, 1);
        if (pl->host_params)
            pl->host_params->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
        return;
    }
    if (w - atomic_load(&pl->edit_r) >= MM_NEDIT)
        return;                                  /* (full: the audio stopped taking them) */
    pl->edits[w % MM_NEDIT].id = p->id;
    pl->edits[w % MM_NEDIT].v = v;
    atomic_store(&pl->edit_w, w + 1u);
    if (pl->screen_host)
        pl->screen_host->redraw(pl->host, MELODEE_SCREEN_RUN);   /* (Melodee runs no silent plug-in: run it) */
    else if (pl->host->request_process)
        pl->host->request_process(pl->host);
}
/* (the audio thread) */
static void edits_take(mm_plug *pl)
{
    uint32_t r = atomic_load(&pl->edit_r), w = atomic_load(&pl->edit_w);
    if (r == w)
        return;
    for (; r != w; r++) {
        const mm_param *p = param_find(pl->edits[r % MM_NEDIT].id);
        if (p)
            value_set(pl, p, pl->edits[r % MM_NEDIT].v);
    }
    atomic_store(&pl->edit_r, r);
    atomic_store(&pl->edited, 1);
    atomic_store(&pl->rescan, 1);
    if (pl->host->request_callback)
        pl->host->request_callback(pl->host);
    if (pl->screen_host)
        pl->screen_host->redraw(pl->host, 0);
}

/* ---- the folders read when the library loaded (Modules, SysEx), watched while an instance is alive (mm_watch.h): a
 * module or a SysEx file added, removed or replaced, and each instance asks its host to restart it. Melodee then opens
 * it again with its state from a fresh copy of the library, which reads the folders again; another host deactivates
 * and activates it, which changes nothing (the ask is made once a change) */
static char mod_dir[4096];                       /* (init's) */
static uint64_t mod_sign;                        /* what init read */
static pthread_mutex_t users_lock = PTHREAD_MUTEX_INITIALIZER;   /* starting and stopping the watch */
static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;    /* the list */
static mm_plug *live_head;
static const char *const WATCH_EXT[] = {".mmod", ".syx"};

static void folders_changed(void *ctx)           /* (the watch's thread) */
{
    (void)ctx;
    fprintf(stderr, "melodee-modules: the Modules or SysEx folder changed: asking the host to open it again\n");
    pthread_mutex_lock(&live_lock);
    for (mm_plug *pl = live_head; pl; pl = pl->live_next)
        if (pl->host->request_restart)
            pl->host->request_restart(pl->host);
    pthread_mutex_unlock(&live_lock);
}
static void live_add(mm_plug *pl)
{
    const char *dirs[2] = {mod_dir, sx_dir};
    pthread_mutex_lock(&users_lock);
    pthread_mutex_lock(&live_lock);
    pl->live_next = live_head;
    live_head = pl;
    pthread_mutex_unlock(&live_lock);
    if (mod_dir[0])
        mm_watch_start(dirs, WATCH_EXT, 2u, mod_sign, folders_changed, 0);
    pthread_mutex_unlock(&users_lock);
}
static void live_remove(mm_plug *pl)
{
    int empty;
    pthread_mutex_lock(&users_lock);
    pthread_mutex_lock(&live_lock);
    for (mm_plug **at = &live_head; *at; at = &(*at)->live_next)
        if (*at == pl) {
            *at = pl->live_next;
            break;
        }
    empty = !live_head;
    pthread_mutex_unlock(&live_lock);
    if (empty)                                   /* (a library left loaded after a reload watches nothing) */
        mm_watch_stop();
    pthread_mutex_unlock(&users_lock);
}

/* ---- the plug-in's calls */
static bool p_init(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    pl->host_params = pl->host->get_extension(pl->host, CLAP_EXT_PARAMS);
    pl->host_rc = pl->host->get_extension(pl->host, CLAP_EXT_REMOTE_CONTROLS);
    if (!pl->host_rc)
        pl->host_rc = pl->host->get_extension(pl->host, CLAP_EXT_REMOTE_CONTROLS_COMPAT);
    pl->screen_host = pl->host->get_extension(pl->host, MELODEE_SCREEN_CLAP_HOST);
    return true;
}
static void p_destroy(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    mm_slot *n = atomic_exchange(&pl->next, 0);
    if (n != pl->live)
        slot_free(n);
    slot_free(atomic_exchange(&pl->old, 0));
    slot_free(pl->live);
    mm_screen_free(pl->scr);
    live_remove(pl);
    free(pl);
}
static bool p_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    mm_plug *pl = plug_of(plugin);
    (void)min_frames, (void)max_frames;
    pl->rate = rate;
    mm_rs_init(&pl->rs, MM_FS, rate);
    pl->tpos = MM_CTL;
    slot_settle(pl);
    atomic_store(&pl->active, 1);
    return true;
}
static void p_deactivate(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    atomic_store(&pl->active, 0);
    slot_settle(pl);
}
static bool p_start(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void p_stop(const clap_plugin_t *plugin) { (void)plugin; }
static void p_reset(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    mm_kill(&pl->live->part);
    mm_rs_reset(&pl->rs);
    pl->tpos = MM_CTL;
}

/* the module's next 44.1 kHz sample */
static void pull(void *ctx, float *l, float *r)
{
    mm_plug *pl = ctx;
    if (pl->tpos >= MM_CTL) {
        mm_part *t = &pl->live->part;
        int st = mm_tick(t, pl->tick), lv = t->h.p[MM_P_LEVEL];
        /* the device's mixer: LEVEL ((v - 112) / 2 dB), a mono part on both sides as it is, a stereo one (host 5)
         * its left side and out - left */
        const float k = lv > 0 ? powf(10.0f, (float)(lv - 112) / 40.0f) / 32768.0f : 0.0f;
        if (st)
            for (uint32_t i = 0; i < MM_CTL; i++) {
                pl->tl[i] = k * (float)t->left[i];
                pl->tr[i] = k * (float)(pl->tick[i] - t->left[i]);
            }
        else
            for (uint32_t i = 0; i < MM_CTL; i++)
                pl->tl[i] = pl->tr[i] = k * (float)pl->tick[i];
        if (atomic_load_explicit(&pl->screen_on, memory_order_relaxed)) {   /* (the scope's) */
            uint32_t at = atomic_load_explicit(&pl->scope_at, memory_order_relaxed);
            for (uint32_t i = 0; i < MM_CTL; i++)
                pl->scope[(at + i) & (MM_SCOPE - 1u)] = 0.5f * (pl->tl[i] + pl->tr[i]);
            atomic_store_explicit(&pl->scope_at, at + MM_CTL, memory_order_release);
        }
        pl->tpos = 0;
    }
    *l = pl->tl[pl->tpos];
    *r = pl->tr[pl->tpos];
    pl->tpos++;
}

static void midi_in(mm_plug *pl, const uint8_t *b)
{
    mm_part *t = &pl->live->part;
    uint32_t st = b[0] & 0xF0u, d1 = b[1] & 127u, d2 = b[2] & 127u;
    switch (st) {
    case 0x90:
        mm_note_on(t, d1, d2);
        break;
    case 0x80:
        mm_note_off(t, d1);
        break;
    case 0xB0:
        switch (d1) {
        case 1: t->h.mw = (uint8_t)d2; break;
        case 2: t->h.breath = (uint8_t)d2; break;
        case 4: t->h.foot = (uint8_t)d2; break;
        case 64: mm_sustain(t, d2 >= 64u); break;
        case 65: t->h.porta = (uint8_t)d2; break;
        case 120: mm_kill(t); break;
        case 123: mm_all_off(t); break;
        default: break;
        }
        break;
    case 0xD0:
        mm_pressure(t, -1, d1);
        break;
    case 0xA0:
        mm_pressure(t, (int32_t)d1, d2);
        break;
    case 0xE0:
        mm_bend(t, (int32_t)((d2 << 7) | d1) - 8192);
        break;
    default:
        break;
    }
}

static void event_in(mm_plug *pl, const clap_event_header_t *h)
{
    mm_part *t = &pl->live->part;
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t *n = (const void *)h;
        long v = lround(n->velocity * 127.0);
        if (n->key >= 0 && n->key < 128)
            mm_note_on(t, (uint32_t)n->key, (uint32_t)(v < 1 ? 1 : v > 127 ? 127 : v));
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t *n = (const void *)h;
        if (n->key >= 0 && n->key < 128)
            mm_note_off(t, (uint32_t)n->key);
        else
            mm_all_off(t);
        break;
    }
    case CLAP_EVENT_NOTE_EXPRESSION: {           /* pressure: a key's (as poly aftertouch), key -1 every key's */
        const clap_event_note_expression_t *x = (const void *)h;
        long v = lround(x->value * 127.0);
        if (x->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && x->key < 128)
            mm_pressure(t, x->key < 0 ? -1 : (int32_t)x->key, (uint32_t)(v < 0 ? 0 : v > 127 ? 127 : v));
        break;
    }
    case CLAP_EVENT_MIDI:
        midi_in(pl, ((const clap_event_midi_t *)(const void *)h)->data);
        break;
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t *e = (const void *)h;
        const mm_param *p = param_find(e->param_id);
        if (p) {
            value_set(pl, p, value_of(p, e->value));
            atomic_store(&pl->last_set, p->id);
            atomic_store(&pl->edited, 1);
            if (pl->screen_host)
                pl->screen_host->redraw(pl->host, 0);
        }
        break;
    }
    case CLAP_EVENT_TRANSPORT: {
        const clap_event_transport_t *tr = (const void *)h;
        if (tr->flags & CLAP_TRANSPORT_HAS_TEMPO)
            t->bpm = tr->tempo;
        break;
    }
    default:
        break;
    }
}

static clap_process_status p_process(const clap_plugin_t *plugin, const clap_process_t *pr)
{
    mm_plug *pl = plug_of(plugin);
    uint32_t frames = pr->frames_count, pos = 0, nev = pr->in_events ? pr->in_events->size(pr->in_events) : 0, k;
    float *l, *r;
    if (!pr->audio_outputs_count || pr->audio_outputs[0].channel_count < 1)
        return CLAP_PROCESS_ERROR;
    l = pr->audio_outputs[0].data32[0];
    r = pr->audio_outputs[0].channel_count > 1 ? pr->audio_outputs[0].data32[1] : 0;
    sound_take(pl);
    edits_take(pl);
    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO))
        pl->live->part.bpm = pr->transport->tempo;
    for (k = 0; k <= nev; k++) {
        const clap_event_header_t *h = k < nev ? pr->in_events->get(pr->in_events, k) : 0;
        uint32_t at = h ? (h->time < frames ? h->time : frames) : frames;
        if (at > pos) {
            float scratch[256];
            uint32_t n = at - pos;
            if (r) {
                mm_rs_run(&pl->rs, pull, pl, l + pos, r + pos, n);
            } else {                             /* (a mono port: the middle) */
                for (uint32_t done = 0; done < n;) {
                    uint32_t c = n - done < 256u ? n - done : 256u;
                    mm_rs_run(&pl->rs, pull, pl, l + pos + done, scratch, c);
                    for (uint32_t i = 0; i < c; i++)
                        l[pos + done + i] = 0.5f * (l[pos + done + i] + scratch[i]);
                    done += c;
                }
            }
            pos = at;
        }
        if (h)
            event_in(pl, h);
    }
    /* the screen asked to draw the scope while the part sounds (every 33 ms), and once more as it goes quiet */
    if (pl->screen_host && atomic_load_explicit(&pl->screen_on, memory_order_relaxed)) {
        int live = mm_live(&pl->live->part);
        pl->ask_frames += frames;
        if ((live || pl->ask_live) && pl->ask_frames >= (uint32_t)(pl->rate * 0.033)) {
            pl->ask_frames = 0;
            pl->ask_live = live;
            pl->screen_host->redraw(pl->host, 0);
        }
    }
    return mm_live(&pl->live->part) ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
}

/* ---- params: every synth's */
static uint32_t pa_count(const clap_plugin_t *plugin) { (void)plugin; return ngp; }
static bool pa_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    const mm_param *p;
    (void)plugin;
    if (index >= ngp)
        return false;
    p = &gp[index];
    memset(info, 0, sizeof *info);
    info->id = p->id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | (p->fmt == MOD3_F_ENUM ? CLAP_PARAM_IS_ENUM : 0);
    info->cookie = 0;
    snprintf(info->name, sizeof info->name, "%s", p->name);
    info->module[0] = 0;                         /* (no groups: a host's pages are the synth's own) */
    info->min_value = p->min;
    info->max_value = p->max;
    info->default_value = p->def;
    return true;
}
static bool pa_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    mm_plug *pl = plug_of(plugin);
    const mm_param *p = param_find(id);
    if (!p)
        return false;
    *value = value_get(pl, p);
    return true;
}
static bool pa_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *buf, uint32_t cap)
{
    mm_plug *pl = plug_of(plugin);
    const mm_param *p = param_find(id);
    if (!p || !cap)
        return false;
    value_text(pl, p, value_of(p, value), buf, cap);
    return true;
}
static bool pa_from_text(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    mm_plug *pl = plug_of(plugin);
    const mm_param *p = param_find(id);
    char buf[64];
    char *end;
    double v;
    if (!p || !text)
        return false;
    for (int32_t k = p->min; k <= p->max; k++) {  /* a value's own text (a name, "+12 st", "35%") */
        value_text(pl, p, k, buf, sizeof buf);
        if (!strcmp(buf, text)) {
            *value = k;
            return true;
        }
    }
    v = strtod(text, &end);
    if (end == text)
        return false;
    *value = value_of(p, v);
    return true;
}
static void pa_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    mm_plug *pl = plug_of(plugin);                /* (the audio thread's call while it processes, else no audio) */
    uint32_t n = in ? in->size(in) : 0;
    (void)out;
    if (atomic_load(&pl->active))
        edits_take(pl);
    for (uint32_t k = 0; k < n; k++) {
        const clap_event_header_t *h = in->get(in, k);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t *e = (const void *)h;
            const mm_param *p = param_find(e->param_id);
            if (p) {
                value_set(pl, p, value_of(p, e->value));
                atomic_store(&pl->last_set, p->id);
                atomic_store(&pl->edited, 1);
                if (pl->screen_host)
                    pl->screen_host->redraw(pl->host, 0);
            }
        }
    }
}
static const clap_plugin_params_t PARAMS = {pa_count, pa_info, pa_value, pa_to_text, pa_from_text, pa_flush};

/* ---- state: "MMOD", version, the synth's module ID, the part's parameters, the bend range, (version 3) VEL and AT
 * FLT, the patch */
typedef struct {
    uint32_t magic, version;
    uint8_t id[8];                               /* the module's ID */
    int16_t p[MOD3_P_COUNT];
    int16_t bend;
    uint16_t patch_len;                          /* the patch's bytes follow the head */
    int16_t vel, at_flt;                         /* (version 3: version 2's head ends before them) */
} mm_state_head;
#define MM_STATE_HEAD2 offsetof(mm_state_head, vel)

static bool write_all(const clap_ostream_t *s, const void *buf, uint64_t n)
{
    const uint8_t *b = buf;
    while (n) {
        int64_t w = s->write(s, b, n);
        if (w <= 0)
            return false;
        b += w;
        n -= (uint64_t)w;
    }
    return true;
}
static bool read_all(const clap_istream_t *s, void *buf, uint64_t n)
{
    uint8_t *b = buf;
    while (n) {
        int64_t r = s->read(s, b, n);
        if (r <= 0)
            return false;
        b += r;
        n -= (uint64_t)r;
    }
    return true;
}
/* after the patch (a state before it ends there; an older plug-in reads no further): the sound loaded, its file's
 * name, its place and whether it was changed since */
#define MM_SOUND_TAIL 10u                       /* "MMSN", its place (int32, little-endian), changed, its name's n */
static bool sound_save(mm_plug *pl, const clap_ostream_t *s)
{
    const char *slash = strrchr(pl->cur_path, '/'), *name = slash ? slash + 1 : pl->cur_path;
    size_t n = strlen(name);
    uint32_t k = (uint32_t)pl->cur_index;
    uint8_t t[MM_SOUND_TAIL] = {'M', 'M', 'S', 'N', (uint8_t)k, (uint8_t)(k >> 8), (uint8_t)(k >> 16),
                                (uint8_t)(k >> 24), (uint8_t)atomic_load(&pl->edited), (uint8_t)(n < 255u ? n : 0u)};
    return write_all(s, t, sizeof t) && write_all(s, name, t[9]);
}
static void sound_restore(mm_plug *pl, const clap_istream_t *s, const mm_kind *m)
{
    uint8_t t[MM_SOUND_TAIL];
    char name[256];
    const char *slash = strrchr(m->path, '/');
    pl->cur_path[0] = 0;
    pl->cur_index = -1;
    if (!read_all(s, t, sizeof t) || memcmp(t, "MMSN", 4) || !read_all(s, name, t[9]))
        return;
    name[t[9]] = 0;
    if (!strcmp(name, slash ? slash + 1 : m->path))
        snprintf(pl->cur_path, sizeof pl->cur_path, "%s", m->path);
    else if (name[0] && !strchr(name, '/') && strlen(sx_dir) + 1u + strlen(name) < sizeof pl->cur_path) {
        strcpy(pl->cur_path, sx_dir);            /* (a file in the SysEx folder) */
        strcat(pl->cur_path, "/");
        strcat(pl->cur_path, name);
    }
    pl->cur_index = pl->cur_path[0] ? (int32_t)((uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 |
                                                (uint32_t)t[7] << 24) : -1;
    atomic_store(&pl->edited, t[8] != 0);
}
static bool st_save(const clap_plugin_t *plugin, const clap_ostream_t *s)
{
    mm_plug *pl = plug_of(plugin);
    mm_state_head h;
    memset(&h, 0, sizeof h);
    h.magic = MM_STATE_MAGIC;
    h.version = MM_STATE_VERSION;
    memcpy(h.id, pl->kind->file.id, sizeof h.id);
    memcpy(h.p, pl->shadow, sizeof h.p);
    h.bend = pl->shadow_own[MM_OWN_BEND];
    h.vel = pl->shadow_own[MM_OWN_VEL];
    h.at_flt = pl->shadow_own[MM_OWN_ATFLT];
    h.patch_len = (uint16_t)pl->kind->file.patch_len;
    return write_all(s, &h, sizeof h) && write_all(s, pl->shadow_patch, h.patch_len) && sound_save(pl, s);
}
/* a state of any synth found: that synth loaded when it is another */
static bool st_load(const clap_plugin_t *plugin, const clap_istream_t *s)
{
    mm_plug *pl = plug_of(plugin);
    mm_state_head h;
    uint8_t patch[MOD3_PATCH_MAX];
    const mm_kind *m = 0;
    const mm_file *f;
    mm_slot *slot;
    int16_t own[MM_NOWN];
    if (!read_all(s, &h, MM_STATE_HEAD2) || h.magic != MM_STATE_MAGIC ||
        (h.version != 2u && h.version != MM_STATE_VERSION) ||
        (h.version == MM_STATE_VERSION && !read_all(s, (uint8_t *)&h + MM_STATE_HEAD2, sizeof h - MM_STATE_HEAD2)) ||
        h.patch_len > MOD3_PATCH_MAX || !read_all(s, patch, h.patch_len))
        return false;
    for (uint32_t i = 0; i < nkinds && !m; i++)
        if (!memcmp(kinds[i].file.id, h.id, sizeof h.id))
            m = &kinds[i];
    if (!m) {
        fprintf(stderr, "melodee-modules: a state of a synth not in Modules\n");
        return false;
    }
    if (h.version == 2u)                         /* (version 2's VEL and AT FLT: as a new part of its synth has them) */
        h.vel = m->own_def[MM_OWN_VEL], h.at_flt = m->own_def[MM_OWN_ATFLT];
    own[MM_OWN_BEND] = h.bend, own[MM_OWN_VEL] = h.vel, own[MM_OWN_ATFLT] = h.at_flt;
    if (m != pl->kind) {
        if (!(slot = slot_open(m)))
            return false;
        synth_post(pl, slot);
    }
    f = &m->file;
    memcpy(pl->shadow, h.p, sizeof pl->shadow);
    for (uint32_t i = m->param0; i < m->param0 + m->nparam; i++) {   /* (the part's own it shows: in their ranges) */
        const mm_param *p = &gp[i];
        if (p->src == MM_SRC_OWN)
            pl->shadow_own[p->local - MM_P_OWN0] = value_of(p, own[p->local - MM_P_OWN0]);
        else if (p->src == MM_SRC_PART)
            pl->shadow[p->local] = value_of(p, h.p[p->local]);
    }
    memset(pl->shadow_patch, 0, sizeof pl->shadow_patch);
    if (f->patch_len) {                          /* (a shorter patch saved: the rest as the schema starts it) */
        memcpy(pl->shadow_patch, f->patch_def, f->patch_len);
        memcpy(pl->shadow_patch, patch, h.patch_len < f->patch_len ? h.patch_len : f->patch_len);
    }
    sound_commit(pl);
    sound_restore(pl, s, m);
    if (pl->screen_host)
        pl->screen_host->redraw(pl->host, 0);
    if (pl->host_params)
        pl->host_params->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    return true;
}
static const clap_plugin_state_t STATE = {st_save, st_load};

/* ---- ports */
static uint32_t ap_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; return is_input ? 0 : 1; }
static bool ap_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (is_input || index)
        return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    snprintf(info->name, sizeof info->name, "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t AUDIO_PORTS = {ap_count, ap_get};

static uint32_t np_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; return is_input ? 1 : 0; }
static bool np_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    (void)plugin;
    if (!is_input || index)
        return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    snprintf(info->name, sizeof info->name, "Notes");
    return true;
}
static const clap_plugin_note_ports_t NOTE_PORTS = {np_count, np_get};

static uint32_t la_get(const clap_plugin_t *plugin) { return mm_rs_latency(&plug_of(plugin)->rs); }
static const clap_plugin_latency_t LATENCY = {la_get};

static uint32_t ta_get(const clap_plugin_t *plugin)   /* the release, to -72 dB */
{
    mm_plug *pl = plug_of(plugin);
    double ms = pow(10000.0, pl->shadow[MM_P_REL] / 127.0) * 1.8 + 50.0;
    return (uint32_t)(ms * (pl->rate > 0 ? pl->rate : MM_FS) / 1000.0) + mm_rs_latency(&pl->rs);
}
static const clap_plugin_tail_t TAIL = {ta_get};

/* ---- pages: the synth's it plays */
static uint32_t rc_count(const clap_plugin_t *plugin) { return plug_of(plugin)->kind->npage; }
static bool rc_get(const clap_plugin_t *plugin, uint32_t index, clap_remote_controls_page_t *page)
{
    const mm_kind *k = plug_of(plugin)->kind;
    if (index >= k->npage)
        return false;
    memset(page, 0, sizeof *page);
    snprintf(page->section_name, sizeof page->section_name, "%s", k->name);
    snprintf(page->page_name, sizeof page->page_name, "%s", gpg[k->page0 + index].name);
    page->page_id = k->tag << MM_TAG_SHIFT | index;
    memcpy(page->param_ids, gpg[k->page0 + index].ids, sizeof page->param_ids);
    page->is_for_preset = false;
    return true;
}
static const clap_plugin_remote_controls_t REMOTE = {rc_count, rc_get};

/* ---- preset load: load key "<module ID>/<index>". A module file's location (or the plug-in's): its factory sound;
 * a SysEx file's: the sound its module offers */
static int is_mmod(const char *path)
{
    size_t n = strlen(path);
    return n > 5 && !strcasecmp(path + n - 5, ".mmod");
}
static bool pl_from_location(const clap_plugin_t *plugin, uint32_t kind, const char *location, const char *key)
{
    mm_plug *pl = plug_of(plugin);
    const mm_kind *m;
    uint32_t k;
    slot_free(atomic_exchange(&pl->old, 0));
    if (key_read(key, &m, &k))
        return false;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location && !is_mmod(location)) {
        if (sx_load(pl, m, location, k))
            return false;
    } else if ((kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) ||
               factory_load(pl, m, k)) {
        return false;
    }
    sound_loaded(pl, kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location && !is_mmod(location) ? location : m->path,
                 (int32_t)k, 0);
    if (pl->screen_host)
        pl->screen_host->redraw(pl->host, 0);
    return true;
}
static const clap_plugin_preset_load_t PRESET_LOAD = {pl_from_location};

static const void *p_ext(const clap_plugin_t *plugin, const char *id)
{
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &PARAMS;
    if (!strcmp(id, CLAP_EXT_STATE)) return &STATE;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &AUDIO_PORTS;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &NOTE_PORTS;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &LATENCY;
    if (!strcmp(id, CLAP_EXT_TAIL)) return &TAIL;
    if (!strcmp(id, CLAP_EXT_REMOTE_CONTROLS) || !strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT)) return &REMOTE;
    if (!strcmp(id, CLAP_EXT_PRESET_LOAD) || !strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &PRESET_LOAD;
    if (!strcmp(id, MELODEE_SCREEN_CLAP)) return &mm_screen_ext;
    return 0;
}
static void p_on_main(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    slot_free(atomic_exchange(&pl->old, 0));    /* (the synth the audio stopped playing) */
    if (atomic_exchange(&pl->rescan, 0) && pl->host_params)
        pl->host_params->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
}

/* ---- the plug-in factory: one plug-in, when there is a synth to play */
static const char *const MM_FEATURES[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                          CLAP_PLUGIN_FEATURE_STEREO, 0};
static const clap_plugin_descriptor_t DESC = {CLAP_VERSION_INIT, MM_PLUGIN_ID, "Melodee Modules", MM_VENDOR, "", "", "",
                                              MM_VERSION, "Melodee synth modules: the .mmod files in its Modules folder",
                                              MM_FEATURES};
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return nkinds ? 1u : 0u; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i)
{
    (void)f;
    return !i && nkinds ? &DESC : 0;
}
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    mm_plug *pl;
    mm_slot *s = 0;
    (void)f;
    if (!clap_version_is_compatible(host->clap_version) || strcmp(id, MM_PLUGIN_ID) || !nkinds)
        return 0;
    if (!(pl = calloc(1, sizeof *pl)))
        return 0;
    pl->host = host;
    for (uint32_t i = 0; i < nkinds && !s; i++)  /* a new part: the first synth (by name) that opens */
        s = slot_open(&kinds[i]);
    if (!s) {
        free(pl);
        return 0;
    }
    synth_post(pl, s);
    memcpy(pl->shadow, s->part.h.p, sizeof pl->shadow);
    for (uint32_t k = 0; k < MM_NOWN; k++)
        pl->shadow_own[k] = (int16_t)*own_of(&s->part, k);
    if (pl->kind->file.patch_len)
        memcpy(pl->shadow_patch, pl->kind->file.patch_def, pl->kind->file.patch_len);
    pl->cur_index = -1;
    if (!pl->kind->file.npresets || factory_load(pl, pl->kind, pl->kind->file.first))   /* .. its first sound */
        sound_commit(pl);
    else
        snprintf(pl->cur_path, sizeof pl->cur_path, "%s", pl->kind->path),
            pl->cur_index = (int32_t)pl->kind->file.first;
    pl->rate = MM_FS;
    mm_rs_init(&pl->rs, MM_FS, MM_FS);
    pl->tpos = MM_CTL;
    pl->plugin.desc = &DESC;
    pl->plugin.plugin_data = pl;
    pl->plugin.init = p_init;
    pl->plugin.destroy = p_destroy;
    pl->plugin.activate = p_activate;
    pl->plugin.deactivate = p_deactivate;
    pl->plugin.start_processing = p_start;
    pl->plugin.stop_processing = p_stop;
    pl->plugin.reset = p_reset;
    pl->plugin.process = p_process;
    pl->plugin.get_extension = p_ext;
    pl->plugin.on_main_thread = p_on_main;
    live_add(pl);
    return &pl->plugin;
}
static const clap_plugin_factory_t PLUGIN_FACTORY = {f_count, f_desc, f_create};

/* ---- the preset discovery factory: one provider. A location for each synth's module file, and for each SysEx file
 * one under the synth it is for ("PPG/Factory", "PPG/<file>"; a synth with no SysEx file just "TRIO") */
static const clap_preset_discovery_provider_descriptor_t PROVIDER_DESC = {
    CLAP_VERSION_INIT, MM_PLUGIN_ID ".presets", "Melodee modules", MM_VENDOR};

int name_order(const void *a, const void *b)   /* (letters without case, numbers by value: 2 before 10) */
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    while (*x && *y) {
        if (*x >= '0' && *x <= '9' && *y >= '0' && *y <= '9') {
            char *ex, *ey;
            unsigned long u = strtoul(x, &ex, 10), v = strtoul(y, &ey, 10);
            if (u != v)
                return u < v ? -1 : 1;
            x = ex, y = ey;
            continue;
        }
        if (tolower((unsigned char)*x) != tolower((unsigned char)*y))
            return tolower((unsigned char)*x) - tolower((unsigned char)*y);
        x++, y++;
    }
    return (unsigned char)*x - (unsigned char)*y;
}
/* a location's name: the synth's name ("/" in it as "-", as a host takes "/" for a folder), then "/" and more */
static void loc_name(char *buf, size_t cap, const mm_kind *k, const char *more)
{
    size_t n = strlen(k->name);
    if (more)
        snprintf(buf, cap, "%s/%s", k->name, more);
    else
        snprintf(buf, cap, "%s", k->name);
    for (size_t i = 0; i < n && buf[i]; i++)
        buf[i] = buf[i] == '/' ? '-' : buf[i];
}
static bool pv_init(const clap_preset_discovery_provider_t *pv)
{
    const clap_preset_discovery_indexer_t *ix = pv->provider_data;
    static const clap_preset_discovery_filetype_t MMOD = {"Synth module", "a Melodee synth module (its sounds)", "mmod"};
    static const clap_preset_discovery_filetype_t SYX = {"SysEx", "a synth module's SysEx (its sounds)", "syx"};
    char *name[MM_MAX_SYX], loc[512], path[4096 + 256];
    const mm_kind *owner[MM_MAX_SYX];
    uint32_t nname = 0;
    struct dirent *de;
    DIR *dp;
    ix->declare_filetype(ix, &MMOD);
    ix->declare_filetype(ix, &SYX);
    if (sx_dir[0] && (dp = opendir(sx_dir))) {
        while ((de = readdir(dp)) && nname < MM_MAX_SYX) {
            size_t n = strlen(de->d_name);
            if (de->d_name[0] != '.' && n > 4 && !strcasecmp(de->d_name + n - 4, ".syx") &&
                (name[nname] = strdup(de->d_name)))
                nname++;
        }
        closedir(dp);
    }
    qsort(name, nname, sizeof name[0], name_order);
    for (uint32_t f = 0; f < nname; f++) {       /* (each file read: the synth it is for) */
        uint32_t n = 0;
        uint8_t *data;
        snprintf(path, sizeof path, "%s/%s", sx_dir, name[f]);
        data = syx_read(path, &n);
        owner[f] = data ? syx_owner(data, n) : 0;
        free(data);
    }
    for (uint32_t i = 0; i < nkinds; i++) {
        const mm_kind *k = &kinds[i];
        uint32_t files = 0;
        for (uint32_t f = 0; f < nname; f++)
            files += owner[f] == k;
        if (k->file.npresets) {
            clap_preset_discovery_location_t l = {CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, loc,
                                                  CLAP_PRESET_DISCOVERY_LOCATION_FILE, k->path};
            loc_name(loc, sizeof loc, k, files ? "Factory" : 0);
            ix->declare_location(ix, &l);
        }
        for (uint32_t f = 0; f < nname; f++) {
            char stem[256];
            clap_preset_discovery_location_t l = {CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, loc,
                                                  CLAP_PRESET_DISCOVERY_LOCATION_FILE, path};
            if (owner[f] != k)
                continue;
            snprintf(stem, sizeof stem, "%.*s", (int)strlen(name[f]) - 4, name[f]);
            loc_name(loc, sizeof loc, k, stem);
            if (snprintf(path, sizeof path, "%s/%s", sx_dir, name[f]) < (int)sizeof path)
                ix->declare_location(ix, &l);
        }
    }
    for (uint32_t f = 0; f < nname; f++)
        free(name[f]);
    return true;
}
static void pv_destroy(const clap_preset_discovery_provider_t *pv) { free((void *)(uintptr_t)pv); }
static bool preset_begin(const clap_preset_discovery_metadata_receiver_t *rx, const char *name, const char *key,
                         uint32_t flags)
{
    static const clap_universal_plugin_id_t UID = {"clap", MM_PLUGIN_ID};
    if (!rx->begin_preset(rx, name, key))
        return false;
    rx->add_plugin_id(rx, &UID);
    rx->set_flags(rx, flags);
    return true;
}
static bool factory_list(const mm_kind *m, const clap_preset_discovery_metadata_receiver_t *rx)   /* false: stop */
{
    for (uint32_t k = 0; k < m->file.npresets; k++) {
        char name[MM_PST_NAME + 1], key[40];
        if (mm_file_sound(&m->file, k, name, 0, 0, 0, 0, 0))
            continue;
        snprintf(key, sizeof key, "%s/%u", m->idhex, k);
        if (!preset_begin(rx, name, key, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT))
            return false;
    }
    return true;
}
/* a module file's sounds: the synth found by where the file is (a host may give it with links followed) */
static bool mmod_metadata(const char *path, const clap_preset_discovery_metadata_receiver_t *rx)
{
    char real[4096];
    if (!realpath(path, real))
        snprintf(real, sizeof real, "%s", path);
    for (uint32_t i = 0; i < nkinds; i++)
        if (!strcmp(kinds[i].real, real) || !strcmp(kinds[i].path, path)) {
            factory_list(&kinds[i], rx);
            return true;
        }
    return false;
}
/* a SysEx file's sounds: those the module of the synth it is for offers, given the file in a copy of its own (a part
 * of its own, as a new instance) */
typedef struct sx_list {
    const mm_kind *m;
    const clap_preset_discovery_metadata_receiver_t *rx;
    uint32_t n;
    int stop;
} sx_list;
static void sx_list_take(void *ctx, const uint8_t *patch, uint32_t len)
{
    sx_list *l = ctx;
    char name[64], key[40];
    if (l->stop)
        return;
    patch_name(&l->m->sch, patch, len, name, sizeof name);
    if (!name[0])
        snprintf(name, sizeof name, "%s %u", l->m->name, (unsigned)l->n + 1u);
    snprintf(key, sizeof key, "%s/%u", l->m->idhex, (unsigned)l->n++);
    l->stop = !preset_begin(l->rx, name, key, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
}
static bool sx_metadata(const char *path, const clap_preset_discovery_metadata_receiver_t *rx)
{
    uint32_t n = 0;
    uint8_t *data = syx_read(path, &n);
    const mm_kind *m = data ? syx_owner(data, n) : 0;
    mm_image img;
    mm_part *t;
    sx_list l = {m, rx, 0, 0};
    char msg[64];
    if (!data)
        return false;
    if (!m) {                                    /* (no synth's: no sound) */
        free(data);
        return true;
    }
    if (mm_image_load(&img, m->file.aa64, m->file.aa64_len)) {
        mm_image_free(&img);
        free(data);
        return true;
    }
    if ((t = calloc(1, sizeof *t)) && !mm_part_open(t, img.desc, &img.eng)) {
        if (img.eng.sysex && !mm_sysex_file(t, &m->sch, data, n, sx_list_take, &l, msg, sizeof msg) && msg[0] &&
            rx->on_error)
            rx->on_error(rx, 0, msg);            /* (none taken: what the module says, "INVALID PPG BANK") */
        mm_part_close(t);
    }
    free(t);
    mm_image_free(&img);
    free(data);
    return true;
}
static bool pv_metadata(const clap_preset_discovery_provider_t *pv, uint32_t kind, const char *location,
                        const clap_preset_discovery_metadata_receiver_t *rx)
{
    (void)pv;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE)
        return location && (is_mmod(location) ? mmod_metadata(location, rx) : sx_metadata(location, rx));
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        return false;
    for (uint32_t i = 0; i < nkinds; i++)        /* (no plug-in location is declared: every synth's, for a host that
                                                  * asks) */
        if (!factory_list(&kinds[i], rx))
            break;
    return true;
}
static const void *pv_ext(const clap_preset_discovery_provider_t *pv, const char *id) { (void)pv, (void)id; return 0; }

static uint32_t pf_count(const clap_preset_discovery_factory_t *f) { (void)f; return 1; }
static const clap_preset_discovery_provider_descriptor_t *pf_desc(const clap_preset_discovery_factory_t *f, uint32_t i)
{
    (void)f;
    return !i ? &PROVIDER_DESC : 0;
}
static const clap_preset_discovery_provider_t *pf_create(const clap_preset_discovery_factory_t *f,
                                                         const clap_preset_discovery_indexer_t *ix, const char *id)
{
    clap_preset_discovery_provider_t *pv;
    (void)f;
    if (strcmp(id, PROVIDER_DESC.id))
        return 0;
    pv = calloc(1, sizeof *pv);
    if (!pv)
        return 0;
    pv->desc = &PROVIDER_DESC;
    pv->provider_data = (void *)(uintptr_t)ix;
    pv->init = pv_init;
    pv->destroy = pv_destroy;
    pv->get_metadata = pv_metadata;
    pv->get_extension = pv_ext;
    return pv;
}
static const clap_preset_discovery_factory_t PRESET_FACTORY = {pf_count, pf_desc, pf_create};

/* ---- the entry */
static void e_deinit(void)
{
    mm_watch_stop();                             /* (no instance should be alive now; the watch never outlives the library) */
    for (uint32_t i = 0; i < nkinds; i++) {
        mm_image_free(&kinds[i].img);
        mm_schema_free(&kinds[i].sch);
        mm_file_free(&kinds[i].file);
    }
    free(kinds);
    free(gp);
    free(gpg);
    free(gsort);
    kinds = 0;
    gp = 0;
    gpg = 0;
    gsort = 0;
    nkinds = ngp = cgp = ngpg = cgpg = 0;
}
static bool e_init(const char *path)
{
    char dir[4096];
    const char *slash = path ? strrchr(path, '/') : 0;
    mm_init();
    if (!slash)                                  /* the Modules and SysEx folders beside the plug-in */
        snprintf(dir, sizeof dir, "Modules"), snprintf(sx_dir, sizeof sx_dir, "SysEx");
    else if (snprintf(dir, sizeof dir, "%.*s/Modules", (int)(slash - path), path) >= (int)sizeof dir ||
             snprintf(sx_dir, sizeof sx_dir, "%.*s/SysEx", (int)(slash - path), path) >= (int)sizeof sx_dir)
        return true;                             /* (a path too long: no modules) */
    kinds_scan(dir);
    {                                            /* what was read, for the watch (mm_watch.h) */
        const char *dirs[2] = {dir, sx_dir};
        snprintf(mod_dir, sizeof mod_dir, "%s", dir);
        mod_sign = mm_watch_sign(dirs, WATCH_EXT, 2u);
    }
    if (tables_make())
        e_deinit();                              /* (no memory: no synths) */
    return true;
}
static const void *e_factory(const char *id)
{
    if (!strcmp(id, CLAP_PLUGIN_FACTORY_ID))
        return &PLUGIN_FACTORY;
    if (!strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) || !strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
        return &PRESET_FACTORY;
    return 0;
}
CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, e_init, e_deinit, e_factory};
