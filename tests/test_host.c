/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The plug-in as a host loads it (dlopen, clap_entry), with module files in the Modules folder beside it (make test:
 * tests/fixtures): one plug-in, its synths a preset location each (a broken file and a second copy left out), the
 * parameters, notes as CLAP and as MIDI in blocks of 1..128 frames at 44.1, 48 and 96 kHz, pitch and bend, the release
 * to silence, state, the factory sounds through preset discovery and preset load, every synth and every sound played,
 * another synth loaded while it plays, two parts at once, SysEx files' sounds under their synths (MM_PPG_BANK=file: a
 * PPG Wave 2.2 factory bank against the factory programs).
 *   test_host DIR/melodee-modules.clap     (MM_WAV_DIR=dir: each module's sounds as WAVs there)
 *   test_host --one DIR/melodee-modules.clap          (one module file alone in DIR/Modules: it lists, loads, plays)
 *   test_host --staged A/melodee-modules.clap B/...   (two copies, each dlopen'd on its own beside a linked Modules
 *                                                       folder, as Melodee stages one per rack entry)
 *   test_host --list DIR/melodee-modules.clap         (each synth's pages: their parameters, values as text)
 *   test_host --screen DIR/melodee-modules.clap OUT STEP..   (its page in Melodee, drawn after each step: OUT/NAME.ppm
 *       at snap:NAME; steps wheel:N swheel:N (SHIFT) push minus plus note:KEY off k1:V k2:V k3:V (a pot to V, 0..127,
 *       driving its value) pot1:V .. pot3:V (a pot at V short of its value) colour:RRGGBB) */
#include <clap/clap.h>
#include <stdatomic.h>
#include <unistd.h>
#include "melodee_screen.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int fails;
static int have_ppg;                             /* the PPG module beside the others (its file stays out of the repository:
                                                  * PPG's waves and programs are PPG's) */
/* a module file changed after signing, made whole again (its total and CRC): what only its signature tells */
static uint32_t t_le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void t_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void t_whole(uint8_t *b, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 60; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++)
            c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    t_put32(b + 8, (uint32_t)n);
    t_put32(b + 28, ~c);
}
static size_t t_chunk(const uint8_t *b, size_t n, uint32_t tag)   /* its chunk head's place, 0 none */
{
    for (size_t at = 60u + t_le32(b + 12) + 4u * t_le32(b + 20); at + 8u <= n; at += 8u + ((t_le32(b + at + 4) + 3u) & ~3u))
        if (t_le32(b + at) == tag)
            return at;
    return 0;
}
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- a host */
static int rescans;
static void hp_rescan(const clap_host_t *h, clap_param_rescan_flags f) { (void)h, (void)f; rescans++; }
static void hp_clear(const clap_host_t *h, clap_id id, clap_param_clear_flags f) { (void)h, (void)id, (void)f; }
static void hp_flush(const clap_host_t *h) { (void)h; }
static const clap_host_params_t HOST_PARAMS = {hp_rescan, hp_clear, hp_flush};
static int pages_changed;                       /* (remote controls' changed()) */
static void hr_changed(const clap_host_t *h) { (void)h; pages_changed++; }
static void hr_suggest(const clap_host_t *h, clap_id id) { (void)h, (void)id; }
static const clap_host_remote_controls_t HOST_RC = {hr_changed, hr_suggest};
static int redraws;                              /* (the screen's redraw()) */
static void hs_redraw(const void *h, uint32_t flags) { (void)h, (void)flags; redraws++; }
static const melodee_screen_host_t HOST_SCREEN = {hs_redraw};
static const void *h_ext(const clap_host_t *h, const char *id)
{
    (void)h;
    return !strcmp(id, CLAP_EXT_PARAMS) ? (const void *)&HOST_PARAMS
         : !strcmp(id, CLAP_EXT_REMOTE_CONTROLS) ? (const void *)&HOST_RC
         : !strcmp(id, MELODEE_SCREEN_CLAP_HOST) ? (const void *)&HOST_SCREEN : 0;
}
static void h_nop(const clap_host_t *h) { (void)h; }
static atomic_int restarts;                      /* (request_restart(), from the plug-in's folder watch) */
static void h_restart(const clap_host_t *h) { (void)h; atomic_fetch_add(&restarts, 1); }
static const clap_host_t HOST = {CLAP_VERSION_INIT, 0, "test", "melodee-modules", 0, "1", h_ext, h_restart, h_nop, h_nop};

/* ---- events */
typedef struct { union { clap_event_header_t h; clap_event_note_t n; clap_event_midi_t m; clap_event_param_value_t p; } e[64]; uint32_t n; } evlist;
static uint32_t ev_size(const clap_input_events_t *l) { return ((const evlist *)l->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) { return &((const evlist *)l->ctx)->e[i].h; }
static void ev_note(evlist *l, uint16_t type, uint32_t time, int key, double vel)
{
    clap_event_note_t *n = &l->e[l->n++].n;
    memset(n, 0, sizeof *n);
    n->header = (clap_event_header_t){sizeof *n, time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    n->note_id = -1, n->port_index = 0, n->channel = 0, n->key = (int16_t)key, n->velocity = vel;
}
static void ev_midi(evlist *l, uint32_t time, uint8_t a, uint8_t b, uint8_t c)
{
    clap_event_midi_t *m = &l->e[l->n++].m;
    m->header = (clap_event_header_t){sizeof *m, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
    m->port_index = 0, m->data[0] = a, m->data[1] = b, m->data[2] = c;
}
static void ev_param(evlist *l, uint32_t time, clap_id id, double v)
{
    clap_event_param_value_t *p = &l->e[l->n++].p;
    memset(p, 0, sizeof *p);
    p->header = (clap_event_header_t){sizeof *p, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    p->param_id = id, p->note_id = -1, p->port_index = -1, p->channel = -1, p->key = -1, p->value = v;
}
static bool out_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l, (void)e; return true; }
static const clap_output_events_t OUT_EVENTS = {0, out_push};

/* ---- a plug-in under test */
typedef struct {
    const clap_plugin_t *p;
    const clap_plugin_params_t *params;
    const clap_plugin_state_t *state;
    const clap_plugin_preset_load_t *preset;
    const clap_plugin_remote_controls_t *rc;
    const clap_plugin_audio_ports_t *ap;
    const clap_plugin_note_ports_t *np;
    const clap_plugin_latency_t *lat;
    double rate;
    uint64_t steady;
} plug;

static const clap_plugin_factory_t *factory;
static float L[96000 * 12], R[96000 * 12];
static const char *wavdir;
static const char *want_key;                     /* the synth whose sounds discovery keeps (found_*): its module ID */
#define PLUGIN_ID "studio.ellic.melodee-modules"
#define TRIO_KEY "c273261a2cf92788"
#define HYDRA_KEY "85bf54aaf90d0ae6"
#define PPG_KEY "3627d34f43eafbf8"

/* the plug-in, playing synth key's sound 0 when key is given (else the synth a new part has) */
static int open_plug(plug *x, const char *key, double rate)
{
    memset(x, 0, sizeof *x);
    x->p = factory->create_plugin(factory, &HOST, PLUGIN_ID);
    if (!x->p || !x->p->init(x->p))
        return 1;
    x->params = x->p->get_extension(x->p, CLAP_EXT_PARAMS);
    x->state = x->p->get_extension(x->p, CLAP_EXT_STATE);
    x->preset = x->p->get_extension(x->p, CLAP_EXT_PRESET_LOAD);
    x->rc = x->p->get_extension(x->p, CLAP_EXT_REMOTE_CONTROLS);
    x->ap = x->p->get_extension(x->p, CLAP_EXT_AUDIO_PORTS);
    x->np = x->p->get_extension(x->p, CLAP_EXT_NOTE_PORTS);
    x->lat = x->p->get_extension(x->p, CLAP_EXT_LATENCY);
    x->rate = rate;
    if (!x->p->activate(x->p, rate, 1, 128) || !x->p->start_processing(x->p))
        return 1;
    if (key) {
        char k[40];
        snprintf(k, sizeof k, "%s/0", key);
        if (!x->preset->from_location(x->p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, k))
            return 1;
    }
    return 0;
}
/* the synth it plays: its parameters' tag (its first page's first id), its own id local as the plug-in's */
static uint32_t tag_of(plug *x)
{
    clap_remote_controls_page_t pg;
    return x->rc->get(x->p, 0, &pg) ? pg.param_ids[0] >> 18 : 0;
}
static clap_id P(plug *x, uint32_t local) { return tag_of(x) << 18 | local; }
static void close_plug(plug *x)
{
    x->p->stop_processing(x->p);
    x->p->deactivate(x->p);
    x->p->destroy(x->p);
}
/* frames into l / r in blocks of 1..128 (as Melodee's steps cut them), the events at the first block's frames */
static void run(plug *x, evlist *ev, float *l, float *r, uint32_t frames)
{
    static uint32_t rng = 12345;
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n;
        float *ch[2];
        clap_audio_buffer_t out;
        clap_process_t pr;
        evlist none = {0};
        clap_input_events_t in;
        rng = rng * 1664525u + 1013904223u;
        n = 1u + (rng >> 25);                    /* 1..128 */
        if (n > frames - done)
            n = frames - done;
        if (ev && !done) {                       /* the events in the first block (which then holds them all) */
            uint32_t last = 0;
            for (uint32_t i = 0; i < ev->n; i++)
                last = ev->e[i].h.time > last ? ev->e[i].h.time : last;
            if (n <= last)
                n = last + 1u < frames ? last + 1u : frames;
        }
        ch[0] = l + done, ch[1] = r + done;
        memset(&out, 0, sizeof out);
        out.data32 = ch, out.channel_count = 2;
        in.ctx = ev && !done ? ev : &none, in.size = ev_size, in.get = ev_get;
        memset(&pr, 0, sizeof pr);
        pr.steady_time = (int64_t)x->steady, pr.frames_count = n, pr.audio_outputs = &out, pr.audio_outputs_count = 1;
        pr.in_events = &in, pr.out_events = &OUT_EVENTS;
        CHECK(x->p->process(x->p, &pr) != CLAP_PROCESS_ERROR, "process");
        x->steady += n;
        done += n;
    }
}
static double peak(const float *a, uint32_t n)
{
    double m = 0;
    for (uint32_t i = 0; i < n; i++)
        m = fabs(a[i]) > m ? fabs(a[i]) : m;
    return m;
}
static double rms(const float *a, uint32_t n)
{
    double s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += (double)a[i] * a[i];
    return n ? sqrt(s / n) : 0;
}
static int finite_all(const float *a, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (!isfinite(a[i]))
            return 0;
    return 1;
}
/* the frequency of a steady tone: rising zero crossings over the span, interpolated */
static double freq(const float *a, uint32_t n, double rate)
{
    double first = -1, last = -1;
    uint32_t count = 0;
    for (uint32_t i = 1; i < n; i++)
        if (a[i - 1] < 0 && a[i] >= 0) {
            double t = (i - 1) + a[i - 1] / (a[i - 1] - a[i]);
            if (first < 0)
                first = t;
            else
                count++;
            last = t;
        }
    return count ? count * rate / (last - first) : 0;
}
static clap_id param_id(plug *x, const char *name)   /* the synth it plays */
{
    uint32_t n = x->params->count(x->p), tag = tag_of(x);
    for (uint32_t i = 0; i < n; i++) {
        clap_param_info_t info;
        x->params->get_info(x->p, i, &info);
        if (info.id >> 18 == tag && !strcmp(info.name, name))
            return info.id;
    }
    return CLAP_INVALID_ID;
}
static double value(plug *x, clap_id id)
{
    double v = -999;
    x->params->get_value(x->p, id, &v);
    return v;
}

/* ---- state streams */
typedef struct { uint8_t b[4096]; uint64_t n, at; } blob;
static int64_t os_write(const clap_ostream_t *s, const void *buf, uint64_t n)
{
    blob *b = s->ctx;
    if (b->n + n > sizeof b->b)
        return -1;
    memcpy(b->b + b->n, buf, n);
    b->n += n;
    return (int64_t)n;
}
static int64_t is_read(const clap_istream_t *s, void *buf, uint64_t n)
{
    blob *b = s->ctx;
    uint64_t k = b->n - b->at < n ? b->n - b->at : n;
    memcpy(buf, b->b + b->at, k);
    b->at += k;
    return (int64_t)k;
}

/* ---- preset discovery */
static int locations, nfound, with_id;
static char cur_key[40], cur_name[40];
static char found_key[8][40], found_name[8][40];   /* want_key's synth's, in order */
static bool ix_filetype(const clap_preset_discovery_indexer_t *ix, const clap_preset_discovery_filetype_t *f) { (void)ix, (void)f; return true; }
static char loc_name[32][256], loc_path[32][4096];   /* (every location a file: a module file or a SysEx file) */
static bool ix_location(const clap_preset_discovery_indexer_t *ix, const clap_preset_discovery_location_t *l)
{
    (void)ix;
    CHECK(l->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && l->location && l->name, "a file location");
    if (locations < 32 && l->location && l->name) {
        snprintf(loc_name[locations], sizeof loc_name[locations], "%s", l->name);
        snprintf(loc_path[locations], sizeof loc_path[locations], "%s", l->location);
    }
    locations++;
    return true;
}
static bool ix_pack(const clap_preset_discovery_indexer_t *ix, const clap_preset_discovery_soundpack_t *s) { (void)ix, (void)s; return true; }
static const void *ix_ext(const clap_preset_discovery_indexer_t *ix, const char *id) { (void)ix, (void)id; return 0; }
static void rx_error(const clap_preset_discovery_metadata_receiver_t *r, int32_t e, const char *m) { (void)r, (void)e; printf("preset error: %s\n", m); fails++; }
static bool rx_begin(const clap_preset_discovery_metadata_receiver_t *r, const char *name, const char *key)
{
    (void)r;
    snprintf(cur_name, sizeof cur_name, "%s", name);
    snprintf(cur_key, sizeof cur_key, "%s", key ? key : "");
    nfound++;
    return true;
}
static void rx_id(const clap_preset_discovery_metadata_receiver_t *r, const clap_universal_plugin_id_t *id)
{
    (void)r;
    CHECK(!strcmp(id->abi, "clap") && !strcmp(id->id, PLUGIN_ID), "a sound of %s", id->id);
    if (!want_key || !strncmp(cur_key, want_key, strlen(want_key))) {
        if (with_id < 8) {
            snprintf(found_name[with_id], 40, "%s", cur_name);
            snprintf(found_key[with_id], 40, "%s", cur_key);
        }
        with_id++;
    }
}
static void rx_pack(const clap_preset_discovery_metadata_receiver_t *r, const char *s) { (void)r, (void)s; }
static void rx_flags(const clap_preset_discovery_metadata_receiver_t *r, uint32_t f) { (void)r, (void)f; }
static void rx_str(const clap_preset_discovery_metadata_receiver_t *r, const char *s) { (void)r, (void)s; }
static void rx_time(const clap_preset_discovery_metadata_receiver_t *r, clap_timestamp a, clap_timestamp b) { (void)r, (void)a, (void)b; }
static void rx_extra(const clap_preset_discovery_metadata_receiver_t *r, const char *k, const char *v) { (void)r, (void)k, (void)v; }

/* ---- WAV */
static void wav(const char *path, const float *l, const float *r, uint32_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t data = n * 4u, u;
    uint16_t s;
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f), u = 36u + data, fwrite(&u, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f);
    u = 16, fwrite(&u, 4, 1, f), s = 1, fwrite(&s, 2, 1, f), s = 2, fwrite(&s, 2, 1, f), fwrite(&rate, 4, 1, f);
    u = rate * 4u, fwrite(&u, 4, 1, f), s = 4, fwrite(&s, 2, 1, f), s = 16, fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) {
        float v[2] = {l[i], r[i]};
        for (int c = 0; c < 2; c++) {
            float x = v[c] > 1 ? 1 : v[c] < -1 ? -1 : v[c];
            int16_t q = (int16_t)lrintf(x * 32767.0f);
            fwrite(&q, 2, 1, f);
        }
    }
    fclose(f);
}

#define SEC(x) ((uint32_t)((x) * rate))
/* the synths as discovery has them: each module file's location (no SysEx files beside: its name the synth's) */
static int nsyn;
static char syn_name[8][64], syn_key[8][17], syn_path[8][4096];
static void synths(const clap_plugin_entry_t *e)
{
    const clap_preset_discovery_factory_t *pf = e->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
    clap_preset_discovery_indexer_t ix = {CLAP_VERSION_INIT, "test", "test", 0, "1", 0, ix_filetype, ix_location, ix_pack, ix_ext};
    clap_preset_discovery_metadata_receiver_t rx = {0, rx_error, rx_begin, rx_id, rx_pack, rx_flags, rx_str, rx_str, rx_time, rx_str, rx_extra};
    const clap_preset_discovery_provider_t *pv = pf ? pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id) : 0;
    nsyn = locations = 0;
    CHECK(pv && pv->init(pv), "discovery");
    for (int i = 0; pv && i < locations && i < 32 && nsyn < 8; i++) {
        size_t n = strlen(loc_path[i]);
        if (n < 5 || strcmp(loc_path[i] + n - 5, ".mmod"))
            continue;
        want_key = 0;
        nfound = with_id = 0;
        CHECK(pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[i], &rx) && nfound > 0 &&
              nfound == with_id && strchr(found_key[0], '/'), "%s: its sounds (%d)", loc_name[i], nfound);
        snprintf(syn_name[nsyn], sizeof syn_name[nsyn], "%.63s", loc_name[i]);
        snprintf(syn_key[nsyn], sizeof syn_key[nsyn], "%.16s", found_key[0]);
        snprintf(syn_path[nsyn++], sizeof syn_path[0], "%s", loc_path[i]);
    }
    if (pv)
        pv->destroy(pv);
}

