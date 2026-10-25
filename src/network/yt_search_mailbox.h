#ifndef YT_SEARCH_MAILBOX_H
#define YT_SEARCH_MAILBOX_H

/* Where a search result waits for the YouTube Music screen (hiby-yt,
 * design D3 of youtube-music-screen). Pure: no LVGL.
 *
 * The screen asks for a ticket before it starts a search and passes it to
 * the search's done callback. The callback runs on the search service's
 * worker thread: it decodes the line into a page on the heap and delivers
 * it with its ticket. The UI thread takes the result from its poll. Only
 * the result of the last ticket is kept: one of an older search, or one
 * delivered after the screen closed the mailbox (teardown), is freed on
 * the spot, so a late result never reaches the screen. Leaving the screen
 * does not close the mailbox: the result waits for the next poll. */

#include "yt_search_line.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    pthread_mutex_t lock;
    uint32_t ticket;      /* the search the screen waits for, 0 for none */
    bool closed;
    bool ready;
    yt_search_line_page_t * page; /* NULL when the search failed */
    char fail_class[YT_SEARCH_LINE_CLASS_MAX + 1];
} yt_search_mailbox_t;

/* An open, empty mailbox. */
void yt_search_mailbox_init(yt_search_mailbox_t * m);

/* UI thread, before starting a search: drops a result still waiting and
 * returns the ticket of the new search (never 0). */
uint32_t yt_search_mailbox_expect(yt_search_mailbox_t * m);

/* UI thread: stops waiting (a search that could not start). */
void yt_search_mailbox_forget(yt_search_mailbox_t * m);

/* Worker thread: delivers the result of the search with this ticket. page
 * (heap, may be NULL) is taken in any case; fail_class (when page is NULL)
 * is copied. Returns false when nobody waits for it any more, and then the
 * page has already been freed. */
bool yt_search_mailbox_deliver(yt_search_mailbox_t * m, uint32_t ticket, yt_search_line_page_t * page,
                               const char * fail_class);

/* UI thread: takes the waiting result, if any. On true, *page is the
 * caller's to free (NULL for a failure, with its class in fail_class, of
 * YT_SEARCH_LINE_CLASS_MAX + 1 bytes). */
bool yt_search_mailbox_take(yt_search_mailbox_t * m, yt_search_line_page_t ** page, char * fail_class);

/* True while a search with a ticket has not been taken yet. */
bool yt_search_mailbox_waiting(yt_search_mailbox_t * m);

/* Teardown: frees a waiting result and refuses later ones. */
void yt_search_mailbox_close(yt_search_mailbox_t * m);

/* After a rebuild: open again, empty. */
void yt_search_mailbox_reopen(yt_search_mailbox_t * m);

#endif /* YT_SEARCH_MAILBOX_H */
