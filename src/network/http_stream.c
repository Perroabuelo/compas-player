#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "http_stream.h"
#include "debug_log.h"
#include "http_conn.h"
#include "stream_log.h"
#include "stream_resume.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define STREAM_RING_CAPACITY (256 * 1024)
#define STREAM_MAX_REDIRECTS 5
/* hiby-yt: bound connect and read so a dropped Wi-Fi ends a blocked recv()
 * and the pump can resume the stream (0 would wait forever). */
#define STREAM_IO_TIMEOUT_MS 15000

typedef struct {
    http_conn_t * conn;
    http_conn_reader_t reader;
    bool chunked;
    bool has_length;
    uint64_t content_length;
    bool range_valid;
    uint64_t range_start, range_end, range_total;
    int status;
    char content_type[128];
    char etag[128], last_modified[64];
    char final_url[2048];
} response_t;

struct http_stream {
    http_conn_t * conn;
    http_conn_reader_t reader;
    bool chunked, has_content_length, seekable;
    uint64_t content_length, remaining;
    uint64_t position, length;
    char content_type[128];
    char etag[128], last_modified[64];
    char final_url[2048];
    bool verify_tls;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    uint8_t * ring;
    size_t ring_head, ring_tail, ring_fill;
    /* Bytes already read that still sit intact just behind ring_tail. A short
     * rewind (a container probe, a parser re-reading a header) reuses them
     * instead of opening a new connection. */
    size_t ring_history;
    /* SEEK_END to the entity length, as parsers do to learn its size. The
     * connection and buffered bytes stay, so seeking back costs nothing. */
    bool at_virtual_end;
    bool stop_requested, ended, thread_started;
    /* hiby-yt: stream_log id, completed resumes, and whether the stream
     * ended because resuming it failed. */
    unsigned log_sid;
    unsigned reconnects;
    bool failed;
    uint64_t received; /* bytes from the network into the ring */
};

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool released;
    http_stream_t * stream;
} pump_gate_t;

#ifdef HTTP_STREAM_TESTING
static bool fail_next_pump_create;
void http_stream_test_fail_next_pump_create(void) {
    fail_next_pump_create = true;
}
static unsigned resume_delay_divisor = 1;
void http_stream_test_set_resume_delay_divisor(unsigned divisor) {
    resume_delay_divisor = divisor ? divisor : 1;
}
#endif

static bool parse_u64(const char * p, uint64_t * out) {
    if (!p || !*p || *p < '0' || *p > '9')
        return false;
    uint64_t n = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        unsigned d = (unsigned)(*p - '0');
        if (n > (UINT64_MAX - d) / 10)
            return false;
        n = n * 10 + d;
    }
    *out = n;
    return true;
}

static bool parse_content_range(const char * v, uint64_t * start, uint64_t * end,
                                uint64_t * total) {
    if (strncasecmp(v, "bytes ", 6) != 0)
        return false;
    const char * p = v + 6;
    const char * dash = strchr(p, '-');
    const char * slash = dash ? strchr(dash + 1, '/') : NULL;
    if (!dash || !slash || strchr(slash + 1, '/'))
        return false;
    char a[32], b[32];
    size_t al = (size_t)(dash - p), bl = (size_t)(slash - dash - 1);
    if (!al || al >= sizeof(a) || !bl || bl >= sizeof(b))
        return false;
    memcpy(a, p, al);
    a[al] = 0;
    memcpy(b, dash + 1, bl);
    b[bl] = 0;
    return parse_u64(a, start) && parse_u64(b, end) &&
           parse_u64(slash + 1, total) && *start <= *end && *end < *total;
}

static bool copy_safe_header_value(char * out, size_t capacity,
                                   const char * value) {
    size_t n = strlen(value);
    while (n && (value[n - 1] == ' ' || value[n - 1] == '\t'))
        --n;
    if (!n || n >= capacity)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] > 0x7e)
            return false;
    memcpy(out, value, n);
    out[n] = '\0';
    return true;
}

