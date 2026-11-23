/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The CLAP side: one plug-in for each synth module file (.mmod) in the Modules folder beside the plug-in, each
 * instance with its own copy of the module's AArch64 code (mm_image.h). Its parameters (its schema's, the engine's
 * E1..E8 and the part's own), its pages (remote controls: the schema's, then the part's), its factory sounds (preset
 * discovery, preset load: the file's PRST), the sounds its module takes from SysEx files (.syx) in the SysEx folder
 * beside the plug-in (preset discovery: a file location each), its state (the part's values and patch). Notes come as
 * CLAP notes or MIDI; the wheels, pressure and the pedal as MIDI.
 * The module plays at 44.1 kHz (mm_resample.h at other rates). */
#include <clap/clap.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm_engine.h"
#include "mm_file.h"
#include "mm_image.h"
#include "mm_resample.h"
#include "mm_schema.h"

#define MM_ID_PREFIX "studio.ellic.melodee-modules."
#define MM_VENDOR "Ellic Studio"
#define MM_VERSION "0.1.0"
#define MM_P_BEND 1000u                          /* the wheel's range (semitones): the plug-in's own parameter */
#define MM_STATE_MAGIC 0x444F4D4Du               /* "MMOD" */
#define MM_STATE_VERSION 2u
#define MM_SCH_ID0 0x10000u                     /* a schema parameter's CLAP id: this + its id */

/* ---- the descriptors: the module files found at init */
static const char *const MM_FEATURES[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                          CLAP_PLUGIN_FEATURE_STEREO, 0};
typedef struct mm_kind {
    clap_plugin_descriptor_t d;
    char id[64], idhex[17], name[MOD3_NAME_MAX + 1], vendor[MOD3_NAME_MAX + 1], version[16], text[64];
    mm_file file;
    mm_schema sch;                               /* its parameters and pages (none: empty) */
} mm_kind;
static mm_kind *kinds;
static uint32_t nkinds;

static int kind_order(const void *a, const void *b)
{
    return strcmp(((const mm_kind *)a)->name, ((const mm_kind *)b)->name);
}
static void kind_fill(mm_kind *k)
{
    for (int i = 0; i < 8; i++)                  /* the ID as mod_build.py --check prints it */
        snprintf(k->idhex + 2 * i, 3, "%02x", k->file.id[7 - i]);
    snprintf(k->id, sizeof k->id, MM_ID_PREFIX "%s", k->idhex);
    snprintf(k->name, sizeof k->name, "%s", k->file.name);
    snprintf(k->vendor, sizeof k->vendor, "%s", k->file.vendor[0] ? k->file.vendor : "-");
    snprintf(k->version, sizeof k->version, "%u", (unsigned)k->file.head.version);
    snprintf(k->text, sizeof k->text, "Melodee synth module %s/%s", k->vendor, k->file.slug);
}

/* every .mmod in dir (the Modules folder beside the plug-in): one plug-in each, the newest file of a module */
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
        kinds[nkinds++].file = f;
    }
    closedir(dp);
    for (uint32_t i = 0; i < nkinds; i++) {
        kind_fill(&kinds[i]);
        if (mm_schema_read(&kinds[i].sch, &kinds[i].file))
            fprintf(stderr, "melodee-modules: %s: a schema this plug-in cannot read (E1..E8 only)\n", kinds[i].name);
    }
    qsort(kinds, nkinds, sizeof *kinds, kind_order);
    for (uint32_t i = 0; i < nkinds; i++) {      /* (the descriptor points into its own kind: after the sort) */
        mm_kind *k = &kinds[i];
        k->d.clap_version = (clap_version_t)CLAP_VERSION_INIT;
        k->d.id = k->id;
        k->d.name = k->name;
        k->d.vendor = k->vendor;
        k->d.version = k->version;
        k->d.description = k->text;
        k->d.features = MM_FEATURES;
    }
}

/* ---- a plug-in */
enum { MM_SRC_PART, MM_SRC_BEND, MM_SRC_SCH };
typedef struct mm_param {
    clap_id id;                                  /* a P_* number, MM_P_BEND, or MM_SCH_ID0 + a schema id */
    uint8_t src;                                 /* MM_SRC_* */
    const mm_sch_param *sp;                      /* MM_SRC_SCH: its schema's record */
    char name[CLAP_NAME_SIZE], module[CLAP_PATH_SIZE];
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;
    const char *unit;
} mm_param;

typedef struct mm_page {
    char name[CLAP_NAME_SIZE];
    clap_id ids[CLAP_REMOTE_CONTROLS_COUNT];
} mm_page;

