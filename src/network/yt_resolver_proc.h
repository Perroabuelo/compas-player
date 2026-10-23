#ifndef YT_RESOLVER_PROC_H
#define YT_RESOLVER_PROC_H

/* The hiby-yt resolver as a child process (hiby-yt). One resolution at a
 * time in the whole player, shared by plugin.youtube_resolve() and the
 * YouTube queue (chain-youtube-queue D2).
 *
 * The resolver is a separate program at a fixed path; its only variable
 * argument is the video id, checked against [A-Za-z0-9_-]{11} before
 * anything starts. It runs with nice RESOLVER_NICE on a detached worker
 * thread that reads its one-line output, and is killed and reaped when it
 * has not finished after YT_RESOLVER_TIMEOUT_MS or when it is cancelled.
 * Every resolution leaves an event=resolve line in the stream log. */

#include "yt_resolve_policy.h"

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

typedef struct {
    /* Set when there is no usable output to decode (fixed strings). */
    const char * fail_class;
    const char * fail_detail;
    bool cancelled;
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

/* Starts resolving video_id for owner. Returns its generation (never 0),
 * or 0 when the id is invalid, a resolution is running or the thread could
 * not start. Thread-safe. */
uint16_t yt_resolver_start(yt_owner_t owner, const char * video_id, yt_resolver_done_fn done, void * arg);

/* Asks the resolution with this generation to stop (it is killed and
 * reaped; done still runs with cancelled set). Returns whether it was
 * running and not already cancelled. Thread-safe. */
bool yt_resolver_cancel(uint16_t generation);

/* Blocks until the resolution with this generation is no longer running. */
void yt_resolver_wait_ended(uint16_t generation);

/* True while a resolution runs. */
bool yt_resolver_busy(void);

#ifdef HOST_BUILD
/* PC tests only: another resolver program and time limit (NULL/0: default). */
void yt_resolver_set_test_overrides(const char * resolver_path, int timeout_ms);
#endif

#endif /* YT_RESOLVER_PROC_H */
