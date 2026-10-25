#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif
#include "yt_search_line.h"

#include <string.h>

#include "cJSON.h"

static bool video_id_valid(const char * id) {
    size_t n = 0;
    for (; id[n]; n++) {
        char c = id[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || n >= YT_SONG_VIDEO_ID_LEN) return false;
    }
    return n == YT_SONG_VIDEO_ID_LEN;
}

static bool class_valid(const char * c) {
    size_t n = 0;
    for (; c[n]; n++) {
        if (!((c[n] >= 'a' && c[n] <= 'z') || c[n] == '_') || n >= YT_SEARCH_LINE_CLASS_MAX) return false;
    }
    return n > 0;
}

/* Copies a text field of at most YT_SONG_TEXT_MAX bytes with no control
 * character (the program removes them) into dst. */
static bool copy_text(char * dst, const cJSON * item, bool may_be_empty) {
    if (!cJSON_IsString(item)) return false;
    const char * s = item->valuestring;
    size_t n = strlen(s);
    if (n > YT_SONG_TEXT_MAX || (n == 0 && !may_be_empty)) return false;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char) s[i] < 0x20 || s[i] == 0x7F) return false;
    }
    memcpy(dst, s, n + 1);
    return true;
}

/* An optional browse id: absent leaves dst "", anything but a valid id is
 * malformed. */
static bool copy_id(char * dst, const cJSON * item) {
    if (!item) return true;
    if (!cJSON_IsString(item) || !yt_browse_id_valid(item->valuestring)) return false;
    strcpy(dst, item->valuestring);
    return true;
}

static bool parse_song(const cJSON * r, yt_song_t * song) {
    if (!cJSON_IsObject(r)) return false;
    const cJSON * id = cJSON_GetObjectItemCaseSensitive(r, "video_id");
    const cJSON * album = cJSON_GetObjectItemCaseSensitive(r, "album");
    const cJSON * dur = cJSON_GetObjectItemCaseSensitive(r, "duration_s");
    if (!cJSON_IsString(id) || !video_id_valid(id->valuestring)) return false;
    strcpy(song->video_id, id->valuestring);
    if (!copy_text(song->title, cJSON_GetObjectItemCaseSensitive(r, "title"), false)) return false;
    if (!copy_text(song->artist, cJSON_GetObjectItemCaseSensitive(r, "artist"), true)) return false;
    if (album && !copy_text(song->album, album, true)) return false;
    if (!cJSON_IsNumber(dur)) return false;
    double d = dur->valuedouble;
    if (!(d >= 0 && d <= YT_SONG_DURATION_MAX_S) || d != (double) (uint32_t) d) return false;
    song->duration_ms = (uint32_t) d * 1000u;
    return copy_id(song->artist_id, cJSON_GetObjectItemCaseSensitive(r, "artist_id")) &&
           copy_id(song->album_id, cJSON_GetObjectItemCaseSensitive(r, "album_id"));
}

static yt_search_line_status_t parse_ok(const cJSON * root, yt_search_line_page_t * page) {
    const cJSON * results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON * t = cJSON_GetObjectItemCaseSensitive(root, "continuation");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) > YT_SEARCH_LINE_MAX_SONGS) {
        return YT_SEARCH_LINE_MALFORMED;
    }
    if (cJSON_IsString(t)) {
        if (!yt_search_token_valid(t->valuestring)) return YT_SEARCH_LINE_MALFORMED;
        strcpy(page->token, t->valuestring);
    } else if (!cJSON_IsNull(t)) {
        return YT_SEARCH_LINE_MALFORMED;
    }
    const cJSON * r;
    cJSON_ArrayForEach(r, results) {
        if (!parse_song(r, &page->songs[page->count])) return YT_SEARCH_LINE_MALFORMED;
        page->count++;
    }
    return YT_SEARCH_LINE_OK;
}

yt_search_line_status_t yt_search_line_parse(const char * line, size_t len, yt_search_line_page_t * page) {
    if (!page) return YT_SEARCH_LINE_MALFORMED;
    memset(page, 0, sizeof(*page));
    if (!line || len == 0) return YT_SEARCH_LINE_MALFORMED;
    size_t body = len;
    if (line[body - 1] == '\n') body--;
    if (body == 0 || memchr(line, '\0', body) || memchr(line, '\n', body)) return YT_SEARCH_LINE_MALFORMED;
    /* cJSON ends a string early at an escaped \u0000. */
    if (memmem(line, body, "\\u0000", 6)) return YT_SEARCH_LINE_MALFORMED;

    const char * end = NULL;
    cJSON * root = cJSON_ParseWithLengthOpts(line, body, &end, 0);
    yt_search_line_status_t st = YT_SEARCH_LINE_MALFORMED;
    bool tail_ok = end != NULL;
    for (const char * p = end; tail_ok && p < line + body; p++) tail_ok = *p == ' ' || *p == '\t' || *p == '\r';
    if (cJSON_IsObject(root) && tail_ok) {
        const cJSON * ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
        const cJSON * cls = cJSON_GetObjectItemCaseSensitive(root, "class");
        if (cJSON_IsTrue(ok)) {
            st = parse_ok(root, page);
        } else if (cJSON_IsFalse(ok) && cJSON_IsString(cls) && class_valid(cls->valuestring)) {
            strcpy(page->fail_class, cls->valuestring);
            st = YT_SEARCH_LINE_FAILED;
        }
    }
    cJSON_Delete(root);
    if (st == YT_SEARCH_LINE_MALFORMED) memset(page, 0, sizeof(*page));
    return st;
}
