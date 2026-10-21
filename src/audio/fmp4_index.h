#ifndef FMP4_INDEX_H
#define FMP4_INDEX_H

/* Fragmented MP4 (single AAC track) index parsing for hiby-yt. Pure: works
 * on buffers the caller already read, never does I/O, and checks every
 * declared size against fixed limits before allocating. Offsets are
 * absolute file offsets; `offset` arguments give where a buffer starts. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FMP4_MAX_SIDX_REFS 8192
#define FMP4_MAX_FRAG_SAMPLES 16384
#define FMP4_MAX_MOOF_BYTES (256 * 1024)

#define FMP4_FOURCC(a, b, c, d) \
    ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

typedef enum {
    FMP4_OK = 0,
    FMP4_ERR_TRUNCATED,   /* the buffer ends before the box does */
    FMP4_ERR_INVALID,     /* inconsistent sizes, offsets or fields */
    FMP4_ERR_LIMIT,       /* a declared count or size is over its limit */
    FMP4_ERR_UNSUPPORTED, /* valid MP4 this player does not handle */
    FMP4_ERR_NOMEM,
} fmp4_status_t;

const char * fmp4_status_str(fmp4_status_t status);

/* Box header at buf: type, header length (8 or 16) and total box size.
 * Size 0 ("to the end of the file") and sizes below the header are
 * FMP4_ERR_INVALID. Only the header needs to be in buf. */
fmp4_status_t fmp4_box_header(const uint8_t * buf, size_t len, uint32_t * type,
                              uint32_t * header_len, uint64_t * size);

/* One sidx reference: a fragment (moof + mdat). 16 bytes. */
typedef struct {
    uint64_t offset; /* absolute offset of the fragment's first byte */
    uint32_t size;
    uint32_t duration; /* in the sidx timescale */
} fmp4_sidx_ref_t;

typedef struct {
    uint32_t timescale;
    uint64_t earliest_pts;
    uint64_t total_duration; /* sum of the reference durations */
    uint32_t count;
    fmp4_sidx_ref_t * refs; /* malloc'd; fmp4_sidx_free() */
} fmp4_sidx_t;

/* Parses the whole sidx box at buf (len bytes available, starting at file
 * offset `offset`). Only references to media (type 0) are supported. On
 * failure *out is left empty. */
fmp4_status_t fmp4_parse_sidx(const uint8_t * buf, size_t len, uint64_t offset,
                              fmp4_sidx_t * out);
void fmp4_sidx_free(fmp4_sidx_t * sidx);

/* Fragment containing time t, in sidx timescale units counted from the
 * start of the first fragment. False when t is at or past the end. */
bool fmp4_sidx_find(const fmp4_sidx_t * sidx, uint64_t t, uint32_t * index,
                    uint64_t * frag_start);

/* Per-track defaults from moov/mvex/trex. */
typedef struct {
    uint32_t track_id;
    uint32_t default_duration;
    uint32_t default_size;
    uint32_t default_flags;
} fmp4_trex_t;

/* Parses the whole trex box at buf. */
fmp4_status_t fmp4_parse_trex(const uint8_t * buf, size_t len, fmp4_trex_t * out);

/* One sample of a fragment: 8 bytes. */
typedef struct {
    uint32_t offset; /* from data_start */
    uint32_t size;
} fmp4_sample_t;

/* The sample table of one fragment (moof + its mdat). */
typedef struct {
    uint64_t decode_time; /* tfdt, in the track timescale; 0 without tfdt */
    bool has_decode_time;
    uint64_t data_start;  /* absolute offset of the mdat payload */
    uint64_t next_offset; /* first byte after the mdat: the next fragment */
    uint32_t count;
    fmp4_sample_t * samples; /* malloc'd; fmp4_frag_free() */
    uint32_t duration;       /* of every sample, unless var_duration */
    bool var_duration;
    uint32_t * durations; /* per sample, only with var_duration */
    uint64_t total_duration;
} fmp4_frag_t;

/* Parses the moof at buf and the header of the mdat that must follow it.
 * buf holds len bytes from file offset `offset`: the whole moof plus at
 * least the mdat header (reading moof size + 16 bytes is enough). Sample
 * values come from trun, else tfhd, else trex. Only one traf is supported,
 * and every sample must lie inside the mdat. On failure *out is left empty. */
fmp4_status_t fmp4_parse_moof(const uint8_t * buf, size_t len, uint64_t offset,
                              const fmp4_trex_t * trex, fmp4_frag_t * out);
void fmp4_frag_free(fmp4_frag_t * frag);

#endif /* FMP4_INDEX_H */
