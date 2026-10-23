#include "yt_resolve_policy.h"

#include <string.h>

static bool same_id(const char * a, const char * b) {
    return a && b && a[0] && strcmp(a, b) == 0;
}

bool yt_video_id_valid(const char * id) {
    if (!id) return false;
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= 11) return false;
    }
    return n == 11;
}

yt_req_action_t yt_resolve_decide(yt_owner_t running, const char * running_id, yt_owner_t request,
                                  const char * request_id) {
    if (running == YT_OWNER_NONE) return YT_REQ_START;
    if (request == YT_OWNER_PLUGIN) return YT_REQ_BUSY;
    if (request == YT_OWNER_PREFETCH) return YT_REQ_SKIP;
    if (request != YT_OWNER_OPEN && request != YT_OWNER_REFRESH) return YT_REQ_SKIP;

    if (running == YT_OWNER_PLUGIN) return YT_REQ_WAIT;
    if (same_id(running_id, request_id)) return YT_REQ_JOIN;
    switch (running) {
        case YT_OWNER_PREFETCH:
            return YT_REQ_CANCEL_AND_START;
        case YT_OWNER_OPEN:
            return request == YT_OWNER_OPEN ? YT_REQ_CANCEL_AND_START : YT_REQ_WAIT;
        case YT_OWNER_REFRESH:
        default:
            return YT_REQ_WAIT;
    }
}

bool yt_resolve_should_retry(const char * cls, unsigned tries_made) {
    if (!cls || tries_made == 0 || tries_made >= YT_RESOLVE_OPEN_TRIES) return false;
    return strcmp(cls, "po_token_required") == 0 || strcmp(cls, "network") == 0;
}

bool yt_failure_count_add(yt_failure_count_t * fc) {
    if (fc->failed_tracks < YT_RESOLVE_MAX_FAILED_TRACKS) fc->failed_tracks++;
    return fc->failed_tracks >= YT_RESOLVE_MAX_FAILED_TRACKS;
}

void yt_failure_count_clear(yt_failure_count_t * fc) {
    fc->failed_tracks = 0;
}
