#ifndef MP4_FRAG_H
#define MP4_FRAG_H

/* Fragmented MP4 sample access for mp4_demux (hiby-yt design D3). Keeps the
 * sidx index and one fragment's sample table; the next fragment is read
 * when playback reaches it, and a jump uses sidx to load its fragment. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct mp4_frag mp4_frag_t;

/* moov holds the whole moov box, read from file offset moov_offset; f is
 * not owned. Reads the sidx right after moov (if any) and the first
 * fragment. NULL on failure, with *stage set to "moov", "sidx", "moof" or
 * "limit" when stage is not NULL. */
mp4_frag_t * mp4_frag_open(FILE * f, uint64_t file_size, const uint8_t * moov, size_t moov_len,
                           uint64_t moov_offset, uint32_t media_timescale, const char ** stage);
void mp4_frag_close(mp4_frag_t * m);

/* Sample count; with no sidx and no declared duration it is UINT32_MAX
 * until the end of the file sets it (mp4_frag_length_known() then turns
 * true). */
uint32_t mp4_frag_sample_count(const mp4_frag_t * m);
bool mp4_frag_length_known(const mp4_frag_t * m);
/* Media-timescale ticks per sample (1024 for AAC-LC at its sample rate). */
uint32_t mp4_frag_sample_ticks(const mp4_frag_t * m);

/* Seeking needs sidx and every sample lasting sample_ticks. */
bool mp4_frag_can_seek(const mp4_frag_t * m);
bool mp4_frag_has_sidx(const mp4_frag_t * m);
uint32_t mp4_frag_sidx_refs(const mp4_frag_t * m);
/* True once a fragment with other sample durations was found. */
bool mp4_frag_var_duration(const mp4_frag_t * m);
/* Fragment index and file offset holding sample (sidx only). */
bool mp4_frag_locate(const mp4_frag_t * m, uint32_t sample, uint32_t * frag, uint64_t * offset);
/* Why the last fragment failed to load ("moof", "limit"), or NULL. */
const char * mp4_frag_error_stage(const mp4_frag_t * m);
const char * mp4_frag_error_detail(const mp4_frag_t * m);

bool mp4_frag_read_sample(mp4_frag_t * m, uint32_t index, uint8_t * buf, uint32_t buf_size,
                          uint32_t * out_size);

#endif /* MP4_FRAG_H */
