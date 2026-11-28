/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The host side of ABI 3 for plug-ins: the shared tables, the host table and the part around a module's callbacks.
 * Written for this host from the contract (sdk/mod_abi3.h): the curves are the device's (the same table formulas),
 * the voice handling follows what the device does, without its shared voice budget, matrix, arpeggiator or mixer. */
#include "mm_engine.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm_fm6_tables.h"

/* ---- tables (the device's formulas; MOD3_T_* by id, the rest the part's own) */
static int16_t T_SINE[1025], T_TANH[257];
static uint16_t T_SVF_G[128];
static uint32_t T_PITCH_OCT[192], T_A4_RATIO[81];
static int32_t T_A4_LOG[81];
static float T_P5_TANH[1025];
static float T_FM6_SIN[1025];                    /* (FM6 and CZ-1's: the device's, mm_fm6_tables.h) */
static uint16_t T_FM6_MKI_LOG[1024], T_FM6_MKI_EXP[1024], T_FM6_OPL_LOG[256], T_FM6_OPL_EXP[256];
static uint32_t ENV_LIN[128], LFO_INC[128];
static uint16_t ENV_EXP[128];

static void tables_make(void)
{
    mm_fm6_tables(T_FM6_SIN, T_FM6_MKI_LOG, T_FM6_MKI_EXP, T_FM6_OPL_LOG, T_FM6_OPL_EXP);
    for (int i = 0; i < 1025; i++)
        T_SINE[i] = (int16_t)nearbyint(32767.0 * sin(2.0 * M_PI * i / 1024.0));
    for (int i = 0; i < 257; i++)
        T_TANH[i] = (int16_t)(32767.0 * tanh(i / 256.0 * 2.0));
    for (int p = 1856; p < 2048; p++) {
        double f = 440.0 * pow(2.0, (p / 16.0 - 69.0) / 12.0), inc = f / MM_FS * 4294967296.0;
        T_PITCH_OCT[p - 1856] = inc > 2147483647.0 ? 0x7FFFFFFFu : (uint32_t)inc;
    }
    for (int i = 0; i < 1025; i++)
        T_P5_TANH[i] = (float)tanh(i / 128.0);
    for (int hz = 400; hz <= 480; hz++) {
        T_A4_RATIO[hz - 400] = (uint32_t)nearbyint(16777216.0 * hz / 440.0);
        T_A4_LOG[hz - 400] = (int32_t)nearbyint(16777216.0 * log2(hz / 440.0));
    }
    for (int v = 0; v < 128; v++) {
        double ms = pow(10000.0, v / 127.0), tau = ms / 1000.0 / 4.6;
        double lin = 16777216.0 * MM_CTL / (ms * MM_FS / 1000.0);
        double k = 65536.0 * (1.0 - exp(-(double)MM_CTL / (tau * MM_FS)));
        double hz = 0.05 * pow(800.0, v / 127.0), fc = 30.0 * pow(16000.0 / 30.0, v / 127.0);
        ENV_LIN[v] = lin < 1.0 ? 1u : (uint32_t)lin;
        ENV_EXP[v] = (uint16_t)(k < 1.0 ? 1.0 : k > 65535.0 ? 65535.0 : k);
        LFO_INC[v] = (uint32_t)(hz * MM_CTL / MM_FS * 4294967296.0);
        T_SVF_G[v] = (uint16_t)(4096.0 * tan(M_PI * fmin(fc, 0.45 * MM_FS) / MM_FS));
    }
}

/* phase increment of pitch p (1/16 semitone, 0..2047) */
static uint32_t pitch_inc(uint32_t p)
{
    uint32_t i = (p > 2047u ? 2047u : p) + 64u, o = (i * 21846u) >> 22;
    return T_PITCH_OCT[i - o * 192u] >> (10u - o);
}
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }
static inline int32_t mulq16(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k) >> 16));
}

/* ---- the host table */
static int16_t h_dither[MOD3_DITH_N];
static float h_dither_f[2u * MOD3_DITH_N];
static uint32_t h_dither_r = 0x2545F491u;
static int16_t h_a4 = 440;
static int16_t h_song_g[64];
static uint8_t h_midi_rel[MOD3_NPART];
static uint8_t h_was;                            /* note_on's: set under h_lock */
static atomic_flag h_lock = ATOMIC_FLAG_INIT;
static const uint8_t h_zero8;
static const int h_zero_int;
static uint32_t h_paired;
static volatile uint32_t h_ms;
static uint32_t h_ms_frac;
static const int32_t h_note_pitch;
static const uint32_t h_note_len;
static const mod_rec_v3 h_rec[MOD3_NREC];
static const volatile uint32_t h_rec_gen;
static uint8_t h_sx_scratch[MOD3_SX_MAX];
/* the part whose module this thread is calling: the host's calls that name a part by number (part_mem, part,
 * part_patch, patch_put) mean it (every part is its own module's part 0) */
static _Thread_local mm_part *h_cur;
#define MM_ENTER(t) (h_cur = (t))
void mm_enter(mm_part *t) { MM_ENTER(t); }
/* host 3, while mm_sysex calls a module: who takes the sounds it offers (sx_stage), where its messages go */
static _Thread_local mm_sx_take h_take;
static _Thread_local void *h_take_ctx;
static _Thread_local char *h_msg;
static _Thread_local uint32_t h_msg_cap, h_taken;

static void dither_block(void)
{
    uint32_t r = h_dither_r;
    for (uint32_t k = 0; k < MOD3_DITH_N; k++) {
        r = r * 1664525u + 1013904223u;
        h_dither[k] = (int16_t)((int32_t)(r >> 17) - (int32_t)((r >> 2) & 32767u));
        h_dither_f[k] = h_dither_f[k + MOD3_DITH_N] = (float)h_dither[k] * (1.0f / 32768.0f);
    }
    h_dither_r = r;
}

