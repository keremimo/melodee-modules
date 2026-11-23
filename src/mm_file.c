/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
#include "mm_file.h"
#include "mm_sign.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"   /* (its writer, the builder's) */
#include "mod_schema.h"
#pragma GCC diagnostic pop

static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    static uint32_t T[256];
    uint32_t c = 0xFFFFFFFFu;
    if (!T[1])
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1u ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            T[i] = v;
        }
    while (n--)
        c = T[(c ^ *p++) & 255u] ^ (c >> 8);
    return ~c;
}
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static void copy_name(char *dst, const uint8_t *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i] >= 32 && src[i] < 127 ? (char)src[i] : '?';
    dst[i] = 0;
}

/* the PRST chunk (the device's module_presets.h): its head, index, names and CRC */
static int prst_ok(const uint8_t *p, uint32_t n, uint32_t count)
{
    uint32_t recs, names, dict_at, i;
    if (n < 16u || le16(p) != 1u || le16(p + 2) != count || le16(p + 8) < MM_PST_COMMON ||
        le16(p + 6) + le16(p + 8) > 640u || crc32_of(p + 16, n - 16u) != le32(p + 12))
        return 0;
    recs = 16u;
    names = recs + 8u * (count + 1u);
    if (names > n)
        return 0;
    dict_at = names + le16(p + recs + 8u * count + 4u);
    if (dict_at + le16(p + 6) > n)
        return 0;
    for (i = 0; i < count; i++) {
        const uint8_t *r = p + recs + 8u * i;
        if (le32(r) > le32(r + 8) || le32(r + 8) > n || le16(r + 4) > le16(r + 12) || le16(r + 12) - le16(r + 4) > MM_PST_NAME)
            return 0;
    }
    return 1;
}

