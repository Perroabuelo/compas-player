#include "yt_radio.h"

#include <string.h>

size_t yt_radio_pick(const yt_page_t * related, yt_radio_in_queue_fn in_queue, void * ctx, size_t queue_count,
                     uint16_t * out) {
    if (!related || queue_count >= YT_RADIO_QUEUE_MAX) return 0;
    size_t room = YT_RADIO_QUEUE_MAX - queue_count;
    size_t max = room < YT_RADIO_PER_REQUEST ? room : YT_RADIO_PER_REQUEST;
    size_t n = 0;
    for (size_t i = 0; i < related->item_count && n < max; i++) {
        const yt_page_item_t * it = &related->items[i];
        if (it->type != YT_ITEM_SONG || (in_queue && in_queue(ctx, it->video_id))) continue;
        bool repeated = false;
        for (size_t k = 0; k < n && !repeated; k++) repeated = strcmp(related->items[out[k]].video_id, it->video_id) == 0;
        if (!repeated) out[n++] = (uint16_t) i;
    }
    return n;
}
