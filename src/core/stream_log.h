#ifndef STREAM_LOG_H
#define STREAM_LOG_H

/* Always-on log of network stream events (hiby-yt). One line per event,
 * appended to <SD>/.hiby-yt/stream.log and rotated to stream.log.1 before a
 * line would take the file to STREAM_LOG_MAX_BYTES:
 *
 *   2026-10-20T23:48:01Z sid=3 event=open host=example.com container=fmp4
 *
 * sid identifies one stream open. Values must not contain spaces; field
 * names are only ever added, never renamed or removed. URLs are never
 * logged: stream_log_url_host() keeps only the host, since stream URLs can
 * carry temporary credentials in their path or query.
 *
 * Each event opens, appends and closes the file under a mutex, so nothing
 * stays open between events. When the card is not mounted the event is
 * dropped silently. */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define STREAM_LOG_MAX_BYTES (256L * 1024)
#define STREAM_LOG_LINE_MAX 512

#ifdef HOST_BUILD
#define STREAM_LOG_DEFAULT_ROOT "."
#define STREAM_LOG_DEFAULT_REQUIRE_MOUNT false
#else
#define STREAM_LOG_DEFAULT_ROOT "/data/mnt/sd_0"
#define STREAM_LOG_DEFAULT_REQUIRE_MOUNT true
#endif

/* Host of url, without scheme, user info, port, path, query or fragment.
 * Writes "-" when there is none. Always NUL-terminates out (size > 0). */
void stream_log_url_host(const char * url, char * out, size_t size);

/* Formats one log line ("<UTC ISO-8601> sid=<n> event=<event> <fields>\n")
 * into out. Newlines and other control characters in event or fields are
 * replaced with '_'; a line longer than size is cut and still ends in '\n'.
 * Returns the line length. */
size_t stream_log_format_line(char * out, size_t size, time_t now, unsigned sid,
                              const char * event, const char * fields);

/* Log directory root (the SD card mount point by default). With
 * require_mount, events are dropped unless root is a mount point, so an
 * unmounted card never makes the log land on internal storage. */
void stream_log_configure(const char * root, bool require_mount);

/* A new stream id, for one stream open. */
unsigned stream_log_next_sid(void);

void stream_log_event(unsigned sid, const char * event, const char * fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* STREAM_LOG_H */