const char *mm_file_read(mm_file *f, const char *path)
{
    FILE *fp = fopen(path, "rb");
    long n;
    uint32_t at, last = 0, nsig = 0, idnt_len = 0; /* (the place of the last chunk the plug-in takes, and NSIG's) */
    const uint8_t *idnt_p = 0;
    const mod_ident_v3 *idnt;
    uint8_t mid[8];
    memset(f, 0, sizeof *f);
    if (!fp)
        return "cannot open";
    if (fseek(fp, 0, SEEK_END) || (n = ftell(fp)) < (long)MOD3_HEAD || n > 4L << 20 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return "not a module file";
    }
    f->b = malloc((size_t)n);
    if (!f->b || fread(f->b, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        mm_file_free(f);
        return "cannot read";
    }
    fclose(fp);
    f->size = (uint32_t)n;
    memcpy(&f->head, f->b, sizeof f->head);
    if (f->head.magic != MOD3_FILE_MAGIC || f->head.abi != MOD3_ABI || f->head.total != f->size ||
        crc32_of(f->b + MOD3_HEAD, f->size - MOD3_HEAD) != f->head.crc) {
        mm_file_free(f);
        return "not an ABI 3 module file";
    }
    if (f->head.host > MOD3_HOST) {
        mm_file_free(f);
        return "needs a newer host";
    }
    copy_name(f->name, (const uint8_t *)f->head.name, MOD3_NAME_MAX);
    at = MOD3_HEAD + f->head.image + 4u * f->head.nrel;
    if (!(f->head.flags & MOD3_FILE_CHUNKS) || at > f->size) {
        mm_file_free(f);
        return "no chunks";
    }
    while (at + 8u <= f->size) {                 /* the chunks: tag, length, payload, padding to 4 */
        uint32_t tag = le32(f->b + at), len = le32(f->b + at + 4u);
        const uint8_t *p = f->b + at + 8u;
        const uint8_t **seen = tag == MM_TAG_PRST ? &f->prst : tag == MM_TAG_SCHM ? &f->schm :
                               tag == MM_TAG_AA64 ? &f->aa64 : tag == MOD3_IDNT_TAG ? &idnt_p : 0;
        if (len > f->size - at - 8u)
            break;
        if ((seen && *seen) || (tag == MOD3_NSIG_TAG && nsig)) {
            mm_file_free(f);
            return "a chunk twice";
        }
        if (seen)
            last = at;
        if (tag == MM_TAG_PRST)
            f->prst = p, f->prst_len = len;
        else if (tag == MM_TAG_SCHM)
            f->schm = p, f->schm_len = len;
        else if (tag == MM_TAG_AA64)
            f->aa64 = p, f->aa64_len = len;
        else if (tag == MOD3_IDNT_TAG)
            idnt_p = p, idnt_len = len;
        else if (tag == MOD3_NSIG_TAG && len == 64u)
            nsig = at;
        at += 8u + ((len + 3u) & ~3u);
    }
    if (at != f->size) {
        mm_file_free(f);
        return "a chunk runs past the file";
    }
    if (!f->aa64) {
        mm_file_free(f);
        return "no AArch64 code (mod_build.py --native)";
    }
    idnt = idnt_len == sizeof(mod_ident_v3) ? (const mod_ident_v3 *)(const void *)idnt_p : 0;
    if (!idnt || idnt->version != MOD3_IDNT_VERSION) {
        mm_file_free(f);
        return "says not who it is (no IDNT chunk: build it again with the SDK)";
    }
    mm_id_make(idnt, mid);
    if (memcmp(mid, idnt->id, 8) || memcmp(mid, &f->head.uid, 4)) {
        mm_file_free(f);
        return "its ID is not the one its key and name make: not its own";
    }
    if (idnt->flags & MOD3_IDNT_SIGNED) {        /* (NSIG after every chunk taken, its signature over all before it) */
        static const char PREFIX[] = MOD3_NSIG_PREFIX;
        enum { CRC_AT = offsetof(mod_file3_t, crc) };
        const uint8_t *part[4] = {(const uint8_t *)PREFIX, f->b, f->b + CRC_AT + 4u, f->b + MOD3_HEAD};
        uint32_t len[4] = {sizeof PREFIX - 1u, CRC_AT, MOD3_HEAD - CRC_AT - 4u, nsig - MOD3_HEAD};
        if (!nsig || nsig < last || mm_sign_check(f->b + nsig + 8u, idnt->key, part, len, 4u)) {
            mm_file_free(f);
            return "its AArch64 code is not signed by its author's key (NSIG): not its own";
        }
        f->signer = memcmp(idnt->key, MM_MELODEE_KEY, 32) ? MM_SIGNER_AUTHOR : MM_SIGNER_MELODEE;
    }
    memcpy(f->id, idnt->id, 8);
    copy_name(f->slug, (const uint8_t *)idnt->slug, MOD3_NAME_MAX);
    copy_name(f->vendor, (const uint8_t *)idnt->vendor, MOD3_NAME_MAX);
    if (f->prst && prst_ok(f->prst, f->prst_len, f->head.npresets))
        f->npresets = f->head.npresets;
    if (f->schm && f->schm_len >= MOD_SCH_HEAD && crc32_of(f->schm + MOD_SCH_HEAD, f->schm_len - MOD_SCH_HEAD) ==
                                                      le32(f->schm + 48)) {
        mod_sch_head h;
        memcpy(&h, f->schm, sizeof h);
        if (h.patch <= MOD3_PATCH_MAX && h.def + (uint32_t)h.patch <= f->schm_len) {
            f->patch_len = h.patch;
            f->patch_def = f->schm + h.def;
            f->first = h.first < f->npresets ? h.first : 0;
        }
    }
    return 0;
}

void mm_file_free(mm_file *f)
{
    free(f->b);
    memset(f, 0, sizeof *f);
}

/* the device's lz_unpack, bounded: n bytes into o (dict bytes before o may be copied from) from src's len bytes */
static int lz_unpack_ok(uint8_t *o, uint32_t dict, uint32_t n, const uint8_t *b, uint32_t len)
{
    uint32_t i = 0, w = 0;
    while (w < n) {
        uint32_t c;
        if (i >= len)
            return 0;
        c = b[i++];
        if (c < 0x80u) {
            c++;
            if (c > n - w || c > len - i)
                return 0;
            memcpy(o + w, b + i, c);
            i += c, w += c;
        } else {
            uint32_t back;
            if (len - i < 2u)
                return 0;
            back = (b[i] | (uint32_t)b[i + 1] << 8) + 1u;
            i += 2u;
            c = (c & 0x7Fu) + 3u;
            if (back > dict + w || c > n - w)
                return 0;
            for (; c; c--, w++)
                o[w] = o[(int32_t)w - (int32_t)back];
        }
    }
    return i == len;
}

int mm_file_sound(const mm_file *f, uint32_t k, char *name, uint8_t *category, uint8_t *common, uint8_t *patch,
                  uint32_t cap, uint32_t *patch_len)
{
    const uint8_t *p = f->prst, *r;
    uint32_t names, dict, ulen, dict_at;
    uint8_t buf[640 + 256];
    if (k >= f->npresets)
        return -1;
    r = p + 16u + 8u * k;
    names = 16u + 8u * (f->npresets + 1u);
    dict = le16(p + 6);
    ulen = le16(p + 8);
    dict_at = names + le16(p + 16u + 8u * f->npresets + 4u);
    if (name) {
        uint32_t a = names + le16(r + 4), b = names + le16(r + 12), i;
        for (i = 0; i < b - a; i++)
            name[i] = (char)p[a + i];
        name[i] = 0;
    }
    if (category)
        *category = r[6];
    memcpy(buf, p + dict_at, dict);
    if (!lz_unpack_ok(buf + dict, dict, ulen, p + le32(r), le32(r + 8) - le32(r)))
        return -1;
    if (common)
        memcpy(common, buf + dict, MM_PST_COMMON);
    if (patch_len)
        *patch_len = ulen - MM_PST_COMMON;
    if (patch) {
        if (ulen - MM_PST_COMMON > cap)
            return -1;
        memcpy(patch, buf + dict + MM_PST_COMMON, ulen - MM_PST_COMMON);
    }
    return 0;
}