/* the synth it plays's parameters: each value in its range with a text read back, each page's slots its parameters,
 * each set by an event read back (in its range), its cutoff heard (the lowest and the highest); the other synths':
 * their defaults, an event for one changes nothing */
static void check_params(plug *x, const char *name)
{
    double rate = 48000;
    uint32_t np = x->params->count(x->p), pages = x->rc->count(x->p), back = 0, set = 0, schema = 0, own = 0, tag = tag_of(x);
    uint32_t others = 0, other_def = 0;
    clap_id cutoff = CLAP_INVALID_ID;
    double cmin = 0, cmax = 0;
    for (uint32_t i = 0; i < np; i++) {
        clap_param_info_t a;
        char txt[64], again[64];
        double v, r, step;
        x->params->get_info(x->p, i, &a);
        v = value(x, a.id);
        CHECK(v >= a.min_value && v <= a.max_value, "%s %s: value %g", name, a.name, v);
        CHECK(!a.module[0], "%s %s: no group", name, a.name);
        if (a.id >> 18 != tag) {
            others++;
            other_def += v == a.default_value;
            continue;
        }
        own++;
        schema += (a.id & 0x3FFFFu) >= 0x10000u;
        step = (a.max_value - a.min_value) > 200 ? (a.max_value - a.min_value) / 37.0 : 1.0;
        for (double k = a.min_value; k <= a.max_value; k += step) {
            double kk = floor(k);
            CHECK(x->params->value_to_text(x->p, a.id, kk, txt, sizeof txt) && txt[0], "%s %s: text of %g", name, a.name, kk);
            CHECK(x->params->text_to_value(x->p, a.id, txt, &r) && x->params->value_to_text(x->p, a.id, r, again, sizeof again) &&
                  !strcmp(txt, again), "%s %s: \"%s\" read %g (\"%s\")", name, a.name, txt, r, again);
        }
        if (cutoff == CLAP_INVALID_ID && strstr(a.name, "CUTOFF"))
            cutoff = a.id, cmin = a.min_value, cmax = a.max_value;
    }
    CHECK(other_def == others, "%s: %u of the other synths' %u parameters at their defaults", name, other_def, others);
    CHECK(pages > 0 && pages <= 256, "%s: %u pages", name, pages);
    for (uint32_t i = 0; i < pages; i++) {
        clap_remote_controls_page_t pg;
        int any = 0;
        CHECK(x->rc->get(x->p, i, &pg) && pg.page_name[0] && !strcmp(pg.section_name, name), "%s page %u", name, i);
        for (int k = 0; k < CLAP_REMOTE_CONTROLS_COUNT; k++) {
            double v;
            any |= pg.param_ids[k] != CLAP_INVALID_ID;
            CHECK(pg.param_ids[k] == CLAP_INVALID_ID || (pg.param_ids[k] >> 18 == tag && x->params->get_value(x->p, pg.param_ids[k], &v)),
                  "%s page %s slot %d", name, pg.page_name, k);
        }
        CHECK(any, "%s page %s: empty", name, pg.page_name);
    }
    {                                            /* every parameter of its own on a page (a host shows only pages) */
        uint32_t paged = 0;
        for (uint32_t i = 0; i < np; i++) {
            clap_param_info_t a;
            int on = 0;
            x->params->get_info(x->p, i, &a);
            for (uint32_t g = 0; a.id >> 18 == tag && g < pages && !on; g++) {
                clap_remote_controls_page_t pg;
                x->rc->get(x->p, g, &pg);
                for (int k = 0; k < CLAP_REMOTE_CONTROLS_COUNT; k++)
                    on |= pg.param_ids[k] == a.id;
            }
            paged += on;
        }
        CHECK(paged == own, "%s: %u of its %u parameters on a page", name, paged, own);
    }
    if (cutoff != CLAP_INVALID_ID) {             /* the filter, closed and open (the sound as it is) */
        double r[2];
        for (int k = 0; k < 2; k++) {
            evlist ev = {0};
            ev_midi(&ev, 0, 0xB0, 120, 0);
            ev_param(&ev, 0, cutoff, k ? cmax : cmin);
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 1, 48, 0.9);
            run(x, &ev, L, R, SEC(0.6));
            r[k] = rms(L + SEC(0.2), SEC(0.4));
            CHECK(value(x, cutoff) == (k ? cmax : cmin), "%s cutoff read back", name);
        }
        CHECK(r[1] > 1.2 * r[0] + 1e-4, "%s: cutoff closed %g, open %g", name, r[0], r[1]);
        printf("%-6s cutoff closed %.4f, open %.4f\n", name, r[0], r[1]);
    }
    for (uint32_t i = 0; i < np; i++) {          /* each to the middle of its range by an event, read back */
        clap_param_info_t a;
        evlist ev = {0};
        double want, was;
        x->params->get_info(x->p, i, &a);
        want = floor((a.min_value + a.max_value) / 2);
        was = value(x, a.id);
        ev_param(&ev, 0, a.id, want);
        run(x, &ev, L, R, 64);
        if (a.id >> 18 != tag) {
            CHECK(value(x, a.id) == was, "%s: another synth's %s set", name, a.name);
            continue;
        }
        set++;
        back += value(x, a.id) == want;
        CHECK(value(x, a.id) >= a.min_value && value(x, a.id) <= a.max_value, "%s %s set: %g", name, a.name, value(x, a.id));
    }
    printf("%-6s %u parameters (%u from its schema; %u of other synths), %u pages, %u of %u read back as set\n", name, own,
           schema, others, pages, back, set);
    CHECK(back * 10 >= set * 9, "%s: only %u of %u parameters read back as set", name, back, set);
    {
        evlist ev = {0};
        ev_midi(&ev, 0, 0xB0, 120, 0);
        run(x, &ev, L, R, 256);
    }
}