static const void *h_table(uint32_t id)
{
    switch (id) {
    case MOD3_T_SINE: return T_SINE;
    case MOD3_T_TANH_Q15: return T_TANH;
    case MOD3_T_SVF_G: return T_SVF_G;
    case MOD3_T_PITCH_OCT: return T_PITCH_OCT;
    case MOD3_T_A4_RATIO: return T_A4_RATIO;
    case MOD3_T_A4_LOG: return T_A4_LOG;
    case MOD3_T_P5_TANH: return T_P5_TANH;
    case MOD3_T_FM6_SIN: return T_FM6_SIN;
    case MOD3_T_FM6_MKI_LOG: return T_FM6_MKI_LOG;
    case MOD3_T_FM6_MKI_EXP: return T_FM6_MKI_EXP;
    case MOD3_T_FM6_OPL_LOG: return T_FM6_OPL_LOG;
    case MOD3_T_FM6_OPL_EXP: return T_FM6_OPL_EXP;
    default: return 0;
    }
}
static mm_part *h_part_of(uint32_t k) { return h_cur && k == h_cur->h.index ? h_cur : 0; }
static void *h_part_mem(uint32_t part, uint32_t bytes)
{
    mm_part *t = h_part_of(part);
    if (!t)
        return 0;
    if (bytes > t->mem_bytes) {                  /* (more than state() asked at open: rare, not real-time) */
        uint8_t *m = calloc(1, bytes);
        if (!m)
            return 0;
        if (t->mem)
            memcpy(m, t->mem, t->mem_bytes);
        free(t->mem);
        t->mem = m;
        t->mem_bytes = bytes;
    }
    return t->mem;
}
static uint32_t h_nvoice(const mod_part_v3 *h)
{
    const mm_part *t = (const mm_part *)(const void *)h;
    uint32_t c = t->e->cap ? t->e->cap(h) : t->e->poly;
    return !c ? MM_NPOLY : c < MOD3_NVOICE ? c : MOD3_NVOICE;
}
static void h_scale_pitch(const mod_part_v3 *h, mod_voice_v3 *v, uint32_t note)
{
    (void)h;
    v->pitch16 = (int32_t)note * 16;             /* (no scales here: equal temperament) */
}
static uint32_t h_pitch_inc(uint32_t p) { return pitch_inc(p); }
static int32_t h_octave(const mod_part_v3 *h) { (void)h; return 0; }
static void h_batch_push(mod3_batch_fn run, const void *data, uint32_t bytes, int32_t *out, uint32_t n, uint32_t cls)
{
    (void)bytes, (void)n, (void)cls;
    run(data, out);                              /* no second core: at once */
}
static void h_batch_push_cost(mod3_batch_fn run, const void *data, uint32_t bytes, int32_t *out, uint32_t n,
                              uint32_t cls, uint32_t cost)
{
    (void)cost;
    h_batch_push(run, data, bytes, out, n, cls);
}
static int h_worker_submit(void (*fn)(void *), void *ctx) { (void)fn, (void)ctx; return 0; }
static int h_worker_join(void) { return 0; }
static void h_profile_add(uint32_t cls, uint32_t ticks) { (void)cls, (void)ticks; }
static void h_lz_unpack(uint8_t *o, const uint8_t *b, uint32_t n)
{
    uint8_t *e = o + n;
    while (o < e) {
        uint32_t c = *b++;
        if (c < 0x80u) {
            for (c++; c; c--)
                *o++ = *b++;
        } else {
            const uint8_t *src = o - (b[0] | (uint32_t)b[1] << 8) - 1;
            b += 2;
            for (c = (c & 0x7fu) + 3u; c; c--)
                *o++ = *src++;
        }
    }
}
static int h_file_read(uint32_t self, uint32_t off, void *dst, uint32_t n) { (void)self, (void)off, (void)dst, (void)n; return -1; }
static uint8_t *h_patch(const mod_part_v3 *h) { return ((mm_part *)(void *)(uintptr_t)h)->patch; }
static uint16_t h_color(uint32_t role) { (void)role; return 0; }
static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { (void)x, (void)y, (void)w, (void)h, (void)c; }
static void h_rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    (void)x, (void)y, (void)w, (void)h, (void)r, (void)c;
}
static void h_text(int32_t x, int32_t y, const char *s, uint32_t font, uint16_t c) { (void)x, (void)y, (void)s, (void)font, (void)c; }
static mod_part_v3 *h_part(uint32_t k) { mm_part *t = h_part_of(k); return t ? &t->h : 0; }
static uint8_t *h_part_patch(uint32_t k, uint32_t engine)
{
    mm_part *t = h_part_of(k);
    return t && t->h.engine == engine ? t->patch : 0;
}
static int h_patch_put(uint32_t k, uint32_t engine, const uint8_t *src, uint32_t n, uint32_t how)
{
    mm_part *t = h_part_of(k);
    if (!t || t->h.engine != engine || n > MOD3_PATCH_MAX)
        return 1;
    memcpy(t->patch, src, n);
    if (t->e->patch_changed)
        t->e->patch_changed(&t->h, t->patch, how, 0xFFFFu);
    return 0;
}
static int h_sx_part(uint32_t engine, uint32_t ch) { (void)engine, (void)ch; return -1; }
static int h_sx_send(const uint8_t *f, uint32_t n) { (void)f, (void)n; return -1; }
static uint8_t *h_sx_scratch_get(void) { return h_sx_scratch; }
/* the sounds a module offers for user slots: each to mm_sysex's taker (none outside it) */
static int h_sx_stage(uint32_t e, const uint8_t *p, uint32_t count, uint32_t stride)
{
    mm_part *t = h_cur;
    if (!h_take || !t || e != t->h.engine || !p || !count || !stride)
        return 0;
    for (uint32_t i = 0; i < count; i++)
        h_take(h_take_ctx, p + (size_t)i * stride, stride < MOD3_PATCH_MAX ? stride : MOD3_PATCH_MAX);
    h_taken += count;
    return 1;
}
static uint32_t h_slot_get(uint32_t e, uint32_t slot, uint8_t *patch) { (void)e, (void)slot, (void)patch; return 0; }
static uint32_t h_store_slot(uint32_t e) { (void)e; return 0; }
static void h_message(const char *s)            /* (mm_sysex's message; else nowhere) */
{
    if (h_msg && h_msg_cap && s)
        snprintf(h_msg, h_msg_cap, "%s", s);
}
static int h_rec_begin(uint32_t slot, uint32_t len) { (void)slot, (void)len; return -1; }
static int h_rec_write(const void *src, uint32_t n) { (void)src, (void)n; return -1; }
static int h_rec_commit(uint32_t slot) { (void)slot; return -1; }
static int h_rec_delete(uint32_t slot) { (void)slot; return -1; }
static void h_wdt(void) {}
static int32_t *h_stereo(const mod_part_v3 *h)
{
    mm_part *t = (mm_part *)(void *)(uintptr_t)h;
    if (!t->in_post)
        return 0;
    t->stereo_used = 1;
    return t->left;
}