static bool write_request(http_conn_t * c, const char * request, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = http_conn_write(c, (const uint8_t *)request + sent, len - sent);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static bool open_response(const char * url, bool verify_tls, int redirects_left,
                          bool range, uint64_t offset, const char * if_range,
                          response_t * r) {
    memset(r, 0, sizeof(*r));
    bool https;
    char host[256], port[16], path[2048];
    if (!http_conn_parse_url(url, &https, host, sizeof(host), port, sizeof(port),
                             path, sizeof(path)))
        return false;
    r->conn = calloc(1, sizeof(*r->conn));
    if (!r->conn)
        return false;
    if (!http_conn_open_ex(r->conn, host, port, https, verify_tls, STREAM_IO_TIMEOUT_MS,
                           STREAM_IO_TIMEOUT_MS, NULL, NULL)) {
        http_conn_close(r->conn);
        free(r->conn);
        r->conn = NULL;
        return false;
    }
    r->reader.conn = r->conn;
    char request[2800];
    int n = range ? snprintf(request, sizeof(request),
                             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
                             "Compas/compas_player\r\nIcy-MetaData: 0\r\nRange: "
                             "bytes=%llu-\r\n%s%s%sConnection: close\r\n\r\n",
                             path, host, (unsigned long long)offset,
                             if_range ? "If-Range: " : "",
                             if_range ? if_range : "", if_range ? "\r\n" : "")
                  : snprintf(request, sizeof(request),
                             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: "
                             "Compas/compas_player\r\nIcy-MetaData: "
                             "0\r\nConnection: close\r\n\r\n",
                             path, host);
    if (n < 0 || (size_t)n >= sizeof(request) ||
        !write_request(r->conn, request, (size_t)n))
        goto fail;
    char line[1024];
    if (!http_conn_reader_line(&r->reader, line, sizeof(line)))
        goto fail;
    const char * sp = strchr(line, ' ');
    r->status = sp ? atoi(sp + 1) : 0;
    char location[1024] = "";
    while (http_conn_reader_line(&r->reader, line, sizeof(line)) && line[0]) {
        char * colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = 0;
        char * v = colon + 1;
        while (*v == ' ' || *v == '\t')
            ++v;
        if (!strcasecmp(line, "Transfer-Encoding") && !strcasecmp(v, "chunked"))
            r->chunked = true;
        else if (!strcasecmp(line, "Content-Length")) {
            uint64_t x;
            if (!parse_u64(v, &x))
                goto fail;
            if (r->has_length && r->content_length != x)
                goto fail;
            r->has_length = true;
            r->content_length = x;
        } else if (!strcasecmp(line, "Content-Range")) {
            if (!parse_content_range(v, &r->range_start, &r->range_end,
                                     &r->range_total))
                goto fail;
            if (r->range_valid)
                goto fail;
            r->range_valid = true;
        } else if (!strcasecmp(line, "Content-Type")) {
            char * semi = strchr(v, ';');
            if (semi)
                *semi = 0;
            snprintf(r->content_type, sizeof(r->content_type), "%s", v);
        } else if (!strcasecmp(line, "ETag")) {
            (void)copy_safe_header_value(r->etag, sizeof(r->etag), v);
        } else if (!strcasecmp(line, "Last-Modified")) {
            (void)copy_safe_header_value(r->last_modified, sizeof(r->last_modified),
                                         v);
        } else if (!strcasecmp(line, "Location"))
            snprintf(location, sizeof(location), "%s", v);
    }
    if (r->status >= 300 && r->status < 400 && location[0]) {
        http_conn_close(r->conn);
        free(r->conn);
        r->conn = NULL;
        if (redirects_left <= 0 || (strncasecmp(location, "http://", 7) &&
                                    strncasecmp(location, "https://", 8)))
            return false;
        /* An entity validator applies only to the original origin and must
     * not be forwarded if a redirect changes the selected resource. */
        return open_response(location, verify_tls, redirects_left - 1, range,
                             offset, NULL, r);
    }
    if (range) {
        /* A server may ignore the initial capability probe and return the
     * ordinary complete response. Keep it for playback; seek callers
     * independently require a matching 206 before switching readers. */
        if (r->status == 200 && offset == 0) {
            /* accepted as a nonseekable response */
        } else if (r->status != 206 || !r->range_valid ||
                   r->range_start != offset ||
                   (r->has_length &&
                    r->content_length != r->range_end - r->range_start + 1))
            goto fail;
    } else if (r->status < 200 || r->status >= 300)
        goto fail;
    else if (r->status == 206 && (!r->range_valid || r->range_start != 0))
        goto fail;
    snprintf(r->final_url, sizeof(r->final_url), "%s", url);
    return true;
fail:
    http_conn_close(r->conn);
    free(r->conn);
    r->conn = NULL;
    return false;
}

/* Opens a replacement 206 response for [dest, length) of the same entity:
 * the range must start at dest and end at the original length, and the
 * entity validators must not have changed. On failure no connection stays
 * open and r->status keeps the HTTP status (0 when no response arrived). */
static bool open_validated_range(http_stream_t * s, uint64_t dest,
                                 response_t * r) {
    const char * if_range = NULL;
    if (s->etag[0] && strncmp(s->etag, "W/", 2) != 0 && s->etag[0] == '"')
        if_range = s->etag;
    else if (s->last_modified[0])
        if_range = s->last_modified;
    bool if_range_is_etag = if_range && s->etag[0] && if_range == s->etag;
    if (!open_response(s->final_url, s->verify_tls, STREAM_MAX_REDIRECTS, true,
                       dest, if_range, r))
        return false;
    if (r->status != 206 || r->range_total != s->length || !r->range_valid ||
        r->range_start != dest || r->range_end + 1 != r->range_total)
        goto reject;
    if ((if_range &&
         strcmp(if_range, if_range_is_etag ? r->etag : r->last_modified) != 0) ||
        (s->etag[0] && r->etag[0] && strcmp(s->etag, r->etag) != 0) ||
        (s->last_modified[0] && r->last_modified[0] &&
         strcmp(s->last_modified, r->last_modified) != 0))
        goto reject;
    return true;
reject:
    http_conn_close(r->conn);
    free(r->conn);
    r->conn = NULL;
    return false;
}

static bool push_ring(http_stream_t * s, const uint8_t * data, size_t len) {
    size_t off = 0;
    while (off < len) {
        pthread_mutex_lock(&s->mutex);
        while (s->ring_fill == STREAM_RING_CAPACITY && !s->stop_requested)
            pthread_cond_wait(&s->cond, &s->mutex);
        if (s->stop_requested) {
            pthread_mutex_unlock(&s->mutex);
            return false;
        }
        size_t space = STREAM_RING_CAPACITY - s->ring_fill,
               take = len - off < space ? len - off : space;
        for (size_t i = 0; i < take; i++) {
            s->ring[s->ring_head] = data[off + i];
            s->ring_head = (s->ring_head + 1) % STREAM_RING_CAPACITY;
        }
        s->ring_fill += take;
        s->received += take;
        if (s->ring_history > STREAM_RING_CAPACITY - s->ring_fill)
            s->ring_history = STREAM_RING_CAPACITY - s->ring_fill;
        off += take;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }
    return true;
}

/* hiby-yt: resuming a finite stream cut short (design D4). Only the pump
 * calls these; the waits end early when the stream is being closed. */
typedef struct {
    http_stream_t * s;
    uint64_t offset;
} resume_ctx_t;

static bool resume_wait_ms(void * arg, unsigned ms) {
    http_stream_t * s = ((resume_ctx_t *)arg)->s;
#ifdef HTTP_STREAM_TESTING
    ms /= resume_delay_divisor;
#endif
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += ms / 1000;
    deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->mutex);
    while (!s->stop_requested &&
           pthread_cond_timedwait(&s->cond, &s->mutex, &deadline) == 0)
        ;
    bool keep_going = !s->stop_requested;
    pthread_mutex_unlock(&s->mutex);
    return keep_going;
}