/* a synth: each factory sound loaded and played (a chord, held, released), its sound finite and heard; its state
 * read back by a new part (which plays the first synth until then) */
/* one note of synth key's first sound (velocity vel, VEL vel_amp; -1 as it starts): its level over 0.1..0.5 s */
static double vel_level(const char *key, int vel, int vel_amp)
{
    double rate = 48000, r;
    plug x;
    evlist ev = {0};
    if (open_plug(&x, key, rate))
        return -1;
    if (vel_amp >= 0)
        ev_param(&ev, 0, P(&x, 1001), vel_amp);
    ev_note(&ev, CLAP_EVENT_NOTE_ON, 1, 60, vel / 127.0);
    run(&x, &ev, L, R, SEC(0.5));
    r = rms(L + SEC(0.1), SEC(0.4));
    close_plug(&x);
    return r;
}
/* velocity: a note at 32 and at 100 (under the accent's 110), each by a new part, VEL as it starts (127): the soft
 * one well under the hard one in every module, those with their own envelopes too (ownenv: PPG, FMINI); VEL 0 (the
 * level as it is) leaves a module that has no velocity of its own alike at both */
static void check_velocity(const char *key, const char *name)
{
    double s = vel_level(key, 32, -1), h = vel_level(key, 100, -1), s0 = vel_level(key, 32, 0),
           h0 = vel_level(key, 100, 0);
    double db = s > 0 && h > 0 ? 20 * log10(h / s) : 0, db0 = s0 > 0 && h0 > 0 ? 20 * log10(h0 / s0) : 0, vel = -1;
    plug x;
    if (!open_plug(&x, key, 48000)) {
        vel = value(&x, P(&x, 1001));
        close_plug(&x);
    }
    if (!strcmp(name, "SID") || !strcmp(name, "FM6"))   /* (their own envelopes follow velocity: VEL 0 as new) */
        CHECK(vel == 0 && fabs(db - db0) < 0.1, "%s: VEL %g as new, %+.1f dB against VEL 0's %+.1f", name, vel, db, db0);
    else
        CHECK(vel == 127 && db > 7.0, "%s: VEL %g as new, velocity 32 to 100 only %+.1f dB", name, vel, db);
    if (!strcmp(name, "TRIO") || !strcmp(name, "PPG"))   /* (their first sounds: no velocity of their own) */
        CHECK(fabs(db0) < 1.0, "%s: VEL 0, velocity 32 to 100 %+.1f dB", name, db0);
    printf("%-6s velocity 32 to 100: %+.1f dB (VEL %g as new), %+.1f dB (VEL 0)\n", name, db, vel, db0);
}

static void play_module(const char *key, const char *name)
{
    double rate = 48000;
    plug x, y;
    uint32_t sounds = 0, quiet = 0;
    static float L2[48000 * 3], R2[48000 * 3];
    check_velocity(key, name);
    if (open_plug(&x, key, rate)) {
        CHECK(0, "open %s", name);
        return;
    }
    for (uint32_t k = 0; k < 256; k++) {
        char sk[40];
        evlist ev = {0};
        snprintf(sk, sizeof sk, "%s/%u", key, k);
        if (!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, sk))
            break;
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 48, 0.8), ev_note(&ev, CLAP_EVENT_NOTE_ON, 3, 55, 0.8);
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 7, 60, 0.8);
        run(&x, &ev, L, R, SEC(1.0));
        ev.n = 0;
        ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 48, 0), ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 55, 0);
        ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 60, 0);
        run(&x, &ev, L + SEC(1.0), R + SEC(1.0), SEC(1.0));
        CHECK(finite_all(L, SEC(2.0)) && finite_all(R, SEC(2.0)), "%s sound %u finite", name, k);
        CHECK(peak(L, SEC(2.0)) < 8.0 && peak(R, SEC(2.0)) < 8.0, "%s sound %u: peak %g", name, k, peak(L, SEC(2.0)));
        quiet += rms(L, SEC(1.0)) < 1e-4 && rms(R, SEC(1.0)) < 1e-4;
        if (wavdir && k < 4) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s-%u.wav", wavdir, name, k);
            wav(path, L, R, SEC(2.0), (uint32_t)rate);
        }
        ev.n = 0;
        ev_midi(&ev, 0, 0xB0, 120, 0);
        run(&x, &ev, L, R, 256);
        sounds++;
    }
    CHECK(sounds > 0 && quiet * 4 < sounds, "%s: %u sounds, %u silent", name, sounds, quiet);
    {                                            /* the cost of eight voices of its first sound */
        char sk[40];
        evlist ev = {0};
        clock_t c0;
        snprintf(sk, sizeof sk, "%s/0", key);
        x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, sk);
        for (int k = 0; k < 8; k++)
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 40 + 5 * k, 0.8);
        c0 = clock();
        run(&x, &ev, L, R, SEC(5.0));
        printf("%-6s %3u factory sounds played, %u silent with a held chord; eight voices %.2f%% of real time here\n",
               name, sounds, quiet, (double)(clock() - c0) / CLOCKS_PER_SEC * 20.0);
        ev.n = 0;
        ev_midi(&ev, 0, 0xB0, 120, 0);
        run(&x, &ev, L, R, 256);
    }
    check_params(&x, name);                      /* (its first sound) */
    {                                            /* (the dither and the voices' ages run on: compare the state) */
        static blob b, b2;
        clap_ostream_t os = {&b, os_write}, os2 = {&b2, os_write};
        clap_istream_t is = {&b, is_read};
        evlist ev = {0};
        uint32_t np = x.params->count(x.p), same = 0;
        b.n = b.at = b2.n = b2.at = 0;
        CHECK(x.state->save(x.p, &os), "%s save", name);
        CHECK(!open_plug(&y, 0, rate), "open %s again", name);
        CHECK(y.state->load(y.p, &is), "%s load", name);
        CHECK(y.state->save(y.p, &os2) && b2.n == b.n && !memcmp(b.b, b2.b, b.n), "%s: the state read back (%u B)",
              name, (unsigned)b.n);
        CHECK(tag_of(&y) == tag_of(&x) && y.rc->count(y.p) == x.rc->count(x.p), "%s: the state's synth's pages", name);
        for (uint32_t k = 0; k < np; k++) {
            clap_param_info_t info;
            x.params->get_info(x.p, k, &info);
            same += value(&x, info.id) == value(&y, info.id);
        }
        CHECK(same == np, "%s: %u of %u values the same", name, same, np);
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 57, 0.9);
        run(&y, &ev, L2, R2, SEC(1.0));
        CHECK(finite_all(L2, SEC(1.0)), "%s: the loaded state plays", name);   /* (its values the middle of each range: maybe silent) */
        close_plug(&y);
    }
    close_plug(&x);
}

/* another synth's sound loaded while one plays (between blocks, as a host's main thread does): the values and pages
 * at once, the sound at the next block, the old synth's code freed on the main thread; several in a row; one while
 * not active */
