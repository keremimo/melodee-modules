/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#include "mm_image.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define AA64_VERSION 1u
#define AA64_HEAD 24u                            /* version, code, data's place, data, bss, address words (u32 each) */

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

const char *mm_image_load(mm_image *im, const uint8_t *c, uint32_t len)
{
#if !defined(__aarch64__)
    (void)c, (void)len;
    memset(im, 0, sizeof *im);
    return "AArch64 code on another processor";
#else
    uint32_t code, data_at, data, bss, nrel, i;
    size_t page = (size_t)sysconf(_SC_PAGESIZE), map, rx;
    uint8_t *base;
    const mod_desc_v3 *d;
    memset(im, 0, sizeof *im);
    if (len < AA64_HEAD || le32(c) != AA64_VERSION)
        return "an AA64 chunk of another version";
    code = le32(c + 4), data_at = le32(c + 8), data = le32(c + 12), bss = le32(c + 16), nrel = le32(c + 20);
    if (code < sizeof(mod_desc_v3) || data_at < code || data_at % page || (uint64_t)AA64_HEAD + code + data + 4ull * nrel != len ||
        (uint64_t)data_at + data + bss > (64u << 20))
        return "a bad AA64 chunk";
    map = ((size_t)data_at + data + bss + page - 1u) & ~(page - 1u);
    rx = ((size_t)code + page - 1u) & ~(page - 1u);
    base = mmap(0, map, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
        return "no memory";
    memcpy(base, c + AA64_HEAD, code);
    memcpy(base + data_at, c + AA64_HEAD + code, data);
    for (i = 0; i < nrel; i++) {                 /* the address words: + base */
        uint32_t w = le32(c + AA64_HEAD + code + data + 4u * i);
        uint64_t v;
        if (!((uint64_t)w + 8u <= code || (w >= data_at && (uint64_t)w + 8u <= (uint64_t)data_at + data))) {
            munmap(base, map);
            return "an address word outside the image";
        }
        memcpy(&v, base + w, 8);
        v += (uint64_t)(uintptr_t)base;
        memcpy(base + w, &v, 8);
    }
    if (mprotect(base, rx, PROT_READ | PROT_EXEC)) {
        munmap(base, map);
        return "cannot map code";
    }
    __builtin___clear_cache((char *)base, (char *)base + code);
    d = (const mod_desc_v3 *)(const void *)base;
    if (d->magic != MOD3_MAGIC || d->abi != MOD3_ABI || d->size != sizeof(mod_desc_v3) || d->host > MOD3_HOST ||
        !d->eng_size || d->eng_size > sizeof(mod_engine_v3) || (const uint8_t *)d->eng < base ||
        (const uint8_t *)d->eng + d->eng_size > base + code) {
        munmap(base, map);
        return "no ABI 3 descriptor first";
    }
    im->base = base;
    im->map = map;
    im->desc = d;
    memcpy(&im->eng, d->eng, d->eng_size);       /* (a field the module is too old to have reads 0) */
    if (!im->eng.note_on || !im->eng.render) {
        mm_image_free(im);
        return "no note_on or render";
    }
    return 0;
#endif
}

void mm_image_free(mm_image *im)
{
    if (im->base)
        munmap(im->base, im->map);
    memset(im, 0, sizeof *im);
}
