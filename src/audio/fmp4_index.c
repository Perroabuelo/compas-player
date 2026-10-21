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

fmp4_status_t fmp4_parse_trex(const uint8_t * buf, size_t len, fmp4_trex_t * out) {
    memset(out, 0, sizeof(*out));
    uint32_t type, hl;
    uint64_t size;
    fmp4_status_t st = fmp4_box_header(buf, len, &type, &hl, &size);
    if (st != FMP4_OK)
        return st;
    if (type != FMP4_FOURCC('t', 'r', 'e', 'x') || size < (uint64_t)hl + 24)
        return FMP4_ERR_INVALID;
    if (size > len)
        return FMP4_ERR_TRUNCATED;
    const uint8_t * p = buf + hl + 4;
    out->track_id = rd32(p);
    out->default_duration = rd32(p + 8);
    out->default_size = rd32(p + 12);
    out->default_flags = rd32(p + 16);
    return FMP4_OK;
}

/* Next child box of a parent whose remaining bytes are [*p, *p + *left).
 * The parent is already in memory, so a child that overruns it is invalid,
 * not truncated. */
static fmp4_status_t next_child(const uint8_t ** p, size_t * left, uint32_t * type,
                                const uint8_t ** body, size_t * body_len) {
    uint32_t hl;
    uint64_t size;
    fmp4_status_t st = fmp4_box_header(*p, *left, type, &hl, &size);
    if (st != FMP4_OK || size > *left)
        return FMP4_ERR_INVALID;
    *body = *p + hl;
    *body_len = (size_t)size - hl;
    *p += size;
    *left -= (size_t)size;
    return FMP4_OK;
}

/* trun layout: flags, sample_count, then where the entries start and how
 * long each one is. The count limit is checked before the box size. */
static fmp4_status_t trun_layout(const uint8_t * b, size_t n, uint32_t * flags,
                                 uint32_t * count, size_t * entries_at,
                                 size_t * entry_len) {
    if (n < 8)
        return FMP4_ERR_INVALID;
    *flags = rd32(b) & 0xffffffu;
    *count = rd32(b + 4);
    if (*count > FMP4_MAX_FRAG_SAMPLES)
        return FMP4_ERR_LIMIT;
    size_t at = 8 + (*flags & 0x1 ? 4 : 0) + (*flags & 0x4 ? 4 : 0);
    size_t entry = 0;
    for (uint32_t bit = 0x100; bit <= 0x800; bit <<= 1)
        entry += *flags & bit ? 4 : 0;
    if (at > n || (uint64_t)*count * entry > n - at)
        return FMP4_ERR_INVALID;
    *entries_at = at;
    *entry_len = entry;
    return FMP4_OK;
}

void fmp4_frag_free(fmp4_frag_t * frag) {
    if (!frag)
        return;
    free(frag->samples);
    free(frag->durations);
    memset(frag, 0, sizeof(*frag));
}

