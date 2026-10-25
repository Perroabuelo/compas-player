#include "yt_search_mailbox.h"

#include <stdlib.h>

_Static_assert(YT_SEARCH_LINE_CLASS_MAX == YT_MAILBOX_CLASS_MAX, "a search's class fits the mailbox's");

void yt_search_mailbox_init(yt_search_mailbox_t * m) {
    yt_mailbox_init(m, free);
}

uint32_t yt_search_mailbox_expect(yt_search_mailbox_t * m) {
    return yt_mailbox_expect(m);
}

void yt_search_mailbox_forget(yt_search_mailbox_t * m) {
    yt_mailbox_forget(m);
}

bool yt_search_mailbox_deliver(yt_search_mailbox_t * m, uint32_t ticket, yt_search_line_page_t * page,
                               const char * fail_class) {
    return yt_mailbox_deliver(m, ticket, page, fail_class);
}

bool yt_search_mailbox_take(yt_search_mailbox_t * m, yt_search_line_page_t ** page, char * fail_class) {
    void * payload = NULL;
    bool ready = yt_mailbox_take(m, &payload, fail_class, NULL);
    if (ready) *page = payload;
    return ready;
}

bool yt_search_mailbox_waiting(yt_search_mailbox_t * m) {
    return yt_mailbox_waiting(m);
}

void yt_search_mailbox_close(yt_search_mailbox_t * m) {
    yt_mailbox_close(m);
}

void yt_search_mailbox_reopen(yt_search_mailbox_t * m) {
    yt_mailbox_reopen(m);
}
