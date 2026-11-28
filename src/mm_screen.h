/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The plug-in's own page in Melodee (melodee_screen.h, Stage's PANEL): its synths, a synth's sounds, and the sound's
 * values a page at a time. Minus / Plus walk the pages (SYNTH, SOUNDS, then the synth's own), the encoder walks a list
 * or turns a page's fourth value, Push picks, K1-K3 turn the three values the page shows. */
#pragma once
#include "melodee_screen.h"

struct mm_screen;
extern const melodee_screen_t mm_screen_ext;
void mm_screen_free(struct mm_screen *s);
