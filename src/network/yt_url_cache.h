#ifndef YT_URL_CACHE_H
#define YT_URL_CACHE_H

/* Resolved YouTube stream URLs kept for a short while (hiby-yt). Pure: no
 * locking and no clock reads -- the caller passes CLOCK_BOOTTIME in ms and
 * the wall clock in s, so tests run it against injected time.
 *
 * Two slots at most: the track that plays and the next one. A URL from the
 * resolver carries an absolute expire (unix seconds, from the server); its
 * time to live is taken against the wall clock once, when it arrives, and
 * from then on measured with the boot clock, which keeps counting while
 * the R1 sleeps and does not jump when the wall clock is set. A wall clock
 * so wrong that the time to live falls outside (0, 7 h] is ignored in
 * favor of the measured 6 h. A slot that is dropped or expires is wiped,
 * so the URL does not stay in memory. Nothing is ever written to disk. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YT_URL_CACHE_SLOTS 2
#define YT_URL_CACHE_VIDEO_ID_MAX 11
#define YT_URL_CACHE_URL_MAX 2048 /* matches remote_track_meta_t.stream_url */
/* A URL counts as expired this long before its deadline. */
#define YT_URL_CACHE_MARGIN_S 60
/* Time to live measured on the R1 (docs/mediciones/2026-10-22-caducidad-url.md). */
#define YT_URL_CACHE_FALLBACK_TTL_S 21600
#define YT_URL_CACHE_MAX_TTL_S (7 * 3600)

typedef struct {
    bool used;
    uint32_t seq; /* put order, to replace the oldest when full */
    char video_id[YT_URL_CACHE_VIDEO_ID_MAX + 1];
    char url[YT_URL_CACHE_URL_MAX];
    uint64_t content_length;
    int64_t deadline_ms; /* boot clock */
} yt_url_slot_t;

typedef struct {
    yt_url_slot_t slot[YT_URL_CACHE_SLOTS];
    uint32_t next_seq;
} yt_url_cache_t;

/* Wipes every slot. */
void yt_url_cache_clear(yt_url_cache_t * c);

/* Time to live in s of a URL that expires at expire (unix s), taken at
 * wall_now (unix s): expire - wall_now, or YT_URL_CACHE_FALLBACK_TTL_S when
 * that is not in (0, YT_URL_CACHE_MAX_TTL_S]. */
int64_t yt_url_cache_ttl_s(int64_t expire, int64_t wall_now);

/* Stores the URL for video_id, replacing that video's slot, else a free
 * one, else the oldest. Returns false (cache unchanged) when video_id or
 * url is empty or does not fit. */
bool yt_url_cache_put(yt_url_cache_t * c, const char * video_id, const char * url, uint64_t content_length,
                      int64_t expire, int64_t wall_now, int64_t boot_now_ms);

/* Copies the URL of video_id into out (and its length into *content_length
 * when not NULL) if it is still valid at boot_now_ms. An expired slot is
 * wiped. Returns false, leaving out untouched, when there is none. */
bool yt_url_cache_get(yt_url_cache_t * c, const char * video_id, int64_t boot_now_ms, char * out, size_t out_size,
                      uint64_t * content_length);

/* True when video_id has a URL still valid at boot_now_ms (an expired one
 * is wiped). */
bool yt_url_cache_has(yt_url_cache_t * c, const char * video_id, int64_t boot_now_ms);

/* Keeps only the slots of current and next (either may be NULL or empty)
 * and wipes the rest. */
void yt_url_cache_keep(yt_url_cache_t * c, const char * current, const char * next);

/* Wipes the slot of video_id, if any. */
void yt_url_cache_drop(yt_url_cache_t * c, const char * video_id);

#endif /* YT_URL_CACHE_H */
