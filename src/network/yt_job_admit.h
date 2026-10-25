#ifndef YT_JOB_ADMIT_H
#define YT_JOB_ADMIT_H

/* Which YouTube job may start (hiby-yt, design D5 of search-youtube-music
 * and D4 of youtube-music-browse). Pure: the resolver service asks this and
 * acts on the answer.
 *
 * The player runs one YouTube job at a time: a resolution (plugin, open,
 * prefetch, refresh; yt_resolve_policy.h), a search, a content page the
 * user asked for (browse) or the related songs of the radio. A search, a
 * content page or a radio request gets:
 *
 *   running  | none  | prefetch       | radio          | open/refresh | plugin | search | browse
 *   search   | start | cancel + start | cancel + start | busy         | busy   | busy   | busy
 *   browse   | start | cancel + start | cancel + start | busy         | busy   | busy   | busy
 *   radio    | start | skip           | skip           | skip         | skip   | skip   | skip
 *
 * What the user asks for goes before a prefetch or the radio, which are
 * asked again on a later tick; the track that plays goes before the user's
 * request. The radio waits like a prefetch: it never stops another job. A
 * resolution asked while a search or a content page runs gets:
 *
 *   request           | plugin | open | refresh | prefetch
 *   (search, browse)  | busy   | wait | wait    | skip
 *   (radio)           | busy   | cancel + start  | skip
 *
 * open and refresh wait for a search or a page (1 to 3 s), and stop the
 * radio, which is asked again later. Every other case is
 * yt_resolve_decide(). */

#include "yt_resolve_policy.h"

yt_req_action_t yt_job_admit(yt_owner_t running, const char * running_id, yt_owner_t request,
                             const char * request_id);

#endif /* YT_JOB_ADMIT_H */
