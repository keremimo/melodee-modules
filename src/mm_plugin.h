/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The plug-in's own state as mm_plugin.c keeps it, for its screen (mm_screen.c): the synths found, every synth's
 * parameters and pages, an instance and the calls the screen makes on it. All of it the main thread's unless it says
 * otherwise; the screen's calls come from Melodee's screen thread, never at the same time as the main thread's. */
#pragma once
#include <clap/clap.h>
#include <stdatomic.h>
#include <stdint.h>
#include "melodee_screen.h"
#include "mm_engine.h"
#include "mm_file.h"
#include "mm_image.h"
#include "mm_resample.h"
#include "mm_schema.h"

enum { MM_SRC_PART, MM_SRC_OWN, MM_SRC_SCH };
/* the plug-in's own parameters (the part's, not the FM-1's P_* numbers): MM_P_OWN0 + MM_OWN_* */
enum { MM_OWN_BEND, MM_OWN_VEL, MM_OWN_ATFLT, MM_NOWN };
#define MM_NEDIT 64u                             /* the screen's values waiting for the audio thread, at most */
#define MM_SCOPE 1024u                           /* the scope's samples (a power of two) */

typedef struct mm_edit {
    clap_id id;
    int32_t v;
} mm_edit;

/* a synth: a module file found at init */
typedef struct mm_kind {
    char idhex[17], name[MOD3_NAME_MAX + 1];
    char path[4096], real[4096];                 /* its file, and where it is (links followed) */
    mm_file file;
    mm_schema sch;                               /* its parameters and pages (none: empty) */
    mm_image img;                                /* its code mapped, never run: its engine's names, ranges, pages */
    uint32_t tag;                                /* its parameters' ids above MM_TAG_SHIFT */
    uint32_t param0, nparam, page0, npage;       /* its parameters and pages in the tables */
    int16_t own_def[MM_NOWN];                    /* the plug-in's own values as a new part and its sounds have them */
} mm_kind;

typedef struct mm_param {
    clap_id id;                                  /* its synth's tag << MM_TAG_SHIFT | local */
    uint32_t local;                              /* a P_* number, MM_P_OWN0 + MM_OWN_*, or MM_SCH_ID0 + a schema id */
    const mm_kind *kind;
    uint8_t src;                                 /* MM_SRC_* */
    const mm_sch_param *sp;                      /* MM_SRC_SCH: its schema's record */
    char name[CLAP_NAME_SIZE];
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;
    const char *unit;
} mm_param;

typedef struct mm_page {
    char name[CLAP_NAME_SIZE];
    clap_id ids[CLAP_REMOTE_CONTROLS_COUNT];
} mm_page;

typedef struct mm_slot {                         /* a synth in an instance: its own copy of its code, its part */
    const mm_kind *kind;
    mm_image img;
    mm_part part;
} mm_slot;

typedef struct mm_plug {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_params_t *host_params;
    const clap_host_remote_controls_t *host_rc;
    /* the main thread's: the synth (the one playing, or the one the audio takes next), its view of the values
     * (get_value, state) */
    const mm_kind *kind;
    mm_slot *slot;
    int16_t shadow[MOD3_P_COUNT];
    int16_t shadow_own[MM_NOWN];                 /* (MM_OWN_*) */
    uint8_t shadow_patch[MOD3_PATCH_MAX];        /* host 2: the patch (kind->file.patch_len bytes) */
    _Atomic(const mm_kind *) want;               /* (kind, for the audio thread: the synth shadow holds) */
    /* the audio thread's: the synth playing; a new one the main thread made, the old one it gives back */
    mm_slot *live;
    _Atomic(mm_slot *) next, old;
    atomic_int pending;                          /* the main thread changed the sound: the audio takes shadow */
    atomic_int active;
    atomic_int rescan;                           /* values changed beyond the one set (the module's): tell the host */
    double rate;
    mm_rs rs;
    int32_t tick[MM_CTL];
    float tl[MM_CTL], tr[MM_CTL];
    uint32_t tpos;
    /* the sound loaded (main thread): its file (a module's, a SysEx file's), "" none, and its place in it, -1 none */
    char cur_path[4096];
    int32_t cur_index;
    atomic_int edited;                           /* a value changed since it loaded */
    /* its screen in Melodee (mm_screen.c): Melodee's calls for it, the screen's own state, the values it sets for the
     * audio thread (a ring: the screen writes, the audio reads), the parameter the host set last (a pot) */
    const melodee_screen_host_t *screen_host;
    struct mm_screen *scr;
    mm_edit edits[MM_NEDIT];
    atomic_uint edit_w, edit_r;
    atomic_uint last_set;                        /* its CLAP id; 0 none */
    /* the module's last samples (the audio thread writes) for the screen's scope, and when to ask it to draw */
    float scope[MM_SCOPE];
    atomic_uint scope_at;
    atomic_int screen_on;                        /* the screen drew in the last second */
    uint32_t ask_frames;                         /* (audio) frames since it last asked for a draw */
    int ask_live;                                /* (audio) the part sounded then */
    struct mm_plug *live_next;                   /* the instances alive (mm_plugin.c's list, for the folder watch) */
} mm_plug;

extern mm_kind *kinds;
extern uint32_t nkinds;
extern mm_param *gp;
extern uint32_t ngp;
extern mm_page *gpg;
extern char sx_dir[4096];                        /* the SysEx folder beside the plug-in */

const mm_param *param_find(clap_id id);
int32_t value_get(mm_plug *pl, const mm_param *p);
void value_text(mm_plug *pl, const mm_param *p, int32_t v, char *buf, uint32_t cap);
/* factory sound k of synth m (that synth loaded when it is another), or sound k a SysEx file of synth m offers: 0,
 * else -1; the host told (its values, its pages, the preset loaded) */
int sound_load(mm_plug *pl, const mm_kind *m, const char *syx, uint32_t k);
/* p (the synth it plays) set to v as a host sets it: the audio thread takes it at its next block (now when none
 * runs); the host told */
void value_edit(mm_plug *pl, const mm_param *p, int32_t v);
uint8_t *syx_read(const char *path, uint32_t *n);
const mm_kind *syx_owner(const uint8_t *data, uint32_t n);
void patch_name(const mm_schema *sc, const uint8_t *patch, uint32_t len, char *buf, uint32_t cap);
int name_order(const void *a, const void *b);