typedef struct mm_plug {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *host_params;
    const mm_kind *kind;
    const mm_file *file;
    mm_image img;
    mm_param *params;
    uint32_t nparams, cparams;
    mm_page *pages;
    uint32_t npages, cpages;
    /* the main thread's view of the values (get_value, state); the audio thread's are in part */
    int16_t shadow[MOD3_P_COUNT];
    int16_t shadow_bend;
    uint8_t shadow_patch[MOD3_PATCH_MAX];        /* host 2: the patch (file->patch_len bytes) */
    atomic_int pending;                          /* the main thread changed the sound: the audio takes shadow */
    atomic_int active;
    atomic_int rescan;                           /* values changed beyond the one set (the module's): tell the host */
    double rate;
    mm_rs rs;
    int32_t tick[MM_CTL];
    float tl[MM_CTL], tr[MM_CTL];
    uint32_t tpos;
    mm_part part;                                /* (last: the module reads its head) */
} mm_plug;

static mm_plug *plug_of(const clap_plugin_t *p) { return (mm_plug *)p->plugin_data; }

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

static const mm_param *param_find(const mm_plug *pl, clap_id id)
{
    for (uint32_t i = 0; i < pl->nparams; i++)
        if (pl->params[i].id == id)
            return &pl->params[i];
    return 0;
}
static mm_param *param_add(mm_plug *pl, clap_id id, uint8_t src, const char *name, const char *module, uint8_t fmt,
                           int16_t min, int16_t max, int16_t def, const char *const *names, const char *unit)
{
    mm_param *p;
    if (param_find(pl, id) || pl->nparams >= pl->cparams)
        return 0;
    p = &pl->params[pl->nparams++];
    memset(p, 0, sizeof *p);
    p->id = id;
    p->src = src;
    snprintf(p->name, sizeof p->name, "%s", name);
    long_words(p->name, sizeof p->name);
    snprintf(p->module, sizeof p->module, "%s", module);
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
static void param_add_sch(mm_plug *pl, const mm_sch_param *sp, const char *title, const char *section)
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
    p = param_add(pl, MM_SCH_ID0 + sp->id, MM_SRC_SCH, name, section && *section ? section : title ? title : "OTHER",
                  sp->fmt, sp->min, sp->max, sp->def, (const char *const *)sp->names, sp->unit);
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
static void page_add(mm_plug *pl, const char *name, const clap_id *ids, uint32_t n)
{
    mm_page *g;
    if (!n || pl->npages >= pl->cpages)
        return;
    g = &pl->pages[pl->npages++];
    snprintf(g->name, sizeof g->name, "%s", name);
    for (uint32_t i = 0; i < CLAP_REMOTE_CONTROLS_COUNT; i++)
        g->ids[i] = i < n ? ids[i] : CLAP_INVALID_ID;
}
static int sch_shown(const mm_sch_param *sp) { return sp && !(sp->flags & MOD3_SF_HIDDEN) && sp->max > sp->min; }

/* the part's own parameters and pages after the schema's whose names a schema's has ("LFO", "LFO RATE"): "TRACK "
 * before them, as the FM-1 shows the track's LFO after the synth's own */
static void rename_clashes(mm_plug *pl, uint32_t first_part, uint32_t first_page)
{
    char t[CLAP_NAME_SIZE];
    for (uint32_t i = first_part; i < pl->nparams; i++)
        for (uint32_t k = 0; k < first_part; k++)
            if (!strcmp(pl->params[i].name, pl->params[k].name)) {
                if (snprintf(t, sizeof t, "TRACK %s", pl->params[i].name) < (int)sizeof t)
                    memcpy(pl->params[i].name, t, sizeof t);
                break;
            }
    for (uint32_t i = first_page; i < pl->npages; i++)
        for (uint32_t k = 0; k < first_page; k++)
            if (!strcmp(pl->pages[i].name, pl->pages[k].name)) {
                if (snprintf(t, sizeof t, "TRACK %s", pl->pages[i].name) < (int)sizeof t)
                    memcpy(pl->pages[i].name, t, sizeof t);
                break;
            }
}

/* the parameters and pages: the schema's pages (four knobs each, as on the FM-1), then the engine's two EDIT pages,
 * the envelope, the LFO and the voice, each of the part's own left out where the schema shows it (its TRACK
 * parameters), the envelope where the synth has its own (ownenv, a schema's ENV pages), E1..E8 where the schema says
 * they are no panel */
static int params_make(mm_plug *pl)
{
    const mod_engine_v3 *e = &pl->img.eng;
    const mm_schema *sc = &pl->kind->sch;
    static const uint32_t ENV[] = {MM_P_ATK, MM_P_DEC, MM_P_SUS, MM_P_REL, MM_P_ED_FLT, MM_P_ED_PIT, MM_P_ED_SHP};
    static const uint32_t LFO[] = {MM_P_LRATE, MM_P_LWAVE, MM_P_LFADE, MM_P_LD_PIT, MM_P_LD_FLT, MM_P_LD_SHP,
                                   MM_P_LD_AMP, MM_P_LSYNC};
    static const uint32_t LFO2[] = {MM_P_LPHASE, MM_P_LTRIG, MM_P_LPOL};
    static const uint32_t VOICE[] = {MM_P_VOICE, MM_P_GLIDE, MM_P_GLMODE, MM_P_DETUNE, MM_P_PRIO, MM_P_ALLOC,
                                     MM_P_LEVEL};
    static const struct { const char *name; const uint32_t *ids; uint32_t n; } GROUPS[] = {
        {"ENV", ENV, 7}, {"LFO", LFO, 8}, {"LFO 2", LFO2, 3}, {"VOICE", VOICE, 7}};
    clap_id ids[CLAP_REMOTE_CONTROLS_COUNT];
    uint8_t covered[MOD3_P_COUNT] = {0};
    int own_env = e->ownenv != 0, own_lfo = 0, edit = !(sc->flags & MOD_SCH_F_NOEDIT);
    uint32_t i, n, first_part, first_page;
    pl->cparams = sc->nparam + 8u + MM_NCOMMON + 1u;
    pl->cpages = sc->npage + 2u + 4u + 1u;
    pl->params = calloc(pl->cparams, sizeof *pl->params);
    pl->pages = calloc(pl->cpages, sizeof *pl->pages);
    if (!pl->params || !pl->pages)
        return 1;
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
            param_add_sch(pl, sp, g->title, g->section);
            ids[n++] = MM_SCH_ID0 + sp->id;
        }
        page_add(pl, g->title, ids, n);
    }
    for (i = 0; i < sc->nparam; i++)             /* (shown, on no page) */
        if (sch_shown(&sc->param[i]))
            param_add_sch(pl, &sc->param[i], 0, 0);
    first_part = pl->nparams;
    first_page = pl->npages;
    for (uint32_t pg = 0; edit && pg < 2u; pg++) {   /* EDIT 1 (E1..E4), EDIT 2 (E5..E8) */
        const char *title = e->page_title[pg] ? e->page_title[pg] : pg ? "EDIT 2" : "EDIT 1";
        n = 0;
        for (uint32_t k = pg * 4u; k < pg * 4u + 4u; k++) {
            const mod_param_v3 *d = &e->edit[k];
            if (covered[MM_P_E0 + k] || !d->label || !strcmp(d->label, "-") || d->max <= d->min)
                continue;
            if (param_add(pl, MM_P_E0 + k, MM_SRC_PART, d->label, title, d->fmt, d->min, d->max, d->def, d->names,
                          d->unit))
                ids[n++] = MM_P_E0 + k;
        }
        page_add(pl, title, ids, n);
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
            if (param_add(pl, c->id, MM_SRC_PART, label, page, c->fmt, c->min, c->max, c->def, c->names, 0))
                ids[n++] = c->id;
        }
        if (g == 3u && param_add(pl, MM_P_BEND, MM_SRC_BEND, "BEND", "VOICE", MOD3_F_SEMI, 0, 24, 2, 0, 0))
            ids[n++] = MM_P_BEND;
        page_add(pl, page, ids, n);
    }
    rename_clashes(pl, first_part, first_page);
    return 0;
}

