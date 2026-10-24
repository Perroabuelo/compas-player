#include "yt_queue.h"

#include "cJSON.h"
#include "stream_log.h"
#include "yt_resolve_policy.h"
#include "yt_resolver_proc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static yt_url_cache_t cache; /* under lock */
/* The last prefetch a search cancelled, until the UI takes it (under lock). */
static char preempted_id[YT_RESOLVER_VIDEO_ID_LEN + 1];

#ifdef HOST_BUILD
static int64_t test_clock_ms = -1;
void yt_queue_set_test_clock(int64_t boot_ms) {
    test_clock_ms = boot_ms;
}
#endif

int64_t yt_queue_boot_ms(void) {
#ifdef HOST_BUILD
    if (test_clock_ms >= 0) return test_clock_ms;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_class(yt_queue_url_t * out, const char * cls) {
    snprintf(out->cls, sizeof(out->cls), "%s", cls && cls[0] ? cls : "internal");
}

/* Decodes the resolver's line into out and puts the URL in the cache.
 * Returns whether it is a usable URL. */
static bool take_result(const char * video_id, const yt_resolver_result_t * r, yt_queue_url_t * out) {
    memset(out, 0, sizeof(*out));
    if (r->fail_class) {
        set_class(out, r->fail_class);
        return false;
    }
    cJSON * root = cJSON_Parse(r->output);
    const cJSON * ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        const cJSON * cls = cJSON_GetObjectItemCaseSensitive(root, "class");
        set_class(out, cJSON_IsString(cls) ? cls->valuestring : "internal");
        cJSON_Delete(root);
        return false;
    }
    const cJSON * url = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON * len = cJSON_GetObjectItemCaseSensitive(root, "content_length");
    const cJSON * expire = cJSON_GetObjectItemCaseSensitive(root, "expire");
    bool usable = cJSON_IsString(url) && url->valuestring[0] && strlen(url->valuestring) < sizeof(out->url) &&
                  cJSON_IsNumber(len) && len->valuedouble > 0 && cJSON_IsNumber(expire);
    if (!usable) {
        set_class(out, "internal");
        cJSON_Delete(root);
        return false;
    }
    snprintf(out->url, sizeof(out->url), "%s", url->valuestring);
    out->content_length = (uint64_t) len->valuedouble;
    int64_t boot = yt_queue_boot_ms();
    int64_t wall = (int64_t) time(NULL);
    int64_t expire_s = (int64_t) expire->valuedouble;
    out->deadline_ms = boot + (yt_url_cache_ttl_s(expire_s, wall) - YT_URL_CACHE_MARGIN_S) * 1000;
    set_class(out, "ok");
    pthread_mutex_lock(&lock);
    yt_url_cache_put(&cache, video_id, out->url, out->content_length, expire_s, wall, boot);
    pthread_mutex_unlock(&lock);
    cJSON_Delete(root);
    return true;
}

static yt_queue_status_t resolve(yt_owner_t owner, const char * video_id, bool retry,
                                 bool (*cancelled)(void * arg), void * arg, yt_queue_url_t * out) {
    yt_resolver_result_t * r = malloc(sizeof(*r));
    if (!r) {
        memset(out, 0, sizeof(*out));
        set_class(out, "internal");
        return YT_QUEUE_URL_FAILED;
    }
    yt_resolver_outcome_t outcome = yt_resolver_request(owner, video_id, retry, cancelled, arg, r);
    yt_queue_status_t status;
    if (outcome == YT_RESOLVER_CANCELLED) {
        status = YT_QUEUE_URL_CANCELLED;
    } else if (outcome == YT_RESOLVER_REFUSED) {
        memset(out, 0, sizeof(*out));
        set_class(out, "internal");
        status = YT_QUEUE_URL_FAILED;
    } else {
        status = take_result(video_id, r, out) ? YT_QUEUE_URL_OK : YT_QUEUE_URL_FAILED;
    }
    memset(r, 0, sizeof(*r)); /* the URL does not stay in freed memory */
    free(r);
    return status;
}

