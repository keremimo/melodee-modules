/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* A module's AArch64 image (its file's AA64 chunk, mod_build.py --native) in memory: the code, constants, descriptor
 * and engine mapped, their address words moved by the base, then read-only and executable; the data and .bss
 * writable on pages of their own. One per plug-in instance, so each part has the module's statics to itself. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "mod_abi3.h"

typedef struct mm_image {
    uint8_t *base;
    size_t map;
    const mod_desc_v3 *desc;
    mod_engine_v3 eng;                           /* the module's engine, eng_size bytes of it (the rest 0) */
} mm_image;

/* 0, else what is wrong (a short text) */
const char *mm_image_load(mm_image *im, const uint8_t *chunk, uint32_t len);
void mm_image_free(mm_image *im);
