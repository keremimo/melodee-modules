/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#include "mm_watch.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MM_WATCH_DIRS 4u

static uint64_t fnv64(uint64_t h, const void *d, size_t n)
{
    const uint8_t *b = d;
    while (n--)
        h = (h ^ *b++) * 1099511628211ull;
    return h;
}

uint64_t mm_watch_sign(const char *const *dirs, const char *const *exts, uint32_t n)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        DIR *dp = dirs[i] && dirs[i][0] ? opendir(dirs[i]) : 0;
        struct dirent *de;
        size_t el = strlen(exts[i]);
        while (dp && (de = readdir(dp))) {
            char path[4096 + 256];
            struct stat st;
            size_t nl = strlen(de->d_name);
            int64_t size, when;
            uint64_t h = 14695981039346656037ull;
            if (de->d_name[0] == '.' || nl <= el || strcasecmp(de->d_name + nl - el, exts[i]))
                continue;
            if (snprintf(path, sizeof path, "%s/%s", dirs[i], de->d_name) >= (int)sizeof path || stat(path, &st))
                continue;
            size = (int64_t)st.st_size, when = (int64_t)st.st_mtime;
            h = fnv64(h, &i, sizeof i);
            h = fnv64(h, de->d_name, nl);
            h = fnv64(h, &size, sizeof size);
            h = fnv64(h, &when, sizeof when);
            sum += h | 1u;                       /* (in any order the folder lists them) */
        }
        if (dp)
            closedir(dp);
    }
    return sum;
}

/* ---- the thread (one for the library) */
static pthread_mutex_t w_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t w_wake = PTHREAD_COND_INITIALIZER;
static pthread_t w_thread;
static int w_running, w_stop;
static char w_dirs[MM_WATCH_DIRS][4096], w_exts[MM_WATCH_DIRS][16];
static uint32_t w_n;
static uint64_t w_read_as;
static void (*w_changed)(void *);
static void *w_ctx;

static uint32_t every_ms(void)
{
    const char *e = getenv("MM_WATCH_MS");
    long v = e ? strtol(e, 0, 10) : 0;
    return v > 0 && v < 60000 ? (uint32_t)v : MM_WATCH_EVERY_MS;
}

static void *w_run(void *arg)
{
    const char *dirs[MM_WATCH_DIRS], *exts[MM_WATCH_DIRS];
    uint32_t every = every_ms(), settle = every == MM_WATCH_EVERY_MS ? MM_WATCH_SETTLE_MS : every / 2u, held = 0;
    uint64_t read_as = w_read_as, seen = w_read_as;
    (void)arg;
    for (uint32_t i = 0; i < w_n; i++)
        dirs[i] = w_dirs[i], exts[i] = w_exts[i];
    pthread_mutex_lock(&w_lock);
    while (!w_stop) {
        struct timespec until;
        uint64_t now;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += (time_t)(every / 1000u);
        until.tv_nsec += (long)(every % 1000u) * 1000000L;
        if (until.tv_nsec >= 1000000000L)
            until.tv_sec++, until.tv_nsec -= 1000000000L;
        while (!w_stop && pthread_cond_timedwait(&w_wake, &w_lock, &until) != ETIMEDOUT)
            ;
        if (w_stop)
            break;
        pthread_mutex_unlock(&w_lock);
        now = mm_watch_sign(dirs, exts, w_n);
        if (now == read_as) {
            seen = now, held = 0;
        } else if (now != seen) {                /* changed, or still changing: from now */
            seen = now, held = 0;
        } else if ((held += every) >= settle) {  /* held still: once */
            read_as = now, held = 0;
            w_changed(w_ctx);
        }
        pthread_mutex_lock(&w_lock);
    }
    pthread_mutex_unlock(&w_lock);
    return 0;
}

int mm_watch_start(const char *const *dirs, const char *const *exts, uint32_t n, uint64_t read_as,
                   void (*changed)(void *ctx), void *ctx)
{
    int r = 0;
    if (n > MM_WATCH_DIRS)
        n = MM_WATCH_DIRS;
    pthread_mutex_lock(&w_lock);
    if (!w_running) {
        for (uint32_t i = 0; i < n; i++) {
            snprintf(w_dirs[i], sizeof w_dirs[i], "%s", dirs[i] ? dirs[i] : "");
            snprintf(w_exts[i], sizeof w_exts[i], "%s", exts[i]);
        }
        w_n = n, w_read_as = read_as, w_changed = changed, w_ctx = ctx, w_stop = 0;
        if (pthread_create(&w_thread, 0, w_run, 0))
            r = -1;
        else
            w_running = 1;
    }
    pthread_mutex_unlock(&w_lock);
    return r;
}

void mm_watch_stop(void)
{
    int join;
    pthread_mutex_lock(&w_lock);
    join = w_running;
    w_stop = 1;
    pthread_cond_broadcast(&w_wake);
    pthread_mutex_unlock(&w_lock);
    if (join)
        pthread_join(w_thread, 0);
    pthread_mutex_lock(&w_lock);
    w_running = 0;
    pthread_mutex_unlock(&w_lock);
}
