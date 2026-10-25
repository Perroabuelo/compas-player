#include "yt_mailbox.h"

#include <stdlib.h>
#include <string.h>

/* Ticket numbers keep growing across mailboxes, close and reopen, so a
 * result of a job started before a rebuild never matches one started after
 * it. Only the UI thread asks for tickets. */
static uint32_t next_ticket;

void yt_mailbox_init(yt_mailbox_t * m, yt_mailbox_free_fn free_payload) {
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->lock, NULL);
    m->free_payload = free_payload ? free_payload : free;
}

/* Under lock. */
static void drop_locked(yt_mailbox_t * m) {
    if (m->payload) m->free_payload(m->payload);
    m->payload = NULL;
    m->ready = false;
    m->fail_class[0] = '\0';
}

uint32_t yt_mailbox_expect(yt_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    if (++next_ticket == 0) next_ticket = 1;
    m->ticket = next_ticket;
    uint32_t t = m->ticket;
    pthread_mutex_unlock(&m->lock);
    return t;
}

void yt_mailbox_forget(yt_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    m->ticket = 0;
    pthread_mutex_unlock(&m->lock);
}

void yt_mailbox_forget_ticket(yt_mailbox_t * m, uint32_t ticket) {
    pthread_mutex_lock(&m->lock);
    if (ticket != 0 && m->ticket == ticket) {
        drop_locked(m);
        m->ticket = 0;
    }
    pthread_mutex_unlock(&m->lock);
}

bool yt_mailbox_deliver(yt_mailbox_t * m, uint32_t ticket, void * payload, const char * fail_class) {
    pthread_mutex_lock(&m->lock);
    bool wanted = !m->closed && ticket != 0 && ticket == m->ticket && !m->ready;
    if (wanted) {
        m->payload = payload;
        m->ready = true;
        if (payload) {
            m->fail_class[0] = '\0';
        } else {
            strncpy(m->fail_class, fail_class ? fail_class : "internal", YT_MAILBOX_CLASS_MAX);
            m->fail_class[YT_MAILBOX_CLASS_MAX] = '\0';
        }
    }
    yt_mailbox_free_fn free_payload = m->free_payload;
    pthread_mutex_unlock(&m->lock);
    if (!wanted && payload) free_payload(payload);
    return wanted;
}

bool yt_mailbox_take(yt_mailbox_t * m, void ** payload, char * fail_class, uint32_t * ticket) {
    pthread_mutex_lock(&m->lock);
    bool ready = m->ready && !m->closed;
    if (ready) {
        *payload = m->payload;
        memcpy(fail_class, m->fail_class, sizeof(m->fail_class));
        if (ticket) *ticket = m->ticket;
        m->payload = NULL;
        m->ready = false;
        m->ticket = 0;
    }
    pthread_mutex_unlock(&m->lock);
    return ready;
}

bool yt_mailbox_waiting(yt_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    bool waiting = !m->closed && m->ticket != 0;
    pthread_mutex_unlock(&m->lock);
    return waiting;
}

void yt_mailbox_close(yt_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    m->ticket = 0;
    m->closed = true;
    pthread_mutex_unlock(&m->lock);
}

void yt_mailbox_reopen(yt_mailbox_t * m) {
    pthread_mutex_lock(&m->lock);
    drop_locked(m);
    m->ticket = 0;
    m->closed = false;
    pthread_mutex_unlock(&m->lock);
}