static void test_switch(void)
{
    double rate = 48000;
    plug x;
    evlist ev = {0};
    uint32_t first_pages, tag0;
    if (open_plug(&x, 0, rate)) {
        CHECK(0, "open");
        return;
    }
    first_pages = x.rc->count(x.p);
    tag0 = tag_of(&x);
    ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 60, 0.9);
    run(&x, &ev, L, R, SEC(0.5));
    CHECK(rms(L + SEC(0.2), SEC(0.3)) > 1e-3, "the first synth plays");
    pages_changed = rescans = 0;
    CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/3"), "TRIO's RING BELL");
    CHECK(pages_changed == 1 && rescans > 0, "the host told: pages %d, values %d", pages_changed, rescans);
    CHECK(tag_of(&x) != tag0 && x.rc->count(x.p) == 7 && first_pages != 7, "TRIO's pages at once (%u)", x.rc->count(x.p));
    CHECK(value(&x, P(&x, 116)) == 13 && value(&x, P(&x, 118)) == 18, "RING BELL's values at once");
    ev.n = 0;
    run(&x, &ev, L, R, SEC(0.3));
    CHECK(rms(L + SEC(0.1), SEC(0.2)) < 1e-6, "the first synth's note gone with it: %g", rms(L + SEC(0.1), SEC(0.2)));
    x.p->on_main_thread(x.p);                    /* (the old synth freed) */
    ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 60, 0.9);
    run(&x, &ev, L, R, SEC(0.5));
    CHECK(rms(L, SEC(0.5)) > 1e-3 && finite_all(L, SEC(0.5)), "TRIO plays");
    {                                            /* another synth's parameter: nothing */
        clap_id other = (tag0 << 18) | 1u;      /* (its ATTACK, or none) */
        double v = value(&x, P(&x, 116));
        ev.n = 0;
        ev_param(&ev, 0, other, 50);
        run(&x, &ev, L, R, 64);
        CHECK(value(&x, P(&x, 116)) == v, "another synth's parameter left TRIO as it was");
    }
    /* three in a row before a block: the last plays, the others never do */
    CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, HYDRA_KEY "/0") &&
          x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/0") &&
          x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, HYDRA_KEY "/2"), "three in a row");
    ev.n = 0;
    ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 62, 0.9);
    run(&x, &ev, L, R, SEC(0.5));
    CHECK(tag_of(&x) == tag0 && rms(L, SEC(0.5)) > 1e-3, "HYDRA's third sound plays");
    /* the next before the audio took the last back (no main-thread call between) */
    CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/1"), "TRIO again");
    run(&x, &ev, L, R, SEC(0.2));
    CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, HYDRA_KEY "/1"), "HYDRA again");
    run(&x, &ev, L, R, SEC(0.5));
    CHECK(rms(L + SEC(0.1), SEC(0.4)) > 1e-3, "HYDRA plays after two in turn");
    /* not active: at once */
    x.p->stop_processing(x.p);
    x.p->deactivate(x.p);
    CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/4"), "CHIP CHOIR, not active");
    CHECK(value(&x, P(&x, 116)) == 1, "CHIP CHOIR's values");
    CHECK(x.p->activate(x.p, rate, 1, 128) && x.p->start_processing(x.p), "active again");
    ev.n = 0;
    ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 60, 0.9);
    run(&x, &ev, L, R, SEC(0.5));
    CHECK(rms(L, SEC(0.5)) > 1e-4, "CHIP CHOIR plays");
    close_plug(&x);
    /* a synth loaded and never played: freed with the plug-in */
    CHECK(!open_plug(&x, 0, rate), "open");
    CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/0"), "TRIO, then closed");
    close_plug(&x);
    printf("another synth while playing: ok\n");
}

/* ---- SysEx files in the SysEx folder beside the plug-in */
static int errors;
static void rx_error_count(const clap_preset_discovery_metadata_receiver_t *r, int32_t e, const char *m)
{
    (void)r, (void)e;
    printf("  SysEx file: %s\n", m);
    errors++;
}
static int last_patch(plug *x, uint8_t *p, uint32_t len)   /* the state's patch: its last bytes before the sound */
{
    static blob b;
    clap_ostream_t os = {&b, os_write};
    size_t n;
    b.n = b.at = 0;
    if (!x->state->save(x->p, &os))
        return -1;
    n = b.n;
    for (size_t l = 0; l < 256u; l++)            /* (the sound loaded after it: "MMSN", place, changed, name) */
        if (n >= l + 10u && b.b[n - l - 1u] == l && !memcmp(b.b + n - l - 10u, "MMSN", 4)) {
            n -= l + 10u;
            break;
        }
    if (n < len)
        return -1;
    memcpy(p, b.b + n - len, len);
    return 0;
}
static FILE *open_syx(const char *dir, const char *name)
{
    char path[4200];
    snprintf(path, sizeof path, "%s/%s.syx", dir, name);
    return fopen(path, "wb");
}
static int loc_at(const char *name)              /* its location, -1 none */
{
    for (int i = 0; i < locations && i < 32; i++)
        if (!strcmp(loc_name[i], name))
            return i;
    return -1;
}
/* files written into the SysEx folder (HYDRA's native frames of three factory sounds with bytes between them, a PPG
 * bank of made-up programs, one cut short, one without a frame; MM_PPG_BANK: a PPG Wave 2.2 factory bank), then
 * discovery: each a location under the synth whose module takes it, the synth's own then "<synth>/Factory", the file
 * no module takes none; the sounds of each; then loaded by their keys: HYDRA's as they were sent, the factory bank's
 * as the factory programs */
static void test_sysex(const clap_plugin_entry_t *entry, const char *clap)
{
    const clap_preset_discovery_factory_t *pf = entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
    clap_preset_discovery_indexer_t ix = {CLAP_VERSION_INIT, "test", "test", 0, "1", 0, ix_filetype, ix_location, ix_pack, ix_ext};
    clap_preset_discovery_metadata_receiver_t rx = {0, rx_error_count, rx_begin, rx_id, rx_pack, rx_flags, rx_str,
                                                    rx_str, rx_time, rx_str, rx_extra};
    const clap_preset_discovery_provider_t *pv;
    const char *slash = strrchr(clap, '/'), *bank = getenv("MM_PPG_BANK");
    char dir[4096], hname[3][40], path[4200];
    uint8_t pat[3][144], p[512], q[512];
    int factory = 0, same = 0, f;
    double rate = 48000;
    plug x;
    FILE *out, *in;
    snprintf(dir, sizeof dir, "%.*s/SysEx", slash ? (int)(slash - clap) : 1, slash ? clap : ".");
    mkdir(dir, 0755);
    /* HYDRA's factory sounds 1..3, their names */
    want_key = HYDRA_KEY;
    nfound = with_id = 0;
    pv = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
    CHECK(pv && pv->init(pv) && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, &rx) && with_id >= 4,
          "HYDRA's factory sounds");
    for (int i = 0; i < 3; i++)
        snprintf(hname[i], sizeof hname[i], "%s", found_name[i + 1]);
    pv->destroy(pv);
    CHECK(!open_plug(&x, HYDRA_KEY, 48000), "open HYDRA");
    if ((out = open_syx(dir, "HYDRA test"))) {
        for (int i = 0; i < 3; i++) {
            char key[40];
            snprintf(key, sizeof key, HYDRA_KEY "/%d", i + 1);
            CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, key) &&
                  !last_patch(&x, pat[i], 144), "HYDRA sound %d", i + 1);
            fputc(0xF0, out), fputc(0x7D, out), fputc(0x48, out), fputc(0x59, out), fputc(0x01, out);
            for (int k = 0; k < 144; k++)
                fputc(pat[i][k] & 15, out), fputc(pat[i][k] >> 4, out);
            fputc(0xF7, out), fputc(0x00, out), fputc(0x12, out);   /* (bytes between frames) */
        }
        fclose(out);
    }
    if ((out = open_syx(dir, "PPG made"))) {     /* (a 2.3's length: no 20 bytes after the programs) */
        fputc(0xF0, out), fputc(0x29, out), fputc(0x01, out), fputc(0x0D, out);
        for (int k = 0; k < 10200; k++)
            fputc((k * 7 + k / 102) & 15, out);
        fputc(0xF7, out);
        fclose(out);
    }
    if ((out = open_syx(dir, "PPG cut"))) {
        fputc(0xF0, out), fputc(0x29, out), fputc(0x01, out), fputc(0x0D, out);
        for (int k = 0; k < 300; k++)
            fputc(k & 15, out);
        fputc(0xF7, out);
        fclose(out);
    }
    if ((out = open_syx(dir, "no frame")))
        fputs("not SysEx", out), fclose(out);
    if (bank && (in = fopen(bank, "rb"))) {
        static uint8_t b[1 << 16];
        size_t n = fread(b, 1, sizeof b, in);
        fclose(in);
        if ((out = open_syx(dir, "PPG factory")))
            fwrite(b, 1, n, out), fclose(out), factory = 1;
    }
    /* discovery: each synth's locations, the SysEx files under the synth they are for, by name */
    locations = 0;
    pv = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
    CHECK(pv && pv->init(pv), "provider with SysEx files");
    {
        const char *with_ppg[] = {"HYDRA/Factory", "HYDRA/HYDRA test", "PPG/Factory", "PPG/PPG cut", "PPG/PPG factory",
                                  "PPG/PPG made", "TRIO", "WHEEL", 0};
        const char *no_ppg[] = {"HYDRA/Factory", "HYDRA/HYDRA test", "TRIO", "WHEEL", 0};
        const char **want = have_ppg ? with_ppg : no_ppg;
        int n = 0, ok = 1;
        for (int i = 0; want[i]; i++) {
            if (!factory && !strcmp(want[i], "PPG/PPG factory"))
                continue;
            ok &= n < locations && n < 32 && !strcmp(loc_name[n], want[i]);
            n++;
        }
        CHECK(ok && locations == n, "the locations (%d):%s%s%s%s%s%s", locations, locations > 0 ? " " : "",
              loc_name[0], locations > 1 ? ", " : "", loc_name[1], locations > 2 ? ", " : "", loc_name[2]);
        CHECK(loc_at("no frame") < 0, "no location for a file no module takes");
    }
    want_key = HYDRA_KEY;
    nfound = with_id = errors = 0;
    f = loc_at("HYDRA/HYDRA test");
    CHECK(f >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[f], &rx) && nfound == 3 &&
          with_id == 3 && !errors, "HYDRA test: three HYDRA sounds (%d, %d HYDRA's)", nfound, with_id);
    for (int i = 0; i < 3 && i < with_id; i++) {
        char key[40];
        snprintf(key, sizeof key, HYDRA_KEY "/%d", i);
        CHECK(found_name[i][0] && !strncmp(found_name[i], hname[i], strlen(found_name[i])) && !strcmp(found_key[i], key),
              "HYDRA test %d: %s %s (factory %s)", i, found_name[i], found_key[i], hname[i]);
    }
    if (have_ppg) {                              /* (the PPG module: a local test file only) */
        want_key = PPG_KEY;
        nfound = with_id = errors = 0;
        f = loc_at("PPG/PPG made");
        CHECK(f >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[f], &rx) && nfound == 100 &&
              with_id == 100 && !errors, "PPG made: a hundred PPG programs (%d, %d PPG's)", nfound, with_id);
        CHECK(!strcmp(found_name[1], "PROGRAM 01") && !strcmp(found_key[1], PPG_KEY "/1"), "PPG made 1: %s %s",
              found_name[1], found_key[1]);
        nfound = errors = 0;
        f = loc_at("PPG/PPG cut");
        CHECK(f >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[f], &rx) && !nfound && errors == 1,
              "PPG cut: no sound, the module's error (%d)", errors);
    }
    nfound = errors = 0;
    snprintf(path, sizeof path, "%s/no frame.syx", dir);
    CHECK(pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path, &rx) && !nfound && !errors, "no frame: no sound");
    pv->destroy(pv);
    /* loaded: HYDRA's sounds as sent; another synth's key, a sound past the file's, no file: refused */
    f = loc_at("HYDRA/HYDRA test");
    for (int i = 0; f >= 0 && i < 3; i++) {
        char key[40];
        evlist ev = {0};
        snprintf(key, sizeof key, HYDRA_KEY "/%d", i);
        CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[f], key) && !last_patch(&x, p, 144) &&
              !memcmp(p, pat[i], 144), "HYDRA test %d loads the sound sent", i);
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 60, 0.8);
        run(&x, &ev, L, R, SEC(0.5));
        CHECK(rms(L, SEC(0.5)) > 0.002, "HYDRA test %d plays", i);
        ev.n = 0;
        ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 60, 0);
        run(&x, &ev, L, R, SEC(0.5));
    }
    if (f >= 0) {
        uint32_t tag = tag_of(&x);
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[f], HYDRA_KEY "/3"),
              "no fourth sound");
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[f], TRIO_KEY "/0") &&
              tag_of(&x) == tag, "not another synth's");
    }
    snprintf(path, sizeof path, "%s/none.syx", dir);
    CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path, HYDRA_KEY "/0"), "no file");
    if (have_ppg) {
        /* PPG: the factory bank's programs are the factory sounds after INIT; loading the first loads PPG */
        if ((f = loc_at("PPG/PPG factory")) >= 0) {
            for (int k = 0; k < 100; k++) {
                char key[40];
                snprintf(key, sizeof key, PPG_KEY "/%d", k);
                if (!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[f], key) || last_patch(&x, p, 115))
                    continue;
                snprintf(key, sizeof key, PPG_KEY "/%d", k + 1);
                same += x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, key) && !last_patch(&x, q, 115) &&
                        !memcmp(p, q, 103);
            }
            printf("PPG    the factory bank gives %d of 100 factory programs back\n", same);
            CHECK(same == 100, "PPG: the factory bank is the factory programs (%d of 100)", same);
        } else {
            printf("PPG    no factory bank (MM_PPG_BANK): the factory programs not compared\n");
        }
    } else {
        printf("PPG    no PPG module (tests/fixtures/ppg.mmod, local only: make fixtures): its banks not tested\n");
    }
    close_plug(&x);
    for (const char *const *n = (const char *const[]){"HYDRA test", "PPG made", "PPG cut", "no frame", "PPG factory", 0};
         *n; n++) {
        snprintf(path, sizeof path, "%s/%s.syx", dir, *n);
        remove(path);
    }
    printf("SysEx files: ok\n");
}

