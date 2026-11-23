/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The plug-in as a host loads it (dlopen, clap_entry), with module files in the Modules folder beside it (make test:
 * tests/fixtures): the factory (a broken file and a second copy left out), the parameters, notes as CLAP and as MIDI
 * in blocks of 1..128 frames at 44.1, 48 and 96 kHz, pitch and bend, the release to silence, state, the factory sounds
 * through preset discovery and preset load, every module and every sound played, two parts at once, SysEx files'
 * sounds (MM_PPG_BANK=file: a PPG Wave 2.2 factory bank against the factory programs).
 *   test_host DIR/melodee-modules.clap     (MM_WAV_DIR=dir: each module's sounds as WAVs there)
 *   test_host --one DIR/melodee-modules.clap          (one module file alone in DIR/Modules: it lists, loads and plays)
 *   test_host --staged A/melodee-modules.clap B/...   (two copies, each dlopen'd on its own beside a linked Modules
 *                                                       folder, as Melodee stages one per rack entry)
 *   test_host --list DIR/melodee-modules.clap         (each plug-in's pages: their parameters, values as text) */
#include <clap/clap.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int fails;
static char trio_version[8];                     /* (tests/fixtures/trio.mmod's head's) */
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
static const void *h_ext(const clap_host_t *h, const char *id) { (void)h; return !strcmp(id, CLAP_EXT_PARAMS) ? &HOST_PARAMS : 0; }
static void h_nop(const clap_host_t *h) { (void)h; }
static const clap_host_t HOST = {CLAP_VERSION_INIT, 0, "test", "melodee-modules", 0, "1", h_ext, h_nop, h_nop, h_nop};

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
static const char *want_id;                      /* the plug-in whose sounds discovery keeps (found_*) */
static const char *TRIO = "studio.ellic.melodee-modules.c273261a2cf92788";
#define TRIO_KEY "c273261a2cf92788"

static int open_plug(plug *x, const char *id, double rate)
{
    memset(x, 0, sizeof *x);
    x->p = factory->create_plugin(factory, &HOST, id);
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
    return 0;
}
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
static clap_id param_id(plug *x, const char *name)
{
    uint32_t n = x->params->count(x->p);
    for (uint32_t i = 0; i < n; i++) {
        clap_param_info_t info;
        x->params->get_info(x->p, i, &info);
        if (!strcmp(info.name, name))
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
static char found_key[8][40], found_name[8][40];   /* TRIO's, in order */
static bool ix_filetype(const clap_preset_discovery_indexer_t *ix, const clap_preset_discovery_filetype_t *f) { (void)ix, (void)f; return true; }
static int nfile;                                /* (file locations: SysEx files) */
static char file_name[16][256], file_path[16][4096];
static bool ix_location(const clap_preset_discovery_indexer_t *ix, const clap_preset_discovery_location_t *l)
{
    (void)ix;
    if (l->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) {
        CHECK(l->location && l->name, "a file location");
        if (nfile < 16 && l->location && l->name) {
            snprintf(file_name[nfile], sizeof file_name[nfile], "%s", l->name);
            snprintf(file_path[nfile], sizeof file_path[nfile], "%s", l->location);
        }
        nfile++;
    } else {
        CHECK(l->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && !l->location, "a plug-in location");
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
    if (!strcmp(id->abi, "clap") && !strcmp(id->id, want_id ? want_id : TRIO)) {
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
/* a module's parameters: each value in its range with a text read back, each page's slots its parameters, each set
 * by an event read back (in its range), its cutoff heard (the lowest and the highest) */
static void check_params(plug *x, const char *name)
{
    double rate = 48000;
    uint32_t np = x->params->count(x->p), pages = x->rc->count(x->p), back = 0, set = 0, schema = 0;
    clap_id cutoff = CLAP_INVALID_ID;
    double cmin = 0, cmax = 0;
    for (uint32_t i = 0; i < np; i++) {
        clap_param_info_t a;
        char txt[64], again[64];
        double v, r, step;
        x->params->get_info(x->p, i, &a);
        schema += a.id >= 0x10000u;
        v = value(x, a.id);
        CHECK(v >= a.min_value && v <= a.max_value, "%s %s: value %g", name, a.name, v);
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
    CHECK(pages > 0 && pages <= 256, "%s: %u pages", name, pages);
    for (uint32_t i = 0; i < pages; i++) {
        clap_remote_controls_page_t pg;
        int any = 0;
        CHECK(x->rc->get(x->p, i, &pg) && pg.page_name[0], "%s page %u", name, i);
        for (int k = 0; k < CLAP_REMOTE_CONTROLS_COUNT; k++) {
            double v;
            any |= pg.param_ids[k] != CLAP_INVALID_ID;
            CHECK(pg.param_ids[k] == CLAP_INVALID_ID || x->params->get_value(x->p, pg.param_ids[k], &v),
                  "%s page %s slot %d", name, pg.page_name, k);
        }
        CHECK(any, "%s page %s: empty", name, pg.page_name);
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
        double want;
        x->params->get_info(x->p, i, &a);
        want = floor((a.min_value + a.max_value) / 2);
        ev_param(&ev, 0, a.id, want);
        run(x, &ev, L, R, 64);
        set++;
        back += value(x, a.id) == want;
        CHECK(value(x, a.id) >= a.min_value && value(x, a.id) <= a.max_value, "%s %s set: %g", name, a.name, value(x, a.id));
    }
    printf("%-6s %u parameters (%u from its schema), %u pages, %u of %u read back as set\n", name, np, schema, pages,
           back, set);
    CHECK(back * 10 >= set * 9, "%s: only %u of %u parameters read back as set", name, back, set);
    {
        evlist ev = {0};
        ev_midi(&ev, 0, 0xB0, 120, 0);
        run(x, &ev, L, R, 256);
    }
}

/* a module: each factory sound loaded and played (a chord, held, released), its sound finite and heard; its state
 * read back by a new part */
static void play_module(const clap_plugin_descriptor_t *d)
{
    {
        double rate = 48000;
        plug x, y;
        uint32_t sounds = 0, quiet = 0;
        char idhex[17];
        static float L2[48000 * 3], R2[48000 * 3];
        snprintf(idhex, sizeof idhex, "%s", d->id + strlen("studio.ellic.melodee-modules."));
        if (open_plug(&x, d->id, rate)) {
            CHECK(0, "open %s", d->name);
            return;
        }
        for (uint32_t k = 0; k < 256; k++) {
            char key[40];
            evlist ev = {0};
            snprintf(key, sizeof key, "%s/%u", idhex, k);
            if (!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, key))
                break;
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 48, 0.8), ev_note(&ev, CLAP_EVENT_NOTE_ON, 3, 55, 0.8);
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 7, 60, 0.8);
            run(&x, &ev, L, R, SEC(1.0));
            ev.n = 0;
            ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 48, 0), ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 55, 0);
            ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 60, 0);
            run(&x, &ev, L + SEC(1.0), R + SEC(1.0), SEC(1.0));
            CHECK(finite_all(L, SEC(2.0)) && finite_all(R, SEC(2.0)), "%s sound %u finite", d->name, k);
            CHECK(peak(L, SEC(2.0)) < 8.0 && peak(R, SEC(2.0)) < 8.0, "%s sound %u: peak %g", d->name, k, peak(L, SEC(2.0)));
            quiet += rms(L, SEC(1.0)) < 1e-4 && rms(R, SEC(1.0)) < 1e-4;
            if (wavdir && k < 4) {
                char path[512];
                snprintf(path, sizeof path, "%s/%s-%u.wav", wavdir, d->name, k);
                wav(path, L, R, SEC(2.0), (uint32_t)rate);
            }
            ev.n = 0;
            ev_midi(&ev, 0, 0xB0, 120, 0);
            run(&x, &ev, L, R, 256);
            sounds++;
        }
        CHECK(sounds > 0 && quiet * 4 < sounds, "%s: %u sounds, %u silent", d->name, sounds, quiet);
        {                                        /* the cost of eight voices of its first sound */
            char key[40];
            evlist ev = {0};
            clock_t c0;
            snprintf(key, sizeof key, "%s/0", idhex);
            x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, key);
            for (int k = 0; k < 8; k++)
                ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 40 + 5 * k, 0.8);
            c0 = clock();
            run(&x, &ev, L, R, SEC(5.0));
            printf("%-6s %3u factory sounds played, %u silent with a held chord; eight voices %.2f%% of real time here\n",
                   d->name, sounds, quiet, (double)(clock() - c0) / CLOCKS_PER_SEC * 20.0);
            ev.n = 0;
            ev_midi(&ev, 0, 0xB0, 120, 0);
            run(&x, &ev, L, R, 256);
        }
        check_params(&x, d->name);               /* (its first sound) */
        {                                        /* (the dither and the voices' ages run on: compare the state) */
            static blob b, b2;
            clap_ostream_t os = {&b, os_write}, os2 = {&b2, os_write};
            clap_istream_t is = {&b, is_read};
            evlist ev = {0};
            uint32_t np = x.params->count(x.p), same = 0;
            b.n = b.at = b2.n = b2.at = 0;
            CHECK(x.state->save(x.p, &os), "%s save", d->name);
            CHECK(!open_plug(&y, d->id, rate), "open %s again", d->name);
            CHECK(y.state->load(y.p, &is), "%s load", d->name);
            CHECK(y.state->save(y.p, &os2) && b2.n == b.n && !memcmp(b.b, b2.b, b.n), "%s: the state read back (%u B)",
                  d->name, (unsigned)b.n);
            for (uint32_t k = 0; k < np; k++) {
                clap_param_info_t info;
                x.params->get_info(x.p, k, &info);
                same += value(&x, info.id) == value(&y, info.id);
            }
            CHECK(same == np, "%s: %u of %u values the same", d->name, same, np);
            ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 57, 0.9);
            run(&y, &ev, L2, R2, SEC(1.0));
            CHECK(finite_all(L2, SEC(1.0)), "%s: the loaded state plays", d->name);
            close_plug(&y);
        }
        close_plug(&x);
    }
}

