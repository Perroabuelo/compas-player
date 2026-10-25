#ifndef YT_RADIO_H
#define YT_RADIO_H

/* The songs the radio adds to the queue (hiby-yt, spec youtube/radio-queue,
 * design D5 of youtube-music-browse). Pure: no LVGL, no I/O.
 *
 * From a related page (one shelf of songs), the radio takes the songs whose
 * videoId the queue does not have yet, in the order they came, without
 * repeating one, at most YT_RADIO_PER_REQUEST of them and only as many as
 * keep the queue at YT_RADIO_QUEUE_MAX or fewer. */

#include "yt_browse_line.h"
#include "yt_radio_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True when the queue already has a track with this videoId. */
typedef bool (*yt_radio_in_queue_fn)(void * ctx, const char * video_id);

/* Writes into out (YT_RADIO_PER_REQUEST entries) the indices of the items
 * of related to append to a queue of queue_count tracks. Returns how many. */
size_t yt_radio_pick(const yt_page_t * related, yt_radio_in_queue_fn in_queue, void * ctx, size_t queue_count,
                     uint16_t * out);

#endif /* YT_RADIO_H */
