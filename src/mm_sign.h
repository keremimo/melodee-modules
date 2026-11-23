/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* Who a module file is and whether its author signed it (sdk/mod_abi3.h: IDNT, NSIG): the module's ID made from its
 * key (or vendor) and slug, and Ed25519 (RFC 8032) as the FM-1 checks a SIGN chunk (Melodee's firmware/src/mod_sign.c) */
#pragma once
#include <stdint.h>
#include "mod_abi3.h"

/* Melodee's release key: the key of every synth Melodee makes */
extern const uint8_t MM_MELODEE_KEY[32];
/* the ID id's parts make (its 8 bytes into out) */
void mm_id_make(const mod_ident_v3 *id, uint8_t out[8]);
/* sig (64 bytes): pk's signature of the n parts' bytes one after another (part[i], len[i])? 0 yes, else not */
int mm_sign_check(const uint8_t *sig, const uint8_t *pk, const uint8_t *const *part, const uint32_t *len, uint32_t n);
