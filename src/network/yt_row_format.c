#include "yt_row_format.h"

#include "utf8_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* U+00B7 MIDDLE DOT between spaces. */
#define SEP " \xC2\xB7 "
#define SEP_LEN 4

size_t yt_row_duration(char * out, size_t size, uint32_t duration_ms) {
    if (!out || size == 0) return 0;
    unsigned s = duration_ms / 1000u;
    int n;
    if (s == 0) {
        n = 0;
        out[0] = '\0';
    } else if (s < 3600u) {
        n = snprintf(out, size, "%u:%02u", s / 60u, s % 60u);
    } else {
        n = snprintf(out, size, "%u:%02u:%02u", s / 3600u, s / 60u % 60u, s % 60u);
    }
    if (n < 0 || (size_t) n >= size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t) n;
}

size_t yt_row_cut(char * out, size_t size, const char * text) {
    if (!out || size == 0) return 0;
    return utf8_truncate_safe_bounded(out, size, text ? text : "", text ? strlen(text) : 0);
}

/* U+2022 BULLET between spaces: YouTube's separator. */
#define YT_SEP " \xE2\x80\xA2 "
#define YT_SEP_LEN 5

/* True when s starts with one of YouTube's type words and its separator. */
static bool starts_with_type(const char * s) {
    static const char * const types[] = {"Album", "Single", "EP", "Playlist", "Artist", "Song", "Video"};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t n = strlen(types[i]);
        if (strncmp(s, types[i], n) == 0 && strncmp(s + n, YT_SEP, YT_SEP_LEN) == 0) return true;
    }
    return false;
}

size_t yt_row_page_subtitle(char * out, size_t size, const char * kind, const char * subtitle) {
    if (!out || size == 0) return 0;
    char line[YT_ROW_SUBTITLE_MAX * 2];
    size_t n = 0;
    const char * sub = subtitle ? subtitle : "";
    if (kind && kind[0] && !starts_with_type(sub)) {
        n = (size_t) snprintf(line, sizeof(line), "%s", kind);
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        if (sub[0] && n + SEP_LEN < sizeof(line)) {
            memcpy(line + n, SEP, SEP_LEN);
            n += SEP_LEN;
        }
    }
    /* YouTube's text, with each " • " as " · " (one byte shorter). */
    for (const char * p = sub; *p && n + 1 < sizeof(line);) {
        if (strncmp(p, YT_SEP, YT_SEP_LEN) == 0) {
            if (n + SEP_LEN >= sizeof(line)) break;
            memcpy(line + n, SEP, SEP_LEN);
            n += SEP_LEN;
            p += YT_SEP_LEN;
        } else {
            line[n++] = *p++;
        }
    }
    line[n] = '\0';
    return yt_row_cut(out, size, line);
}

size_t yt_row_subtitle(char * out, size_t size, const char * artist, const char * album, uint32_t duration_ms) {
    if (!out || size == 0) return 0;
    char dur[16];
    size_t dur_len = yt_row_duration(dur, sizeof(dur), duration_ms);
    bool has_artist = artist && artist[0];
    bool has_album = album && album[0];

    /* Room for artist and album (NUL included): the duration and its
     * separator come off. */
    size_t reserve = dur_len ? dur_len + (has_artist || has_album ? SEP_LEN : 0) : 0;
    size_t room = size > reserve ? size - reserve : 1;
    size_t n = 0;
    if (has_artist) n = yt_row_cut(out, room, artist);
    if (has_album && n + (has_artist ? SEP_LEN : 0) + 1 < room) {
        if (has_artist) {
            memcpy(out + n, SEP, SEP_LEN);
            n += SEP_LEN;
        }
        n += yt_row_cut(out + n, room - n, album);
    }
    out[n] = '\0';
    if (dur_len && n + (n ? SEP_LEN : 0) + dur_len < size) {
        if (n) {
            memcpy(out + n, SEP, SEP_LEN);
            n += SEP_LEN;
        }
        memcpy(out + n, dur, dur_len + 1);
        n += dur_len;
    }
    return n;
}