static const mod_host_v3 HOST = {
    .version = MOD3_HOST, .size = sizeof(mod_host_v3),
    .was = &h_was, .a4 = &h_a4, .song_g = h_song_g, .midi_rel = h_midi_rel, .dither = h_dither, .dither_f = h_dither_f,
    .table = h_table, .part_mem = h_part_mem, .scale_pitch = h_scale_pitch, .nvoice = h_nvoice,
    .pitch_inc = h_pitch_inc, .octave = h_octave,
    .batch_active = &h_zero8, .batch_push = h_batch_push, .batch_push_cost = h_batch_push_cost,
    .worker_online = &h_zero_int, .worker_submit = h_worker_submit, .worker_join = h_worker_join,
    .profile_add = h_profile_add, .lz_unpack = h_lz_unpack, .file_read = h_file_read, .legacy = 0,
    .patch = h_patch, .color = h_color, .fill = h_fill, .rrect = h_rrect, .text = h_text, .paired = &h_paired,
    .part = h_part, .part_patch = h_part_patch, .patch_put = h_patch_put, .sx_part = h_sx_part, .sx_send = h_sx_send,
    .sx_scratch = h_sx_scratch_get, .sx_stage = h_sx_stage, .slot_get = h_slot_get, .store_slot = h_store_slot,
    .message = h_message, .ms = &h_ms,
    .note_pitch = &h_note_pitch, .note_len = &h_note_len, .rec = h_rec, .rec_gen = &h_rec_gen, .rec_base = 0,
    .rec_begin = h_rec_begin, .rec_write = h_rec_write, .rec_commit = h_rec_commit, .rec_delete = h_rec_delete,
    .wdt = h_wdt, .stereo = h_stereo,
};

/* ---- opening a part */
static int inited;
int mm_init(void)
{
    if (!inited) {
        tables_make();
        dither_block();
        inited = 1;
    }
    return 0;
}

int mm_part_open(mm_part *t, const mod_desc_v3 *d, const mod_engine_v3 *e)
{
    mm_init();
    memset(t, 0, sizeof *t);
    if (d->magic != MOD3_MAGIC || d->abi != MOD3_ABI || d->host > MOD3_HOST)
        return 1;
    t->desc = d;
    t->e = e;
    t->h.size = sizeof(mod_part_v3);
    t->h.index = 0;
    t->h.engine = d->engine;
    t->h.voices = t->v;
    t->h.vsize = sizeof(mm_voice);
    for (uint32_t i = 0; i < MOD3_NVOICE; i++)
        t->v[i].h.index = (uint8_t)i;
    t->bend_semis = 2;
    t->vel_amp = MM_VEL_DEF;
    t->at_flt = MM_ATFLT_DEF;
    t->bpm = 120.0;
    t->seed = 0x1234567u;
    t->lfo_fade = 32767;
    mm_defaults(e, t->h.p);
    if (e->state) {
        uint32_t n = e->state(0);
        if (n) {
            t->mem = calloc(1, n);
            t->mem_bytes = t->mem ? n : 0;
        }
    }
    MM_ENTER(t);
    if (d->bind && d->bind(&HOST, (uint32_t)d->engine << 8)) {   /* (slot 0: its own copy) */
        mm_part_close(t);
        return 1;
    }
    return 0;
}

void mm_part_close(mm_part *t)
{
    if (h_cur == t)
        h_cur = 0;
    free(t->mem);
    t->mem = 0;
    t->desc = 0;
}

uint32_t mm_sysex(mm_part *t, const uint8_t *f, uint32_t n, mm_sx_take take, void *ctx, char *msg, uint32_t cap)
{
    uint32_t r, got;
    if (msg && cap)
        msg[0] = 0;
    if (!t->e->sysex || n < 2u || f[0] != 0xF0u || f[n - 1u] != 0xF7u)
        return 0;
    MM_ENTER(t);
    h_take = take, h_take_ctx = ctx, h_msg = msg, h_msg_cap = cap, h_taken = 0;
    r = t->e->sysex(f, n - 1u, MOD3_SX_MORE);    /* (its bytes came, then its F7) */
    if (r != MOD3_SXR_DONE)
        r = t->e->sysex(f, n, MOD3_SX_END);
    if (r != MOD3_SXR_DONE)                      /* (it waits for a handshake: this host answers none) */
        (void)t->e->sysex(f, n, MOD3_SX_ABORT);
    got = h_taken;
    h_take = 0, h_take_ctx = 0, h_msg = 0, h_msg_cap = 0;
    return got;
}

void mm_patch_set(mm_part *t, const uint8_t *patch, uint32_t len, uint32_t how)
{
    MM_ENTER(t);
    memset(t->patch, 0, sizeof t->patch);
    memcpy(t->patch, patch, len < MOD3_PATCH_MAX ? len : MOD3_PATCH_MAX);
    if (t->e->patch_changed)
        t->e->patch_changed(&t->h, t->patch, how, 0xFFFFu);
}

void mm_patch_note(mm_part *t, uint32_t how, uint32_t id)
{
    MM_ENTER(t);
    if (t->e->patch_changed)
        t->e->patch_changed(&t->h, t->patch, how, id);
}