yt_queue_status_t yt_queue_url_for_open(const char * video_id, bool (*cancelled)(void * arg), void * arg,
                                        yt_queue_url_t * out) {
    int64_t boot = yt_queue_boot_ms();
    pthread_mutex_lock(&lock);
    bool hit = yt_url_cache_get(&cache, video_id, boot, out->url, sizeof(out->url), &out->content_length);
    int64_t deadline = hit ? yt_url_cache_deadline(&cache, video_id) : 0;
    pthread_mutex_unlock(&lock);
    if (hit) {
        out->deadline_ms = deadline;
        out->cache_hit = true;
        set_class(out, "ok");
        stream_log_event(stream_log_next_sid(), "resolve", "video_id=%s stage=open cache=hit class=ok ms=0 exit=-",
                         video_id);
        return YT_QUEUE_URL_OK;
    }
    yt_queue_status_t status = YT_QUEUE_URL_FAILED;
    for (unsigned tries = 1;; tries++) {
        status = resolve(YT_OWNER_OPEN, video_id, tries > 1, cancelled, arg, out);
        if (status != YT_QUEUE_URL_FAILED || !yt_resolve_should_retry(out->cls, tries)) break;
    }
    return status;
}

yt_queue_status_t yt_queue_url_reopen(const char * video_id, bool (*cancelled)(void * arg), void * arg,
                                      yt_queue_url_t * out) {
    pthread_mutex_lock(&lock);
    yt_url_cache_drop(&cache, video_id);
    pthread_mutex_unlock(&lock);
    return yt_queue_url_for_open(video_id, cancelled, arg, out);
}

yt_queue_status_t yt_queue_url_refresh(const char * video_id, bool (*cancelled)(void * arg), void * arg,
                                       yt_queue_url_t * out) {
    return resolve(YT_OWNER_REFRESH, video_id, false, cancelled, arg, out);
}

/* On the resolver's worker thread. */
static void prefetch_done(const yt_resolver_result_t * r, void * arg) {
    char * video_id = arg;
    if (!r->cancelled) {
        yt_queue_url_t * out = malloc(sizeof(*out));
        if (out) {
            take_result(video_id, r, out);
            memset(out, 0, sizeof(*out));
            free(out);
        }
    } else if (r->preempted) {
        pthread_mutex_lock(&lock);
        snprintf(preempted_id, sizeof(preempted_id), "%s", video_id);
        pthread_mutex_unlock(&lock);
    }
    free(video_id);
}

bool yt_queue_take_preempted(char * video_id, size_t size) {
    pthread_mutex_lock(&lock);
    bool has = preempted_id[0] != '\0' && size > strlen(preempted_id);
    if (has) snprintf(video_id, size, "%s", preempted_id);
    preempted_id[0] = '\0';
    pthread_mutex_unlock(&lock);
    return has;
}

bool yt_queue_prefetch(const char * video_id) {
    if (!yt_video_id_valid(video_id)) return false;
    char * id = strdup(video_id);
    if (!id) return false;
    if (yt_resolver_start(YT_OWNER_PREFETCH, id, prefetch_done, id) == 0) {
        free(id);
        return false;
    }
    return true;
}

bool yt_queue_has_url(const char * video_id) {
    int64_t boot = yt_queue_boot_ms();
    pthread_mutex_lock(&lock);
    bool has = yt_url_cache_has(&cache, video_id, boot);
    pthread_mutex_unlock(&lock);
    return has;
}

void yt_queue_track_started(const char * video_id) {
    pthread_mutex_lock(&lock);
    yt_url_cache_keep(&cache, video_id, NULL);
    pthread_mutex_unlock(&lock);
}

void yt_queue_clear(void) {
    pthread_mutex_lock(&lock);
    yt_url_cache_clear(&cache);
    pthread_mutex_unlock(&lock);
}
