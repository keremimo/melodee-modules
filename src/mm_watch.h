/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The folders the plug-in reads when its library loads (Modules, SysEx) looked at by a thread of its own: once what is
 * in them has changed and held still, a call to `changed`, once a change, so the plug-in asks its host to open it again
 * (CLAP request_restart: Melodee opens it with its state from a fresh copy of its library, which reads the folders
 * again). Only files with the given endings count: their names, sizes and times. */
#pragma once
#include <stdint.h>

#define MM_WATCH_EVERY_MS 2000u                  /* (MM_WATCH_MS in the environment: other, for the tests) */
#define MM_WATCH_SETTLE_MS 1000u

/* a fingerprint of the files in dirs[i] ending in exts[i] (n of each): 0 none there */
uint64_t mm_watch_sign(const char *const *dirs, const char *const *exts, uint32_t n);

/* the thread started (when none runs), `read_as` the fingerprint of what the library read (mm_watch_sign() when it
 * loaded): 0, else -1. `changed(ctx)` on the thread, once each change. Stopped (and waited for) by mm_watch_stop(),
 * from any thread but its own */
int mm_watch_start(const char *const *dirs, const char *const *exts, uint32_t n, uint64_t read_as,
                   void (*changed)(void *ctx), void *ctx);
void mm_watch_stop(void);
