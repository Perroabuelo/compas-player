/* Fragmented MP4 sample access for mp4_demux (hiby-yt, see mp4_frag.h). */
#include "mp4_frag.h"
#include "fmp4_index.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Largest sidx accepted: its fixed fields plus FMP4_MAX_SIDX_REFS entries. */
#define MP4_FRAG_MAX_SIDX_BYTES (16u + 36u + 12u * FMP4_MAX_SIDX_REFS)

struct mp4_frag {
    FILE * f;
    uint64_t file_size;
    uint32_t media_timescale;
    fmp4_trex_t trex;
    fmp4_sidx_t sidx; /* count 0 without sidx */
    uint32_t sample_ticks;
    uint32_t sample_count;
    bool length_known;
    bool var_duration;
    bool failed;
    const char * error_stage;
    const char * error_detail;
    /* The fragment in memory and the global index of its first sample. */
    fmp4_frag_t cur;
    uint32_t cur_first;
    bool cur_valid;
};

static uint32_t rd32(const uint8_t * p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t rd64(const uint8_t * p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

/* A box header whose 32-bit size is 1 carries a 64-bit size next. */
static bool has_large_size(const uint8_t * p) { return rd32(p) == 1; }

/* Payload of the first child of `type` among the boxes in [p, p + len). */
static const uint8_t * find_child(const uint8_t * p, size_t len, uint32_t type, size_t * out_len) {
    while (len) {
        uint32_t t, hl;
        uint64_t size;
        if (fmp4_box_header(p, len, &t, &hl, &size) != FMP4_OK || size > len)
            return NULL;
        if (t == type) {
            *out_len = (size_t)size - hl;
            return p + hl;
        }
        p += size;
        len -= (size_t)size;
    }
    return NULL;
}

/* The audio trak's mdhd duration (media timescale), or 0 when unknown. */
static uint64_t audio_media_duration(const uint8_t * moov, size_t len) {
    while (len) {
        uint32_t t, hl;
        uint64_t size;
        if (fmp4_box_header(moov, len, &t, &hl, &size) != FMP4_OK || size > len)
            return 0;
        size_t mdia_len = 0, hdlr_len = 0, mdhd_len = 0;
        const uint8_t * mdia = t == FMP4_FOURCC('t', 'r', 'a', 'k')
                                   ? find_child(moov + hl, (size_t)size - hl,
                                                FMP4_FOURCC('m', 'd', 'i', 'a'), &mdia_len)
                                   : NULL;
        const uint8_t * hdlr =
            mdia ? find_child(mdia, mdia_len, FMP4_FOURCC('h', 'd', 'l', 'r'), &hdlr_len) : NULL;
        const uint8_t * mdhd =
            mdia ? find_child(mdia, mdia_len, FMP4_FOURCC('m', 'd', 'h', 'd'), &mdhd_len) : NULL;
        if (hdlr && hdlr_len >= 12 && rd32(hdlr + 8) == FMP4_FOURCC('s', 'o', 'u', 'n') && mdhd) {
            if (mdhd_len >= 32 && mdhd[0] == 1)
                return rd64(mdhd + 24);
            if (mdhd_len >= 20 && mdhd[0] == 0)
                return rd32(mdhd + 16);
            return 0;
        }
        moov += size;
        len -= (size_t)size;
    }
    return 0;
}

static bool read_at(mp4_frag_t * m, uint64_t offset, void * buf, size_t len) {
    if (offset > (uint64_t)LONG_MAX || offset > m->file_size || len > m->file_size - offset)
        return false;
    return fseek(m->f, (long)offset, SEEK_SET) == 0 && fread(buf, 1, len, m->f) == len;
}

static bool fail(mp4_frag_t * m, const char * stage, const char * detail) {
    m->failed = true;
    m->error_stage = stage;
    m->error_detail = detail;
    return false;
}

static const char * stage_for(fmp4_status_t st) {
    return st == FMP4_ERR_LIMIT ? "limit" : "moof";
}

/* Reads len bytes at the current position, up to the end of the file;
 * returns how many. */
static size_t read_on(mp4_frag_t * m, uint8_t * buf, size_t len) {
    return fread(buf, 1, len, m->f);
}

/* Loads the fragment at offset, whose first sample has global index first.
 * The new table replaces the old one only once it parsed. Reads strictly
 * forward and stop at the first byte of sample data: over HTTP a step back
 * past what the stream keeps behind its position is a new range request. */
static bool load_fragment(mp4_frag_t * m, uint64_t offset, uint32_t first) {
    uint8_t head[16];
    if (offset >= m->file_size)
        return fail(m, "moof", "past_end");
    size_t got = m->file_size - offset < 8 ? (size_t)(m->file_size - offset) : 8;
    if (!read_at(m, offset, head, got))
        return fail(m, "moof", "read");
    if (got == 8 && has_large_size(head))
        got += read_on(m, head + 8, 8);
    uint32_t type, hl;
    uint64_t size;
    fmp4_status_t st = fmp4_box_header(head, got, &type, &hl, &size);
    if (st == FMP4_OK && type != FMP4_FOURCC('m', 'o', 'o', 'f'))
        st = FMP4_ERR_INVALID;
    if (st == FMP4_OK && size > FMP4_MAX_MOOF_BYTES)
        st = FMP4_ERR_LIMIT;
    if (st != FMP4_OK)
        return fail(m, stage_for(st), fmp4_status_str(st));
    /* The moof, then exactly the mdat header (8 bytes, 16 with a 64-bit
     * size), as far as the file goes. */
    uint8_t * buf = malloc((size_t)size + 16);
    if (!buf)
        return fail(m, "moof", "nomem");
    memcpy(buf, head, got);
    size_t len = got + read_on(m, buf + got, (size_t)size + 8 - got);
    if (len == size + 8 && has_large_size(buf + size))
        len += read_on(m, buf + len, 8);
    fmp4_frag_t frag;
    st = fmp4_parse_moof(buf, len, offset, &m->trex, &frag);
    free(buf);
    if (st != FMP4_OK)
        return fail(m, stage_for(st), fmp4_status_str(st));
    if (frag.var_duration || (m->sample_ticks && frag.duration != m->sample_ticks))
        m->var_duration = true;
    fmp4_frag_free(&m->cur);
    m->cur = frag;
    m->cur_first = first;
    m->cur_valid = true;
    return true;
}

/* Media ticks <-> sidx ticks. */
static uint64_t to_sidx(const mp4_frag_t * m, uint64_t media_ticks) {
    return media_ticks * m->sidx.timescale / m->media_timescale;
}

static uint64_t to_media(const mp4_frag_t * m, uint64_t sidx_ticks) {
    return sidx_ticks * m->media_timescale / m->sidx.timescale;
}

mp4_frag_t * mp4_frag_open(FILE * f, uint64_t file_size, const uint8_t * moov, size_t moov_len,
                           uint64_t moov_offset, uint32_t media_timescale, const char ** stage) {
    const char * dummy;
    if (!stage)
        stage = &dummy;
    *stage = "moov";
    uint32_t type, hl;
    uint64_t moov_size;
    if (!f || !media_timescale ||
        fmp4_box_header(moov, moov_len, &type, &hl, &moov_size) != FMP4_OK ||
        type != FMP4_FOURCC('m', 'o', 'o', 'v') || moov_size != moov_len)
        return NULL;
    uint32_t moov_hl = hl;
    size_t mvex_len = 0, trex_len = 0;
    const uint8_t * mvex =
        find_child(moov + hl, moov_len - hl, FMP4_FOURCC('m', 'v', 'e', 'x'), &mvex_len);
    const uint8_t * trex =
        mvex ? find_child(mvex, mvex_len, FMP4_FOURCC('t', 'r', 'e', 'x'), &trex_len) : NULL;
    mp4_frag_t * m = calloc(1, sizeof(*m));
    if (!trex || !m) {
        free(m);
        return NULL;
    }
    m->f = f;
    m->file_size = file_size;
    m->media_timescale = media_timescale;
    /* trex is re-parsed with its header, the 8 bytes before its payload
     * (a trex never needs a 64-bit size). */
    if (fmp4_parse_trex(trex - 8, trex_len + 8, &m->trex) != FMP4_OK) {
        free(m);
        return NULL;
    }

    /* A sidx right after moov; otherwise the first fragment is there. */
    uint64_t first = moov_offset + moov_size;
    uint8_t head[16];
    /* Only a box header: reading past it would step back for the moof. */
    size_t avail = file_size - first >= 8 ? 8 : 0;
    if (avail >= 8 && read_at(m, first, head, avail) &&
        fmp4_box_header(head, avail, &type, &hl, &moov_size) == FMP4_OK &&
        type == FMP4_FOURCC('s', 'i', 'd', 'x')) {
        uint64_t sidx_size = moov_size;
        fmp4_status_t st = FMP4_ERR_LIMIT;
        uint8_t * buf = sidx_size <= MP4_FRAG_MAX_SIDX_BYTES ? malloc((size_t)sidx_size) : NULL;
        if (buf) {
            st = read_at(m, first, buf, (size_t)sidx_size)
                     ? fmp4_parse_sidx(buf, (size_t)sidx_size, first, &m->sidx)
                     : FMP4_ERR_TRUNCATED;
            free(buf);
        } else if (sidx_size <= MP4_FRAG_MAX_SIDX_BYTES) {
            st = FMP4_ERR_NOMEM;
        }
        if (st != FMP4_OK) {
            *stage = st == FMP4_ERR_LIMIT ? "limit" : "sidx";
            free(m);
            return NULL;
        }
        first = m->sidx.refs[0].offset;
    }

    /* The first fragment gives the ticks per sample. */
    if (!load_fragment(m, first, 0)) {
        *stage = m->error_stage;
        mp4_frag_close(m);
        return NULL;
    }
    m->sample_ticks = m->cur.var_duration ? m->cur.durations[0] : m->cur.duration;
    m->var_duration = m->cur.var_duration;

    uint64_t total = 0;
    if (m->sidx.count)
        total = to_media(m, m->sidx.total_duration);
    else
        total = audio_media_duration(moov + moov_hl, moov_len - moov_hl);
    if (total) {
        uint64_t count = (total + m->sample_ticks - 1) / m->sample_ticks;
        m->sample_count = count < UINT32_MAX ? (uint32_t)count : UINT32_MAX - 1;
        m->length_known = true;
    } else {
        m->sample_count = UINT32_MAX;
    }
    if (m->sample_count < m->cur.count) {
        *stage = "moof";
        mp4_frag_close(m);
        return NULL;
    }
    return m;
}

void mp4_frag_close(mp4_frag_t * m) {
    if (!m)
        return;
    fmp4_sidx_free(&m->sidx);
    fmp4_frag_free(&m->cur);
    free(m);
}

uint32_t mp4_frag_sample_count(const mp4_frag_t * m) { return m->sample_count; }
bool mp4_frag_length_known(const mp4_frag_t * m) { return m->length_known; }
uint32_t mp4_frag_sample_ticks(const mp4_frag_t * m) { return m->sample_ticks; }
bool mp4_frag_can_seek(const mp4_frag_t * m) { return m->sidx.count && !m->var_duration; }
bool mp4_frag_has_sidx(const mp4_frag_t * m) { return m->sidx.count > 0; }
uint32_t mp4_frag_sidx_refs(const mp4_frag_t * m) { return m->sidx.count; }
bool mp4_frag_var_duration(const mp4_frag_t * m) { return m->var_duration; }
const char * mp4_frag_error_stage(const mp4_frag_t * m) { return m->error_stage; }
const char * mp4_frag_error_detail(const mp4_frag_t * m) { return m->error_detail; }

bool mp4_frag_locate(const mp4_frag_t * m, uint32_t sample, uint32_t * frag, uint64_t * offset) {
    uint64_t start;
    if (!mp4_frag_can_seek(m) ||
        !fmp4_sidx_find(&m->sidx, to_sidx(m, (uint64_t)sample * m->sample_ticks), frag, &start))
        return false;
    *offset = m->sidx.refs[*frag].offset;
    return true;
}

bool mp4_frag_read_sample(mp4_frag_t * m, uint32_t index, uint8_t * buf, uint32_t buf_size,
                          uint32_t * out_size) {
    if (m->failed || index >= m->sample_count)
        return false;
    if (!m->cur_valid || index < m->cur_first || index - m->cur_first >= m->cur.count) {
        uint32_t next = m->cur_first + m->cur.count;
        if (m->cur_valid && index == next) {
            /* In order: the next moof follows this fragment's mdat. At the
             * end of the file the sample count becomes exact. */
            if (m->cur.next_offset >= m->file_size) {
                m->sample_count = index;
                m->length_known = true;
                return false;
            }
            if (!load_fragment(m, m->cur.next_offset, index))
                return false;
        } else {
            uint32_t k;
            uint64_t offset;
            if (!mp4_frag_locate(m, index, &k, &offset))
                return false; /* no sidx: only in order */
            uint64_t start = 0;
            for (uint32_t i = 0; i < k; i++)
                start += m->sidx.refs[i].duration;
            if (!load_fragment(m, offset, (uint32_t)(to_media(m, start) / m->sample_ticks)))
                return false;
            if (m->cur.has_decode_time)
                m->cur_first = (uint32_t)(m->cur.decode_time / m->sample_ticks);
        }
        if (index < m->cur_first || index - m->cur_first >= m->cur.count)
            return fail(m, "moof", "index");
    }
    const fmp4_sample_t * s = &m->cur.samples[index - m->cur_first];
    if (s->size > buf_size)
        return fail(m, "decode", "sample_size");
    if (!read_at(m, m->cur.data_start + s->offset, buf, s->size))
        return fail(m, "moof", "read");
    *out_size = s->size;
    return true;
}
