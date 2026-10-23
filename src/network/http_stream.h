#ifndef HTTP_STREAM_H
#define HTTP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct http_stream http_stream_t;

/* Opens an HTTP(S) stream. A byte-range probe is used to identify finite,
 * seekable responses; live and ordinary non-range responses remain readable. */
http_stream_t * http_stream_open(const char * url, bool verify_tls);
size_t http_stream_read(http_stream_t * s, void * buf, size_t n);
const char * http_stream_content_type(http_stream_t * s);

/* Finite HTTP byte-range streams support stdio SEEK_SET/CUR/END. Length and
 * position are byte offsets; length returns -1 when the entity is not known
 * to be finite. A failed seek leaves the current stream position unchanged. */
bool http_stream_can_seek(http_stream_t * s);
int64_t http_stream_tell(http_stream_t * s);
int64_t http_stream_length(http_stream_t * s);
bool http_stream_seek(http_stream_t * s, int64_t offset, int origin);

/* hiby-yt: a finite, seekable stream whose connection ends early is reopened
 * at its first missing byte (stream_resume.h policy). Each open gets a
 * stream_log id; reconnects counts completed resumes; failed is true once
 * the stream ended because every resume attempt failed. */
unsigned http_stream_log_sid(http_stream_t * s);
unsigned http_stream_reconnects(http_stream_t * s);
bool http_stream_failed(http_stream_t * s);
/* Host of the final (post-redirect) URL, for stream_log; never the URL. */
void http_stream_log_host(http_stream_t * s, char * out, size_t size);
/* Bytes received from the network so far, across seeks and resumes. */
uint64_t http_stream_bytes_received(http_stream_t * s);

/* hiby-yt (chain-youtube-queue D5): a way to get a new URL for the same
 * entity, for a stream whose URL expires (a YouTube track). When a cut is
 * resumed and expired() says so, or a resume attempt gets 403, the pump
 * calls refresh() once for that cut and goes on with the new URL, if its
 * total length is the stream's; otherwise the stream ends with
 * error stage=reconnect detail=length_changed. */
typedef struct {
    bool (*expired)(void * ctx); /* may be NULL */
    /* Writes the new URL and its total length. cancelled(arg) turns true
     * when the stream is closing. Returns 0 on success, 1 when cancelled,
     * -1 when it failed. */
    int (*refresh)(void * ctx, bool (*cancelled)(void * arg), void * arg, char * url, size_t url_size,
                   uint64_t * total_length);
    void (*free_ctx)(void * ctx); /* may be NULL */
    void * ctx;
} http_stream_refresher_t;

/* Gives the stream a refresher; the stream owns r->ctx from then on and
 * frees it with free_ctx when it closes. */
void http_stream_set_refresher(http_stream_t * s, const http_stream_refresher_t * r);

/* The caller owns and serializes read/seek/close operations. Close stops the
 * internal pump; it must not race a caller that is reading or seeking. */
void http_stream_close(http_stream_t * s);

#ifdef HTTP_STREAM_TESTING
/* Deterministically fail the next stream-pump pthread_create in host tests. */
void http_stream_test_fail_next_pump_create(void);
/* Divides resume delays, so tests do not wait the real 23 s. */
void http_stream_test_set_resume_delay_divisor(unsigned divisor);
#endif

#endif