/* ---- values: the main thread's (shadow, shadow_patch) and the audio's (part) */
static int16_t value_of(const mm_param *p, double v)
{
    long x = lround(v);
    return (int16_t)(x < p->min ? p->min : x > p->max ? p->max : x);
}
/* (main thread) */
static int32_t value_get(mm_plug *pl, const mm_param *p)
{
    if (p->src == MM_SRC_BEND)
        return pl->shadow_bend;
    if (p->src == MM_SRC_PART)
        return pl->shadow[p->id];
    if (p->sp->kind == MOD3_SK_TRACK)
        return pl->shadow[p->sp->off];
    return mm_sch_get(&pl->part, pl->shadow_patch, p->sp);
}
static void value_text(mm_plug *pl, const mm_param *p, int32_t v, char *buf, uint32_t cap)
{
    if (p->src == MM_SRC_SCH)
        mm_sch_text(&pl->part, pl->shadow_patch, p->sp, v, buf, cap);
    else
        mm_format(p->fmt, v, p->min, p->max, p->names, p->unit, buf, cap);
}
/* (the audio thread, or no audio running) set as the FM-1's pages do: the patch written, the module told of an edit
 * that changed it (or one it asks to hear of), the main thread's copies after it */
static void value_set(mm_plug *pl, const mm_param *p, int32_t v)
{
    mm_part *t = &pl->part;
    const mm_sch_param *sp = p->sp;
    int changed;
    if (p->src == MM_SRC_BEND) {
        t->bend_semis = pl->shadow_bend = (int16_t)v;
        return;
    }
    if (p->src == MM_SRC_PART) {
        t->h.p[p->id] = pl->shadow[p->id] = (int16_t)v;
        return;
    }
    changed = mm_sch_put(t, t->patch, sp, v);
    if (changed || (sp->flags & MOD3_SF_NOTIFY))
        mm_patch_note(t, MOD3_PATCH_EDIT, sp->id);
    memcpy(pl->shadow, t->h.p, sizeof pl->shadow);   /* (a TRACK parameter, or what the module set) */
    memcpy(pl->shadow_patch, t->patch, pl->file->patch_len);
    if (sp->kind == MOD3_SK_CALL || (sp->flags & MOD3_SF_NOTIFY)) {   /* (it may have moved others) */
        atomic_store(&pl->rescan, 1);
        if (pl->host->request_callback)
            pl->host->request_callback(pl->host);
    }
}

