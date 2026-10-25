#ifndef YT_RADIO_POLICY_H
#define YT_RADIO_POLICY_H

/* When the radio asks for related songs (hiby-yt, spec youtube/radio-queue,
 * design D5 and D7 of youtube-music-browse). Pure: the caller polls it from
 * the prefetch timer with the track that plays, the same way as
 * yt_prefetch_policy.h.
 *
 * The radio asks while a YouTube track plays that has no next track in the
 * play order (with repeat all there always is one, so it never asks), when
 * the radio is on and the queue has fewer than YT_RADIO_QUEUE_MAX tracks:
 * once the track has played YT_RADIO_AFTER_MS, or as soon as it has played
 * at all when YT_RADIO_BEFORE_END_MS or less remain. It asks at most
 * YT_RADIO_MAX_STARTS times per track: a request that fails is tried once
 * more near the end; one that answers ends the radio for that track. A
 * request cancelled to make way for the user or the track that opens
 * (preempted) does not count. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YT_RADIO_AFTER_MS 10000
#define YT_RADIO_BEFORE_END_MS 30000
#define YT_RADIO_MAX_STARTS 2
#define YT_RADIO_QUEUE_MAX 200
#define YT_RADIO_PER_REQUEST 25

typedef struct {
    char video_id[12]; /* the track these starts were for */
    unsigned starts;
} yt_radio_state_t;

typedef struct {
    /* Video id of the track that plays, or NULL/"" when it is not a
     * YouTube track. */
    const char * current_video_id;
    bool has_next;     /* the play order has a next track (repeat all: always) */
    bool enabled;      /* the radio switch */
    size_t queue_count;
    bool service_busy; /* a YouTube job runs */
    /* Since the track's first audio; negative before it. */
    int64_t played_ms;
    /* Until the track's end; negative when unknown. */
    int64_t remaining_ms;
} yt_radio_input_t;

/* Starts over (no track known). */
void yt_radio_reset(yt_radio_state_t * st);

/* True when a related request for in->current_video_id should start now;
 * the start is counted. Another track than the last call starts over. */
bool yt_radio_should_start(yt_radio_state_t * st, const yt_radio_input_t * in);

/* The request for video_id ended. answered: it brought a page (even with no
 * new song), so the radio asks no more for this track; otherwise a second
 * start may follow. */
void yt_radio_finished(yt_radio_state_t * st, const char * video_id, bool answered);

/* The request for video_id was preempted: it does not count as a start. */
void yt_radio_uncount(yt_radio_state_t * st, const char * video_id);

#endif /* YT_RADIO_POLICY_H */
