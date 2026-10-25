#include "yt_search_mailbox.h"

#include <stdlib.h>
#include <string.h>

/* Ticket numbers keep growing across close and reopen, so a result of a
 * search started before a rebuild never matches one started after it. */
static uint32_t next_ticket;

void yt_search_mailbox_init(yt_search_mailbox_t * m) {
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->lock, NULL);
}

/* Under lock. */
static void drop_locked(yt_search_mailbox_t * m) {
    free(m->page);
    m->page = NULL;
    m->ready = false;
    m->fail_class[0] = '\0';
}

uint32_t yt_search_mailbox_expect(yt_search_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    if (++next_ticket == 0) next_ticket = 1;
    m->ticket = next_ticket;
    uint32_t t = m->ticket;
    pthread_mutex_unlock(&m->lock);
    return t;
}

void yt_search_mailbox_forget(yt_search_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    m->ticket = 0;
    pthread_mutex_unlock(&m->lock);
}

bool yt_search_mailbox_deliver(yt_search_mailbox_t * m, uint32_t ticket, yt_search_line_page_t * page,
                               const char * fail_class) {
    pthread_mutex_lock(&m->lock);
    bool wanted = !m->closed && ticket != 0 && ticket == m->ticket && !m->ready;
    if (wanted) {
        m->page = page;
        m->ready = true;
        if (page) {
            m->fail_class[0] = '\0';
        } else {
            strncpy(m->fail_class, fail_class ? fail_class : "internal", YT_SEARCH_LINE_CLASS_MAX);
            m->fail_class[YT_SEARCH_LINE_CLASS_MAX] = '\0';
        }
    }
    pthread_mutex_unlock(&m->lock);
    if (!wanted) free(page);
    return wanted;
}

bool yt_search_mailbox_take(yt_search_mailbox_t * m, yt_search_line_page_t ** page, char * fail_class) {
    pthread_mutex_lock(&m->lock);
    bool ready = m->ready && !m->closed;
    if (ready) {
        *page = m->page;
        memcpy(fail_class, m->fail_class, sizeof(m->fail_class));
        m->page = NULL;
        m->ready = false;
        m->ticket = 0;
    }
    pthread_mutex_unlock(&m->lock);
    return ready;
}

bool yt_search_mailbox_waiting(yt_search_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    bool waiting = !m->closed && m->ticket != 0;
    pthread_mutex_unlock(&m->lock);
    return waiting;
}

void yt_search_mailbox_close(yt_search_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    m->ticket = 0;
    m->closed = true;
    pthread_mutex_unlock(&m->lock);
}

void yt_search_mailbox_reopen(yt_search_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    m->ticket = 0;
    m->closed = false;
    pthread_mutex_unlock(&m->lock);
}
