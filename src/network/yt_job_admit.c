#include "yt_job_admit.h"

yt_req_action_t yt_job_admit(yt_owner_t running, const char * running_id, yt_owner_t request,
                             const char * request_id) {
    if (request == YT_OWNER_SEARCH) {
        switch (running) {
            case YT_OWNER_NONE:
                return YT_REQ_START;
            case YT_OWNER_PREFETCH:
                return YT_REQ_CANCEL_AND_START;
            default:
                return YT_REQ_BUSY;
        }
    }
    if (running == YT_OWNER_SEARCH) {
        switch (request) {
            case YT_OWNER_OPEN:
            case YT_OWNER_REFRESH:
                return YT_REQ_WAIT;
            case YT_OWNER_PREFETCH:
                return YT_REQ_SKIP;
            default:
                return YT_REQ_BUSY;
        }
    }
    return yt_resolve_decide(running, running_id, request, request_id);
}
