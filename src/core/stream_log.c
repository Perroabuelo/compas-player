#include "stream_log.h"

#include "db_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define STREAM_LOG_SUBDIR "/.hiby-yt"
#define STREAM_LOG_PATH_MAX 256

static pthread_mutex_t stream_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char stream_log_root[STREAM_LOG_PATH_MAX] = STREAM_LOG_DEFAULT_ROOT;
static bool stream_log_require_mount = STREAM_LOG_DEFAULT_REQUIRE_MOUNT;
static atomic_uint stream_log_sid;

void stream_log_url_host(const char * url, char * out, size_t size) {
    if (!out || size == 0) return;
    out[0] = '\0';
    if (!url) url = "";

    const char * start = strstr(url, "://");
    start = start ? start + 3 : url;
    size_t authority_len = strcspn(start, "/?#");
    const char * at = memchr(start, '@', authority_len);
    if (at) {
        authority_len -= (size_t) (at + 1 - start);
        start = at + 1;
    }

    size_t host_len;
    if (authority_len > 0 && start[0] == '[') {
        const char * close = memchr(start, ']', authority_len);
        host_len = close ? (size_t) (close + 1 - start) : authority_len;
    } else {
        const char * colon = memchr(start, ':', authority_len);
        host_len = colon ? (size_t) (colon - start) : authority_len;
    }

    if (host_len == 0) {
        snprintf(out, size, "-");
        return;
    }
    if (host_len >= size) host_len = size - 1;
    memcpy(out, start, host_len);
    out[host_len] = '\0';
}

static void stream_log_sanitize(char * s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c < 0x20 || c == 0x7f) s[i] = '_';
    }
}

size_t stream_log_format_line(char * out, size_t size, time_t now, unsigned sid,
                              const char * event, const char * fields) {
    if (!out || size < 2) return 0;
    struct tm tm;
    char stamp[32];
    if (!gmtime_r(&now, &tm) || strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        snprintf(stamp, sizeof(stamp), "-");

    int prefix = snprintf(out, size, "%s sid=%u event=", stamp, sid);
    if (prefix < 0) prefix = 0;
    size_t len = (size_t) prefix < size - 1 ? (size_t) prefix : size - 2;
    size_t body = len;
    int n = snprintf(out + len, size - 1 - len, "%s%s%s", event ? event : "-",
                     fields && fields[0] ? " " : "", fields ? fields : "");
    if (n > 0) len += (size_t) n < size - 1 - len ? (size_t) n : size - 2 - len;
    stream_log_sanitize(out + body, len - body);
    out[len++] = '\n';
    out[len] = '\0';
    return len;
}

void stream_log_configure(const char * root, bool require_mount) {
    pthread_mutex_lock(&stream_log_mutex);
    snprintf(stream_log_root, sizeof(stream_log_root), "%s", root ? root : STREAM_LOG_DEFAULT_ROOT);
    stream_log_require_mount = require_mount;
    pthread_mutex_unlock(&stream_log_mutex);
}

unsigned stream_log_next_sid(void) {
    return atomic_fetch_add(&stream_log_sid, 1) + 1;
}

/* A mount point lives on a different device than its parent directory. */
static bool stream_log_is_mount_point(const char * root) {
    char parent[STREAM_LOG_PATH_MAX + 4];
    struct stat st_root, st_parent;
    snprintf(parent, sizeof(parent), "%s/..", root);
    if (stat(root, &st_root) != 0 || stat(parent, &st_parent) != 0) return false;
    return st_root.st_dev != st_parent.st_dev || st_root.st_ino == st_parent.st_ino;
}

void stream_log_event(unsigned sid, const char * event, const char * fmt, ...) {
    char fields[STREAM_LOG_LINE_MAX];
    char line[STREAM_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fields, sizeof(fields), fmt ? fmt : "", ap);
    va_end(ap);
    size_t len = stream_log_format_line(line, sizeof(line), time(NULL), sid, event, fields);

    char dir[STREAM_LOG_PATH_MAX + sizeof(STREAM_LOG_SUBDIR)];
    char path[sizeof(dir) + 16];
    char rotated[sizeof(dir) + 16];
    pthread_mutex_lock(&stream_log_mutex);
    if (stream_log_require_mount && !stream_log_is_mount_point(stream_log_root)) goto out;
    snprintf(dir, sizeof(dir), "%s" STREAM_LOG_SUBDIR, stream_log_root);
    snprintf(path, sizeof(path), "%s/stream.log", dir);
    snprintf(rotated, sizeof(rotated), "%s/stream.log.1", dir);
    (void) mkdir(dir, 0755);

    FILE * file = fopen(path, "a");
    if (!file) goto out;
    /* Rotate when this line would take the file to the limit, so the
     * current file always stays below it. */
    db_log_rotate_file_if_needed_locked(&file, path, rotated, STREAM_LOG_MAX_BYTES - (long) len);
    if (file) {
        fwrite(line, 1, len, file);
        fclose(file);
    }
out:
    pthread_mutex_unlock(&stream_log_mutex);
}
