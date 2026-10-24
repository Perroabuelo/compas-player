#ifndef YT_PREFETCH_POLICY_H
#define YT_PREFETCH_POLICY_H

/* When to resolve the next YouTube track while the current one plays
 * (hiby-yt). Pure: the caller polls it from a UI timer with the playback
 * position and the cache state, and starts a prefetch when it says so.
 *
 * A prefetch starts once the current track has played
 * YT_PREFETCH_AFTER_MS since its first audio (so skipping through tracks
 * quickly does not resolve each next one), or as soon as it has played at
 * all when YT_PREFETCH_BEFORE_END_MS or less remain (short tracks). A
 * failed prefetch is retried once, when YT_PREFETCH_BEFORE_END_MS or less
 * remain; after that the track is resolved when it opens. */

#include <stdbool.h>
#include <stdint.h>

#define YT_PREFETCH_AFTER_MS 10000
#define YT_PREFETCH_BEFORE_END_MS 30000
#define YT_PREFETCH_MAX_STARTS 2

typedef struct {
    char video_id[12]; /* next track these starts were for */
    unsigned starts;
} yt_prefetch_state_t;

typedef struct {
    /* Video id of the track that would play next, or NULL/"" when there
     * is none or it is not a YouTube track. */
    const char * next_video_id;
    bool next_cached;      /* it already has a valid URL */
    bool service_busy;     /* a resolution is running */
    /* Since the current track's first audio; negative before it. */
    int64_t played_ms;
    /* Until the current track's end; negative when unknown. */
    int64_t remaining_ms;
} yt_prefetch_input_t;

/* Starts over (no next track known). */
void yt_prefetch_reset(yt_prefetch_state_t * st);

/* True when a prefetch of in->next_video_id should start now; the start is
 * counted. A different next track than the last call starts over. */
bool yt_prefetch_should_start(yt_prefetch_state_t * st, const yt_prefetch_input_t * in);

/* A prefetch of video_id was cancelled to make way for a user's search
 * (search-youtube-music D5): it does not count as a start, so it starts
 * again under the same conditions once the service is free. */
void yt_prefetch_uncount(yt_prefetch_state_t * st, const char * video_id);

#endif /* YT_PREFETCH_POLICY_H */