fmp4_status_t fmp4_parse_moof(const uint8_t * buf, size_t len, uint64_t offset,
                              const fmp4_trex_t * trex, fmp4_frag_t * out) {
    memset(out, 0, sizeof(*out));
    uint32_t type, hl, mhl;
    uint64_t moof_size, mdat_size;
    fmp4_status_t st = fmp4_box_header(buf, len, &type, &hl, &moof_size);
    if (st != FMP4_OK)
        return st;
    if (type != FMP4_FOURCC('m', 'o', 'o', 'f'))
        return FMP4_ERR_INVALID;
    if (moof_size > FMP4_MAX_MOOF_BYTES)
        return FMP4_ERR_LIMIT;
    if (moof_size > len)
        return FMP4_ERR_TRUNCATED;
    size_t ms = (size_t)moof_size;
    st = fmp4_box_header(buf + ms, len - ms, &type, &mhl, &mdat_size);
    if (st != FMP4_OK)
        return st;
    if (type != FMP4_FOURCC('m', 'd', 'a', 't') || offset > UINT64_MAX - moof_size ||
        offset + moof_size > UINT64_MAX - mdat_size)
        return FMP4_ERR_INVALID;
    uint64_t data_start = offset + moof_size + mhl;
    uint64_t data_end = offset + moof_size + mdat_size;
    if (data_end - data_start > UINT32_MAX)
        return FMP4_ERR_LIMIT;

    /* moof: mfhd and exactly one traf. */
    const uint8_t * p = buf + hl, * traf = NULL, * body;
    size_t left = ms - hl, traf_len = 0, body_len;
    while (left) {
        if (next_child(&p, &left, &type, &body, &body_len) != FMP4_OK)
            return FMP4_ERR_INVALID;
        if (type == FMP4_FOURCC('t', 'r', 'a', 'f')) {
            if (traf)
                return FMP4_ERR_UNSUPPORTED; /* more than one track */
            traf = body;
            traf_len = body_len;
        }
    }
    if (!traf)
        return FMP4_ERR_INVALID;

    /* First pass over traf: tfhd, tfdt and the truns' total sample count. */
    const uint8_t * tfhd = NULL, * tfdt = NULL;
    size_t tfhd_len = 0, tfdt_len = 0;
    uint32_t total = 0, flags, count;
    size_t entries_at, entry_len;
    p = traf;
    left = traf_len;
    while (left) {
        if (next_child(&p, &left, &type, &body, &body_len) != FMP4_OK)
            return FMP4_ERR_INVALID;
        if (type == FMP4_FOURCC('t', 'f', 'h', 'd')) {
            tfhd = body;
            tfhd_len = body_len;
        } else if (type == FMP4_FOURCC('t', 'f', 'd', 't')) {
            tfdt = body;
            tfdt_len = body_len;
        } else if (type == FMP4_FOURCC('t', 'r', 'u', 'n')) {
            st = trun_layout(body, body_len, &flags, &count, &entries_at, &entry_len);
            if (st != FMP4_OK)
                return st;
            if (count > FMP4_MAX_FRAG_SAMPLES - total)
                return FMP4_ERR_LIMIT;
            total += count;
        }
    }
    if (!tfhd || total == 0)
        return FMP4_ERR_INVALID;

    /* tfhd: track, base data offset and the defaults over trex's. */
    if (tfhd_len < 8)
        return FMP4_ERR_INVALID;
    uint32_t tf_flags = rd32(tfhd) & 0xffffffu;
    size_t need = 8 + (tf_flags & 0x1 ? 8 : 0) + (tf_flags & 0x2 ? 4 : 0) +
                  (tf_flags & 0x8 ? 4 : 0) + (tf_flags & 0x10 ? 4 : 0) +
                  (tf_flags & 0x20 ? 4 : 0);
    if (need > tfhd_len || (trex && rd32(tfhd + 4) != trex->track_id))
        return FMP4_ERR_INVALID;
    const uint8_t * q = tfhd + 8;
    /* Without base-data-offset, the base is the moof (one traf only). */
    uint64_t base = offset;
    if (tf_flags & 0x1) {
        base = rd64(q);
        q += 8;
    }
    if (tf_flags & 0x2)
        q += 4;
    uint32_t def_duration = trex ? trex->default_duration : 0;
    uint32_t def_size = trex ? trex->default_size : 0;
    if (tf_flags & 0x8) {
        def_duration = rd32(q);
        q += 4;
    }
    if (tf_flags & 0x10)
        def_size = rd32(q);

    if (tfdt) {
        if (tfdt_len < 8 || tfdt[0] > 1 || (tfdt[0] == 1 && tfdt_len < 12))
            return FMP4_ERR_INVALID;
        out->decode_time = tfdt[0] == 1 ? rd64(tfdt + 4) : rd32(tfdt + 4);
        out->has_decode_time = true;
    }

    /* Second pass: the samples. total is within FMP4_MAX_FRAG_SAMPLES. */
    fmp4_sample_t * samples = malloc(total * sizeof(*samples));
    uint32_t * durations = NULL;
    if (!samples) {
        memset(out, 0, sizeof(*out));
        return FMP4_ERR_NOMEM;
    }
    uint32_t k = 0, first_duration = 0;
    uint64_t cursor = base, total_duration = 0;
    p = traf;
    left = traf_len;
    while (left && st == FMP4_OK) {
        next_child(&p, &left, &type, &body, &body_len); /* checked above */
        if (type != FMP4_FOURCC('t', 'r', 'u', 'n'))
            continue;
        trun_layout(body, body_len, &flags, &count, &entries_at, &entry_len);
        if (flags & 0x1) {
            int64_t d = (int32_t)rd32(body + 8);
            if ((d < 0 && (uint64_t)-d > base) || (d > 0 && base > UINT64_MAX - (uint64_t)d)) {
                st = FMP4_ERR_INVALID;
                break;
            }
            cursor = base + (uint64_t)d;
        }
        const uint8_t * e = body + entries_at;
        for (uint32_t i = 0; i < count; i++, k++, e += entry_len) {
            const uint8_t * f = e;
            uint32_t duration = def_duration, size = def_size;
            if (flags & 0x100) {
                duration = rd32(f);
                f += 4;
            }
            if (flags & 0x200)
                size = rd32(f);
            if (duration == 0 || size == 0 || cursor < data_start ||
                cursor > data_end || size > data_end - cursor) {
                st = FMP4_ERR_INVALID;
                break;
            }
            samples[k].offset = (uint32_t)(cursor - data_start);
            samples[k].size = size;
            cursor += size;
            if (k == 0)
                first_duration = duration;
            if (duration != first_duration && !durations) {
                durations = malloc(total * sizeof(*durations));
                if (!durations) {
                    st = FMP4_ERR_NOMEM;
                    break;
                }
                for (uint32_t j = 0; j < k; j++)
                    durations[j] = first_duration;
            }
            if (durations)
                durations[k] = duration;
            total_duration += duration;
        }
    }
    if (st != FMP4_OK) {
        free(samples);
        free(durations);
        memset(out, 0, sizeof(*out));
        return st;
    }
    out->data_start = data_start;
    out->next_offset = data_end;
    out->count = total;
    out->samples = samples;
    out->duration = first_duration;
    out->var_duration = durations != NULL;
    out->durations = durations;
    out->total_duration = total_duration;
    return FMP4_OK;
}
