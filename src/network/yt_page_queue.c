#include "yt_page_queue.h"

#include <string.h>

#include "remote_youtube.h"
#include "utf8_util.h"

static size_t songs_between(const yt_page_t * page, size_t from, size_t end, uint16_t * out, size_t max) {
    size_t n = 0;
    for (size_t i = from; i < end && n < max; i++) {
        if (page->items[i].type == YT_ITEM_SONG) out[n++] = (uint16_t) i;
    }
    return n;
}

size_t yt_page_queue_from(const yt_page_t * page, size_t index, bool whole_page, uint16_t * out, size_t max) {
    if (!page || index >= page->item_count || page->items[index].type != YT_ITEM_SONG) return 0;
    const yt_page_shelf_t * shelf = &page->shelves[page->items[index].shelf];
    size_t end = whole_page ? page->item_count : (size_t) shelf->first + shelf->count;
    return songs_between(page, index, end, out, max);
}

size_t yt_page_queue_all(const yt_page_t * page, uint16_t * out, size_t max) {
    return page ? songs_between(page, 0, page->item_count, out, max) : 0;
}

void yt_page_queue_shuffle(uint16_t * idx, size_t n, yt_rand_below_fn rand_below, void * ctx) {
    for (size_t i = n; i > 1; i--) {
        size_t j = rand_below(ctx, (uint32_t) i);
        if (j >= i) j = i - 1;
        uint16_t t = idx[i - 1];
        idx[i - 1] = idx[j];
        idx[j] = t;
    }
}

static void copy_field(char * dst, size_t size, const char * src) {
    utf8_truncate_safe_bounded(dst, size, src, strlen(src));
}

bool yt_page_item_to_track(const yt_page_t * page, const yt_page_item_t * item, remote_track_meta_t * out) {
    memset(out, 0, sizeof(*out));
    if (!page || !item || item->type != YT_ITEM_SONG) return false;
    strcpy(out->provider, REMOTE_YOUTUBE_PROVIDER);
    if (strlen(item->video_id) >= sizeof(out->track_id)) return false;
    strcpy(out->track_id, item->video_id);
    copy_field(out->title, sizeof(out->title), yt_page_str(page, item->title));
    copy_field(out->artist, sizeof(out->artist), yt_page_str(page, item->artist));
    copy_field(out->album, sizeof(out->album), yt_page_str(page, item->album));
    out->duration_ms = item->duration_s > 0 ? (uint32_t) item->duration_s * 1000u : 0;
    out->verify_tls = true;
    return remote_youtube_normalize(out) == NULL;
}

bool yt_page_queue_fill(void * ctx, int index, remote_track_meta_t * out) {
    const yt_page_queue_t * q = ctx;
    if (!q || index < 0 || (size_t) index >= q->count || q->order[index] >= q->page->item_count) return false;
    return yt_page_item_to_track(q->page, &q->page->items[q->order[index]], out);
}