/* the main thread replaced the sound (a preset, a state): the audio takes it at its next block, or now when it is
 * not running */
static void sound_put(mm_plug *pl)
{
    memcpy(pl->part.h.p, pl->shadow, sizeof pl->shadow);
    pl->part.bend_semis = pl->shadow_bend;
    mm_kill(&pl->part);                          /* a new sound: its notes stop */
    if (pl->file->patch_len) {                   /* (the module may set the part's own values from it, correct it) */
        mm_patch_set(&pl->part, pl->shadow_patch, pl->file->patch_len, MOD3_PATCH_NEW);
        memcpy(pl->shadow, pl->part.h.p, sizeof pl->shadow);
        memcpy(pl->shadow_patch, pl->part.patch, pl->file->patch_len);
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
static void sound_take(mm_plug *pl)
{
    if (atomic_exchange(&pl->pending, 0))
        sound_put(pl);
}

/* a sound: the part's own defaults, then a common block (c; 0: none), as the device loads a sound; its patch (n bytes)
 * over the schema's default patch */
static void sound_apply(mm_plug *pl, const uint8_t *c, const uint8_t *patch, uint32_t n)
{
    if (c)
        mm_preset(&pl->img.eng, pl->shadow, (const int8_t *)c, c + 8, (int8_t)c[12], c[13]);
    else
        mm_defaults(&pl->img.eng, pl->shadow);
    pl->shadow_bend = 2;
    memset(pl->shadow_patch, 0, sizeof pl->shadow_patch);
    if (pl->file->patch_len) {
        memcpy(pl->shadow_patch, pl->file->patch_def, pl->file->patch_len);
        memcpy(pl->shadow_patch, patch, n < pl->file->patch_len ? n : pl->file->patch_len);
    }
    sound_commit(pl);
}
/* factory sound k of the file */
static int preset_apply(mm_plug *pl, uint32_t k)
{
    uint8_t c[MM_PST_COMMON], patch[MOD3_PATCH_MAX];
    uint32_t n = 0;
    if (mm_file_sound(pl->file, k, 0, 0, c, patch, sizeof patch, &n))
        return 1;
    sound_apply(pl, c, patch, n);
    return 0;
}

/* ---- SysEx files: the .syx files in the SysEx folder beside the plug-in, each given to a module as the FM-1 gives it
 * SysEx that came; the sounds it offers for user slots are presets (load key "<module ID>/<index>", its offers in the
 * file in order) */
#define MM_SYX_MAX (1u << 20)                    /* (a file, at most: a DX7 bank is 4 KB, a PPG bank 10 KB) */
#define MM_MAX_SYX 512u
static char sx_dir[4096];                        /* (init's) */
static uint8_t *syx_read(const char *path, uint32_t *n)   /* the whole file, 0: none or too big */
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
/* a sound's name in its patch (its schema's), printable, the spaces after it left out: "" none */
static void patch_name(const mm_schema *sc, const uint8_t *patch, uint32_t len, char *buf, uint32_t cap)
{
    uint32_t k = 0;
    for (uint32_t i = 0; sc->nlen && i < sc->nlen && sc->name + i < len && k + 1u < cap; i++)
        buf[k++] = patch[sc->name + i] < 32u || patch[sc->name + i] > 126u ? ' ' : (char)patch[sc->name + i];
    while (k && buf[k - 1u] == ' ')
        k--;
    buf[k] = 0;
}
/* sound k of key "<module ID>/<k>" for kind m: 0, else -1 */
static int sx_key(const mm_kind *m, const char *key, uint32_t *k)
{
    size_t n = strlen(m->idhex);
    char *end;
    unsigned long v;
    if (!key || strncmp(key, m->idhex, n) || key[n] != '/')
        return -1;
    v = strtoul(key + n + 1, &end, 10);
    if (end == key + n + 1 || *end)
        return -1;
    *k = (uint32_t)v;
    return 0;
}
/* the plug-in's sound want of a file (the sounds its own module offers, in order) */
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
static int sx_load(mm_plug *pl, const char *path, const char *key)
{
    uint8_t c[MM_PST_COMMON], first[MOD3_PATCH_MAX], *data;
    uint32_t n = 0, flen = 0;
    sx_pick p = {0};
    if (!pl->kind->sch.nsysex || !pl->img.eng.sysex || sx_key(pl->kind, key, &p.want) || !(data = syx_read(path, &n)))
        return -1;
    mm_sysex_file(&pl->part, &pl->kind->sch, data, n, sx_pick_take, &p, 0, 0);   /* (as the FM-1: beside the audio) */
    free(data);
    if (p.n <= p.want)
        return -1;
    /* the part's own values: the file's first sound's (its INIT), as a user slot's on the device start from */
    sound_apply(pl, pl->file->npresets && !mm_file_sound(pl->file, 0, 0, 0, c, first, sizeof first, &flen) ? c : 0,
                p.patch, p.len);
    return 0;
}

/* ---- the plug-in's calls */
static bool p_init(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    pl->host_params = pl->host->get_extension(pl->host, CLAP_EXT_PARAMS);
    return true;
}
static void p_destroy(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    mm_part_close(&pl->part);
    mm_image_free(&pl->img);
    free(pl->params);
    free(pl->pages);
    free(pl);
}
static bool p_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    mm_plug *pl = plug_of(plugin);
    (void)min_frames, (void)max_frames;
    pl->rate = rate;
    mm_rs_init(&pl->rs, MM_FS, rate);
    pl->tpos = MM_CTL;
    sound_take(pl);
    atomic_store(&pl->active, 1);
    return true;
}
static void p_deactivate(const clap_plugin_t *plugin) { atomic_store(&plug_of(plugin)->active, 0); }
static bool p_start(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void p_stop(const clap_plugin_t *plugin) { (void)plugin; }
static void p_reset(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    mm_kill(&pl->part);
    mm_rs_reset(&pl->rs);
    pl->tpos = MM_CTL;
}

/* the module's next 44.1 kHz sample */
static void pull(void *ctx, float *l, float *r)
{
    mm_plug *pl = ctx;
    if (pl->tpos >= MM_CTL) {
        int st = mm_tick(&pl->part, pl->tick), lv = pl->part.h.p[MM_P_LEVEL];
        /* the device's mixer: LEVEL ((v - 112) / 2 dB), a mono part on both sides as it is, a stereo one (host 5)
         * its left side and out - left */
        const float k = lv > 0 ? powf(10.0f, (float)(lv - 112) / 40.0f) / 32768.0f : 0.0f;
        if (st)
            for (uint32_t i = 0; i < MM_CTL; i++) {
                pl->tl[i] = k * (float)pl->part.left[i];
                pl->tr[i] = k * (float)(pl->tick[i] - pl->part.left[i]);
            }
        else
            for (uint32_t i = 0; i < MM_CTL; i++)
                pl->tl[i] = pl->tr[i] = k * (float)pl->tick[i];
        pl->tpos = 0;
    }
    *l = pl->tl[pl->tpos];
    *r = pl->tr[pl->tpos];
    pl->tpos++;
}

static void midi_in(mm_plug *pl, const uint8_t *b)
{
    mm_part *t = &pl->part;
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
        t->h.at = (uint8_t)d1;
        break;
    case 0xA0:
        t->h.at = (uint8_t)d2;
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
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t *n = (const void *)h;
        long v = lround(n->velocity * 127.0);
        if (n->key >= 0 && n->key < 128)
            mm_note_on(&pl->part, (uint32_t)n->key, (uint32_t)(v < 1 ? 1 : v > 127 ? 127 : v));
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const clap_event_note_t *n = (const void *)h;
        if (n->key >= 0 && n->key < 128)
            mm_note_off(&pl->part, (uint32_t)n->key);
        else
            mm_all_off(&pl->part);
        break;
    }
    case CLAP_EVENT_MIDI:
        midi_in(pl, ((const clap_event_midi_t *)(const void *)h)->data);
        break;
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t *e = (const void *)h;
        const mm_param *p = param_find(pl, e->param_id);
        if (p)
            value_set(pl, p, value_of(p, e->value));
        break;
    }
    case CLAP_EVENT_TRANSPORT: {
        const clap_event_transport_t *tr = (const void *)h;
        if (tr->flags & CLAP_TRANSPORT_HAS_TEMPO)
            pl->part.bpm = tr->tempo;
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
    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO))
        pl->part.bpm = pr->transport->tempo;
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
    return mm_live(&pl->part) ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
}