static const clap_plugin_entry_t *open_entry(const char *path, void **lib)
{
    const clap_plugin_entry_t *e;
    *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!*lib) {
        printf("dlopen %s: %s\n", path, dlerror());
        return 0;
    }
    e = dlsym(*lib, "clap_entry");
    if (!e || !clap_version_is_compatible(e->clap_version) || !e->init(path)) {
        printf("%s: no clap_entry, or its init failed\n", path);
        return 0;
    }
    return e;
}

/* one module file alone in the Modules folder: the plug-in, one synth its location, its sounds, it plays */
static int one_file(const char *path)
{
    void *lib;
    const clap_plugin_entry_t *e = open_entry(path, &lib);
    char name[64], key[17];
    if (!e)
        return 1;
    factory = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    CHECK(factory && factory->get_plugin_count(factory) == 1, "one plug-in: %u", factory ? factory->get_plugin_count(factory) : 0);
    synths(e);
    CHECK(nsyn == 1 && locations == 1, "one synth: %d", nsyn);
    if (fails)
        return 1;
    snprintf(name, sizeof name, "%s", syn_name[0]);
    snprintf(key, sizeof key, "%s", syn_key[0]);
    play_module(key, name);
    e->deinit();
    dlclose(lib);
    if (fails) {
        printf("%s: %d failed\n", name, fails);
        return 1;
    }
    printf("%s alone: ok\n", name);
    return 0;
}

/* two copies of the plug-in, each dlopen'd on its own (Melodee stages one per rack entry): a part in each, both
 * playing in turn, each with its own copy of the module */
static int staged(const char *a, const char *b)
{
    void *la, *lb;
    const clap_plugin_entry_t *ea = open_entry(a, &la), *eb = open_entry(b, &lb);
    const clap_plugin_factory_t *fa, *fb;
    plug x, y;
    double rate = 48000;
    static float L2[48000], R2[48000];
    if (!ea || !eb)
        return 1;
    CHECK(ea != eb, "two copies, two entries");
    fa = ea->get_factory(CLAP_PLUGIN_FACTORY_ID);
    fb = eb->get_factory(CLAP_PLUGIN_FACTORY_ID);
    CHECK(fa->get_plugin_count(fa) == 1 && fb->get_plugin_count(fb) == 1, "the plug-in in each");
    synths(ea);
    CHECK(nsyn >= 3, "the synths in A: %d", nsyn);
    synths(eb);
    CHECK(nsyn >= 3, "the synths in B: %d", nsyn);
    factory = fa;
    CHECK(!open_plug(&x, TRIO_KEY, rate), "open TRIO in A");
    factory = fb;
    CHECK(!open_plug(&y, TRIO_KEY, rate), "open TRIO in B");
    if (fails)
        return 1;
    for (int k = 0; k < 4; k++) {
        evlist ev = {0};
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 48 + 7 * k, 0.8);
        run(&x, &ev, L + SEC(0.25) * k, R + SEC(0.25) * k, SEC(0.25));
        run(&y, &ev, L2 + SEC(0.25) * k, R2 + SEC(0.25) * k, SEC(0.25));
    }
    CHECK(rms(L, SEC(1.0)) > 0.01 && rms(L2, SEC(1.0)) > 0.01 && finite_all(L, SEC(1.0)) && finite_all(L2, SEC(1.0)),
          "both copies play");
    close_plug(&x);
    close_plug(&y);
    ea->deinit();
    eb->deinit();
    dlclose(la);
    dlclose(lb);
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("two staged copies: ok\n");
    return 0;
}

/* each synth's pages and their parameters with their values (what a host's pages show) */
static int list(const char *path)
{
    void *lib;
    const clap_plugin_entry_t *e = open_entry(path, &lib);
    if (!e)
        return 1;
    factory = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    synths(e);
    for (int i = 0; i < nsyn; i++) {
        plug x;
        if (open_plug(&x, syn_key[i], 48000))
            continue;
        printf("== %s (%s)\n", syn_name[i], syn_key[i]);
        for (uint32_t k = 0; k < x.rc->count(x.p); k++) {
            clap_remote_controls_page_t pg;
            x.rc->get(x.p, k, &pg);
            printf("  %-14s", pg.page_name);
            for (int c = 0; c < CLAP_REMOTE_CONTROLS_COUNT; c++) {
                clap_param_info_t a;
                char txt[64] = "";
                if (pg.param_ids[c] == CLAP_INVALID_ID)
                    continue;
                for (uint32_t q = 0; q < x.params->count(x.p); q++)
                    if (x.params->get_info(x.p, q, &a) && a.id == pg.param_ids[c])
                        break;
                x.params->value_to_text(x.p, a.id, value(&x, a.id), txt, sizeof txt);
                printf(" | %s = %s", a.name, txt);
            }
            printf("\n");
        }
        close_plug(&x);
    }
    e->deinit();
    dlclose(lib);
    return 0;
}

/* ---- the screen: steps as Melodee's --melodee-plugin-screen takes them, a picture at each snap */
static int screen(const char *path, const char *out, int argc, char **argv)
{
    void *lib;
    const clap_plugin_entry_t *e = open_entry(path, &lib);
    const melodee_screen_t *sc;
    static uint32_t px[MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_PAGE_HEIGHT];
    melodee_screen_frame_t fr;
    plug x;
    int held = -1, draws = 0;
    double worst = 0;
    if (!e)
        return 1;
    factory = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (open_plug(&x, 0, 48000) || !(sc = x.p->get_extension(x.p, MELODEE_SCREEN_CLAP)))
        return 1;
    memset(&fr, 0, sizeof fr);
    fr.pixels = px, fr.width = MELODEE_SCREEN_WIDTH, fr.height = MELODEE_SCREEN_PAGE_HEIGHT;
    fr.stride = MELODEE_SCREEN_WIDTH, fr.area = MELODEE_SCREEN_PAGE, fr.colour = 0xFF7A5C;
    fr.redraw_all = 1;
    for (int k = 0; k < 3; k++)
        fr.pots[k] = -1, fr.pots_live[k] = 1;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        melodee_screen_input_t in = {0, 0, 0};
        evlist ev = {0};
        int v = 0, k = 0;
        if (!strncmp(a, "wheel:", 6) || !strncmp(a, "swheel:", 7)) {
            in.kind = MELODEE_SCREEN_WHEEL, in.value = atoi(strchr(a, ':') + 1), in.shift = a[0] == 's';
            sc->input(x.p, &in);
        } else if (!strcmp(a, "push") || !strcmp(a, "minus") || !strcmp(a, "plus")) {
            in.kind = a[1] == 'u' ? MELODEE_SCREEN_PUSH : MELODEE_SCREEN_STEP, in.value = a[1] == 'i' ? -1 : 1;
            sc->input(x.p, &in);
        } else if (!strncmp(a, "note:", 5)) {
            if (held >= 0)
                ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, held, 0);
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 1, held = atoi(a + 5), 0.8);
        } else if (!strcmp(a, "off") && held >= 0) {
            ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, held, 0);
            held = -1;
        } else if (sscanf(a, "k%d:%d", &k, &v) == 2 && k >= 1 && k <= 3) {
            uint32_t ids[3];
            clap_param_info_t inf;
            sc->controls(x.p, ids);
            for (uint32_t q = 0; ids[k - 1] != MELODEE_SCREEN_NO_CONTROL && q < x.params->count(x.p); q++)
                if (x.params->get_info(x.p, q, &inf) && inf.id == ids[k - 1]) {
                    ev_param(&ev, 0, inf.id, inf.min_value + (inf.max_value - inf.min_value) * v / 127.0);
                    break;
                }
            fr.pots[k - 1] = (int16_t)v, fr.pots_live[k - 1] = 1;
        } else if (sscanf(a, "pot%d:%d", &k, &v) == 2 && k >= 1 && k <= 3) {
            fr.pots[k - 1] = (int16_t)v, fr.pots_live[k - 1] = 0;
        } else if (!strncmp(a, "colour:", 7)) {
            fr.colour = (uint32_t)strtoul(a + 7, 0, 16);
        } else if (!strncmp(a, "snap:", 5)) {
            char file[4200];
            FILE *f;
            struct timespec t0, t1;
            double ms;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            draws += sc->draw(x.p, &fr);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
            worst = ms > worst ? ms : worst;
            fr.redraw_all = 0;
            snprintf(file, sizeof file, "%s/%s.ppm", out, a + 5);
            if ((f = fopen(file, "wb"))) {
                fprintf(f, "P6\n%d %d\n255\n", MELODEE_SCREEN_WIDTH, MELODEE_SCREEN_PAGE_HEIGHT);
                for (uint32_t p = 0; p < MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_PAGE_HEIGHT; p++) {
                    uint8_t rgb[3] = {(uint8_t)(px[p] >> 16), (uint8_t)(px[p] >> 8), (uint8_t)px[p]};
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
            }
            continue;
        } else {
            printf("step? %s\n", a);
            fails++;
            continue;
        }
        run(&x, &ev, L, R, (uint32_t)(x.rate * (ev.n ? 0.25 : 0.02)));   /* (the edits taken; a note sounding) */
        x.p->on_main_thread(x.p);
    }
    printf("%d draws, the slowest %.2f ms, %d redraws asked\n", draws, worst, redraws);
    close_plug(&x);
    return fails != 0;
}

