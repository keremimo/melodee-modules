/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The part a synth module plays in a plug-in: the host table (mod_host_v3) and what the Melodee firmware does around
 * a module's callbacks (voices, the ADSR, the LFO, glide, POLY / MONO / LEGATO / UNISON, bend), written for this
 * host from the ABI 3 contract (sdk/mod_abi3.h). Fixed point at MOD3_RATE in blocks of MOD3_CTL samples, as on the
 * device. */
#pragma once
#include <stdint.h>
#include "mod_abi3.h"

#define MM_CTL MOD3_CTL
#define MM_FS MOD3_RATE
#define MM_NPOLY 8u                              /* POLY's voices when the engine names none */
#define MM_VEL_DEF 127                           /* VEL as a new part has it: the level follows velocity (the FM-1's) */
#define MM_ATFLT_DEF 32                          /* AT FLT as a new part has it: full pressure opens the filter by a
                                                  * quarter of its range */

/* the part's parameters by P_* number (frozen by ABI 3: mod_part_v3.p) */
enum {
    MM_P_LEVEL = 0,
    MM_P_ATK = 1, MM_P_DEC = 2, MM_P_SUS = 3, MM_P_REL = 4,
    MM_P_ED_FLT = 5, MM_P_ED_PIT = 6, MM_P_ED_SHP = 7,
    MM_P_LRATE = 9, MM_P_LWAVE = 10, MM_P_LPHASE = 11, MM_P_LFADE = 12,
    MM_P_LD_PIT = 13, MM_P_LD_FLT = 14, MM_P_LD_SHP = 15, MM_P_LD_AMP = 16,
    MM_P_VOICE = 37, MM_P_GLIDE = 38, MM_P_PAN = 39, MM_P_MUTE = 40,
    MM_P_GLMODE = 41, MM_P_PRIO = 42, MM_P_ALLOC = 43, MM_P_DETUNE = 44,
    MM_P_LSYNC = 92, MM_P_LTRIG = 93, MM_P_LPOL = 94, MM_P_SPRD = 95,
    MM_P_E0 = MOD3_P_E0
};
enum { MM_V_POLY, MM_V_MONO, MM_V_LEGATO, MM_V_UNISON };

typedef struct mm_voice {
    mod_voice_v3 h;                              /* what the module reads (first) */
    int32_t fine;                                /* UNISON's detune, 1/4096 */
    int32_t gstep;                               /* glide TIME's step, 1/16 semitone a block */
    int32_t at_q15;                              /* its key's pressure as the filter hears it (smoothed), Q15 */
    uint8_t side, rsv[3];
} mm_voice;

typedef struct mm_part {
    mod_part_v3 h;                               /* what the module reads (first): p[], the voices, the wheels */
    const mod_desc_v3 *desc;
    const mod_engine_v3 *e;
    mm_voice v[MOD3_NVOICE];
    uint32_t age, lfo_ph, lfo_rnd, seed;
    int32_t lfo_val, lfo_fade;
    int32_t bend_q8;                             /* the bend as it glides to its target, Q8 semitones */
    int32_t bend_semis;                          /* the wheel's range when the engine names none */
    int32_t vel_amp;                             /* VEL: how far the level follows velocity, 0..127 (127: as the FM-1) */
    int32_t at_flt;                              /* AT FLT: pressure to the filter, -64..63 (as ENV FLT) */
    uint8_t chan_at;                             /* the channel's pressure (channel aftertouch), 0..127 */
    uint8_t key_at[128];                         /* each key's pressure (poly aftertouch) since it was pressed */
    double bpm, lfo_sync_frac;                   /* LFO SYNC: the host's tempo */
    uint8_t mono_note, rr, sp_alt, in_post, stereo_used, sustain;
    uint8_t held[128];                           /* keys down (1) and keys the pedal holds (2) */
    uint8_t *mem;                                /* the module's working memory (host part_mem) */
    uint32_t mem_bytes;
    int32_t left[MM_CTL];                        /* host 5: the left side post writes */
    uint8_t patch[MOD3_PATCH_MAX];               /* host 2: the patch the part plays */
} mm_part;

/* the parameters the plug-in shows besides the engine's, by P_* number: their range, default, name and format */
typedef struct mm_common {
    uint8_t id;
    const char *label;
    uint8_t fmt;                                 /* MOD3_F_* */
    int16_t min, max, def;
    const char *const *names;
} mm_common;
extern const mm_common MM_COMMON[];
extern const uint32_t MM_NCOMMON;

int mm_init(void);                               /* the tables, once: 0 ready */
/* a part playing a loaded module (d its descriptor, e its engine as the host copied it), bound to the host table: 0
 * ready. Each part has its own copy of the module (mm_image.h), so every part is the module's part 0 */
int mm_part_open(mm_part *t, const mod_desc_v3 *d, const mod_engine_v3 *e);
void mm_part_close(mm_part *t);
/* a part's parameters (p: MOD3_P_COUNT values) as a new part has them, and as a factory sound sets them */
void mm_defaults(const mod_engine_v3 *e, int16_t *p);
void mm_preset(const mod_engine_v3 *e, int16_t *p, const int8_t ev[8], const uint8_t env[4], int8_t fenv, uint8_t voice);
void mm_note_on(mm_part *t, uint32_t note, uint32_t vel);
void mm_note_off(mm_part *t, uint32_t note);
void mm_sustain(mm_part *t, int down);
void mm_all_off(mm_part *t);                     /* release every voice */
void mm_kill(mm_part *t);                        /* silence at once (reset) */
void mm_bend(mm_part *t, int32_t raw);           /* -8192..8191 */
/* pressure (0..127): poly aftertouch for the voices playing key note, or note -1 the channel's (every voice) */
void mm_pressure(mm_part *t, int32_t note, uint32_t value);
/* host 2: the part's patch replaced by len bytes (the rest of it 0), the module told how (MOD3_PATCH_*) */
void mm_patch_set(mm_part *t, const uint8_t *patch, uint32_t len, uint32_t how);
/* host 2: the module told its patch changed (MOD3_PATCH_*, a parameter's id; 0xFFFF several) */
void mm_patch_note(mm_part *t, uint32_t how, uint32_t id);
/* one block of MM_CTL samples into out (cleared first); 1: stereo, t->left the left side and out L + R */
int mm_tick(mm_part *t, int32_t *out);
int mm_live(const mm_part *t);                   /* any voice sounding */

/* the text of v in a format (MOD3_F_*) of range min..max, as the FM-1 shows it */
void mm_format(uint32_t fmt, int32_t v, int32_t min, int32_t max, const char *const *names, const char *unit,
               char *buf, uint32_t cap);
/* the part whose module the calls that follow make (the host's calls that name a part by number mean it): before a
 * call into the module from outside mm_engine.c (pget, pput, pformat, desc, patch_changed) */
void mm_enter(mm_part *t);

/* host 3: a sound a module offers for a user slot (host sx_stage): its patch, len bytes */
typedef void (*mm_sx_take)(void *ctx, const uint8_t *patch, uint32_t len);
/* host 3: SysEx frame f (F0 .. F7, n bytes) to part t's module as the FM-1 hands one that came (its bytes, then its
 * end), outside the audio thread. The sounds it offers go to take, its last message to msg (cap bytes, "" none); it
 * is never given a part to play one on (sx_part: none). The sounds offered */
uint32_t mm_sysex(mm_part *t, const uint8_t *f, uint32_t n, mm_sx_take take, void *ctx, char *msg, uint32_t cap);