/* ---- params */
static uint32_t pa_count(const clap_plugin_t *plugin) { return plug_of(plugin)->nparams; }
static bool pa_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    mm_plug *pl = plug_of(plugin);
    const mm_param *p;
    if (index >= pl->nparams)
        return false;
    p = &pl->params[index];
    memset(info, 0, sizeof *info);
    info->id = p->id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | (p->fmt == MOD3_F_ENUM ? CLAP_PARAM_IS_ENUM : 0);
    info->cookie = 0;
    snprintf(info->name, sizeof info->name, "%s", p->name);
    snprintf(info->module, sizeof info->module, "%s", p->module);
    info->min_value = p->min;
    info->max_value = p->max;
    info->default_value = p->def;
    return true;
}
static bool pa_value(const clap_plugin_t *plugin, clap_id id, double *value)
{
    mm_plug *pl = plug_of(plugin);
    const mm_param *p = param_find(pl, id);
    if (!p)
        return false;
    *value = value_get(pl, p);
    return true;
}
static bool pa_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *buf, uint32_t cap)
{
    mm_plug *pl = plug_of(plugin);
    const mm_param *p = param_find(pl, id);
    if (!p || !cap)
        return false;
    value_text(pl, p, value_of(p, value), buf, cap);
    return true;
}
static bool pa_from_text(const clap_plugin_t *plugin, clap_id id, const char *text, double *value)
{
    mm_plug *pl = plug_of(plugin);
    const mm_param *p = param_find(pl, id);
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
    for (uint32_t k = 0; k < n; k++) {
        const clap_event_header_t *h = in->get(in, k);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t *e = (const void *)h;
            const mm_param *p = param_find(pl, e->param_id);
            if (p)
                value_set(pl, p, value_of(p, e->value));
        }
    }
}
static const clap_plugin_params_t PARAMS = {pa_count, pa_info, pa_value, pa_to_text, pa_from_text, pa_flush};