static stream_resume_result_t resume_attempt(void * arg, unsigned attempt) {
    resume_ctx_t * ctx = arg;
    http_stream_t * s = ctx->s;
    response_t r;
    stream_resume_result_t result;
    if (open_validated_range(s, ctx->offset, &r)) {
        pthread_mutex_lock(&s->mutex);
        if (s->stop_requested) {
            pthread_mutex_unlock(&s->mutex);
            http_conn_close(r.conn);
            free(r.conn);
            return STREAM_RESUME_STOPPED;
        }
        s->conn = r.conn;
        s->reader = r.reader;
        s->reader.conn = s->conn;
        s->chunked = r.chunked;
        s->has_content_length = !r.chunked;
        s->remaining = r.has_length ? r.content_length
                                    : r.range_end - r.range_start + 1;
        s->reconnects++;
        pthread_mutex_unlock(&s->mutex);
        result = STREAM_RESUME_OK;
    } else {
        /* A response arrived but cannot continue the same entity. */
        result = r.status == 200 || r.status == 206 ? STREAM_RESUME_BAD_RANGE
                                                    : STREAM_RESUME_FAIL;
    }
    stream_log_event(s->log_sid, "reconnect", "attempt=%u offset=%llu result=%s",
                     attempt, (unsigned long long)ctx->offset,
                     result == STREAM_RESUME_OK          ? "ok"
                     : result == STREAM_RESUME_BAD_RANGE ? "bad_range"
                                                         : "fail");
    return result;
}

