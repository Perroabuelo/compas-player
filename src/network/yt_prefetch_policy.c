#include "yt_prefetch_policy.h"

#include <string.h>

void yt_prefetch_reset(yt_prefetch_state_t * st) {
    memset(st, 0, sizeof(*st));
}

bool yt_prefetch_should_start(yt_prefetch_state_t * st, const yt_prefetch_input_t * in) {
    const char * next = in->next_video_id;
    if (!next || !next[0] || strlen(next) >= sizeof(st->video_id)) {
        yt_prefetch_reset(st);
        return false;
    }
    if (strcmp(st->video_id, next) != 0) {
        yt_prefetch_reset(st);
        memcpy(st->video_id, next, strlen(next) + 1);
    }
    if (in->next_cached || in->service_busy || in->played_ms < 0) return false;
    if (st->starts >= YT_PREFETCH_MAX_STARTS) return false;

    bool near_end = in->remaining_ms >= 0 && in->remaining_ms <= YT_PREFETCH_BEFORE_END_MS;
    bool due = st->starts == 0 ? (in->played_ms >= YT_PREFETCH_AFTER_MS || near_end) : near_end;
    if (!due) return false;
    st->starts++;
    return true;
}