/* ---- state: "MMOD", version, the slug, the part's parameters, the bend range, the patch */
typedef struct {
    uint32_t magic, version;
    uint8_t id[8];                               /* the module's ID */
    int16_t p[MOD3_P_COUNT];
    int16_t bend;
    uint16_t patch_len;                          /* the patch's bytes follow */
} mm_state_head;

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
static bool st_save(const clap_plugin_t *plugin, const clap_ostream_t *s)
{
    mm_plug *pl = plug_of(plugin);
    mm_state_head h;
    memset(&h, 0, sizeof h);
    h.magic = MM_STATE_MAGIC;
    h.version = MM_STATE_VERSION;
    memcpy(h.id, pl->file->id, sizeof h.id);
    memcpy(h.p, pl->shadow, sizeof h.p);
    h.bend = pl->shadow_bend;
    h.patch_len = (uint16_t)pl->file->patch_len;
    return write_all(s, &h, sizeof h) && write_all(s, pl->shadow_patch, h.patch_len);
}
static bool st_load(const clap_plugin_t *plugin, const clap_istream_t *s)
{
    mm_plug *pl = plug_of(plugin);
    mm_state_head h;
    uint8_t patch[MOD3_PATCH_MAX];
    if (!read_all(s, &h, sizeof h) || h.magic != MM_STATE_MAGIC || h.version != MM_STATE_VERSION ||
        memcmp(h.id, pl->file->id, sizeof h.id) || h.patch_len > MOD3_PATCH_MAX || !read_all(s, patch, h.patch_len))
        return false;
    memcpy(pl->shadow, h.p, sizeof pl->shadow);
    for (uint32_t i = 0; i < pl->nparams; i++) {   /* (the part's own it shows: in their ranges) */
        const mm_param *p = &pl->params[i];
        if (p->src == MM_SRC_BEND)
            pl->shadow_bend = value_of(p, h.bend);
        else if (p->src == MM_SRC_PART)
            pl->shadow[p->id] = value_of(p, h.p[p->id]);
    }
    if (pl->file->patch_len) {                   /* (a shorter patch saved: the rest as the schema starts it) */
        memcpy(pl->shadow_patch, pl->file->patch_def, pl->file->patch_len);
        memcpy(pl->shadow_patch, patch, h.patch_len < pl->file->patch_len ? h.patch_len : pl->file->patch_len);
    }
    sound_commit(pl);
    if (pl->host_params)
        pl->host_params->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES);
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

