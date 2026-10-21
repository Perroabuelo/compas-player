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

#endif /* FMP4_INDEX_H */
