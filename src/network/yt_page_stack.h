#ifndef YT_PAGE_STACK_H
#define YT_PAGE_STACK_H

/* The views of the YouTube Music screen and the pages opened on top of
 * them (hiby-yt, design D5 and D6 of youtube-music-browse). Pure: no LVGL,
 * no I/O; UI thread only.
 *
 * A view shows the shelves of one or more pages in one vertical list: Home
 * has one page, Explore three (explore, moods and charts, in the order they
 * arrive), an album or a playlist one. Its rows are a header for each shelf
 * with a title, then one row per item.
 *
 * The pages the user opens (album, playlist, artist, category) stack on top
 * of the tabs and the search results. At most YTM_PAGES_MAX of them keep
 * their decoded page: opening one more frees the page of the oldest, which
 * stays in the stack marked released, and is asked for again when the user
 * goes back to it. The stack itself holds at most YTM_STACK_DEPTH entries;
 * past that the oldest entry is dropped. Each entry also keeps the screen's
 * own state for it: its scroll position and an opaque pointer to its
 * objects. */

#include "yt_browse_line.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YT_VIEW_MAX_PAGES 3
#define YTM_PAGES_MAX 3
#define YTM_STACK_DEPTH 8

typedef struct {
    yt_page_t * pages[YT_VIEW_MAX_PAGES];
    size_t page_count;
} yt_page_view_t;

/* Frees the view's pages; the view is empty again. */
void yt_page_view_clear(yt_page_view_t * v);

/* Appends page to the view, which takes it. Returns false, and frees the
 * page, when the view is full. */
bool yt_page_view_add(yt_page_view_t * v, yt_page_t * page);

/* Items over the view's pages. */
size_t yt_page_view_items(const yt_page_view_t * v);

typedef struct {
    const yt_page_t * page;
    size_t shelf;               /* index in page->shelves */
    const yt_page_item_t * item; /* NULL for a shelf header */
    size_t item_index;          /* index in page->items */
} yt_page_row_t;

/* Rows of the view: a header per shelf with a title, then its items. */
size_t yt_page_view_rows(const yt_page_view_t * v);

/* The row at index. False past the last row. */
bool yt_page_view_row(const yt_page_view_t * v, size_t index, yt_page_row_t * out);

typedef struct {
    yt_browse_kind_t kind;
    char id[YT_BROWSE_ID_MAX + 1];
    char params[YT_BROWSE_PARAMS_MAX + 1];
    yt_page_view_t view; /* empty while loading, after an error, or released */
    bool released;       /* its page was freed for the limit: ask again */
    uint32_t ticket;     /* the request it waits for (yt_mailbox), 0 for none */
    bool more;           /* that request is the next page of a playlist */
    char error[24];      /* the class of its last failed request, "" for none */
    int32_t scroll;      /* scroll position saved when covered */
    void * ui;           /* the screen's objects for this page */
} yt_page_entry_t;

typedef struct {
    yt_page_entry_t entries[YTM_STACK_DEPTH];
    size_t depth;
} yt_page_stack_t;

/* An empty stack. */
void yt_page_stack_init(yt_page_stack_t * s);

/* Pushes a page of kind with id and params (checked with
 * yt_browse_fields_valid()), waiting to load. When more than YTM_PAGES_MAX
 * entries would keep a page, the oldest one's is freed and it is marked
 * released; *released (may be NULL) gets that entry, or NULL. When the
 * stack is at YTM_STACK_DEPTH, the oldest entry is dropped first and
 * *dropped_ui (may be NULL) gets its ui pointer (else NULL). Returns the new
 * top, or NULL when the fields are invalid. Entries move when the oldest is
 * dropped: pointers to them are valid until the next push. */
yt_page_entry_t * yt_page_stack_push(yt_page_stack_t * s, yt_browse_kind_t kind, const char * id, const char * params,
                                     yt_page_entry_t ** released, void ** dropped_ui);

/* The top entry, or NULL when the stack is empty (a tab or the results
 * show). */
yt_page_entry_t * yt_page_stack_top(yt_page_stack_t * s);

/* Pops the top entry and frees its page. *ui (may be NULL) gets its ui
 * pointer and *ticket (may be NULL) the request it waited for, so the
 * screen deletes its objects and forgets the ticket. Returns false when the
 * stack is empty. */
bool yt_page_stack_pop(yt_page_stack_t * s, void ** ui, uint32_t * ticket);

/* Pages the stack keeps decoded. */
size_t yt_page_stack_loaded(const yt_page_stack_t * s);

/* Gives entry its page (taken), clearing released. Returns false, and frees
 * the page, when the view is full. */
bool yt_page_entry_set_page(yt_page_entry_t * e, yt_page_t * page);

/* Frees every page and empties the stack (reload, teardown). The screen
 * deletes the entries' objects first. */
void yt_page_stack_clear(yt_page_stack_t * s);

#endif /* YT_PAGE_STACK_H */
