/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Michroma's letters (mm_font.c, OFL-1.1), the faceplates' lettering, as coverage bitmaps: ASCII 32..126 then the
 * middle dot, at 11, 13, 15 and 18 pixels to the em. */
#pragma once
#include <stdint.h>

typedef struct mm_glyph {
    int8_t x, y;                                 /* the bitmap's top left from the pen on the baseline */
    uint8_t w, h;
    uint16_t adv;                                /* the pen's advance, 1/64 pixel */
    uint32_t at;                                 /* its first byte in the face's bitmaps */
} mm_glyph;

typedef struct mm_face {
    uint8_t em, ascent, descent, cap;            /* pixels */
    const mm_glyph *g;                           /* MM_FONT_NGLYPH */
    const uint8_t *bits;                         /* coverage, 0..255, row after row */
} mm_face;

#define MM_FONT_NGLYPH 96u

extern const mm_face MM_FACE_M11, MM_FACE_M13, MM_FACE_M15, MM_FACE_M18;
