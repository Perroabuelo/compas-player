/* Fragmented MP4 index parsing for hiby-yt (see fmp4_index.h). */
#include "fmp4_index.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t * p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t rd64(const uint8_t * p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

const char * fmp4_status_str(fmp4_status_t status) {
    switch (status) {
    case FMP4_OK:
        return "ok";
    case FMP4_ERR_TRUNCATED:
        return "truncated";
    case FMP4_ERR_INVALID:
        return "invalid";
    case FMP4_ERR_LIMIT:
        return "limit";
    case FMP4_ERR_UNSUPPORTED:
        return "unsupported";
    case FMP4_ERR_NOMEM:
        return "nomem";
    }
    return "unknown";
}

fmp4_status_t fmp4_box_header(const uint8_t * buf, size_t len, uint32_t * type,
                              uint32_t * header_len, uint64_t * size) {
    if (len < 8)
        return FMP4_ERR_TRUNCATED;
    uint64_t sz = rd32(buf);
    uint32_t hl = 8;
    if (sz == 1) {
        if (len < 16)
            return FMP4_ERR_TRUNCATED;
        sz = rd64(buf + 8);
        hl = 16;
    }
    if (sz < hl)
        return FMP4_ERR_INVALID;
    *type = rd32(buf + 4);
    *header_len = hl;
    *size = sz;
    return FMP4_OK;
}

fmp4_status_t fmp4_parse_sidx(const uint8_t * buf, size_t len, uint64_t offset,
                              fmp4_sidx_t * out) {
    memset(out, 0, sizeof(*out));
    uint32_t type, hl;
    uint64_t box_size;
    fmp4_status_t st = fmp4_box_header(buf, len, &type, &hl, &box_size);
    if (st != FMP4_OK)
        return st;
    if (type != FMP4_FOURCC('s', 'i', 'd', 'x'))
        return FMP4_ERR_INVALID;
    /* version/flags, reference_ID, timescale, then two 32- or 64-bit times,
     * reserved and reference_count. */
    if (len < (size_t)hl + 4)
        return FMP4_ERR_TRUNCATED;
    uint8_t version = buf[hl];
    if (version > 1)
        return FMP4_ERR_UNSUPPORTED;
    size_t fixed = hl + 12 + (version ? 16 : 8) + 4;
    if (len < fixed)
        return FMP4_ERR_TRUNCATED;
    const uint8_t * p = buf + hl + 8;
    uint32_t timescale = rd32(p);
    p += 4;
    uint64_t earliest, first_offset;
    if (version) {
        earliest = rd64(p);
        first_offset = rd64(p + 8);
        p += 16;
    } else {
        earliest = rd32(p);
        first_offset = rd32(p + 4);
        p += 8;
    }
    uint32_t count = (uint32_t)p[2] << 8 | p[3];
    p += 4;
    /* The limit comes first: nothing is sized from an oversized count. */
    if (count > FMP4_MAX_SIDX_REFS)
        return FMP4_ERR_LIMIT;
    if (count == 0 || timescale == 0 || fixed + (uint64_t)count * 12 > box_size)
        return FMP4_ERR_INVALID;
    if (box_size > len)
        return FMP4_ERR_TRUNCATED;
    if (offset > UINT64_MAX - box_size || offset + box_size > UINT64_MAX - first_offset)
        return FMP4_ERR_INVALID;
    fmp4_sidx_ref_t * refs = malloc(count * sizeof(*refs));
    if (!refs)
        return FMP4_ERR_NOMEM;
    uint64_t at = offset + box_size + first_offset, total = 0;
    for (uint32_t i = 0; i < count; i++, p += 12) {
        uint32_t ref = rd32(p);
        if (ref >> 31) {
            free(refs);
            return FMP4_ERR_UNSUPPORTED; /* a reference to another sidx */
        }
        uint32_t size = ref & 0x7fffffffu;
        if (size == 0 || at > UINT64_MAX - size) {
            free(refs);
            return FMP4_ERR_INVALID;
        }
        refs[i].offset = at;
        refs[i].size = size;
        refs[i].duration = rd32(p + 4);
        at += size;
        total += refs[i].duration;
    }
    out->timescale = timescale;
    out->earliest_pts = earliest;
    out->total_duration = total;
    out->count = count;
    out->refs = refs;
    return FMP4_OK;
}

void fmp4_sidx_free(fmp4_sidx_t * sidx) {
    if (!sidx)
        return;
    free(sidx->refs);
    memset(sidx, 0, sizeof(*sidx));
}

bool fmp4_sidx_find(const fmp4_sidx_t * sidx, uint64_t t, uint32_t * index,
                    uint64_t * frag_start) {
    if (!sidx || t >= sidx->total_duration)
        return false;
    uint64_t start = 0;
    for (uint32_t i = 0; i < sidx->count; i++) {
        if (t < start + sidx->refs[i].duration) {
            *index = i;
            *frag_start = start;
            return true;
        }
        start += sidx->refs[i].duration;
    }
    return false;
}
