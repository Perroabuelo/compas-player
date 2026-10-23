#include "remote_youtube.h"

#include "yt_resolve_policy.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

bool remote_youtube_is(const remote_track_meta_t * t) {
    return t && strcmp(t->provider, REMOTE_YOUTUBE_PROVIDER) == 0;
}

const char * remote_youtube_normalize(remote_track_meta_t * t) {
    if (!yt_video_id_valid(t->track_id)) return "track_id must be a YouTube video id ([A-Za-z0-9_-]{11})";
    if (t->codec[0] && strcasecmp(t->codec, "aac") != 0) return "codec must be \"aac\" or omitted for YouTube";
    snprintf(t->codec, sizeof(t->codec), "aac");
    t->verify_tls = true;
    return NULL;
}