/* ---- pages */
static uint32_t rc_count(const clap_plugin_t *plugin) { return plug_of(plugin)->npages; }
static bool rc_get(const clap_plugin_t *plugin, uint32_t index, clap_remote_controls_page_t *page)
{
    mm_plug *pl = plug_of(plugin);
    if (index >= pl->npages)
        return false;
    memset(page, 0, sizeof *page);
    snprintf(page->section_name, sizeof page->section_name, "%s", pl->kind->name);
    snprintf(page->page_name, sizeof page->page_name, "%s", pl->pages[index].name);
    page->page_id = index;
    memcpy(page->param_ids, pl->pages[index].ids, sizeof page->param_ids);
    page->is_for_preset = false;
    return true;
}
static const clap_plugin_remote_controls_t REMOTE = {rc_count, rc_get};

/* ---- factory sounds (the plug-in location) and SysEx files' sounds (a file location): load key "<module ID>/<index>" */
static bool pl_from_location(const clap_plugin_t *plugin, uint32_t kind, const char *location, const char *key)
{
    mm_plug *pl = plug_of(plugin);
    uint32_t k;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) {
        if (!location || sx_load(pl, location, key))
            return false;
    } else if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || sx_key(pl->kind, key, &k) || k >= pl->file->npresets ||
               preset_apply(pl, k)) {
        return false;
    }
    if (pl->host_params)
        pl->host_params->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES);
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
    return 0;
}
static void p_on_main(const clap_plugin_t *plugin)
{
    mm_plug *pl = plug_of(plugin);
    if (atomic_exchange(&pl->rescan, 0) && pl->host_params)
        pl->host_params->rescan(pl->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
}

/* ---- the plug-in factory */
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return nkinds; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i)
{
    (void)f;
    return i < nkinds ? &kinds[i].d : 0;
}
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    (void)f;
    if (!clap_version_is_compatible(host->clap_version))
        return 0;
    for (uint32_t i = 0; i < nkinds; i++) {
        mm_plug *pl;
        const char *err;
        if (strcmp(id, kinds[i].id))
            continue;
        pl = calloc(1, sizeof *pl);
        if (!pl)
            return 0;
        pl->host = host;
        pl->kind = &kinds[i];
        pl->file = &kinds[i].file;
        if ((err = mm_image_load(&pl->img, pl->file->aa64, pl->file->aa64_len)) ||
            mm_part_open(&pl->part, pl->img.desc, &pl->img.eng)) {
            fprintf(stderr, "melodee-modules: %s: %s\n", kinds[i].name, err ? err : "the module refused the host");
            mm_image_free(&pl->img);
            free(pl);
            return 0;
        }
        if (params_make(pl)) {
            mm_part_close(&pl->part);
            mm_image_free(&pl->img);
            free(pl->params);
            free(pl->pages);
            free(pl);
            return 0;
        }
        memcpy(pl->shadow, pl->part.h.p, sizeof pl->shadow);
        pl->shadow_bend = (int16_t)pl->part.bend_semis;
        if (pl->file->patch_len)
            memcpy(pl->shadow_patch, pl->file->patch_def, pl->file->patch_len);
        if (!pl->file->npresets || preset_apply(pl, pl->file->first))   /* a new part: the file's first sound */
            sound_commit(pl);
        pl->rate = MM_FS;
        mm_rs_init(&pl->rs, MM_FS, MM_FS);
        pl->tpos = MM_CTL;
        pl->plugin.desc = &kinds[i].d;
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
        return &pl->plugin;
    }
    return 0;
}
static const clap_plugin_factory_t PLUGIN_FACTORY = {f_count, f_desc, f_create};

/* ---- the preset discovery factory: one provider, the factory sounds of every module, then a location for each SysEx
 * file (Melodee's PRESETS: a folder each, by the file's name) */
static const clap_preset_discovery_provider_descriptor_t PROVIDER_DESC = {
    CLAP_VERSION_INIT, MM_ID_PREFIX "presets", "Melodee modules", MM_VENDOR};

