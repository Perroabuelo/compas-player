#include "stream_resume.h"

#include <stddef.h>

int stream_resume_next_delay_ms(unsigned attempt) {
    static const int delays_ms[STREAM_RESUME_MAX_ATTEMPTS] = { 1000, 2000, 4000, 8000, 8000 };
    if (attempt == 0 || attempt > STREAM_RESUME_MAX_ATTEMPTS) return -1;
    return delays_ms[attempt - 1];
}

stream_resume_result_t stream_resume_run(const stream_resume_ops_t * ops, void * ctx, unsigned * attempts) {
    unsigned made = 0;
    stream_resume_result_t result = STREAM_RESUME_FAIL;
    bool refreshed = false, forbidden = false;
    for (unsigned attempt = 1;; attempt++) {
        int delay = stream_resume_next_delay_ms(attempt);
        if (delay < 0) {
            result = STREAM_RESUME_FAIL;
            break;
        }
        if (!ops->wait_ms(ctx, (unsigned) delay)) {
            result = STREAM_RESUME_STOPPED;
            break;
        }
        made = attempt;
        if (ops->refresh && !refreshed && (forbidden || (ops->url_expired && ops->url_expired(ctx)))) {
            refreshed = true;
            result = ops->refresh(ctx);
            if (result == STREAM_RESUME_STOPPED || result == STREAM_RESUME_LENGTH_CHANGED) break;
            if (result != STREAM_RESUME_OK) {
                result = STREAM_RESUME_FAIL;
                continue;
            }
        }
        result = ops->attempt(ctx, attempt);
        if (result == STREAM_RESUME_OK || result == STREAM_RESUME_STOPPED) break;
        forbidden = result == STREAM_RESUME_FORBIDDEN;
    }
    if (attempts) *attempts = made;
    return result;
}
