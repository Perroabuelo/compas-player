#ifndef YT_JOB_ADMIT_H
#define YT_JOB_ADMIT_H

/* Which YouTube job may start (hiby-yt, design D5 of search-youtube-music).
 * Pure: the resolver service asks this and acts on the answer.
 *
 * The player runs one YouTube job at a time: a resolution (plugin, open,
 * prefetch, refresh; yt_resolve_policy.h) or a search. A search request
 * gets:
 *
 *   running  | none  | prefetch       | open | refresh | plugin | search
 *   search   | start | cancel + start | busy | busy    | busy   | busy
 *
 * The user's search goes before a prefetch, which is asked again on a later
 * tick; the track that plays goes before the search. A resolution asked
 * while a search runs gets:
 *
 *   request  | plugin | open | refresh | prefetch
 *   (search) | busy   | wait | wait    | skip
 *
 * open and refresh wait like they wait for other jobs: a search takes 1 to
 * 3 s. Every other case is yt_resolve_decide(). */

#include "yt_resolve_policy.h"

yt_req_action_t yt_job_admit(yt_owner_t running, const char * running_id, yt_owner_t request,
                             const char * request_id);

#endif /* YT_JOB_ADMIT_H */