static int name_order(const void *a, const void *b)   /* (letters without case, numbers by value: 2 before 10) */
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
static bool pv_init(const clap_preset_discovery_provider_t *pv)
{
    const clap_preset_discovery_indexer_t *ix = pv->provider_data;
    clap_preset_discovery_location_t loc = {CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory",
                                            CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0};
    clap_preset_discovery_filetype_t ft = {"SysEx", "a synth module's SysEx (its sounds)", "syx"};
    char *name[MM_MAX_SYX];
    uint32_t nname = 0, takes = 0;
    struct dirent *de;
    DIR *dp;
    if (!ix->declare_location(ix, &loc))
        return false;
    for (uint32_t i = 0; i < nkinds; i++)
        takes |= kinds[i].sch.nsysex != 0;
    if (!takes || !sx_dir[0] || !(dp = opendir(sx_dir)))
        return true;                             /* (no module takes SysEx, or no SysEx folder) */
    ix->declare_filetype(ix, &ft);
    while ((de = readdir(dp)) && nname < MM_MAX_SYX) {
        size_t n = strlen(de->d_name);
        if (de->d_name[0] != '.' && n > 4 && !strcasecmp(de->d_name + n - 4, ".syx") &&
            (name[nname] = strdup(de->d_name)))
            nname++;
    }
    closedir(dp);
    qsort(name, nname, sizeof name[0], name_order);
    for (uint32_t f = 0; f < nname; f++) {
        char path[4096], stem[256];
        clap_preset_discovery_location_t file = {0, stem, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path};
        snprintf(stem, sizeof stem, "%.*s", (int)strlen(name[f]) - 4, name[f]);
        if (snprintf(path, sizeof path, "%s/%s", sx_dir, name[f]) < (int)sizeof path)
            ix->declare_location(ix, &file);
        free(name[f]);
    }
    return true;
}
static void pv_destroy(const clap_preset_discovery_provider_t *pv) { free((void *)(uintptr_t)pv); }
/* a SysEx file's sounds: each module whose SysEx it holds, given it in a copy of its own (a part of its own, as a new
 * instance) */
typedef struct sx_list {
    const mm_kind *m;
    const clap_preset_discovery_metadata_receiver_t *rx;
    uint32_t n;
    int stop;
} sx_list;
static void sx_list_take(void *ctx, const uint8_t *patch, uint32_t len)
{
    sx_list *l = ctx;
    clap_universal_plugin_id_t uid = {"clap", l->m->id};
    char name[64], key[40];
    if (l->stop)
        return;
    patch_name(&l->m->sch, patch, len, name, sizeof name);
    if (!name[0])
        snprintf(name, sizeof name, "%s %u", l->m->name, (unsigned)l->n + 1u);
    snprintf(key, sizeof key, "%s/%u", l->m->idhex, (unsigned)l->n++);
    if (!l->rx->begin_preset(l->rx, name, key)) {
        l->stop = 1;
        return;
    }
    l->rx->add_plugin_id(l->rx, &uid);
    l->rx->set_flags(l->rx, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
}
static bool sx_metadata(const char *path, const clap_preset_discovery_metadata_receiver_t *rx)
{
    uint32_t n = 0;
    uint8_t *data = syx_read(path, &n);
    if (!data)
        return false;
    for (uint32_t i = 0; i < nkinds; i++) {
        const mm_kind *m = &kinds[i];
        uint32_t at = 0, his = 0;
        mm_image img;
        mm_part *t;
        sx_list l = {m, rx, 0, 0};
        char msg[64];
        for (; at < n && !his; at++)             /* (a frame of its SysEx in the file: else its code is not mapped) */
            his = data[at] == 0xF0u && mm_schema_sysex(&m->sch, data + at, n - at);
        if (!his || !m->file.aa64)
            continue;
        if (mm_image_load(&img, m->file.aa64, m->file.aa64_len)) {
            mm_image_free(&img);
            continue;
        }
        if ((t = calloc(1, sizeof *t)) && !mm_part_open(t, img.desc, &img.eng)) {
            if (img.eng.sysex && !mm_sysex_file(t, &m->sch, data, n, sx_list_take, &l, msg, sizeof msg) && msg[0] &&
                rx->on_error)
                rx->on_error(rx, 0, msg);        /* (none taken: what the module says, "INVALID PPG BANK") */
            mm_part_close(t);
        }
        free(t);
        mm_image_free(&img);
        if (l.stop)
            break;
    }
    free(data);
    return true;
}
static bool pv_metadata(const clap_preset_discovery_provider_t *pv, uint32_t kind, const char *location,
                        const clap_preset_discovery_metadata_receiver_t *rx)
{
    (void)pv;
    if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE)
        return location && sx_metadata(location, rx);
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        return false;
    for (uint32_t i = 0; i < nkinds; i++) {
        const mm_kind *m = &kinds[i];
        clap_universal_plugin_id_t uid = {"clap", m->id};
        for (uint32_t k = 0; k < m->file.npresets; k++) {
            char name[MM_PST_NAME + 1], key[40];
            if (mm_file_sound(&m->file, k, name, 0, 0, 0, 0, 0))
                continue;
            snprintf(key, sizeof key, "%s/%u", m->idhex, k);
            if (!rx->begin_preset(rx, name, key))
                return true;
            rx->add_plugin_id(rx, &uid);
            rx->set_flags(rx, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
        }
    }
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
    return true;
}
static void e_deinit(void)
{
    for (uint32_t i = 0; i < nkinds; i++) {
        mm_schema_free(&kinds[i].sch);
        mm_file_free(&kinds[i].file);
    }
    free(kinds);
    kinds = 0;
    nkinds = 0;
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
