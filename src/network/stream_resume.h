#ifndef STREAM_RESUME_H
#define STREAM_RESUME_H

/* Retry policy for resuming a network stream after a premature end
 * (hiby-yt). Pure: the waiting and the reopening are supplied by the
 * caller, so tests run it against an injected clock. */

#include <stdbool.h>

#define STREAM_RESUME_MAX_ATTEMPTS 5

typedef enum {
    STREAM_RESUME_OK,
    STREAM_RESUME_FAIL,      /* attempt failed: no connection or bad status */
    STREAM_RESUME_BAD_RANGE, /* attempt failed: response not resumable */
    STREAM_RESUME_STOPPED,   /* the stream is being closed */
    STREAM_RESUME_FORBIDDEN, /* attempt failed with 403: the URL may have expired */
    STREAM_RESUME_LENGTH_CHANGED, /* refresh: the new URL is for another entity */
} stream_resume_result_t;

/* Delay in ms before attempt (1-based): 1, 2, 4, 8 and 8 s, 23 s in total.
 * Returns -1 for attempt 0 or past STREAM_RESUME_MAX_ATTEMPTS. */
int stream_resume_next_delay_ms(unsigned attempt);

typedef struct {
    /* Waits ms; returns false when the stream is being closed. */
    bool (*wait_ms)(void * ctx, unsigned ms);
    stream_resume_result_t (*attempt)(void * ctx, unsigned attempt);
    /* Optional (a YouTube track, chain-youtube-queue D5): whether the URL
     * in use has expired, and getting a new one for the same track. refresh
     * returns OK (the next attempts use the new URL), FAIL, STOPPED or
     * LENGTH_CHANGED (the new URL is not the same entity). */
    bool (*url_expired)(void * ctx);
    stream_resume_result_t (*refresh)(void * ctx);
} stream_resume_ops_t;

/* Waits and attempts until an attempt succeeds, the stream is stopped or
 * the attempts run out (then STREAM_RESUME_FAIL). *attempts gets the number
 * of attempts made.
 *
 * With refresh set, a new URL is asked for at most once per cut, after the
 * wait and before an attempt: when url_expired says so, or when the
 * previous attempt ended in STREAM_RESUME_FORBIDDEN. A refresh that fails
 * stands for that attempt (nothing is requested); one that reports
 * LENGTH_CHANGED ends the run with that result. */
stream_resume_result_t stream_resume_run(const stream_resume_ops_t * ops, void * ctx, unsigned * attempts);

#endif /* STREAM_RESUME_H */