/* ---- the common parameters: ranges and defaults as the device has them */
static const char *const N_LSYNC[] = {"OFF", "4BAR", "2BAR", "1/1", "1/2", "1/4", "1/8", "8T", "1/16", "16T", "1/32"};
static const char *const N_LWAVE[] = {"SIN", "TRI", "SAW", "SQR", "S&H"};
static const char *const N_LTRIG[] = {"NOTE", "FREE"};
static const char *const N_LPOL[] = {"BI", "UNI"};
static const char *const N_VOICE[] = {"POLY", "MONO", "LEG", "UNI"};
static const char *const N_GLMODE[] = {"RATE", "TIME"};
static const char *const N_PRIO[] = {"LAST", "LOW", "HIGH"};
static const char *const N_ALLOC[] = {"ROT", "REUSE"};
const mm_common MM_COMMON[] = {
    {MM_P_ATK, "ATK", MOD3_F_TIME, 0, 127, 10, 0},
    {MM_P_DEC, "DEC", MOD3_F_TIME, 0, 127, 70, 0},
    {MM_P_SUS, "SUS", MOD3_F_PCT, 0, 127, 90, 0},
    {MM_P_REL, "REL", MOD3_F_TIME, 0, 127, 60, 0},
    {MM_P_ED_FLT, "ENV FLT", MOD3_F_BIPCT, -64, 63, 0, 0},
    {MM_P_ED_PIT, "ENV PIT", MOD3_F_BIPCT, -64, 63, 0, 0},
    {MM_P_ED_SHP, "ENV SHP", MOD3_F_BIPCT, -64, 63, 0, 0},
    {MM_P_LRATE, "LFO RATE", MOD3_F_LFOHZ, 0, 127, 60, 0},
    {MM_P_LWAVE, "LFO WAVE", MOD3_F_ENUM, 0, 4, 0, N_LWAVE},
    {MM_P_LPHASE, "LFO PHS", MOD3_F_INT, 0, 127, 0, 0},
    {MM_P_LFADE, "LFO FADE", MOD3_F_TIME, 0, 127, 0, 0},
    {MM_P_LSYNC, "LFO SYNC", MOD3_F_ENUM, 0, 10, 0, N_LSYNC},
    {MM_P_LTRIG, "LFO TRIG", MOD3_F_ENUM, 0, 1, 0, N_LTRIG},
    {MM_P_LPOL, "LFO POL", MOD3_F_ENUM, 0, 1, 0, N_LPOL},
    {MM_P_LD_PIT, "LFO PIT", MOD3_F_BIPCT, -64, 63, 0, 0},
    {MM_P_LD_FLT, "LFO FLT", MOD3_F_BIPCT, -64, 63, 0, 0},
    {MM_P_LD_SHP, "LFO SHP", MOD3_F_BIPCT, -64, 63, 0, 0},
    {MM_P_LD_AMP, "LFO AMP", MOD3_F_PCT, 0, 127, 0, 0},
    {MM_P_VOICE, "VOICE", MOD3_F_ENUM, 0, 3, 0, N_VOICE},
    {MM_P_GLIDE, "GLIDE", MOD3_F_TIME, 0, 127, 0, 0},
    {MM_P_GLMODE, "GL MODE", MOD3_F_ENUM, 0, 1, 0, N_GLMODE},
    {MM_P_PRIO, "PRIO", MOD3_F_ENUM, 0, 2, 0, N_PRIO},
    {MM_P_ALLOC, "ALLOC", MOD3_F_ENUM, 0, 1, 0, N_ALLOC},
    {MM_P_DETUNE, "DETUNE", MOD3_F_INT, 0, 127, 40, 0},
    {MM_P_LEVEL, "LEVEL", MOD3_F_DB, 0, 127, 104, 0},
};
const uint32_t MM_NCOMMON = sizeof MM_COMMON / sizeof MM_COMMON[0];

void mm_defaults(const mod_engine_v3 *e, int16_t *p)
{
    memset(p, 0, MOD3_P_COUNT * sizeof *p);
    p[MM_P_LEVEL] = 104;
    for (uint32_t i = 0; i < MM_NCOMMON; i++)
        p[MM_COMMON[i].id] = MM_COMMON[i].def;
    for (uint32_t k = 0; k < 8u; k++)
        p[MM_P_E0 + k] = e->edit[k].def;
}

/* a factory sound (mod3_prow_t): E1..E8, the ADSR, ENV -> FILTER, the voice mode; the rest as a new part has it */
void mm_preset(const mod_engine_v3 *e, int16_t *p, const int8_t ev[8], const uint8_t env[4], int8_t fenv, uint8_t voice)
{
    mm_defaults(e, p);
    for (uint32_t k = 0; k < 8u; k++)
        p[MM_P_E0 + k] = (int16_t)clamp(ev[k], e->edit[k].min, e->edit[k].max);
    p[MM_P_ATK] = env[0] & 127;
    p[MM_P_DEC] = env[1] & 127;
    p[MM_P_SUS] = env[2] & 127;
    p[MM_P_REL] = env[3] & 127;
    p[MM_P_ED_FLT] = (int16_t)clamp(fenv, -64, 63);
    p[MM_P_VOICE] = (int16_t)(voice & 3u);
}

/* ---- voices */
static uint32_t nvoice(const mm_part *t) { return h_nvoice(&t->h); }
static uint32_t vmode(const mm_part *t) { return t->e->oneshot ? MM_V_POLY : (uint32_t)t->h.p[MM_P_VOICE] & 3u; }

