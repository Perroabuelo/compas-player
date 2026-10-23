#include "yt_url_cache.h"

#include <string.h>

static void wipe(yt_url_slot_t * s) {
    memset(s, 0, sizeof(*s));
}

static bool id_matches(const yt_url_slot_t * s, const char * video_id) {
    return s->used && video_id && strcmp(s->video_id, video_id) == 0;
}

static yt_url_slot_t * find(yt_url_cache_t * c, const char * video_id) {
    for (int i = 0; i < YT_URL_CACHE_SLOTS; i++)
        if (id_matches(&c->slot[i], video_id)) return &c->slot[i];
    return NULL;
}

/* The slot of video_id if it is still valid; an expired one is wiped. */
static yt_url_slot_t * find_valid(yt_url_cache_t * c, const char * video_id, int64_t boot_now_ms) {
    yt_url_slot_t * s = find(c, video_id);
    if (!s) return NULL;
    if (boot_now_ms >= s->deadline_ms) {
        wipe(s);
        return NULL;
    }
    return s;
}

void yt_url_cache_clear(yt_url_cache_t * c) {
    memset(c, 0, sizeof(*c));
}

int64_t yt_url_cache_ttl_s(int64_t expire, int64_t wall_now) {
    int64_t ttl = expire - wall_now;
    return (ttl > 0 && ttl <= YT_URL_CACHE_MAX_TTL_S) ? ttl : YT_URL_CACHE_FALLBACK_TTL_S;
}

bool yt_url_cache_put(yt_url_cache_t * c, const char * video_id, const char * url, uint64_t content_length,
                      int64_t expire, int64_t wall_now, int64_t boot_now_ms) {
    if (!video_id || !url) return false;
    size_t id_len = strlen(video_id), url_len = strlen(url);
    if (id_len == 0 || id_len > YT_URL_CACHE_VIDEO_ID_MAX || url_len == 0 || url_len >= YT_URL_CACHE_URL_MAX)
        return false;

    yt_url_slot_t * s = find(c, video_id);
    for (int i = 0; !s && i < YT_URL_CACHE_SLOTS; i++)
        if (!c->slot[i].used) s = &c->slot[i];
    if (!s) {
        s = &c->slot[0];
        for (int i = 1; i < YT_URL_CACHE_SLOTS; i++)
            if (c->slot[i].seq < s->seq) s = &c->slot[i];
    }
    wipe(s);
    s->used = true;
    s->seq = c->next_seq++;
    memcpy(s->video_id, video_id, id_len + 1);
    memcpy(s->url, url, url_len + 1);
    s->content_length = content_length;
    s->deadline_ms = boot_now_ms + (yt_url_cache_ttl_s(expire, wall_now) - YT_URL_CACHE_MARGIN_S) * 1000;
    return true;
}

bool yt_url_cache_get(yt_url_cache_t * c, const char * video_id, int64_t boot_now_ms, char * out, size_t out_size,
                      uint64_t * content_length) {
    yt_url_slot_t * s = find_valid(c, video_id, boot_now_ms);
    if (!s || !out || strlen(s->url) >= out_size) return false;
    memcpy(out, s->url, strlen(s->url) + 1);
    if (content_length) *content_length = s->content_length;
    return true;
}

bool yt_url_cache_has(yt_url_cache_t * c, const char * video_id, int64_t boot_now_ms) {
    return find_valid(c, video_id, boot_now_ms) != NULL;
}

int64_t yt_url_cache_deadline(const yt_url_cache_t * c, const char * video_id) {
    for (int i = 0; i < YT_URL_CACHE_SLOTS; i++)
        if (id_matches(&c->slot[i], video_id)) return c->slot[i].deadline_ms;
    return 0;
}

void yt_url_cache_keep(yt_url_cache_t * c, const char * current, const char * next) {
    for (int i = 0; i < YT_URL_CACHE_SLOTS; i++) {
        yt_url_slot_t * s = &c->slot[i];
        if (!s->used) continue;
        bool keep = (current && current[0] && id_matches(s, current)) || (next && next[0] && id_matches(s, next));
        if (!keep) wipe(s);
    }
}

void yt_url_cache_drop(yt_url_cache_t * c, const char * video_id) {
    yt_url_slot_t * s = find(c, video_id);
    if (s) wipe(s);
}