/* ---- SysEx files in the SysEx folder beside the plug-in */
#define HYDRA_KEY "85bf54aaf90d0ae6"
#define PPG_KEY "3627d34f43eafbf8"
#define ID_PREFIX "studio.ellic.melodee-modules."
static int errors;
static void rx_error_count(const clap_preset_discovery_metadata_receiver_t *r, int32_t e, const char *m)
{
    (void)r, (void)e;
    printf("  SysEx file: %s\n", m);
    errors++;
}
static int last_patch(plug *x, uint8_t *p, uint32_t len)   /* the state's patch: its last bytes */
{
    static blob b;
    clap_ostream_t os = {&b, os_write};
    b.n = b.at = 0;
    if (!x->state->save(x->p, &os) || b.n < len)
        return -1;
    memcpy(p, b.b + b.n - len, len);
    return 0;
}
static FILE *open_syx(const char *dir, const char *name)
{
    char path[4200];
    snprintf(path, sizeof path, "%s/%s.syx", dir, name);
    return fopen(path, "wb");
}
static int file_at(const char *name)             /* its file location, -1 none */
{
    for (int i = 0; i < nfile && i < 16; i++)
        if (!strcmp(file_name[i], name))
            return i;
    return -1;
}
/* files written into the SysEx folder (HYDRA's native frames of three factory sounds with bytes between them, a PPG
 * bank of made-up programs, one cut short, one without a frame; MM_PPG_BANK: a PPG Wave 2.2 factory bank), then
 * discovery: a file location each, the sounds of each by the plug-in of the module that takes it; then loaded by their
 * keys: HYDRA's as they were sent, the factory bank's as the factory programs */
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
    want_id = ID_PREFIX HYDRA_KEY;
    nfound = with_id = 0;
    pv = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
    CHECK(pv && pv->init(pv) && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, &rx) && with_id >= 4,
          "HYDRA's factory sounds");
    for (int i = 0; i < 3; i++)
        snprintf(hname[i], sizeof hname[i], "%s", found_name[i + 1]);
    pv->destroy(pv);
    CHECK(!open_plug(&x, ID_PREFIX HYDRA_KEY, 48000), "open HYDRA");
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
    /* discovery: the factory location, then the files by name */
    locations = nfile = 0;
    pv = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
    CHECK(pv && pv->init(pv), "provider with SysEx files");
    CHECK(nfile == 4 + factory && locations == nfile + 1, "%d file locations", nfile);
    CHECK(!strcmp(file_name[0], "HYDRA test") && !strcmp(file_name[1], "no frame") && !strcmp(file_name[2], "PPG cut") &&
          !strcmp(file_name[3 + factory], "PPG made"), "the files by name");
    want_id = ID_PREFIX HYDRA_KEY;
    nfound = with_id = errors = 0;
    f = file_at("HYDRA test");
    CHECK(f >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], &rx) && nfound == 3 &&
          with_id == 3 && !errors, "HYDRA test: three HYDRA sounds (%d, %d HYDRA's)", nfound, with_id);
    for (int i = 0; i < 3 && i < with_id; i++) {
        char key[40];
        snprintf(key, sizeof key, HYDRA_KEY "/%d", i);
        CHECK(found_name[i][0] && !strncmp(found_name[i], hname[i], strlen(found_name[i])) && !strcmp(found_key[i], key),
              "HYDRA test %d: %s %s (factory %s)", i, found_name[i], found_key[i], hname[i]);
    }
    if (have_ppg) {                              /* (the PPG module: a local test file only) */
        want_id = ID_PREFIX PPG_KEY;
        nfound = with_id = errors = 0;
        f = file_at("PPG made");
        CHECK(f >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], &rx) && nfound == 100 &&
              with_id == 100 && !errors, "PPG made: a hundred PPG programs (%d, %d PPG's)", nfound, with_id);
        CHECK(!strcmp(found_name[1], "PROGRAM 01") && !strcmp(found_key[1], PPG_KEY "/1"), "PPG made 1: %s %s",
              found_name[1], found_key[1]);
        nfound = errors = 0;
        f = file_at("PPG cut");
        CHECK(f >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], &rx) && !nfound && errors == 1,
              "PPG cut: no sound, the module's error (%d)", errors);
    }
    nfound = errors = 0;
    f = file_at("no frame");
    CHECK(f >= 0 && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], &rx) && !nfound && !errors,
          "no frame: no sound");
    pv->destroy(pv);
    /* loaded: HYDRA's sounds as sent; another synth's key, a sound past the file's, no file: refused */
    f = file_at("HYDRA test");
    for (int i = 0; f >= 0 && i < 3; i++) {
        char key[40];
        evlist ev = {0};
        snprintf(key, sizeof key, HYDRA_KEY "/%d", i);
        CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], key) && !last_patch(&x, p, 144) &&
              !memcmp(p, pat[i], 144), "HYDRA test %d loads the sound sent", i);
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 60, 0.8);
        run(&x, &ev, L, R, SEC(0.5));
        CHECK(rms(L, SEC(0.5)) > 0.002, "HYDRA test %d plays", i);
        ev.n = 0;
        ev_note(&ev, CLAP_EVENT_NOTE_OFF, 0, 60, 0);
        run(&x, &ev, L, R, SEC(0.5));
    }
    if (f >= 0) {
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], HYDRA_KEY "/3"),
              "no fourth sound");
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], PPG_KEY "/0"),
              "not another synth's");
    }
    snprintf(path, sizeof path, "%s/none.syx", dir);
    CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path, HYDRA_KEY "/0"), "no file");
    close_plug(&x);
    if (have_ppg) {
        /* PPG: the factory bank's programs are the factory sounds after INIT */
        CHECK(!open_plug(&x, ID_PREFIX PPG_KEY, 48000), "open PPG");
        if ((f = file_at("PPG factory")) >= 0) {
            for (int k = 0; k < 100; k++) {
                char key[40];
                snprintf(key, sizeof key, PPG_KEY "/%d", k);
                if (!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_FILE, file_path[f], key) || last_patch(&x, p, 115))
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
        close_plug(&x);
    } else {
        printf("PPG    no PPG module (tests/fixtures/ppg.mmod, local only: make fixtures): its banks not tested\n");
    }
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

/* one module file alone in the Modules folder: one plug-in, its sounds its own, it plays */
static int one_file(const char *path)
{
    void *lib;
    const clap_plugin_entry_t *e = open_entry(path, &lib);
    const clap_plugin_descriptor_t *d;
    if (!e)
        return 1;
    factory = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    CHECK(factory && factory->get_plugin_count(factory) == 1, "one plug-in: %u", factory ? factory->get_plugin_count(factory) : 0);
    if (fails)
        return 1;
    d = factory->get_plugin_descriptor(factory, 0);
    {
        const clap_preset_discovery_factory_t *pf = e->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
        clap_preset_discovery_indexer_t ix = {CLAP_VERSION_INIT, "test", "test", 0, "1", 0, ix_filetype, ix_location, ix_pack, ix_ext};
        clap_preset_discovery_metadata_receiver_t rx = {0, rx_error, rx_begin, rx_id, rx_pack, rx_flags, rx_str, rx_str, rx_time, rx_str, rx_extra};
        const clap_preset_discovery_provider_t *pv = pf ? pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id) : 0;
        want_id = d->id;
        CHECK(pv && pv->init(pv) && pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, &rx), "discovery");
        CHECK(nfound > 0 && nfound == with_id, "%s: %d sounds, %d of them its own", d->name, nfound, with_id);
        if (pv)
            pv->destroy(pv);
    }
    char name[CLAP_NAME_SIZE];
    snprintf(name, sizeof name, "%s", d->name);   /* (the descriptor goes with deinit) */
    play_module(d);
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
    CHECK(fa->get_plugin_count(fa) == fb->get_plugin_count(fb) && fa->get_plugin_count(fa) > 0, "the same modules");
    factory = fa;
    CHECK(!open_plug(&x, TRIO, rate), "open TRIO in A");
    factory = fb;
    CHECK(!open_plug(&y, TRIO, rate), "open TRIO in B");
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