/* True when the pump should go on reading a resumed connection. */
static bool resume_after_cut(http_stream_t * s) {
    pthread_mutex_lock(&s->mutex);
    bool cut = !s->stop_requested && s->seekable && !s->chunked &&
               s->has_content_length && s->remaining > 0;
    resume_ctx_t ctx = {s, s->length - s->remaining};
    if (cut) {
        /* Never hold two connections: the broken one goes first. */
        http_conn_close(s->conn);
        free(s->conn);
        s->conn = NULL;
        s->reader.conn = NULL;
    }
    pthread_mutex_unlock(&s->mutex);
    if (!cut)
        return false;
    static const stream_resume_ops_t ops = {resume_wait_ms, resume_attempt};
    unsigned attempts = 0;
    stream_resume_result_t result = stream_resume_run(&ops, &ctx, &attempts);
    if (result == STREAM_RESUME_OK)
        return true;
    if (result != STREAM_RESUME_STOPPED) {
        stream_log_event(s->log_sid, "error",
                         "stage=reconnect detail=exhausted attempts=%u offset=%llu",
                         attempts, (unsigned long long)ctx.offset);
        pthread_mutex_lock(&s->mutex);
        s->failed = true;
        pthread_mutex_unlock(&s->mutex);
    }
    return false;
}

