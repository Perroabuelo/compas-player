#ifndef YT_QUEUE_H
#define YT_QUEUE_H

/* Stream URLs for the YouTube tracks of the play queue (hiby-yt,
 * chain-youtube-queue). Ties together the URL cache (yt_url_cache), the
 * resolver service (yt_resolver_proc) and its rules (yt_resolve_policy).
 * Thread-safe: the audio thread opens tracks, the stream pump refreshes
 * URLs, and the UI thread prefetches and reports track changes. URLs only
 * ever live in memory. */

#include "yt_url_cache.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char url[YT_URL_CACHE_URL_MAX];
    uint64_t content_length;
    int64_t deadline_ms; /* boot clock (yt_queue_boot_ms); 0: unknown */
    char cls[24];        /* "ok", or the class of the failure */
    bool cache_hit;      /* the URL came from the cache, not a resolution */
} yt_queue_url_t;

typedef enum {
    YT_QUEUE_URL_OK,
    YT_QUEUE_URL_FAILED,    /* out->cls says why */
    YT_QUEUE_URL_CANCELLED, /* cancelled() said so */
} yt_queue_status_t;

/* The URL to open video_id with: a cached one still valid (logged as
 * cache=hit), else a resolution; one that fails with po_token_required or
 * network is tried once more. Blocks; never call it from the UI thread. */
yt_queue_status_t yt_queue_url_for_open(const char * video_id, bool (*cancelled)(void * arg), void * arg,
                                        yt_queue_url_t * out);

/* Like yt_queue_url_for_open, after a cached URL for video_id could not be
 * opened: that URL is dropped and the track is resolved again. */
yt_queue_status_t yt_queue_url_reopen(const char * video_id, bool (*cancelled)(void * arg), void * arg,
                                      yt_queue_url_t * out);

/* A new URL for video_id, never the cached one (the stream's got 403 or
 * expired). One try. Blocks; never call it from the UI thread. */
yt_queue_status_t yt_queue_url_refresh(const char * video_id, bool (*cancelled)(void * arg), void * arg,
                                       yt_queue_url_t * out);

/* Starts resolving video_id in the background into the cache, unless a
 * resolution is running. Returns whether it started. */
bool yt_queue_prefetch(const char * video_id);

/* When a search cancelled a prefetch since the last call, copies its video
 * id into video_id (size bytes) and returns true, once: the prefetch
 * policy then does not count that try (yt_prefetch_uncount()). */
bool yt_queue_take_preempted(char * video_id, size_t size);

/* True when video_id has a cached URL still valid. */
bool yt_queue_has_url(const char * video_id);

/* A track started: keeps only video_id's URL (NULL or "": none). */
void yt_queue_track_started(const char * video_id);

/* A new queue: forgets every URL. */
void yt_queue_clear(void);

/* CLOCK_BOOTTIME in ms. */
int64_t yt_queue_boot_ms(void);

#ifdef HOST_BUILD
/* PC tests only: a fixed boot clock (negative: the real one). */
void yt_queue_set_test_clock(int64_t boot_ms);
#endif

#endif /* YT_QUEUE_H */
