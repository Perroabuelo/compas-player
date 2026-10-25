#include "yt_results.h"

#include "remote_youtube.h"
#include "utf8_util.h"

#include <stdlib.h>
#include <string.h>

#ifdef HOST_BUILD
static void * (*block_alloc)(size_t) = malloc;

void yt_results_set_test_alloc(void * (*alloc)(size_t)) {
    block_alloc = alloc ? alloc : malloc;
}
#else
#define block_alloc malloc
#endif

void yt_results_init(yt_results_t * r) {
    memset(r, 0, sizeof(*r));
}

bool yt_results_reserve(yt_results_t * r) {
    if (!r->songs) r->songs = block_alloc(sizeof(yt_song_t) * YT_RESULTS_MAX);
    return r->songs != NULL;
}

void yt_results_clear(yt_results_t * r) {
    r->count = 0;
    r->token[0] = '\0';
}

void yt_results_free(yt_results_t * r) {
    free(r->songs);
    yt_results_init(r);
}

static bool present(const yt_results_t * r, const char * video_id) {
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->songs[i].video_id, video_id) == 0) return true;
    }
    return false;
}

size_t yt_results_add_page(yt_results_t * r, const yt_search_line_page_t * page) {
    if (!r->songs) return 0;
    size_t added = 0;
    for (size_t i = 0; i < page->count && r->count < YT_RESULTS_MAX; i++) {
        if (present(r, page->songs[i].video_id)) continue;
        r->songs[r->count++] = page->songs[i];
        added++;
    }
    if (r->count < YT_RESULTS_MAX) {
        strcpy(r->token, page->token);
    } else {
        r->token[0] = '\0';
    }
    return added;
}

bool yt_results_has_more(const yt_results_t * r) {
    return r->token[0] != '\0' && r->count < YT_RESULTS_MAX;
}

bool yt_results_song_to_track(const yt_song_t * song, remote_track_meta_t * out) {
    memset(out, 0, sizeof(*out));
    strcpy(out->provider, REMOTE_YOUTUBE_PROVIDER);
    if (strlen(song->video_id) >= sizeof(out->track_id)) return false;
    strcpy(out->track_id, song->video_id);
    utf8_truncate_safe_bounded(out->title, sizeof(out->title), song->title, strlen(song->title));
    utf8_truncate_safe_bounded(out->artist, sizeof(out->artist), song->artist, strlen(song->artist));
    utf8_truncate_safe_bounded(out->album, sizeof(out->album), song->album, strlen(song->album));
    out->duration_ms = song->duration_ms;
    out->verify_tls = true;
    return remote_youtube_normalize(out) == NULL;
}

size_t yt_results_to_queue(const yt_results_t * r, size_t from, remote_track_meta_t * out, size_t max) {
    size_t n = 0;
    for (size_t i = from; i < r->count && n < max; i++, n++) {
        if (!yt_results_song_to_track(&r->songs[i], &out[n])) return 0;
    }
    return n;
}
