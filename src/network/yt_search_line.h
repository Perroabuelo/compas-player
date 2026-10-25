#ifndef YT_SEARCH_LINE_H
#define YT_SEARCH_LINE_H

/* Decoding the search mode's output line in the player (hiby-yt, design D2
 * of youtube-music-screen). Pure: no LVGL, no I/O. The YouTube Music
 * screen decodes the line on the search service's worker thread, so the UI
 * thread only appends the page to its results.
 *
 * The line is the one hiby-yt-resolve --search prints (spec
 * youtube/track-search):
 *
 *   {"ok":true,"results":[{"video_id","title","artist","album"?,
 *    "duration_s","artist_id"?,"album_id"?},...],"continuation":"<token>"|null}
 *   {"ok":false,"class":"<class>","detail":"<detail>"}
 *
 * Anything else, or a line that breaks the limits the program keeps
 * (YT_SEARCH_LINE_MAX_SONGS songs, YT_SONG_TEXT_MAX bytes of text, a valid
 * token), is malformed. */

#include "yt_browse_input.h"
#include "yt_search_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YT_SONG_VIDEO_ID_LEN 11
/* Bytes of title, artist and album, as the program cuts them. */
#define YT_SONG_TEXT_MAX 256
/* Songs on one page, as the program bounds them. */
#define YT_SEARCH_LINE_MAX_SONGS 40
/* Longest duration the program writes: 99:59:59. */
#define YT_SONG_DURATION_MAX_S 359999u
/* A failure class: [a-z_], like "network" or "bad_input". */
#define YT_SEARCH_LINE_CLASS_MAX 23

typedef struct {
    char video_id[YT_SONG_VIDEO_ID_LEN + 1];
    char title[YT_SONG_TEXT_MAX + 1];
    char artist[YT_SONG_TEXT_MAX + 1];
    char album[YT_SONG_TEXT_MAX + 1]; /* "" when the result has none */
    uint32_t duration_ms;             /* 0 when unknown */
    /* Pages of the artist and the album, for «Ir al artista» and «Ir al
     * álbum» (youtube-music-browse D5); "" when the result has none. */
    char artist_id[YT_BROWSE_ID_MAX + 1];
    char album_id[YT_BROWSE_ID_MAX + 1];
} yt_song_t;

typedef struct {
    yt_song_t songs[YT_SEARCH_LINE_MAX_SONGS];
    size_t count;
    char token[YT_SEARCH_TOKEN_MAX + 1];             /* next page, "" when there is none */
    char fail_class[YT_SEARCH_LINE_CLASS_MAX + 1];   /* the program's class when FAILED */
} yt_search_line_page_t;

typedef enum {
    YT_SEARCH_LINE_OK,        /* songs, count and token are set (count may be 0) */
    YT_SEARCH_LINE_FAILED,    /* the program's failure line; fail_class is set */
    YT_SEARCH_LINE_MALFORMED, /* not the contract */
} yt_search_line_status_t;

/* Decodes the line (len bytes, with or without its final newline) into
 * *page. On MALFORMED, *page is zeroed; on FAILED only fail_class is set. */
yt_search_line_status_t yt_search_line_parse(const char * line, size_t len, yt_search_line_page_t * page);

#endif /* YT_SEARCH_LINE_H */
