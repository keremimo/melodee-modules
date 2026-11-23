/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* A synth module file (.mmod, ABI 3: sdk/mod_abi3.h) as the plug-in reads it: its head and CRC, its chunks (PRST the
 * factory sounds, SCHM the schema, IDNT who it is, AA64 its code for AArch64: mod_build.py --native, NSIG that code
 * signed), its sounds unpacked. The FM-1's image (pi32v2) is not used. A file whose ID is not its own, or a signed
 * one whose NSIG is not its key's over everything the plug-in takes from it, is refused: its code never runs. */
#pragma once
#include <stdint.h>
#include "mod_abi3.h"

#define MM_TAG_PRST 0x54535250u                  /* "PRST" */
#define MM_TAG_SCHM 0x4D484353u                  /* "SCHM" */
#define MM_TAG_AA64 0x34364141u                  /* "AA64" */
#define MM_SIGNER_NONE 0u                        /* mm_file.signer: unsigned (anyone can make a file with its ID) */
#define MM_SIGNER_AUTHOR 1u                      /* .. signed by its author's key */
#define MM_SIGNER_MELODEE 2u                     /* .. one of Melodee's synths, signed by Melodee's key */
#define MM_PST_COMMON 16u                        /* a sound's common block: E1..E8, ATK DEC SUS REL, ENV FLT, MONO */
#define MM_PST_NAME 20u

typedef struct mm_file {
    uint8_t *b;                                  /* the whole file */
    uint32_t size;
    mod_file3_t head;
    char name[MOD3_NAME_MAX + 1];
    uint8_t id[8];                               /* IDNT's */
    uint8_t signer;                              /* MM_SIGNER_* */
    char slug[MOD3_NAME_MAX + 1], vendor[MOD3_NAME_MAX + 1];
    const uint8_t *prst, *schm, *aa64;           /* chunk payloads, 0 none */
    uint32_t prst_len, schm_len, aa64_len;
    uint32_t npresets;                           /* the sounds PRST holds (checked) */
    uint32_t patch_len;                          /* SCHM: the part's patch's bytes, 0 none */
    const uint8_t *patch_def;                    /* .. and the patch it starts as */
    uint32_t first;                              /* SCHM: the sound a new part starts from */
} mm_file;

/* path into f: 0, else what is wrong with it (a short text) */
const char *mm_file_read(mm_file *f, const char *path);
void mm_file_free(mm_file *f);
/* sound k: its name (MM_PST_NAME + 1 bytes), category, common block (MM_PST_COMMON bytes) and patch (cap bytes; its
 * length into *patch_len). 0, else -1 */
int mm_file_sound(const mm_file *f, uint32_t k, char *name, uint8_t *category, uint8_t *common, uint8_t *patch,
                  uint32_t cap, uint32_t *patch_len);
