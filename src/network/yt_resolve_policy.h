#ifndef YT_RESOLVE_POLICY_H
#define YT_RESOLVE_POLICY_H

/* Rules shared by everything that runs the YouTube resolver (hiby-yt).
 * Pure: the resolver service asks these and acts on the answer.
 *
 * Only one resolver runs at a time in the whole player. A request finds
 * the running one, if any, and gets an action:
 *
 *   request \ running | none  | plugin | open/refresh, same id | open, other id  | refresh, other id | prefetch, same id | prefetch, other id
 *   plugin            | start | busy   | busy                  | busy            | busy              | busy              | busy
 *   open              | start | wait   | join                  | cancel + start  | wait              | join              | cancel + start
 *   refresh           | start | wait   | join                  | wait            | wait              | join              | cancel + start
 *   prefetch          | start | skip   | skip                  | skip            | skip              | skip              | skip
 *
 * An open of another track replaces a running open (the user moved on),
 * but waits for a refresh: that one belongs to the track that still plays
 * (a gapless or crossfade open of the next track). A refresh is urgent and
 * replaces a prefetch of another track.
 *
 * An open that fails with po_token_required or network is tried once more
 * before the track is skipped: googlevideo can answer 403 once to a fresh
 * URL (docs/mediciones/2026-10-22-caducidad-url.md). After
 * YT_RESOLVE_MAX_FAILED_TRACKS tracks in a row fail to resolve, playback
 * stops; any track that reaches its first audio clears the count. */

#include <stdbool.h>

#define YT_RESOLVE_MAX_FAILED_TRACKS 3
#define YT_RESOLVE_OPEN_TRIES 2

typedef enum {
    YT_OWNER_NONE,
    YT_OWNER_PLUGIN,
    YT_OWNER_OPEN,
    YT_OWNER_PREFETCH,
    YT_OWNER_REFRESH,
    YT_OWNER_SEARCH, /* a YouTube Music search (yt_job_admit.h) */
} yt_owner_t;

typedef enum {
    YT_REQ_START,            /* nothing runs: start it */
    YT_REQ_JOIN,             /* the running one resolves the same track: wait for its result */
    YT_REQ_CANCEL_AND_START, /* stop the running one, then start */
    YT_REQ_WAIT,             /* wait until the running one ends, then ask again */
    YT_REQ_BUSY,             /* refuse: the plugin gets "busy" */
    YT_REQ_SKIP,             /* do not start now (a prefetch is asked again later) */
} yt_req_action_t;

/* True for an 11-character [A-Za-z0-9_-] video id. */
bool yt_video_id_valid(const char * id);

yt_req_action_t yt_resolve_decide(yt_owner_t running, const char * running_id, yt_owner_t request,
                                  const char * request_id);

/* True when an open that failed with cls on try number tries_made (1-based)
 * should be tried again. */
bool yt_resolve_should_retry(const char * cls, unsigned tries_made);

typedef struct {
    unsigned failed_tracks;
} yt_failure_count_t;

/* A track failed to resolve: true when playback should stop. */
bool yt_failure_count_add(yt_failure_count_t * fc);

/* A track reached its first audio. */
void yt_failure_count_clear(yt_failure_count_t * fc);

#endif /* YT_RESOLVE_POLICY_H */