/* its page in Melodee: drawn, and not again while nothing it shows changed; - / + walk its pages; the encoder turns a
 * page's fourth value, which the audio thread takes and the host is told of; the sound loaded kept in its state */
static void test_screen(void)
{
    static uint32_t px[MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_PAGE_HEIGHT];
    static blob a, b;
    const melodee_screen_t *sc = 0;
    melodee_screen_frame_t fr;
    melodee_screen_input_t in = {MELODEE_SCREEN_STEP, 1, 0};
    clap_ostream_t osa = {&a, os_write}, osb = {&b, os_write};
    clap_istream_t isa = {&a, is_read};
    uint32_t ids[3], colours = 0, first;
    clap_id dtn;
    double before;
    int r0, ok = 1;
    plug x, y;
    CHECK(!open_plug(&x, TRIO_KEY, 48000) && (sc = x.p->get_extension(x.p, MELODEE_SCREEN_CLAP)), "a screen");
    if (!sc)
        return;
    memset(&fr, 0, sizeof fr);
    fr.pixels = px, fr.width = MELODEE_SCREEN_WIDTH, fr.height = MELODEE_SCREEN_PAGE_HEIGHT;
    fr.stride = MELODEE_SCREEN_WIDTH, fr.area = MELODEE_SCREEN_PAGE, fr.colour = 0xFF7A5C, fr.redraw_all = 1;
    for (int k = 0; k < 3; k++)
        fr.pots[k] = -1, fr.pots_live[k] = 1;
    CHECK(sc->draw(x.p, &fr), "screen: drawn");
    first = px[0];
    for (uint32_t p = 0; p < MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_PAGE_HEIGHT && colours < 2; p++)
        colours += px[p] != first;
    CHECK(colours >= 2, "screen: a picture, not one colour");
    fr.redraw_all = 0;
    CHECK(!sc->draw(x.p, &fr), "screen: nothing new, no draw");
    fr.area = MELODEE_SCREEN_STRIP;
    CHECK(!sc->draw(x.p, &fr), "screen: no strip (an instrument)");
    fr.area = MELODEE_SCREEN_PAGE;
    sc->controls(x.p, ids);
    for (int k = 0; k < 3; k++)
        ok &= ids[k] == MELODEE_SCREEN_NO_CONTROL || value(&x, ids[k]) != -999;
    CHECK(ok, "screen: K1-K3 on the sounds are TRIO's");
    CHECK(sc->input(x.p, &in) && sc->draw(x.p, &fr), "screen: + to TRIO's first page");
    sc->controls(x.p, ids);
    CHECK(ids[0] == param_id(&x, "WAVE"), "screen: K1 is OSC's WAVE");
    dtn = param_id(&x, "DTN");
    before = value(&x, dtn);
    r0 = rescans;
    in.kind = MELODEE_SCREEN_WHEEL, in.value = 3;
    CHECK(sc->input(x.p, &in), "screen: the encoder turns DTN");
    run(&x, 0, L, R, 256);
    x.p->on_main_thread(x.p);
    CHECK(value(&x, dtn) == before + 3 && rescans > r0, "screen: DTN %.0f to %.0f, the host told", before,
          value(&x, dtn));
    CHECK(sc->draw(x.p, &fr), "screen: drawn again after the change");
    /* the sound loaded in the state: another part loading it has it, and saves the same */
    CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/2"), "TRIO sound 2");
    a.n = a.at = b.n = b.at = 0;
    CHECK(x.state->save(x.p, &osa) && a.n > 10 && !open_plug(&y, 0, 48000) && y.state->load(y.p, &isa) &&
          y.state->save(y.p, &osb) && a.n == b.n && !memcmp(a.b, b.b, a.n),
          "screen: the sound loaded saved and loaded");
    close_plug(&y);
    close_plug(&x);
}

/* the Modules folder watched while a part is alive (mm_watch.h, MM_WATCH_MS 100 here): a module file added, then
 * taken away, and the host asked to restart the plug-in once each, after it has held still; nothing asked while
 * nothing changes */
static void test_watch(const char *clap)
{
    const char *slash = strrchr(clap, '/');
    char path[4200];
    FILE *out;
    int r0;
    plug x;
    snprintf(path, sizeof path, "%.*s/Modules/watch-test.mmod", slash ? (int)(slash - clap) : 1, slash ? clap : ".");
    CHECK(!open_plug(&x, 0, 48000), "open");
    usleep(600000);                              /* (SysEx files written since the library loaded: asked once now) */
    r0 = atomic_load(&restarts);
    usleep(600000);
    CHECK(atomic_load(&restarts) == r0, "watch: nothing changed, nothing asked (%d)", atomic_load(&restarts) - r0);
    if ((out = fopen(path, "wb")))
        fputs("not a module", out), fclose(out);
    usleep(600000);
    CHECK(atomic_load(&restarts) == r0 + 1, "watch: a module file added, asked once (%d)", atomic_load(&restarts) - r0);
    usleep(600000);
    CHECK(atomic_load(&restarts) == r0 + 1, "watch: asked once only (%d)", atomic_load(&restarts) - r0);
    unlink(path);
    usleep(600000);
    CHECK(atomic_load(&restarts) == r0 + 2, "watch: taken away, asked again (%d)", atomic_load(&restarts) - r0);
    close_plug(&x);
    printf("watch: Modules folder changes ask the host to restart, once each\n");
}

