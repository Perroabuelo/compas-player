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
} stream_resume_result_t;

/* Delay in ms before attempt (1-based): 1, 2, 4, 8 and 8 s, 23 s in total.
 * Returns -1 for attempt 0 or past STREAM_RESUME_MAX_ATTEMPTS. */
int stream_resume_next_delay_ms(unsigned attempt);

typedef struct {
    /* Waits ms; returns false when the stream is being closed. */
    bool (*wait_ms)(void * ctx, unsigned ms);
    stream_resume_result_t (*attempt)(void * ctx, unsigned attempt);
} stream_resume_ops_t;

/* Waits and attempts until an attempt succeeds, the stream is stopped or
 * the attempts run out (then STREAM_RESUME_FAIL). *attempts gets the number
 * of attempts made. */
stream_resume_result_t stream_resume_run(const stream_resume_ops_t * ops, void * ctx, unsigned * attempts);

#endif /* STREAM_RESUME_H */
