#ifndef YT_MAILBOX_H
#define YT_MAILBOX_H

/* Where the result of a YouTube job waits for the YouTube Music screen
 * (hiby-yt, design D3 of youtube-music-screen, generalized in D6 of
 * youtube-music-browse). Pure: no LVGL.
 *
 * The screen asks for a ticket before it starts a job and passes it to the
 * job's done callback. The callback runs on the service's worker thread: it
 * decodes the line into a payload on the heap (a search page, a content
 * page) and delivers it with its ticket. The UI thread takes the result
 * from its poll. Only the result of the last ticket is kept: one of an
 * older job, of a ticket the screen forgot (the page that asked was
 * closed), or one delivered after the screen closed the mailbox (teardown),
 * is freed on the spot with the mailbox's free function, so a late result
 * never reaches the screen. Leaving the screen does not close the mailbox:
 * the result waits for the next poll. */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* A failure class: [a-z_]. */
#define YT_MAILBOX_CLASS_MAX 23

typedef void (*yt_mailbox_free_fn)(void * payload);

typedef struct {
    pthread_mutex_t lock;
    yt_mailbox_free_fn free_payload;
    uint32_t ticket; /* the job the screen waits for, 0 for none */
    bool closed;
    bool ready;
    void * payload; /* NULL when the job failed */
    char fail_class[YT_MAILBOX_CLASS_MAX + 1];
} yt_mailbox_t;

/* An open, empty mailbox whose payloads free_payload frees. */
void yt_mailbox_init(yt_mailbox_t * m, yt_mailbox_free_fn free_payload);

/* UI thread, before starting a job: drops a result still waiting and
 * returns the ticket of the new job (never 0). */
uint32_t yt_mailbox_expect(yt_mailbox_t * m);

/* UI thread: stops waiting (a job that could not start). */
void yt_mailbox_forget(yt_mailbox_t * m);

/* UI thread: stops waiting for ticket only, if it is still the one waited
 * for (the page that asked for it was closed). */
void yt_mailbox_forget_ticket(yt_mailbox_t * m, uint32_t ticket);

/* Worker thread: delivers the result of the job with this ticket. payload
 * (heap, may be NULL) is taken in any case; fail_class (when payload is
 * NULL) is copied. Returns false when nobody waits for it any more, and
 * then the payload has already been freed. */
bool yt_mailbox_deliver(yt_mailbox_t * m, uint32_t ticket, void * payload, const char * fail_class);

/* UI thread: takes the waiting result, if any. On true, *payload is the
 * caller's to free (NULL for a failure, with its class in fail_class, of
 * YT_MAILBOX_CLASS_MAX + 1 bytes), and *ticket (may be NULL) is its ticket. */
bool yt_mailbox_take(yt_mailbox_t * m, void ** payload, char * fail_class, uint32_t * ticket);

/* True while a job with a ticket has not been taken yet. */
bool yt_mailbox_waiting(yt_mailbox_t * m);

/* Teardown: frees a waiting result and refuses later ones. */
void yt_mailbox_close(yt_mailbox_t * m);

/* After a rebuild: open again, empty. */
void yt_mailbox_reopen(yt_mailbox_t * m);

#endif /* YT_MAILBOX_H */