int main(int argc, char **argv)
{
    void *lib;
    const clap_plugin_entry_t *entry;
    wavdir = getenv("MM_WAV_DIR");
    if (!getenv("MM_WATCH_MS"))
        setenv("MM_WATCH_MS", "100", 1);         /* (test_watch(): the folder watch looks every 100 ms) */
    if (argc == 3 && !strcmp(argv[1], "--one"))
        return one_file(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--staged"))
        return staged(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "--list"))
        return list(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "--screen"))
        return screen(argv[2], argv[3], argc - 4, argv + 4);
    if (argc < 2) {
        fprintf(stderr, "usage: test_host PLUGIN.clap\n");
        return 2;
    }
    lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf("dlopen: %s\n", dlerror());
        return 1;
    }
    entry = dlsym(lib, "clap_entry");
    CHECK(entry && clap_version_is_compatible(entry->clap_version), "clap_entry");
    {                                            /* a broken file and a second copy of TRIO beside the four */
        char dir[4096], path[4200];
        FILE *in, *out;
        static uint8_t b[1 << 20], orig[1 << 20];
        size_t n;
        const char *slash = strrchr(argv[1], '/');
        snprintf(dir, sizeof dir, "%.*s/Modules", slash ? (int)(slash - argv[1]) : 1, slash ? argv[1] : ".");
        snprintf(path, sizeof path, "%s/trio.mmod", dir);
        in = fopen(path, "rb");
        n = in ? fread(b, 1, sizeof b, in) : 0;
        if (in)
            fclose(in);
        CHECK(n > 1000, "tests/fixtures/trio.mmod in %s", dir);
        snprintf(path, sizeof path, "%s/ppg.mmod", dir);
        if ((in = fopen(path, "rb")))
            fclose(in), have_ppg = 1;
        snprintf(path, sizeof path, "%s/trio-copy.mmod", dir);
        if ((out = fopen(path, "wb")))
            fwrite(b, 1, n, out), fclose(out);
        memcpy(orig, b, n);
        b[n / 2] ^= 0x55;
        snprintf(path, sizeof path, "%s/broken.mmod", dir);
        if ((out = fopen(path, "wb")))
            fwrite(b, 1, n, out), fclose(out);
        /* TRIO changed after Melodee signed it, each whole again: a byte of its code, a newer version in its head (it
         * would be the one taken), its code's signature cut off. None is a synth */
        for (int k = 0; k < 3; k++) {
            static const char *const BAD[] = {"tampered-code", "tampered-version", "tampered-nsig"};
            size_t m = n, aa = t_chunk(orig, n, 0x34364141u /* AA64 */), ns = t_chunk(orig, n, 0x4749534Eu /* NSIG */);
            CHECK(aa && ns > aa, "TRIO's AA64 chunk, then NSIG");
            memcpy(b, orig, n);
            if (k == 0)
                b[aa + 8u + 24u + 64u] ^= 1u;   /* (past AA64's head and the descriptor's start) */
            else if (k == 1)
                b[48]++;                         /* (mod_file3_t.version) */
            else
                m = ns;
            t_whole(b, m);
            snprintf(path, sizeof path, "%s/%s.mmod", dir, BAD[k]);
            if ((out = fopen(path, "wb")))
                fwrite(b, 1, m, out), fclose(out);
        }
    }
    for (int k = 0; k < 3; k++) {                /* (Melodee's picker opens and closes it again and again) */
        CHECK(entry->init(argv[1]), "init");
        if (k < 2)
            entry->deinit();
    }
    factory = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    CHECK(factory && factory->get_plugin_count(factory) == 1, "one plug-in: %u", factory ? factory->get_plugin_count(factory) : 0);
    {
        const clap_plugin_descriptor_t *d = factory->get_plugin_descriptor(factory, 0);
        int instrument = 0;
        CHECK(d && !strcmp(d->id, PLUGIN_ID) && !strcmp(d->name, "Melodee Modules"), "the plug-in: %s", d ? d->id : "none");
        for (const char *const *f = d->features; *f; f++)
            instrument |= !strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT);
        CHECK(instrument, "an instrument");
        CHECK(!factory->create_plugin(factory, &HOST, PLUGIN_ID "." TRIO_KEY), "no plug-in for each synth");
    }
    synths(entry);                               /* the synths: a location each, by name */
    {
        static const char *const ALL[] = {"HYDRA", "PPG", "TRIO", "WHEEL"}, *const NO_PPG[] = {"HYDRA", "TRIO", "WHEEL"};
        const char *const *NAMES = have_ppg ? ALL : NO_PPG;
        int trio = -1;
        CHECK(nsyn == 3 + have_ppg && locations == nsyn, "%d synths: %d (%d locations)", 3 + have_ppg, nsyn, locations);
        for (int i = 0; i < nsyn && i < 3 + have_ppg; i++) {
            CHECK(!strcmp(syn_name[i], NAMES[i]), "synth %d: %s", i, syn_name[i]);
            if (!strcmp(syn_key[i], TRIO_KEY))
                trio = i;
        }
        CHECK(trio >= 0, "TRIO by its module ID");
        if (trio >= 0) {                         /* (the changed copies none: its own file, or its copy) */
            const char *leaf = strrchr(syn_path[trio], '/');
            CHECK(leaf && (!strcmp(leaf, "/trio.mmod") || !strcmp(leaf, "/trio-copy.mmod")), "TRIO's file: %s", syn_path[trio]);
        }
    }

    /* the parameters: unique ids, defaults in range, TRIO's values' texts read back */
    {
        plug x;
        uint32_t n, pages, own = 0, tag;
        CHECK(!open_plug(&x, TRIO_KEY, 48000), "open");
        CHECK(x.params && x.state && x.preset && x.rc && x.ap && x.np && x.lat, "extensions");
        CHECK(x.ap->count(x.p, false) == 1 && x.ap->count(x.p, true) == 0, "audio ports");
        {
            clap_note_port_info_t ni;
            CHECK(x.np->get(x.p, 0, true, &ni) && (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI) &&
                  (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP), "note port dialects");
        }
        n = x.params->count(x.p);
        tag = tag_of(&x);
        for (uint32_t i = 0; i < n; i++) {
            clap_param_info_t a, b;
            char txt[64];
            double v, back;
            x.params->get_info(x.p, i, &a);
            for (uint32_t j = i + 1; j < n; j++) {
                x.params->get_info(x.p, j, &b);
                CHECK(a.id != b.id, "id %u twice", a.id);
            }
            CHECK(a.id != CLAP_INVALID_ID && a.min_value <= a.default_value && a.default_value <= a.max_value, "%s default", a.name);
            v = value(&x, a.id);
            CHECK(v >= a.min_value && v <= a.max_value, "%s value %g", a.name, v);
            if (a.id >> 18 != tag)
                continue;
            own++;
            for (double k = a.min_value; k <= a.max_value; k++) {
                CHECK(x.params->value_to_text(x.p, a.id, k, txt, sizeof txt) && txt[0], "%s text", a.name);
                {                                /* (128 steps in 0..100 %: several values share a text) */
                    char again[64] = "";
                    CHECK(x.params->text_to_value(x.p, a.id, txt, &back) &&
                          x.params->value_to_text(x.p, a.id, back, again, sizeof again) && !strcmp(again, txt),
                          "%s: \"%s\" read %g (\"%s\")", a.name, txt, back, again);
                }
            }
        }
        CHECK(own == 8 + 25 + 3 && n > own, "TRIO's parameters: %u of %u", own, n);   /* (E1..E8, the part's, BEND VEL
                                                                                          * AT FLT) */
        pages = x.rc->count(x.p);
        CHECK(pages == 7, "pages: %u", pages);
        CHECK(param_id(&x, "VEL") == P(&x, 1001) && param_id(&x, "AT FLT") == P(&x, 1002) &&
              value(&x, P(&x, 1001)) == 127 && value(&x, P(&x, 1002)) == 32, "VEL and AT FLT as a new part has them");
        for (uint32_t i = 0; i < pages; i++) {
            clap_remote_controls_page_t pg;
            CHECK(x.rc->get(x.p, i, &pg) && pg.page_name[0] && !strcmp(pg.section_name, "TRIO"), "page %u", i);
            for (int k = 0; k < CLAP_REMOTE_CONTROLS_COUNT; k++) {
                double v;
                CHECK(pg.param_ids[k] == CLAP_INVALID_ID || x.params->get_value(x.p, pg.param_ids[k], &v),
                      "page %s slot %d", pg.page_name, k);
            }
        }
        CHECK(param_id(&x, "ATTACK") != CLAP_INVALID_ID && param_id(&x, "CUTOFF") != CLAP_INVALID_ID &&
              param_id(&x, "RESO") != CLAP_INVALID_ID, "the names Melodee's pictures look for");
        close_plug(&x);
    }

    /* a note at every rate: sound, nothing but finite values, a pitch of 440 Hz, the release to silence */
    {
        static const double RATES[] = {44100, 48000, 96000};
        for (int ri = 0; ri < 3; ri++) {
            double rate = RATES[ri], f, f_bend;
            plug x;
            evlist ev = {0};
            uint32_t on = SEC(1.0), lat;
            CHECK(!open_plug(&x, TRIO_KEY, rate), "open at %g", rate);
            lat = x.lat->get(x.p);
            CHECK(rate == 44100 ? lat == 0 : lat > 0 && lat < 64, "latency %u at %g", lat, rate);
            /* a plain tone: TRI3 with its three at unison, no detune, the filter open, POLY */
            ev_param(&ev, 0, P(&x, 116), 4), ev_param(&ev, 0, P(&x, 117), 0), ev_param(&ev, 0, P(&x, 118), 0);
            ev_param(&ev, 0, P(&x, 119), 0), ev_param(&ev, 0, P(&x, 121), 127), ev_param(&ev, 0, P(&x, 122), 0);
            ev_param(&ev, 0, P(&x, 37), 0), ev_param(&ev, 0, P(&x, 5), 0);
            ev_param(&ev, 0, P(&x, 1), 0), ev_param(&ev, 0, P(&x, 3), 127);
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 10, 69, 0.8);
            run(&x, &ev, L, R, on);
            CHECK(finite_all(L, on) && finite_all(R, on), "finite at %g", rate);
            CHECK(peak(L, on) > 0.05 && peak(L, on) < 2.0, "level %g at %g", peak(L, on), rate);
            f = freq(L + SEC(0.3), SEC(0.6), rate);
            CHECK(fabs(f - 440.0) < 0.5, "pitch %.2f Hz at %g", f, rate);
            /* the wheel up its range (2 semitones) as MIDI */
            ev.n = 0;
            ev_midi(&ev, 0, 0xE0, 0x7F, 0x7F);
            run(&x, &ev, L, R, SEC(0.5));
            f_bend = freq(L + SEC(0.2), SEC(0.3), rate);
            CHECK(fabs(f_bend - 493.88) < 1.0, "bend %.2f Hz at %g", f_bend, rate);
            ev.n = 0;
            ev_midi(&ev, 0, 0xE0, 0x00, 0x40);
            ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 69, 0);
            run(&x, &ev, L, R, SEC(3.0));
            CHECK(rms(L + SEC(2.5), SEC(0.5)) < 1e-5, "silent after the release at %g: %g", rate, rms(L + SEC(2.5), SEC(0.5)));
            close_plug(&x);
        }
    }

    /* MIDI notes, the pedal, MONO glide, UNISON, all sounds off */
    {
        double rate = 48000;
        plug x;
        evlist ev = {0};
        CHECK(!open_plug(&x, TRIO_KEY, rate), "open");
        ev_midi(&ev, 0, 0xB0, 64, 127);
        ev_midi(&ev, 5, 0x90, 60, 100);
        ev_midi(&ev, 6, 0x90, 64, 100);
        ev_midi(&ev, 50, 0x80, 60, 0);
        ev_midi(&ev, 50, 0x80, 64, 0);
        run(&x, &ev, L, R, SEC(1.0));
        CHECK(rms(L + SEC(0.8), SEC(0.2)) > 0.01, "the pedal holds the notes");
        ev.n = 0;
        ev_midi(&ev, 0, 0xB0, 64, 0);
        run(&x, &ev, L, R, SEC(3.0));
        CHECK(rms(L + SEC(2.5), SEC(0.5)) < 1e-5, "the pedal up releases them");
        ev.n = 0;
        ev_param(&ev, 0, P(&x, 37), 3);          /* UNISON */
        ev_param(&ev, 0, P(&x, 38), 60);         /* GLIDE */
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 48, 1.0);
        run(&x, &ev, L, R, SEC(0.5));
        CHECK(finite_all(L, SEC(0.5)) && peak(L, SEC(0.5)) < 4.0 && rms(L, SEC(0.5)) > 0.01, "UNISON");
        ev.n = 0;
        ev_midi(&ev, 0, 0xB0, 120, 0);           /* all sounds off */
        run(&x, &ev, L, R, SEC(0.1));
        CHECK(rms(L + SEC(0.05), SEC(0.05)) < 1e-6, "all sounds off: %g", rms(L + SEC(0.05), SEC(0.05)));
        close_plug(&x);
    }

    /* state: saved, changed, loaded back; a bad one and an unknown synth's refused; TRIO's into a part playing another
     * synth: TRIO */
    {
        plug x, y;
        blob b = {0};
        clap_ostream_t os = {&b, os_write};
        clap_istream_t is = {&b, is_read};
        CHECK(!open_plug(&x, TRIO_KEY, 48000), "open");
        {
            evlist ev = {0};
            ev_param(&ev, 0, P(&x, 121), 33), ev_param(&ev, 0, P(&x, 1000), 7), ev_param(&ev, 0, P(&x, 1001), 40);
            ev_param(&ev, 0, P(&x, 1002), -20);
            run(&x, &ev, L, R, 64);
        }
        CHECK(value(&x, P(&x, 121)) == 33 && value(&x, P(&x, 1000)) == 7 && value(&x, P(&x, 1001)) == 40 &&
              value(&x, P(&x, 1002)) == -20, "values from events");
        CHECK(x.state->save(x.p, &os), "save");
        {
            evlist ev = {0};
            ev_param(&ev, 0, P(&x, 121), 100), ev_param(&ev, 0, P(&x, 1000), 12), ev_param(&ev, 0, P(&x, 1001), 0);
            ev_param(&ev, 0, P(&x, 1002), 63);
            run(&x, &ev, L, R, 64);
        }
        rescans = 0;
        CHECK(x.state->load(x.p, &is), "load");
        CHECK(value(&x, P(&x, 121)) == 33 && value(&x, P(&x, 1000)) == 7 && value(&x, P(&x, 1001)) == 40 &&
              value(&x, P(&x, 1002)) == -20 && rescans, "values loaded");
        {                                        /* a version 2 state (no VEL, AT FLT): its values, those two as new */
            static blob b2;
            clap_istream_t is2 = {&b2, is_read};
            const uint32_t head2 = 4 + 4 + 8 + 124 * 2 + 2 + 2;   /* (MOD3_P_COUNT 124) */
            memcpy(b2.b, b.b, head2);
            memcpy(b2.b + head2, b.b + head2 + 4, b.n - head2 - 4);
            b2.n = b.n - 4, b2.at = 0;
            t_put32(b2.b + 4, 2);
            CHECK(x.state->load(x.p, &is2), "a version 2 state");
            CHECK(value(&x, P(&x, 121)) == 33 && value(&x, P(&x, 1000)) == 7 && value(&x, P(&x, 1001)) == 127 &&
                  value(&x, P(&x, 1002)) == 32, "a version 2 state's values: %g %g %g %g", value(&x, P(&x, 121)),
                  value(&x, P(&x, 1000)), value(&x, P(&x, 1001)), value(&x, P(&x, 1002)));
            b.at = 0;
            CHECK(x.state->load(x.p, &is), "load again");
        }
        CHECK(!open_plug(&y, HYDRA_KEY, 48000), "open HYDRA");
        b.at = 0;
        pages_changed = 0;
        CHECK(y.state->load(y.p, &is) && tag_of(&y) == tag_of(&x) && value(&y, P(&y, 121)) == 33 && pages_changed == 1,
              "TRIO's state loads TRIO");
        close_plug(&y);
        b.b[8] ^= 0xFF, b.at = 0;                /* (another module ID) */
        CHECK(!x.state->load(x.p, &is) && value(&x, P(&x, 121)) == 33, "an unknown synth's state refused");
        b.b[8] ^= 0xFF, b.b[0] ^= 0xFF, b.at = 0;
        CHECK(!x.state->load(x.p, &is), "a bad state refused");
        close_plug(&x);
    }

    /* pressure (AT FLT): a key held with TRIO's filter half shut, then pressed hard: brighter, by poly aftertouch on
     * its key, by channel aftertouch, by CLAP's pressure expression; not by another key's, nor at AT FLT 0 */
    {
        static const char *const HOW[] = {"poly on its key", "channel", "CLAP expression", "another key's",
                                          "poly at AT FLT 0", "poly at AT FLT -64"};
        double rate = 48000, ratio[6];
        for (int k = 0; k < 6; k++) {
            plug x;
            evlist ev = {0};
            double before, after;
            CHECK(!open_plug(&x, TRIO_KEY, rate), "open");
            ev_param(&ev, 0, P(&x, 121), 40), ev_param(&ev, 0, P(&x, 122), 0), ev_param(&ev, 0, P(&x, 5), 0);
            ev_param(&ev, 0, P(&x, 3), 127), ev_param(&ev, 0, P(&x, 37), 0);
            ev_param(&ev, 0, P(&x, 1002), k == 4 ? 0 : k == 5 ? -64 : 63);
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 1, 48, 0.8);
            run(&x, &ev, L, R, SEC(0.5));
            before = rms(L + SEC(0.3), SEC(0.2));
            ev.n = 0;
            if (k == 1) {
                ev_midi(&ev, 0, 0xD0, 127, 0);
            } else if (k == 2) {
                clap_event_note_expression_t *e = (clap_event_note_expression_t *)&ev.e[ev.n++];
                memset(e, 0, sizeof *e);
                e->header = (clap_event_header_t){sizeof *e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
                e->expression_id = CLAP_NOTE_EXPRESSION_PRESSURE, e->note_id = -1, e->key = 48, e->channel = 0;
                e->value = 1.0;
            } else {
                ev_midi(&ev, 0, 0xA0, k == 3 ? 60 : 48, 127);
            }
            run(&x, &ev, L, R, SEC(0.3));
            after = rms(L + SEC(0.1), SEC(0.2));
            ratio[k] = before > 0 ? after / before : 0;
            close_plug(&x);
        }
        /* (against AT FLT 0's: the sound settles a little from the first span to the second as it is) */
        CHECK(ratio[0] > 1.15 * ratio[4] && ratio[1] > 1.15 * ratio[4] && ratio[2] > 1.15 * ratio[4],
              "pressure opens the filter: %.3f %.3f %.3f (%.3f)", ratio[0], ratio[1], ratio[2], ratio[4]);
        CHECK(fabs(ratio[3] / ratio[4] - 1.0) < 0.01, "another key's: %.3f (%.3f)", ratio[3], ratio[4]);
        CHECK(ratio[5] < 0.87 * ratio[4], "AT FLT -64 shuts it: %.3f (%.3f)", ratio[5], ratio[4]);
        for (int k = 0; k < 6; k++)
            printf("pressure %-18s level x %.3f against AT FLT 0's\n", HOW[k], ratio[k] / ratio[4]);
    }

    /* factory sounds: discovered under the plug-in's id in their synth's location, loaded by their key */
    {
        const clap_preset_discovery_factory_t *pf = entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
        clap_preset_discovery_indexer_t ix = {CLAP_VERSION_INIT, "test", "test", 0, "1", 0, ix_filetype, ix_location, ix_pack, ix_ext};
        clap_preset_discovery_metadata_receiver_t rx = {0, rx_error, rx_begin, rx_id, rx_pack, rx_flags, rx_str, rx_str, rx_time, rx_str, rx_extra};
        const clap_preset_discovery_provider_t *pv;
        plug x;
        int t = -1;
        CHECK(pf && pf->count(pf) == 1, "preset factory");
        locations = 0;
        pv = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
        CHECK(pv && pv->init(pv) && locations == nsyn, "provider");
        for (int i = 0; i < locations && i < 32; i++)
            t = !strcmp(loc_name[i], "TRIO") ? i : t;
        want_key = TRIO_KEY;
        nfound = with_id = 0;
        CHECK(t >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[t], &rx), "metadata");
        CHECK(nfound == 5 && with_id == 5, "TRIO's five sounds: %d, %d", nfound, with_id);
        CHECK(!strcmp(found_name[3], "RING BELL") && !strcmp(found_key[3], TRIO_KEY "/3"), "RING BELL: %s %s", found_name[3], found_key[3]);
        nfound = with_id = 0;
        CHECK(pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, &rx) && nfound > 5 && with_id == 5,
              "every synth's sounds for a host that asks: %d, TRIO's %d", nfound, with_id);
        pv->destroy(pv);
        CHECK(!open_plug(&x, TRIO_KEY, 48000), "open");
        CHECK(value(&x, P(&x, 116)) == 0 && value(&x, P(&x, 3)) == 60 && value(&x, P(&x, 37)) == 1, "TRIO's FAT BASS");
        CHECK(t >= 0 && x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, loc_path[t], TRIO_KEY "/3"),
              "load RING BELL");
        CHECK(value(&x, P(&x, 116)) == 13 && value(&x, P(&x, 118)) == 18 && value(&x, P(&x, 3)) == 0 && value(&x, P(&x, 37)) == 0,
              "RING BELL's values");
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/9"), "no tenth sound");
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, "0000000000000000/0"), "no such synth");
        if (wavdir) {                            /* each sound: a chord, held, released */
            double rate = 48000;
            for (int k = 0; k < with_id; k++) {
                evlist ev = {0};
                char path[512];
                uint32_t n = SEC(4.0);
                x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, found_key[k]);
                ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 48, 0.8);
                ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 55, 0.8);
                ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 60, 0.8);
                run(&x, &ev, L, R, SEC(2.0));
                ev.n = 0;
                ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 48, 0), ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 55, 0);
                ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 60, 0);
                run(&x, &ev, L + SEC(2.0), R + SEC(2.0), n - SEC(2.0));
                snprintf(path, sizeof path, "%s/trio-%d-%s.wav", wavdir, k, found_name[k]);
                for (char *c = path + strlen(wavdir) + 1; *c; c++)
                    *c = *c == ' ' ? '-' : *c;
                wav(path, L, R, n, (uint32_t)rate);
            }
        }
        close_plug(&x);
    }

    for (int i = 0; i < nsyn; i++)
        play_module(syn_key[i], syn_name[i]);
    test_switch();

    /* two parts at once, each its own sound; then the cost of eight voices */
    {
        double rate = 48000;
        plug a, b;
        evlist ev = {0};
        static float L2[48000], R2[48000];
        clock_t c0;
        double secs;
        CHECK(!open_plug(&a, TRIO_KEY, rate) && !open_plug(&b, TRIO_KEY, rate), "two parts");
        CHECK(b.preset->from_location(b.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/4"), "B: CHIP CHOIR");
        CHECK(value(&a, P(&a, 116)) == 0 && value(&b, P(&b, 116)) == 1, "each its own values");
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 60, 0.9);
        run(&a, &ev, L, R, SEC(1.0));
        run(&b, &ev, L2, R2, SEC(1.0));
        CHECK(rms(L, SEC(1.0)) > 0.01 && rms(L2, SEC(1.0)) > 0.001, "both sound");
        close_plug(&b);
        ev.n = 0;
        for (int k = 0; k < 8; k++)
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 40 + 5 * k, 0.8);
        c0 = clock();
        run(&a, &ev, L, R, SEC(10.0));
        secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
        printf("eight TRIO voices, 10 s at 48 kHz: %.1f ms (%.2f%% of real time on this machine)\n", secs * 1000, secs * 10);
        close_plug(&a);
    }

    test_sysex(entry, argv[1]);
    test_screen();
    test_watch(argv[1]);

    entry->deinit();
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("melodee-modules ok\n");
    return 0;
}
