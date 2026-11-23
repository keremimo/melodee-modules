/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* A module's schema (its file's SCHM chunk, sdk/mod_schema.h) as the plug-in reads it: its parameters by id (bits of
 * the patch, the part's own, or the module's pget / pput) and its pages (the FM-1's four knobs each; an instanced page
 * once per instance). Values got, set and shown as the firmware's schema.c does. */
#pragma once
#include <stdint.h>
#include "mm_engine.h"
#include "mm_file.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   /* (its writer, the builder's) */
#include "mod_schema.h"                          /* (MOD3_SK_*, MOD3_SF_*, MOD3_PG_*, MOD_SCH_*) */
#pragma GCC diagnostic pop

typedef struct mm_sch_param {
    uint16_t id;
    uint8_t kind, fmt;                           /* MOD3_SK_*, MOD3_F_* */
    int16_t min, max, def;
    uint16_t off;                                /* FIELD: its byte; TRACK: the P_* number */
    uint8_t shift, width, flags, flags2;         /* MOD3_SF_*, MOD3_SF2_* */
    const char *label, *name, *unit;             /* (in the file's bytes; 0 none) */
    const char **names;                          /* MOD3_F_ENUM: max - min + 1 names */
} mm_sch_param;

typedef struct mm_sch_page {
    char title[40];                              /* its short name (and its instance's) */
    char section[24];
    uint8_t flags;                               /* MOD3_PG_* */
    uint16_t id[4];                              /* MOD_SCH_NONE: an empty column */
} mm_sch_page;

#define MM_SCH_NSYSEX 8u
typedef struct mm_schema {
    uint32_t nparam, npage, flags;               /* flags: the head's MOD_SCH_F_* */
    mm_sch_param *param;                         /* ids ascending */
    mm_sch_page *page;                           /* the pages that show parameters (no STORE, TOOLS, knob banks) */
    uint32_t name, nlen;                         /* the sound's name in the patch: its first byte, its bytes (0 none) */
    uint32_t nsysex;                             /* the SysEx it answers (MOD3_R_SYSEX): n (1..3), then the n bytes */
    uint8_t sysex[MM_SCH_NSYSEX][4];             /* after F0 a frame of its begins with */
} mm_schema;

/* f's schema into s: 0 (none: s empty), else -1 (a schema this reader cannot use) */
int mm_schema_read(mm_schema *s, const mm_file *f);
void mm_schema_free(mm_schema *s);
const mm_sch_param *mm_schema_find(const mm_schema *s, uint32_t id);

/* 1: SysEx frame f (F0 .. F7, n bytes) begins as one of s's patterns */
int mm_schema_sysex(const mm_schema *s, const uint8_t *f, uint32_t n);
/* the frames in data (a .syx file's n bytes: F0 .. F7 frames; bytes between them, and a frame another status byte
 * breaks, skipped) that s's patterns name, each to part t's module (mm_sysex); the sounds it offers to take, its last
 * message to msg (cap bytes, "" none). The sounds offered */
uint32_t mm_sysex_file(mm_part *t, const mm_schema *s, const uint8_t *data, uint32_t n, mm_sx_take take, void *ctx,
                       char *msg, uint32_t cap);

/* p's value on part t with patch (main thread: the plug-in's copy of the patch; audio: the part's) */
int32_t mm_sch_get(mm_part *t, const uint8_t *patch, const mm_sch_param *p);
/* .. set to v in its range (the module's descriptor's when it has one): 1 when it changed; the module is not told */
int mm_sch_put(mm_part *t, uint8_t *patch, const mm_sch_param *p, int32_t v);
/* .. its text for v */
void mm_sch_text(mm_part *t, const uint8_t *patch, const mm_sch_param *p, int32_t v, char *buf, uint32_t cap);