static mm_voice *voice_alloc(mm_part *t, uint32_t note)
{
    const mod_engine_v3 *e = t->e;
    uint32_t i, np = nvoice(t), best = np, low = np;
    int32_t bd = 0x7FFFFFFF;
    if (e->alloc)                                /* the engine's own choice */
        return &t->v[e->alloc(&t->h, note) % np];
    if (e->reuse) {                              /* a hit on its lane's voice */
        mod_voice_v3 *v = e->reuse(&t->h, note);
        if (v)
            return (mm_voice *)(void *)v;
    }
    for (i = 0; i < np; i++)
        if (t->v[i].h.active && t->v[i].h.note == note)
            return &t->v[i];
    if (t->h.p[MM_P_ALLOC] || t->h.p[MM_P_GLIDE]) {  /* REUSE (or glide): the free voice whose pitch is closest */
        int32_t target = (int32_t)note * 16;
        for (i = 0; i < np; i++) {
            int32_t d = t->v[i].h.pitch_cur - target;
            if (t->v[i].h.active)
                continue;
            d = d < 0 ? -d : d;
            if (!t->v[i].h.pitch_cur)
                d = 0x7FFFFFF0;
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
    } else {                                     /* ROTATE: the next free one */
        for (i = 0; i < np && best == np; i++) {
            uint32_t k = (t->rr + i) % np;
            if (!t->v[k].h.active)
                best = k;
        }
        t->rr = (uint8_t)(((best == np ? t->rr : best) + 1u) % np);
    }
    if (best < np)
        return &t->v[best];
    for (i = 0; i < np; i++)                     /* steal: never the lowest held note */
        if (t->v[i].h.gate && (low == np || t->v[i].h.note < t->v[low].h.note))
            low = i;
    for (i = 0; i < np; i++)                     /* the oldest released */
        if (!t->v[i].h.gate && (best == np || t->v[i].h.age < t->v[best].h.age))
            best = i;
    if (best == np)
        for (i = 0; i < np; i++)                 /* the oldest held, not the bass */
            if (i != low && (best == np || t->v[i].h.age < t->v[best].h.age))
                best = i;
    return &t->v[best == np ? 0 : best];
}

static void glide_set(mm_part *t, mm_voice *v, int glide)
{
    if (!glide || !v->h.pitch_cur) {
        v->h.pitch_cur = v->h.pitch16;
        v->gstep = 0;
        return;
    }
    if (t->h.p[MM_P_GLMODE]) {                   /* TIME: the same time for any interval */
        int32_t d = v->h.pitch16 - v->h.pitch_cur, ticks = 1 + t->h.p[MM_P_GLIDE] * t->h.p[MM_P_GLIDE] / 8;
        d = d < 0 ? -d : d;
        v->gstep = d / ticks > 0 ? d / ticks : 1;
    } else {
        v->gstep = 0;
    }
}

static void voice_start(mm_part *t, mm_voice *v, uint32_t note, uint32_t vel, uint32_t mvel, int glide)
{
    const mod_engine_v3 *e = t->e;
    mod_voice_v3 *h = &v->h;
    int sounding = h->active && h->stage != 0;
    uint32_t ph0 = h->ph[0], ph1 = h->ph[1], ph2 = h->ph[2], keep = e->keep;
    int32_t s0 = h->s[0], s1 = h->s[1], s4 = h->s[4], s5 = h->s[5];
    uint8_t was = (uint8_t)(!h->active ? 0u : h->gate ? 2u : 1u);
    h->note = (uint8_t)note;
    h->vel = (uint8_t)vel;
    h->mvel = (uint8_t)mvel;
    h->gate = 1;
    h->active = 1;
    h->stage = 1;
    h->age = ++t->age;
    h->pitch16 = (int32_t)note * 16;
    glide_set(t, v, glide);
    if (!sounding) {
        h->env = 0;
        h->env_out = 0;
        v->at_q15 = 0;
        v->side = t->sp_alt;
        t->sp_alt ^= 1u;
    }
    while (atomic_flag_test_and_set_explicit(&h_lock, memory_order_acquire))
        ;
    h_was = was;
    e->note_on(&t->h, h);
    atomic_flag_clear_explicit(&h_lock, memory_order_release);
    if (sounding && !e->sampled) {               /* retrigger: keep phases and the slots it names */
        h->ph[0] = ph0;
        h->ph[1] = ph1;
        h->ph[2] = ph2;
        if (keep & 0x01u)
            h->s[0] = s0;
        if (keep & 0x02u)
            h->s[1] = s1;
        if (keep & 0x10u)
            h->s[4] = s4;
        if (keep & 0x20u)
            h->s[5] = s5;
    }
}

/* MONO / LEGATO / UNISON: one note on one voice, or on the engine's voices spread by DETUNE (UNISON) */
static void mono_play(mm_part *t, uint32_t note, uint32_t vel, int retrig, int glide)
{
    uint32_t mode = (uint32_t)t->h.p[MM_P_VOICE] & 3u, nv = mode == MM_V_UNISON ? nvoice(t) : 1u, i;
    for (i = 0; i < nv; i++) {
        mm_voice *v = &t->v[i];
        int32_t k = 2 * (int32_t)i - (int32_t)(nv - 1u);
        if (nv > 1u && nv != MM_NPOLY)
            k = k * (int32_t)(MM_NPOLY - 1u) / (int32_t)(nv - 1u);
        v->fine = nv > 1u ? k * t->h.p[MM_P_DETUNE] * 56 / 889 : 0;   /* up to ~±40 cents */
        if (retrig || !v->h.active || !v->h.gate) {
            voice_start(t, v, note, nv >= MM_NPOLY ? vel * 36u / 100u : nv > 1u ? vel / 2u : vel, vel, glide);
            v->side = (uint8_t)(i & 1u);
            if (nv > 1u && i && !t->e->sampled) {   /* random start phases: in phase they stack */
                t->seed = t->seed * 1664525u + 1013904223u;
                v->h.ph[0] += t->seed;
            }
        } else {                                 /* legato: a new pitch, the same envelope */
            v->h.note = (uint8_t)note;
            v->h.pitch16 = (int32_t)note * 16;
            v->h.mvel = (uint8_t)vel;
            if (vel > v->h.vel && nv == 1u)
                v->h.vel = (uint8_t)vel;
            glide_set(t, v, glide);
            if (t->e->legato)
                t->e->legato(&t->h, &v->h);
        }
    }
    t->mono_note = (uint8_t)note;
}

static uint32_t mono_pick(const mm_part *t)      /* PRIO: LAST / LOW / HIGH; 0 none */
{
    uint32_t i, n;
    if (!t->h.nmono)
        return 0;
    n = t->h.mono_stack[t->h.nmono - 1u];
    for (i = 0; i < t->h.nmono; i++) {
        if (t->h.p[MM_P_PRIO] == 1 && t->h.mono_stack[i] < n)
            n = t->h.mono_stack[i];
        if (t->h.p[MM_P_PRIO] == 2 && t->h.mono_stack[i] > n)
            n = t->h.mono_stack[i];
    }
    return n;
}
static void mono_remove(mm_part *t, uint32_t note)
{
    uint32_t j, k = 0;
    for (j = 0; j < t->h.nmono; j++)
        if (t->h.mono_stack[j] != note)
            t->h.mono_stack[k++] = t->h.mono_stack[j];
    t->h.nmono = (uint8_t)k;
}
static void mono_push(mm_part *t, uint32_t note)
{
    mono_remove(t, note);
    if (t->h.nmono >= MOD3_NVOICE)
        mono_remove(t, t->h.mono_stack[0]);
    t->h.mono_stack[t->h.nmono++] = (uint8_t)note;
}
static int is_chord(const mm_part *t) { return t->e->chord && t->e->chord(&t->h); }

void mm_note_on(mm_part *t, uint32_t note, uint32_t vel)
{
    uint32_t any = 0, i, mode = vmode(t);
    MM_ENTER(t);
    if (note > 127u)
        return;
    if (!vel) {
        mm_note_off(t, note);
        return;
    }
    vel = vel > 127u ? 127u : vel;
    t->held[note] = 1;
    t->key_at[note] = 0;                         /* a key pressed again: no pressure yet */
    for (i = 0; i < MOD3_NVOICE; i++)
        any |= t->v[i].h.gate;
    if (!any) {                                  /* a fresh phrase: LFO retrigger (TRIG NOTE) and fade */
        if (!t->h.p[MM_P_LTRIG])
            t->lfo_ph = (uint32_t)t->h.p[MM_P_LPHASE] << 25;
        t->lfo_fade = 0;
    }
    if (mode == MM_V_POLY) {
        mm_voice *v = voice_alloc(t, note);
        if (!is_chord(t) || !any)
            t->h.nmono = 0;
        if (is_chord(t))
            mono_push(t, note);
        t->mono_note = 0;
        v->fine = 0;
        voice_start(t, v, note, vel, vel, t->h.p[MM_P_GLIDE] != 0 && !t->e->oneshot);
        return;
    }
    {
        uint32_t sounding = t->mono_note && t->v[0].h.active && t->v[0].h.gate, want;
        mono_push(t, note);
        if (t->e->mono_key)
            t->e->mono_key(&t->h, note);
        want = mono_pick(t);
        if (sounding && want == t->mono_note)
            return;                              /* LOW / HIGH: this key does not win */
        if (mode == MM_V_MONO)
            mono_play(t, want, vel, 1, t->h.p[MM_P_GLIDE] != 0);
        else
            mono_play(t, want, vel, !sounding, sounding && t->h.p[MM_P_GLIDE] != 0);
    }
}

static void note_release(mm_part *t, uint32_t note)
{
    uint32_t i, mode = vmode(t), chord = is_chord(t), held = 0;
    for (i = 0; i < t->h.nmono; i++)
        held |= t->h.mono_stack[i] == note;
    mono_remove(t, note);
    if (mode != MM_V_POLY) {
        uint32_t nv = mode == MM_V_UNISON ? nvoice(t) : 1u;
        if (t->mono_note == note) {
            uint32_t next = mono_pick(t);
            if (next) {                          /* back to a held note, legato */
                mono_play(t, next, t->v[0].h.vel, 0, t->h.p[MM_P_GLIDE] != 0);
            } else {
                for (i = 0; i < nv; i++)
                    if (t->v[i].h.gate) {
                        t->v[i].h.gate = 0;
                        t->v[i].h.stage = 3;
                    }
                t->mono_note = 0;
            }
        }
    }
    for (i = 0; i < MOD3_NVOICE; i++)
        if (t->v[i].h.gate && (chord && !i ? !t->h.nmono && (held || t->v[0].h.note == note) : t->v[i].h.note == note)) {
            t->v[i].h.gate = 0;
            t->v[i].h.stage = 3;
        }
}

void mm_note_off(mm_part *t, uint32_t note)
{
    MM_ENTER(t);
    if (note > 127u)
        return;
    if (t->sustain && t->held[note]) {
        t->held[note] = 2;                       /* the pedal keeps it */
        return;
    }
    t->held[note] = 0;
    note_release(t, note);
}

void mm_sustain(mm_part *t, int down)
{
    MM_ENTER(t);
    t->sustain = (uint8_t)(down != 0);
    if (down)
        return;
    for (uint32_t n = 0; n < 128u; n++)
        if (t->held[n] == 2u) {
            t->held[n] = 0;
            note_release(t, n);
        }
}

void mm_all_off(mm_part *t)
{
    for (uint32_t i = 0; i < MOD3_NVOICE; i++) {
        t->v[i].h.gate = 0;
        t->v[i].h.stage = t->v[i].h.active ? 3 : 0;
    }
    memset(t->held, 0, sizeof t->held);
    t->h.nmono = 0;
    t->mono_note = 0;
}

void mm_kill(mm_part *t)
{
    for (uint32_t i = 0; i < MOD3_NVOICE; i++) {
        mod_voice_v3 *h = &t->v[i].h;
        h->gate = h->active = 0;
        h->stage = 0;
        h->env = h->env_out = 0;
    }
    memset(t->held, 0, sizeof t->held);
    t->h.nmono = 0;
    t->mono_note = 0;
    t->bend_q8 = 0;
}

void mm_bend(mm_part *t, int32_t raw) { t->h.bend_raw = (int16_t)clamp(raw, -8192, 8191); }

void mm_pressure(mm_part *t, int32_t note, uint32_t value)
{
    uint32_t top;
    value = value > 127u ? 127u : value;
    if (note < 0)
        t->chan_at = (uint8_t)value;
    else if (note < 128)
        t->key_at[note] = (uint8_t)value;
    /* the part's (mod_part_v3.at, what a module reads): the channel's, or the hardest pressed of the keys held */
    top = t->chan_at;
    for (uint32_t n = 0; n < 128u; n++)
        if (t->held[n] && t->key_at[n] > top)
            top = t->key_at[n];
    t->h.at = (uint8_t)top;
}

int mm_live(const mm_part *t)
{
    for (uint32_t i = 0; i < MOD3_NVOICE; i++)
        if (t->v[i].h.active)
            return 1;
    return 0;
}

/* ---- one block */
static int32_t lfo_wave(mm_part *t, uint32_t ph)
{
    switch (t->h.p[MM_P_LWAVE]) {
    case 1: {
        int32_t s = (int32_t)(ph >> 15);
        return s < 65536 ? s - 32768 : 98303 - s;
    }
    case 2:
        return (int32_t)(ph >> 16) - 32768;
    case 3:
        return ph < 0x80000000u ? 32767 : -32767;
    case 4:
        return (int32_t)(t->lfo_rnd >> 16) - 32768;
    default: {
        uint32_t i = ph >> 22;
        int32_t a = T_SINE[i], b = T_SINE[(i + 1u) & 1023u];
        return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
    }
    }
}

/* LFO SYNC's cycles in quarter notes: 4BAR .. 1/32 */
static const double LSYNC_BEATS[11] = {0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
static void lfo_tick(mm_part *t)
{
    uint32_t old = t->lfo_ph, sync = (uint32_t)t->h.p[MM_P_LSYNC];
    if (sync && sync < 11u) {
        double cyc = (t->bpm > 1.0 ? t->bpm : 120.0) / 60.0 / LSYNC_BEATS[sync] * MM_CTL / MM_FS * 4294967296.0;
        double whole = floor(cyc + t->lfo_sync_frac);
        t->lfo_sync_frac += cyc - whole;
        t->lfo_ph += (uint32_t)whole;
    } else {
        t->lfo_ph += LFO_INC[t->h.p[MM_P_LRATE] & 127];
    }
    if (t->lfo_ph < old) {                       /* a new cycle: S&H's next value */
        uint32_t s = t->lfo_rnd ? t->lfo_rnd : 0x9E3779B9u;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        t->lfo_rnd = s;
    }
    t->lfo_val = lfo_wave(t, t->lfo_ph);
    if (t->h.p[MM_P_LPOL])                       /* UNI: 0..+1 */
        t->lfo_val = (t->lfo_val + 32768) >> 1;
    if (t->lfo_fade < 32767) {
        int32_t step = (int32_t)(ENV_LIN[t->h.p[MM_P_LFADE] & 127] >> 9);
        t->lfo_fade = t->h.p[MM_P_LFADE] ? clamp(t->lfo_fade + (step ? step : 1), 0, 32767) : 32767;
    }
}

static int32_t env_tick(mm_part *t, mm_voice *v)
{
    const int16_t *p = t->h.p;
    mod_voice_v3 *h = &v->h;
    int32_t sus = (int32_t)p[MM_P_SUS] << 17;
    switch (h->stage) {
    case 1:
        h->env += (int32_t)ENV_LIN[p[MM_P_ATK] & 127];
        if (h->env >= (1 << 24)) {
            h->env = 1 << 24;
            h->stage = 2;
        }
        break;
    case 2:
        h->env += mulq16(sus - h->env, ENV_EXP[p[MM_P_DEC] & 127]);
        break;
    case 3:
        h->env -= mulq16(h->env, ENV_EXP[p[MM_P_REL] & 127]);
        if (h->env < (1 << 12)) {
            h->env = 0;
            h->stage = 0;
            h->active = 0;
        }
        break;
    default:
        h->env = 0;
        h->stage = 0;
        h->active = 0;
        break;
    }
    return h->env >> 9;
}

/* VEL: the level of velocity vel (1..127), Q15: 1 - VEL x (1 - vel / 127); at 127 vel x 258 (the FM-1's), at 0 32767
 * (the level as it is) */
static int32_t vel_gain(const mm_part *t, uint32_t vel)
{
    uint32_t d = (uint32_t)clamp(t->vel_amp, 0, 127);
    vel = vel > 127u ? 127u : vel;
    return d ? 32766 - (int32_t)((127u - vel) * 258u * d / 127u) : 32767;
}

int mm_tick(mm_part *t, int32_t *out)
{
    const mod_engine_v3 *e = t->e;
    const int16_t *p = t->h.p;
    uint32_t i, nr = 0, live = 0;
    int32_t lfo, bend, bend16, bend_fine, target, range;
    MM_ENTER(t);
    dither_block();
    h_ms_frac += MM_CTL * 1000u;
    h_ms += h_ms_frac / MM_FS;
    h_ms_frac %= MM_FS;
    memset(out, 0, MM_CTL * sizeof *out);
    t->stereo_used = 0;
    /* the wheel: its range (the engine's own, else the part's), then ~6 ms to get there */
    range = e->bend_range ? e->bend_range(&t->h) : t->bend_semis * 256;
    target = e->oneshot ? 0 : t->h.bend_raw * range / (t->h.bend_raw < 0 ? 8192 : 8191);
    {
        int32_t d = target - t->bend_q8;
        t->bend_q8 += d > 0 ? (d < 4 ? d : (d + 3) / 4) : (d > -4 ? d : (d - 3) / 4);
    }
    for (i = 0; i < MOD3_NVOICE; i++)
        live |= t->v[i].h.active;
    lfo_tick(t);
    lfo = mulq15(t->lfo_val, t->lfo_fade);
    if (e->block)
        e->block(&t->h);
    if (!live) {
        if (e->post) {
            t->in_post = 1;
            e->post(&t->h, out, MM_CTL, 0);
            t->in_post = 0;
        }
        return t->stereo_used;
    }
    bend = t->bend_q8;
    bend16 = bend >= 0 ? bend / 16 : -((-bend + 15) / 16);
    bend_fine = (bend - bend16 * 16) * 2367 / 2560;
    for (i = 0; i < MOD3_NVOICE; i++) {
        mm_voice *v = &t->v[i];
        mod_voice_v3 *h = &v->h;
        mod_vmod_v3 m;
        int32_t env, pitch, vg;
        if (!h->active)
            continue;
        env = env_tick(t, v);
        if (e->ownenv && h->active) {            /* the engine's envelopes end the voice */
            if (e->done && e->done(&t->h, h)) {
                h->active = h->gate = 0;
                h->stage = 0;
                h->env = 0;
                env = 0;
            } else {
                h->env = 1 << 24;
                env = 32767;
            }
        }
        if (e->amp)
            env = e->amp(&t->h, h, env);
        m.envq15 = e->ownenv ? 0 : env;
        if (e->env_q15)
            m.envq15 = e->env_q15(&t->h, h);
        vg = vel_gain(t, h->vel);                /* (an engine with its own envelopes too: its env is 32767 here) */
        m.amp1 = vg < 32767 ? mulq15(env, vg) : env;
        if (p[MM_P_LD_AMP])
            m.amp1 = mulq15(m.amp1, 32767 - mulq15(p[MM_P_LPOL] ? lfo : (lfo + 32768) >> 1, p[MM_P_LD_AMP] * 258));
        m.amp0 = h->env_out;
        h->env_out = m.amp1;
        if (h->pitch_cur != h->pitch16) {        /* glide */
            int32_t d = h->pitch16 - h->pitch_cur;
            int32_t st = v->gstep ? v->gstep : (int32_t)(ENV_LIN[(p[MM_P_GLIDE] ? p[MM_P_GLIDE] : 40) & 127] >> 14);
            if (st < 1)
                st = 1;
            h->pitch_cur += d > 0 ? (d < st ? d : st) : (-d < st ? d : -st);
        }
        if (!env && !m.amp0 && h->stage == 2 && !e->sampled)
            continue;                            /* held at a silent sustain */
        pitch = h->pitch_cur + bend16 + ((lfo * p[MM_P_LD_PIT] * 3) >> 15) + ((m.envq15 * p[MM_P_ED_PIT] * 3) >> 15);
        m.pitch16 = clamp(pitch, 0, 2047);
        m.inc = pitch_inc((uint32_t)m.pitch16);
        m.fine = v->fine + bend_fine;
        if (m.fine)
            m.inc += (uint32_t)((int32_t)(m.inc >> 12) * m.fine);
        m.cutoff = ((lfo * p[MM_P_LD_FLT]) >> 7) + ((m.envq15 * p[MM_P_ED_FLT]) >> 7);
        if (h->vel > 110)                        /* accent opens the filter with the envelope */
            m.cutoff += (m.envq15 * 24) >> 7;
        {                                        /* AT FLT: its key's pressure (or the channel's), ~3 ms to get there */
            uint32_t a = t->key_at[h->note] > t->chan_at ? t->key_at[h->note] : t->chan_at;
            v->at_q15 += ((int32_t)a * 258 - v->at_q15) >> 2;
            if (t->at_flt)
                m.cutoff = clamp(m.cutoff + ((v->at_q15 * t->at_flt) >> 7), -150 * 256, 150 * 256);
        }
        m.shape = (64 << 8) + ((lfo * p[MM_P_LD_SHP]) >> 7) + ((m.envq15 * p[MM_P_ED_SHP]) >> 7);
        if (e->menv)
            m.envq15 = e->menv(&t->h, h);
        m.plog = (m.pitch16 - bend16 - (int32_t)h->note * 16) * 87381 + (m.fine - bend_fine) * 5909;
        if (e->render)
            e->render(&t->h, h, out, MM_CTL, &m);
        nr++;
    }
    if (e->post) {
        t->in_post = 1;
        e->post(&t->h, out, MM_CTL, nr);
        t->in_post = 0;
    }
    return t->stereo_used;
}

/* ---- values as text (the firmware's params.c) */
static const char *const N_NOTE[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void sign(char *val, uint32_t cap, int32_t v) { snprintf(val, cap, v > 0 ? "+%d" : "%d", (int)v); }
void mm_format(uint32_t fmt, int32_t v, int32_t min, int32_t max, const char *const *names, const char *unit,
               char *buf, uint32_t cap)
{
    char val[16];
    const char *u = 0;
    double x;
    uint32_t h;
    switch (fmt) {
    case MOD3_F_ENUM:
        snprintf(val, sizeof val, "%s", names ? names[v < min ? min : v > max ? max : v] : "?");
        u = unit;
        break;
    case MOD3_F_PCT:                             /* a 0..100 range is its value, others a share of 127 */
        snprintf(val, sizeof val, "%d", (int)(max == 100 ? v : (v * 100 + 63) / 127));
        u = "%";
        break;
    case MOD3_F_BIPCT:
        sign(val, sizeof val, v * 100 / 64);
        u = "%";
        break;
    case MOD3_F_TIME:
        h = (uint32_t)lround(10.0 * pow(10000.0, (v & 127) / 127.0));   /* ms x 10 */
        if (h < 100u)
            snprintf(val, sizeof val, "%u.%u", h / 10u, h % 10u), u = "ms";
        else if (h < 10000u)
            snprintf(val, sizeof val, "%u", (h + 5u) / 10u), u = "ms";
        else if (h < 100000u)
            snprintf(val, sizeof val, "%u.%02u", h / 10000u, h / 100u % 100u), u = "s";
        else
            snprintf(val, sizeof val, "%u.%u", h / 10000u, h / 1000u % 10u), u = "s";
        break;
    case MOD3_F_LFOHZ:
        h = (uint32_t)lround(100.0 * 0.05 * pow(800.0, (v & 127) / 127.0));   /* Hz x 100 */
        if (h < 1000u)
            snprintf(val, sizeof val, "%u.%02u", h / 100u, h % 100u);
        else
            snprintf(val, sizeof val, "%u.%u", h / 100u, h / 10u % 10u);
        u = "Hz";
        break;
    case MOD3_F_CUTOFF:
        h = (uint32_t)lround(30.0 * pow(16000.0 / 30.0, (v & 127) / 127.0));
        if (h < 1000u)
            snprintf(val, sizeof val, "%u", h), u = "Hz";
        else
            snprintf(val, sizeof val, "%u.%u", h / 1000u, h / 100u % 10u), u = "kHz";
        break;
    case MOD3_F_DB:                              /* the track level: (v - 112) / 2 dB */
        if (v <= 0)
            snprintf(val, sizeof val, "OFF");
        else
            x = (v - 112) / 2.0, snprintf(val, sizeof val, "%+.1f", x), u = "dB";
        break;
    case MOD3_F_SEMI:
        sign(val, sizeof val, v);
        u = "st";
        break;
    case MOD3_F_OCT:
        sign(val, sizeof val, v);
        u = "oct";
        break;
    case MOD3_F_BPM:
        snprintf(val, sizeof val, "%d", (int)v), u = "BPM";
        break;
    case MOD3_F_NOTE:
        snprintf(val, sizeof val, "%s", N_NOTE[((v % 12) + 12) % 12]);
        break;
    case MOD3_F_ONOFF:
        snprintf(val, sizeof val, "%s", v ? "ON" : "OFF");
        break;
    case MOD3_F_STEPS:
        snprintf(val, sizeof val, "%d", (int)v), u = "STEP";
        break;
    case MOD3_F_OFS:                             /* 0 at the middle of the range */
        sign(val, sizeof val, v - (min + max) / 2);
        u = unit;
        break;
    case MOD3_F_FMNOTE:                          /* the DX7 break point: 0 = A-1 */
        snprintf(val, sizeof val, "%s%d", N_NOTE[(v + 21) % 12], (int)((v + 21) / 12 - 1));
        break;
    default:
        snprintf(val, sizeof val, "%d", (int)v);
        u = unit;
        break;
    }
    snprintf(buf, cap, u && *u ? (strcmp(u, "%") ? "%s %s" : "%s%s") : "%s", val, u ? u : "");
}
