#ifndef REMOTE_YOUTUBE_H
#define REMOTE_YOUTUBE_H

/* Remote tracks of the "youtube" provider (hiby-yt, chain-youtube-queue).
 * Pure. Their track_id is a video id, and their stream_url may be empty:
 * the player then resolves it on the device when the track opens. A
 * stream_url the plugin already resolved is used as given. */

#include "remote_track.h"

#include <stdbool.h>

#define REMOTE_YOUTUBE_PROVIDER "youtube"

/* True for a track of the youtube provider. */
bool remote_youtube_is(const remote_track_meta_t * t);

/* Checks and completes a parsed youtube track: the track_id must be a
 * video id, the codec empty or "aac" (it becomes "aac"), and TLS is always
 * verified. Returns NULL when the track is fine, else a fixed message. */
const char * remote_youtube_normalize(remote_track_meta_t * t);

#endif /* REMOTE_YOUTUBE_H */
