#ifndef YT_RESOLVER_PROC_H
#define YT_RESOLVER_PROC_H

/* The hiby-yt resolver as a child process (hiby-yt). One YouTube job at a
 * time in the whole player, shared by plugin.youtube_resolve(),
 * plugin.youtube_search() and the YouTube queue (chain-youtube-queue D2,
 * search-youtube-music D5): a resolution or a search, admitted by
 * yt_job_admit().
 *
 * The resolver is a separate program at a fixed path; its only variable
 * argument is the video id, checked against [A-Za-z0-9_-]{11} before
 * anything starts. A search runs the same program with --search and
 * writes the checked query and token to its stdin (yt_search_input.h), so
 * they never show in its command line. It runs with nice RESOLVER_NICE on
 * a detached worker thread that reads its one-line output, and is killed
 * and reaped when it has not finished after YT_RESOLVER_TIMEOUT_MS
 * (YT_SEARCH_PROC_TIMEOUT_MS for a search) or when it is cancelled. Every
 * resolution leaves an event=resolve line in the stream log, with the
 * owner as stage (plugin, open, prefetch or refresh) and cache=miss; a
 * prefetch cancelled by a search adds reason=search. Every search leaves
 * an event=search line with its class, time and exit, never the query.
 *
 * A content page (youtube-music-browse D4) runs the program with --browse
 * and its checked request on stdin (yt_browse_input.h), for the user
 * (YT_OWNER_BROWSE) or for the radio (YT_OWNER_RADIO, kind related). It
 * leaves an event=browse line with its kind, class, time and exit, never an
 * identifier. A prefetch or a radio stopped by a search or a page is
 * preempted (reason=search or reason=browse in the log) and not counted as
 * a try. */

#include "yt_browse_input.h"
#include "yt_resolve_policy.h"
#include "yt_search_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HOST_BUILD
#define YT_RESOLVER_PATH "./hiby-yt/hiby-yt-resolve"
#else
#define YT_RESOLVER_PATH "/usr/data/hiby-yt/hiby-yt-resolve"
#endif
#define YT_RESOLVER_TIMEOUT_MS 90000
/* The resolver prints one line of about 1-2 KiB. */
#define YT_RESOLVER_OUTPUT_MAX 8192
#define YT_RESOLVER_VIDEO_ID_LEN 11
/* How often a waiting request checks its cancel callback. */
#define YT_RESOLVER_POLL_MS 100
/* A search (spec youtube/search-play): its time limit, and its output line,
 * which the program bounds to 68 KiB (search-youtube-music D2). */
#define YT_SEARCH_PROC_TIMEOUT_MS 45000
#define YT_SEARCH_PROC_OUTPUT_MAX (68 * 1024)
/* A content page (spec youtube/content-browse): the same time limit, and
 * its output line, which the program bounds to 256 KiB
 * (youtube-music-browse D3). */
#define YT_BROWSE_PROC_TIMEOUT_MS 45000
#define YT_BROWSE_PROC_OUTPUT_MAX (256 * 1024)

typedef struct {
    /* Set when there is no usable output to decode (fixed strings). */
    const char * fail_class;
    const char * fail_detail;
    bool cancelled;
    /* Cancelled to make way for a search: the prefetch policy does not
     * count it as a try (yt_prefetch_uncount()). */
    bool preempted;
    /* The class written to the log: "ok", the resolver's class,
     * fail_class or "cancelled". */
    char cls[24];
    char output[YT_RESOLVER_OUTPUT_MAX + 1];
    size_t output_len;
} yt_resolver_result_t;

/* Called on the worker thread when a resolution ends, cancelled or not.
 * result is valid only during the call. The service is still busy during
 * the call and idle right after it. */
typedef void (*yt_resolver_done_fn)(const yt_resolver_result_t * result, void * arg);

/* True for an 11-character [A-Za-z0-9_-] id. */
bool yt_resolver_video_id_valid(const char * id);

/* Starts resolving video_id for owner without waiting (the plugin and the
 * prefetch). Returns its generation (never 0), or 0 when the id is
 * invalid, a resolution is running or the thread could not start.
 * Thread-safe. */
uint16_t yt_resolver_start(yt_owner_t owner, const char * video_id, yt_resolver_done_fn done, void * arg);

