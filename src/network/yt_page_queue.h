#ifndef YT_PAGE_QUEUE_H
#define YT_PAGE_QUEUE_H

/* The YouTube queue built from a content page (hiby-yt, design D5 of
 * youtube-music-browse, spec youtube/music-screen). Pure: no LVGL, no I/O.
 *
 * Tapping a song of a shelf queues the songs of that shelf from it to the
 * last; on an album or a playlist page, the page's songs from it to the
 * last. «Reproducir» queues every song of the page in order, «Aleatorio» in
 * an order shuffled here (Fisher-Yates), without touching the player's
 * shuffle mode. The queue is a list of item indices; yt_page_queue_fill()
 * hands each entry to remote_track_meta_set_all_from(), so no second copy
 * of the tracks is kept. */

#include "remote_track.h"
#include "yt_browse_line.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Writes into out (at most max) the indices of the songs to queue when the
 * user taps item index: the songs of its shelf from it to the last, or of
 * the whole page when whole_page. Returns how many; 0 when index is not a
 * song. */
size_t yt_page_queue_from(const yt_page_t * page, size_t index, bool whole_page, uint16_t * out, size_t max);

/* Every song of the page in order (at most max). */
size_t yt_page_queue_all(const yt_page_t * page, uint16_t * out, size_t max);

/* A number in [0, bound) (bound > 0). */
typedef uint32_t (*yt_rand_below_fn)(void * ctx, uint32_t bound);

/* Shuffles idx[0..n) in place with rand_below (Fisher-Yates). */
void yt_page_queue_shuffle(uint16_t * idx, size_t n, yt_rand_below_fn rand_below, void * ctx);

/* The queue entry of a song item: a youtube track by videoId without
 * stream_url, with its title, artist and album cut to the entry's fields at
 * a UTF-8 character boundary, and its duration (0 when unknown). False when
 * it is not a song or makes no valid youtube track. */
bool yt_page_item_to_track(const yt_page_t * page, const yt_page_item_t * item, remote_track_meta_t * out);

typedef struct {
    const yt_page_t * page;
    const uint16_t * order;
    size_t count;
} yt_page_queue_t;

/* remote_track_fill_fn over a yt_page_queue_t: entry index of the queue. */
bool yt_page_queue_fill(void * ctx, int index, remote_track_meta_t * out);

#endif /* YT_PAGE_QUEUE_H */
