#ifndef YT_RESULTS_H
#define YT_RESULTS_H

/* The results of the YouTube Music screen (hiby-yt, design D2 of
 * youtube-music-screen). Pure: no LVGL, no I/O; UI thread only.
 *
 * The store reserves one block of YT_RESULTS_MAX songs at the first search
 * (about 155 KiB) and reuses it for later searches, so the list does not
 * fragment the heap page by page. Pages are appended in order of arrival,
 * dropping songs whose videoId is already there, up to YT_RESULTS_MAX; the
 * token of the last page asks for the next one. A page that fails is never
 * added, so the store keeps what it had and its token. The screen's list
 * points at the songs' strings, so the block lives as long as the store. */

#include "remote_track.h"
#include "yt_search_line.h"

#include <stdbool.h>
#include <stddef.h>

#define YT_RESULTS_MAX 200

typedef struct {
    yt_song_t * songs; /* YT_RESULTS_MAX entries once reserved, else NULL */
    size_t count;
    char token[YT_SEARCH_TOKEN_MAX + 1]; /* next page, "" when there is none */
} yt_results_t;

/* An empty store without a block. */
void yt_results_init(yt_results_t * r);

/* Allocates the block if the store has none. Returns false when the
 * allocation fails (the store stays empty and usable). */
bool yt_results_reserve(yt_results_t * r);

/* Empties the store for a new search, keeping its block. */
void yt_results_clear(yt_results_t * r);

/* Frees the block; the store is empty again. */
void yt_results_free(yt_results_t * r);

/* Appends a page that decoded OK: its songs in order, skipping videoIds
 * already present, until the store holds YT_RESULTS_MAX. Takes the page's
 * token, or none once the store is full. Returns how many songs were added
 * (0 without a block). */
size_t yt_results_add_page(yt_results_t * r, const yt_search_line_page_t * page);

/* True when there is a next page to ask for: a token, and room. */
bool yt_results_has_more(const yt_results_t * r);

/* The queue entry of a song: a youtube track by videoId without
 * stream_url, with its title, artist and album cut to the entry's fields
 * at a UTF-8 character boundary, and its duration. Returns false when the
 * song does not make a valid youtube track. */
bool yt_results_song_to_track(const yt_song_t * song, remote_track_meta_t * out);

/* Fills out with the queue for "play from here": the songs from index from
 * to the last, at most max. Returns how many entries were written (0 when
 * from is past the end, or an entry is invalid). */
size_t yt_results_to_queue(const yt_results_t * r, size_t from, remote_track_meta_t * out, size_t max);

#ifdef HOST_BUILD
/* PC tests only: the block's allocator (NULL: malloc). */
void yt_results_set_test_alloc(void * (*alloc)(size_t));
#endif

#endif /* YT_RESULTS_H */