typedef enum {
    YT_RESOLVER_DONE,      /* *out has the result (which may be a failure) */
    YT_RESOLVER_CANCELLED, /* cancelled() said so; *out is untouched */
    YT_RESOLVER_REFUSED,   /* the policy said busy or skip; *out is untouched */
} yt_resolver_outcome_t;

/* Asks for video_id on behalf of owner (open or refresh) and waits for the
 * result, following yt_resolve_decide(): it starts a resolution, joins the
 * running one for the same track, waits for another one to end, or
 * cancels it first. cancelled(arg), when not NULL, is polled every
 * YT_RESOLVER_POLL_MS while waiting; when it returns true the call returns
 * YT_RESOLVER_CANCELLED, and a resolution this call started is cancelled
 * (the worker still kills and reaps the resolver). retry marks the log
 * line of a second try. Blocks the calling thread: never from the UI
 * thread. */
yt_resolver_outcome_t yt_resolver_request(yt_owner_t owner, const char * video_id, bool retry,
                                          bool (*cancelled)(void * arg), void * arg, yt_resolver_result_t * out);

/* Asks the resolution with this generation to stop (it is killed and
 * reaped; done still runs with cancelled set). Returns whether it was
 * running and not already cancelled. Thread-safe. */
bool yt_resolver_cancel(uint16_t generation);

/* Blocks until the resolution with this generation is no longer running. */
void yt_resolver_wait_ended(uint16_t generation);

/* True while a resolution or a search runs (or waits to start). */
bool yt_resolver_busy(void);

/* The owner of the job that runs or waits to start, YT_OWNER_NONE when
 * the service is free («Iniciar radio» checks for a search or a page). */
yt_owner_t yt_resolver_running_owner(void);

typedef struct {
    /* Set when there is no usable output to decode (fixed strings). */
    const char * fail_class;
    const char * fail_detail;
    bool cancelled;
    /* A radio request cancelled to make way for a search, a page, or the
     * open of a track: it is asked again and not counted as a try. */
    bool preempted;
    /* The class written to the log, as for a resolution. */
    char cls[24];
    /* The program's line (NUL-terminated), valid during the done call. */
    const char * output;
    size_t output_len;
} yt_search_result_t;

/* Called on a worker thread when a search ends, cancelled or not. The
 * service is still busy during the call and idle right after it. */
typedef void (*yt_search_done_fn)(const yt_search_result_t * result, void * arg);

/* Starts a search for query (and token, NULL or "" for page 1) without
 * waiting, under yt_job_admit(): it starts at once, or cancels a running
 * prefetch and starts as soon as that has been reaped. Returns its
 * generation (never 0; yt_resolver_cancel() and yt_resolver_wait_ended()
 * take it), or 0 with *why set to "bad_input" (query or token invalid:
 * nothing starts), "busy" or "internal". Thread-safe; never blocks on the
 * child. */
uint16_t yt_resolver_start_search(const char * query, const char * token, yt_search_done_fn done, void * arg,
                                  const char ** why);

/* A content page's result: the same as a search's, with the program's
 * --browse line as output. */
typedef yt_search_result_t yt_browse_result_t;
typedef void (*yt_browse_done_fn)(const yt_browse_result_t * result, void * arg);

/* Starts a content request of kind for owner (YT_OWNER_BROWSE, or
 * YT_OWNER_RADIO for related) without waiting, under yt_job_admit(): a
 * page the user asks for starts at once, or cancels a running prefetch or
 * radio and starts as soon as that has been reaped; the radio starts only
 * when nothing runs. id, params and token (NULL or "" for none) are what
 * kind takes (yt_browse_fields_valid()). Returns its generation, or 0 with
 * *why set to "bad_input" (nothing starts), "busy" (for the radio: try
 * again later) or "internal". Thread-safe; never blocks on the child. */
uint16_t yt_resolver_start_browse(yt_owner_t owner, yt_browse_kind_t kind, const char * id, const char * params,
                                  const char * token, yt_browse_done_fn done, void * arg, const char ** why);

#ifdef HOST_BUILD
/* PC tests only: another resolver program and time limit (NULL/0: default). */
void yt_resolver_set_test_overrides(const char * resolver_path, int timeout_ms);
/* PC tests only: the search time limit (0: default). */
void yt_resolver_set_search_test_timeout(int timeout_ms);
/* PC tests only: the content time limit (0: default). */
void yt_resolver_set_browse_test_timeout(int timeout_ms);
#endif

#endif /* YT_RESOLVER_PROC_H */