/* each plug-in's pages and their parameters with their values (what a host's pages show) */
static int list(const char *path)
{
    void *lib;
    const clap_plugin_entry_t *e = open_entry(path, &lib);
    if (!e)
        return 1;
    factory = e->get_factory(CLAP_PLUGIN_FACTORY_ID);
    for (uint32_t i = 0; i < factory->get_plugin_count(factory); i++) {
        const clap_plugin_descriptor_t *d = factory->get_plugin_descriptor(factory, i);
        plug x;
        if (open_plug(&x, d->id, 48000))
            continue;
        printf("== %s (%s)\n", d->name, d->id);
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

int main(int argc, char **argv)
{
    void *lib;
    const clap_plugin_entry_t *entry;
    wavdir = getenv("MM_WAV_DIR");
    if (argc == 3 && !strcmp(argv[1], "--one"))
        return one_file(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--staged"))
        return staged(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "--list"))
        return list(argv[2]);
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
        snprintf(trio_version, sizeof trio_version, "%u", (unsigned)(orig[48] | orig[49] << 8));
        b[n / 2] ^= 0x55;
        snprintf(path, sizeof path, "%s/broken.mmod", dir);
        if ((out = fopen(path, "wb")))
            fwrite(b, 1, n, out), fclose(out);
        /* TRIO changed after Melodee signed it, each whole again: a byte of its code, a newer version in its head (it
         * would be the one taken), its code's signature cut off. None is a plug-in */
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
    CHECK(factory && factory->get_plugin_count(factory) == 3u + (uint32_t)have_ppg, "%d modules: %u", 3 + have_ppg,
          factory ? factory->get_plugin_count(factory) : 0);
    {
        static const char *const ALL[] = {"HYDRA", "PPG", "TRIO", "WHEEL"}, *const NO_PPG[] = {"HYDRA", "TRIO", "WHEEL"};
        const char *const *NAMES = have_ppg ? ALL : NO_PPG;
        int trio = 0;
        for (uint32_t i = 0; i < factory->get_plugin_count(factory); i++) {
            const clap_plugin_descriptor_t *d = factory->get_plugin_descriptor(factory, i);
            int instrument = 0;
            CHECK(i < 3u + (uint32_t)have_ppg && !strcmp(d->name, NAMES[i]), "plug-in %u: %s", i, d->name);
            for (const char *const *f = d->features; *f; f++)
                instrument |= !strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT);
            CHECK(instrument, "%s an instrument", d->name);
            trio |= !strcmp(d->id, TRIO) && !strcmp(d->vendor, "melodee");
            if (!strcmp(d->id, TRIO))
                CHECK(!strcmp(d->version, trio_version), "TRIO's own version, not the changed file's: %s", d->version);
        }
        CHECK(trio, "TRIO by its module ID");
    }

    /* the parameters: unique ids, defaults in range, every value's text read back */
    {
        plug x;
        uint32_t n, pages;
        CHECK(!open_plug(&x, TRIO, 48000), "open");
        CHECK(x.params && x.state && x.preset && x.rc && x.ap && x.np && x.lat, "extensions");
        CHECK(x.ap->count(x.p, false) == 1 && x.ap->count(x.p, true) == 0, "audio ports");
        {
            clap_note_port_info_t ni;
            CHECK(x.np->get(x.p, 0, true, &ni) && (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI) &&
                  (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP), "note port dialects");
        }
        n = x.params->count(x.p);
        CHECK(n == 8 + 25 + 1, "parameters: %u", n);
        for (uint32_t i = 0; i < n; i++) {
            clap_param_info_t a, b;
            char txt[64];
            double v, back;
            x.params->get_info(x.p, i, &a);
            for (uint32_t j = i + 1; j < n; j++) {
                x.params->get_info(x.p, j, &b);
                CHECK(a.id != b.id, "id %u twice", a.id);
            }
            CHECK(a.min_value <= a.default_value && a.default_value <= a.max_value, "%s default", a.name);
            v = value(&x, a.id);
            CHECK(v >= a.min_value && v <= a.max_value, "%s value %g", a.name, v);
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
        pages = x.rc->count(x.p);
        CHECK(pages == 6, "pages: %u", pages);
        for (uint32_t i = 0; i < pages; i++) {
            clap_remote_controls_page_t pg;
            CHECK(x.rc->get(x.p, i, &pg) && pg.page_name[0], "page %u", i);
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
            CHECK(!open_plug(&x, TRIO, rate), "open at %g", rate);
            lat = x.lat->get(x.p);
            CHECK(rate == 44100 ? lat == 0 : lat > 0 && lat < 64, "latency %u at %g", lat, rate);
            /* a plain tone: TRI3 with its three at unison, no detune, the filter open, POLY */
            ev_param(&ev, 0, 116, 4), ev_param(&ev, 0, 117, 0), ev_param(&ev, 0, 118, 0), ev_param(&ev, 0, 119, 0);
            ev_param(&ev, 0, 121, 127), ev_param(&ev, 0, 122, 0), ev_param(&ev, 0, 37, 0), ev_param(&ev, 0, 5, 0);
            ev_param(&ev, 0, 1, 0), ev_param(&ev, 0, 3, 127);
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
        CHECK(!open_plug(&x, TRIO, rate), "open");
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
        ev_param(&ev, 0, 37, 3);                 /* UNISON */
        ev_param(&ev, 0, 38, 60);                /* GLIDE */
        ev_note(&ev, CLAP_EVENT_NOTE_ON, 0, 48, 1.0);
        run(&x, &ev, L, R, SEC(0.5));
        CHECK(finite_all(L, SEC(0.5)) && peak(L, SEC(0.5)) < 4.0 && rms(L, SEC(0.5)) > 0.01, "UNISON");
        ev.n = 0;
        ev_midi(&ev, 0, 0xB0, 120, 0);           /* all sounds off */
        run(&x, &ev, L, R, SEC(0.1));
        CHECK(rms(L + SEC(0.05), SEC(0.05)) < 1e-6, "all sounds off: %g", rms(L + SEC(0.05), SEC(0.05)));
        close_plug(&x);
    }

    /* state: saved, changed, loaded back; another plug-in's state refused */
    {
        plug x;
        blob b = {0};
        clap_ostream_t os = {&b, os_write};
        clap_istream_t is = {&b, is_read};
        CHECK(!open_plug(&x, TRIO, 48000), "open");
        {
            evlist ev = {0};
            ev_param(&ev, 0, 121, 33), ev_param(&ev, 0, 1000, 7);
            run(&x, &ev, L, R, 64);
        }
        CHECK(value(&x, 121) == 33 && value(&x, 1000) == 7, "values from events");
        CHECK(x.state->save(x.p, &os), "save");
        {
            evlist ev = {0};
            ev_param(&ev, 0, 121, 100), ev_param(&ev, 0, 1000, 12);
            run(&x, &ev, L, R, 64);
        }
        rescans = 0;
        CHECK(x.state->load(x.p, &is), "load");
        CHECK(value(&x, 121) == 33 && value(&x, 1000) == 7 && rescans, "values loaded");
        b.b[0] ^= 0xFF, b.at = 0;
        CHECK(!x.state->load(x.p, &is), "a bad state refused");
        close_plug(&x);
    }

    /* factory sounds: discovered with this plug-in's id, loaded by their key */
    {
        const clap_preset_discovery_factory_t *pf = entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
        clap_preset_discovery_indexer_t ix = {CLAP_VERSION_INIT, "test", "test", 0, "1", 0, ix_filetype, ix_location, ix_pack, ix_ext};
        clap_preset_discovery_metadata_receiver_t rx = {0, rx_error, rx_begin, rx_id, rx_pack, rx_flags, rx_str, rx_str, rx_time, rx_str, rx_extra};
        const clap_preset_discovery_provider_t *pv;
        plug x;
        CHECK(pf && pf->count(pf) == 1, "preset factory");
        pv = pf->create(pf, &ix, pf->get_descriptor(pf, 0)->id);
        CHECK(pv && pv->init(pv) && locations == 1, "provider");
        CHECK(pv->get_metadata(pv, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, &rx), "metadata");
        CHECK(nfound > 5 && with_id == 5, "TRIO's five sounds among %d: %d", nfound, with_id);
        CHECK(!strcmp(found_name[3], "RING BELL") && !strcmp(found_key[3], TRIO_KEY "/3"), "RING BELL: %s %s", found_name[3], found_key[3]);
        pv->destroy(pv);
        CHECK(!open_plug(&x, TRIO, 48000), "open");
        CHECK(value(&x, 116) == 0 && value(&x, 3) == 60 && value(&x, 37) == 1, "a new part plays FAT BASS");
        CHECK(x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/3"), "load RING BELL");
        CHECK(value(&x, 116) == 13 && value(&x, 118) == 18 && value(&x, 3) == 0 && value(&x, 37) == 0, "RING BELL's values");
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/9"), "no tenth sound");
        CHECK(!x.preset->from_location(x.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, "0000000000000000/0"), "not another synth's");
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

    for (uint32_t i = 0; i < factory->get_plugin_count(factory); i++)
        play_module(factory->get_plugin_descriptor(factory, i));

    /* two parts at once, each its own sound; then the cost of eight voices */
    {
        double rate = 48000;
        plug a, b;
        evlist ev = {0};
        static float L2[48000], R2[48000];
        clock_t c0;
        double secs;
        CHECK(!open_plug(&a, TRIO, rate) && !open_plug(&b, TRIO, rate), "two parts");
        CHECK(b.preset->from_location(b.p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, 0, TRIO_KEY "/4"), "B: CHIP CHOIR");
        CHECK(value(&a, 116) == 0 && value(&b, 116) == 1, "each its own values");
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

    entry->deinit();
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("melodee-modules ok\n");
    return 0;
}
