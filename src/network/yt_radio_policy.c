#include "yt_radio_policy.h"

#include <string.h>

void yt_radio_reset(yt_radio_state_t * st) {
    memset(st, 0, sizeof(*st));
}

bool yt_radio_should_start(yt_radio_state_t * st, const yt_radio_input_t * in) {
    const char * cur = in->current_video_id;
    if (!cur || !cur[0] || strlen(cur) >= sizeof(st->video_id)) {
        yt_radio_reset(st);
        return false;
    }
    if (strcmp(st->video_id, cur) != 0) {
        yt_radio_reset(st);
        memcpy(st->video_id, cur, strlen(cur) + 1);
    }
    if (!in->enabled || in->has_next || in->queue_count >= YT_RADIO_QUEUE_MAX) return false;
    if (in->service_busy || in->played_ms < 0 || st->starts >= YT_RADIO_MAX_STARTS) return false;

    bool near_end = in->remaining_ms >= 0 && in->remaining_ms <= YT_RADIO_BEFORE_END_MS;
    bool due = st->starts == 0 ? (in->played_ms >= YT_RADIO_AFTER_MS || near_end) : near_end;
    if (!due) return false;
    st->starts++;
    return true;
}

void yt_radio_finished(yt_radio_state_t * st, const char * video_id, bool answered) {
    if (answered && video_id && strcmp(st->video_id, video_id) == 0) st->starts = YT_RADIO_MAX_STARTS;
}

void yt_radio_uncount(yt_radio_state_t * st, const char * video_id) {
    if (video_id && st->starts > 0 && strcmp(st->video_id, video_id) == 0) st->starts--;
}
