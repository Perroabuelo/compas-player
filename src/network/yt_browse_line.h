#ifndef YT_BROWSE_LINE_H
#define YT_BROWSE_LINE_H

/* Decoding the content mode's output line in the player (hiby-yt, design
 * D5 of youtube-music-browse). Pure: no LVGL, no I/O. The YouTube Music
 * screen decodes the line on the service's worker thread, so the UI thread
 * only takes the page.
 *
 * The line is the one hiby-yt-resolve --browse prints (spec
 * youtube/content-browse):
 *
 *   {"ok":true,"title":"…","shelves":[{"title":"…","items":[…]}],
 *    "continuation":"<token>"|null}
 *   {"ok":false,"class":"<class>","detail":"<detail>"}
 *
 * A page is two blocks: one with the page, its shelves and its items (small
 * structs of offsets), and one arena that holds every text and identifier
 * once, sized to what the line carries. yt_page_free() frees both. Anything
 * that breaks the program's limits (YT_PAGE_MAX_SHELVES shelves,
 * YT_PAGE_MAX_ITEMS items, texts of YT_PAGE_TEXT_MAX bytes, valid ids and
 * token) is malformed. */

#include "yt_browse_input.h"
#include "yt_search_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YT_PAGE_MAX_SHELVES 20
#define YT_PAGE_MAX_ITEMS 200
#define YT_PAGE_TEXT_MAX 256
/* Longest duration the program writes: 99:59:59. */
#define YT_PAGE_DURATION_MAX_S 359999
#define YT_PAGE_CLASS_MAX 23

typedef enum {
    YT_ITEM_SONG,
    YT_ITEM_ALBUM,
    YT_ITEM_PLAYLIST,
    YT_ITEM_ARTIST,
    YT_ITEM_CATEGORY,
} yt_item_type_t;

/* Offsets into the page's arena; 0 is the empty string. */
typedef uint32_t yt_page_str_t;

typedef struct {
    uint8_t type;       /* yt_item_type_t */
    uint8_t shelf;      /* index of its shelf */
    int32_t duration_s; /* a song's; -1 when unknown */
    char video_id[YT_BROWSE_VIDEO_ID_LEN + 1]; /* a song's; "" otherwise */
    yt_page_str_t title;
    yt_page_str_t subtitle; /* album, playlist, artist, category */
    yt_page_str_t artist;   /* song */
    yt_page_str_t album;    /* song */
    yt_page_str_t id;       /* album, playlist, artist, category: its browse id */
    yt_page_str_t params;   /* category */
    yt_page_str_t artist_id;
    yt_page_str_t album_id;
} yt_page_item_t;

typedef struct {
    yt_page_str_t title;
    uint16_t first; /* index of its first item */
    uint16_t count;
} yt_page_shelf_t;

typedef struct {
    yt_page_str_t title;
    yt_page_str_t token; /* next page of a playlist; 0 when none */
    size_t shelf_count;
    size_t item_count;
    yt_page_shelf_t * shelves; /* in the page's block */
    yt_page_item_t * items;    /* in the page's block, shelf by shelf */
    char * arena;
    size_t arena_len;
} yt_page_t;

typedef enum {
    YT_BROWSE_LINE_OK,        /* *out is a page (shelves may be 0) */
    YT_BROWSE_LINE_FAILED,    /* the program's failure line; fail_class is set */
    YT_BROWSE_LINE_MALFORMED, /* not the contract */
    YT_BROWSE_LINE_NO_MEMORY, /* a block could not be allocated */
} yt_browse_line_status_t;

/* Decodes the line (len bytes, with or without its final newline). On OK
 * *out gets a page the caller frees with yt_page_free(); otherwise *out is
 * NULL. fail_class (YT_PAGE_CLASS_MAX + 1 bytes, may be NULL) gets the
 * program's class on FAILED and "" otherwise. */
yt_browse_line_status_t yt_browse_line_parse(const char * line, size_t len, yt_page_t ** out, char * fail_class);

/* The text at offset s of page ("" for 0 or an offset past the arena). */
const char * yt_page_str(const yt_page_t * page, yt_page_str_t s);

/* Frees the page and its arena. NULL is fine. */
void yt_page_free(yt_page_t * page);

/* «Más» of a playlist (spec youtube/music-screen): appends to the last shelf
 * of *page the songs of more whose videoId it does not have yet, in order,
 * until it holds max_items items, and takes more's token. When nothing new
 * comes, the token is dropped, so «Más» goes away (a playlist YouTube Music
 * makes answers its first page again; design D6). *page is replaced by a new
 * page and the old one freed; more is left to the caller. Returns how many
 * songs were added, or -1 when memory runs out (then *page is unchanged). */
int yt_page_append_songs(yt_page_t ** page, const yt_page_t * more, size_t max_items);

#ifdef HOST_BUILD
/* PC tests only: the allocator of both blocks (NULL: malloc). */
void yt_browse_line_set_test_alloc(void * (*alloc)(size_t));
#endif

#endif /* YT_BROWSE_LINE_H */