static void * pump(void * arg) {
    http_stream_t * s = arg;
    uint8_t buf[HTTP_CONN_READ_CHUNK];
resume:
    for (;;) {
        pthread_mutex_lock(&s->mutex);
        bool stop = s->stop_requested;
        bool limited = s->has_content_length;
        uint64_t rem = s->remaining;
        pthread_mutex_unlock(&s->mutex);
        if (stop || (limited && rem == 0))
            break;
        if (s->chunked) {
            char line[64];
            if (!http_conn_reader_line(&s->reader, line, sizeof(line)))
                break;
            char * semi = strchr(line, ';');
            if (semi)
                *semi = 0;
            errno = 0;
            char * end = NULL;
            unsigned long long z = strtoull(line, &end, 16);
            if (errno || !end || *end || end == line || z > UINT32_MAX)
                break;
            if (z == 0)
                break;
            uint64_t left = z;
            bool ok = true;
            while (left) {
                size_t take = left < sizeof(buf) ? (size_t)left : sizeof(buf);
                if (!http_conn_reader_read_exact(&s->reader, buf, take) ||
                    !push_ring(s, buf, take)) {
                    ok = false;
                    break;
                }
                left -= take;
            }
            if (!ok)
                break;
            char crlf[8];
            if (!http_conn_reader_line(&s->reader, crlf, sizeof(crlf)) || crlf[0])
                break;
        } else {
            size_t want = sizeof(buf);
            if (limited && rem < want)
                want = (size_t)rem;
            int n = http_conn_reader_read_some(&s->reader, buf, want);
            if (n <= 0)
                break;
            if (!push_ring(s, buf, (size_t)n))
                break;
            pthread_mutex_lock(&s->mutex);
            if (s->has_content_length)
                s->remaining -= (uint64_t)n;
            pthread_mutex_unlock(&s->mutex);
        }
    }
    /* hiby-yt: a finite stream cut short resumes at its first missing byte. */
    if (resume_after_cut(s))
        goto resume;
    pthread_mutex_lock(&s->mutex);
    s->ended = true;
    pthread_cond_broadcast(&s->cond);
    if (s->conn)
        http_conn_close(s->conn);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

static bool start_pump(http_stream_t * s) {
#ifdef HTTP_STREAM_TESTING
    if (fail_next_pump_create) {
        fail_next_pump_create = false;
        return false;
    }
#endif
    if (pthread_create(&s->thread, NULL, pump, s) != 0)
        return false;
    s->thread_started = true;
    return true;
}

static void * gated_pump(void * arg) {
    pump_gate_t * gate = arg;
    pthread_mutex_lock(&gate->mutex);
    while (!gate->released)
        pthread_cond_wait(&gate->cond, &gate->mutex);
    http_stream_t * stream = gate->stream;
    pthread_mutex_unlock(&gate->mutex);
    pthread_cond_destroy(&gate->cond);
    pthread_mutex_destroy(&gate->mutex);
    free(gate);
    return pump(stream);
}

static bool create_gated_pump(http_stream_t * stream, pthread_t * thread_out,
                              pump_gate_t ** gate_out) {
    pump_gate_t * gate = calloc(1, sizeof(*gate));
    if (!gate)
        return false;
    gate->stream = stream;
    if (pthread_mutex_init(&gate->mutex, NULL) != 0) {
        free(gate);
        return false;
    }
    if (pthread_cond_init(&gate->cond, NULL) != 0) {
        pthread_mutex_destroy(&gate->mutex);
        free(gate);
        return false;
    }
#ifdef HTTP_STREAM_TESTING
    if (fail_next_pump_create) {
        fail_next_pump_create = false;
        pthread_cond_destroy(&gate->cond);
        pthread_mutex_destroy(&gate->mutex);
        free(gate);
        return false;
    }
#endif
    if (pthread_create(thread_out, NULL, gated_pump, gate) != 0) {
        pthread_cond_destroy(&gate->cond);
        pthread_mutex_destroy(&gate->mutex);
        free(gate);
        return false;
    }
    *gate_out = gate;
    return true;
}

static void release_gated_pump(pump_gate_t * gate) {
    pthread_mutex_lock(&gate->mutex);
    gate->released = true;
    pthread_cond_signal(&gate->cond);
    pthread_mutex_unlock(&gate->mutex);
}

http_stream_t * http_stream_open(const char * url, bool verify_tls) {
    response_t r;
    /* First negotiate Range: 0-. Live servers commonly ignore it and still
   * return 200/chunked, preserving the established radio behavior. */
    bool negotiated =
        open_response(url, verify_tls, STREAM_MAX_REDIRECTS, true, 0, NULL, &r);
    if (!negotiated) {
        if (!open_response(url, verify_tls, STREAM_MAX_REDIRECTS, false, 0, NULL,
                           &r))
            return NULL;
    }
    http_stream_t * s = calloc(1, sizeof(*s));
    if (!s) {
        http_conn_close(r.conn);
        free(r.conn);
        return NULL;
    }
    s->ring = malloc(STREAM_RING_CAPACITY);
    if (!s->ring) {
        http_conn_close(r.conn);
        free(r.conn);
        free(s);
        return NULL;
    }
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->conn = r.conn;
    r.conn = NULL;
    s->reader = r.reader;
    s->reader.conn = s->conn;
    s->chunked = r.chunked;
    s->has_content_length = r.has_length && !r.chunked;
    s->content_length = r.content_length;
    s->remaining = r.content_length;
    s->seekable = negotiated && r.status == 206 && r.range_valid &&
                  r.range_start == 0 && r.range_end + 1 == r.range_total &&
                  r.range_total <= INT64_MAX &&
                  (!r.has_length || r.content_length == r.range_total);
    if (s->seekable && !r.has_length) {
        s->has_content_length = true;
        s->remaining = r.range_total;
    }
    s->length = s->seekable ? r.range_total
                            : (r.has_length ? r.content_length : UINT64_MAX);
    s->verify_tls = verify_tls;
    snprintf(s->content_type, sizeof(s->content_type), "%s", r.content_type);
    snprintf(s->final_url, sizeof(s->final_url), "%s", r.final_url);
    snprintf(s->etag, sizeof(s->etag), "%s", r.etag);
    snprintf(s->last_modified, sizeof(s->last_modified), "%s", r.last_modified);
    s->log_sid = stream_log_next_sid();
    if (!start_pump(s)) {
        http_conn_close(s->conn);
        free(s->conn);
        pthread_mutex_destroy(&s->mutex);
        pthread_cond_destroy(&s->cond);
        free(s->ring);
        free(s);
        return NULL;
    }
    return s;
}

size_t http_stream_read(http_stream_t * s, void * buf, size_t n) {
    if (!s || !buf || !n)
        return 0;
    uint8_t * out = buf;
    size_t total = 0;
    pthread_mutex_lock(&s->mutex);
    while (total < n && !s->at_virtual_end) {
        while (!s->ring_fill && !s->ended && !s->stop_requested)
            pthread_cond_wait(&s->cond, &s->mutex);
        if (!s->ring_fill)
            break;
        size_t take = n - total < s->ring_fill ? n - total : s->ring_fill;
        if (s->seekable && s->length - s->position < take)
            take = (size_t)(s->length - s->position);
        if (!take)
            break;
        for (size_t i = 0; i < take; i++) {
            out[total + i] = s->ring[s->ring_tail];
            s->ring_tail = (s->ring_tail + 1) % STREAM_RING_CAPACITY;
        }
        s->ring_fill -= take;
        s->ring_history += take;
        if (s->ring_history > STREAM_RING_CAPACITY - s->ring_fill)
            s->ring_history = STREAM_RING_CAPACITY - s->ring_fill;
        total += take;
        s->position += take;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->mutex);
    return total;
}

const char * http_stream_content_type(http_stream_t * s) {
    return s ? s->content_type : "";
}
bool http_stream_can_seek(http_stream_t * s) { return s && s->seekable; }
unsigned http_stream_log_sid(http_stream_t * s) { return s ? s->log_sid : 0; }
void http_stream_log_host(http_stream_t * s, char * out, size_t size) {
    stream_log_url_host(s ? s->final_url : "", out, size);
}
uint64_t http_stream_bytes_received(http_stream_t * s) {
    if (!s)
        return 0;
    pthread_mutex_lock(&s->mutex);
    uint64_t n = s->received;
    pthread_mutex_unlock(&s->mutex);
    return n;
}
unsigned http_stream_reconnects(http_stream_t * s) {
    if (!s)
        return 0;
    pthread_mutex_lock(&s->mutex);
    unsigned n = s->reconnects;
    pthread_mutex_unlock(&s->mutex);
    return n;
}
bool http_stream_failed(http_stream_t * s) {
    if (!s)
        return false;
    pthread_mutex_lock(&s->mutex);
    bool failed = s->failed;
    pthread_mutex_unlock(&s->mutex);
    return failed;
}
int64_t http_stream_tell(http_stream_t * s) {
    if (!s)
        return -1;
    pthread_mutex_lock(&s->mutex);
    uint64_t p = s->at_virtual_end ? s->length : s->position;
    pthread_mutex_unlock(&s->mutex);
    return p > INT64_MAX ? -1 : (int64_t)p;
}
int64_t http_stream_length(http_stream_t * s) {
    if (!s || s->length == UINT64_MAX || s->length > INT64_MAX)
        return -1;
    return (int64_t)s->length;
}

bool http_stream_seek(http_stream_t * s, int64_t offset, int origin) {
    if (!s || !s->seekable)
        return false;
    pthread_mutex_lock(&s->mutex);
    uint64_t pos = s->at_virtual_end ? s->length : s->position;
    bool active = !s->stop_requested;
    pthread_mutex_unlock(&s->mutex);
    if (!active)
        return false;
    uint64_t base;
    if (origin == SEEK_SET)
        base = 0;
    else if (origin == SEEK_CUR)
        base = pos;
    else if (origin == SEEK_END)
        base = s->length;
    else
        return false;
    uint64_t dest;
    if (offset >= 0) {
        uint64_t forward = (uint64_t)offset;
        if (base > s->length || forward > s->length - base)
            return false;
        dest = base + forward;
    } else {
        /* Avoid negating INT64_MIN directly. */
        uint64_t backward = (uint64_t)(-(offset + 1)) + 1;
        if (backward > base)
            return false;
        dest = base - backward;
    }
    /* The cheap cases below move only within bytes the connection already
     * delivered; anything else opens a validated replacement response. */
    pthread_mutex_lock(&s->mutex);
    if (dest == s->length) {
        s->at_virtual_end = true;
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    if (dest == s->position) {
        s->at_virtual_end = false;
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    if (dest > s->position && dest - s->position <= s->ring_fill) {
        size_t skip = (size_t)(dest - s->position);
        s->ring_tail = (s->ring_tail + skip) % STREAM_RING_CAPACITY;
        s->ring_fill -= skip;
        s->ring_history += skip;
        if (s->ring_history > STREAM_RING_CAPACITY - s->ring_fill)
            s->ring_history = STREAM_RING_CAPACITY - s->ring_fill;
        s->position = dest;
        s->at_virtual_end = false;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    if (dest < s->position && s->position - dest <= s->ring_history) {
        size_t back = (size_t)(s->position - dest);
        s->ring_tail = (s->ring_tail + STREAM_RING_CAPACITY - back) % STREAM_RING_CAPACITY;
        s->ring_fill += back;
        s->ring_history -= back;
        s->position = dest;
        s->at_virtual_end = false;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mutex);
        return true;
    }
    pthread_mutex_unlock(&s->mutex);
    response_t candidate;
    bool have_body = dest < s->length;
    if (have_body && !open_validated_range(s, dest, &candidate))
        return false;
    pthread_t candidate_thread;
    pump_gate_t * candidate_gate = NULL;
    if (have_body && !create_gated_pump(s, &candidate_thread, &candidate_gate)) {
        http_conn_close(candidate.conn);
        free(candidate.conn);
        return false;
    }
    /* Stop and join the old pump only after the replacement response has
   * passed validation. shutdown is done while the mutex protects fd life. */
    pthread_mutex_lock(&s->mutex);
    s->stop_requested = true;
    pthread_cond_broadcast(&s->cond);
    /* The pump closes the connection under this same mutex. Shutdown while
   * holding it so the descriptor cannot be closed/reused between lookup
   * and shutdown. */
    if (s->conn && s->conn->net.fd >= 0)
        shutdown(s->conn->net.fd, SHUT_RDWR);
    pthread_mutex_unlock(&s->mutex);
    if (s->thread_started)
        pthread_join(s->thread, NULL);
    free(s->conn);
    s->conn = NULL;
    pthread_mutex_lock(&s->mutex);
    s->ring_head = s->ring_tail = s->ring_fill = 0;
    s->ring_history = 0;
    s->at_virtual_end = false;
    s->position = dest;
    s->stop_requested = false;
    s->ended = !have_body;
    if (have_body) {
        s->conn = candidate.conn;
        candidate.conn = NULL;
        s->reader = candidate.reader;
        s->reader.conn = s->conn;
        s->chunked = candidate.chunked;
        s->has_content_length = !candidate.chunked;
        s->remaining = candidate.has_length
                           ? candidate.content_length
                           : candidate.range_end - candidate.range_start + 1;
        s->thread = candidate_thread;
        s->thread_started = true;
    } else {
        s->thread_started = false;
        s->conn = NULL;
        s->reader.conn = NULL;
    }
    pthread_mutex_unlock(&s->mutex);
    if (have_body)
        release_gated_pump(candidate_gate);
    return true;
}

void http_stream_close(http_stream_t * s) {
    if (!s)
        return;
    pthread_mutex_lock(&s->mutex);
    s->stop_requested = true;
    pthread_cond_broadcast(&s->cond);
    if (s->conn && s->conn->net.fd >= 0)
        shutdown(s->conn->net.fd, SHUT_RDWR);
    pthread_mutex_unlock(&s->mutex);
    if (s->thread_started)
        pthread_join(s->thread, NULL);
    free(s->conn);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->cond);
    free(s->ring);
    free(s);
}
