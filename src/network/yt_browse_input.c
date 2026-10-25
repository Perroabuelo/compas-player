#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif
#include "yt_browse_input.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static const char * const kind_names[YT_BROWSE_KIND_COUNT] = {
    "home", "explore", "moods", "charts", "category", "artist", "album", "playlist", "related",
};

const char * yt_browse_kind_name(yt_browse_kind_t kind) {
    if ((int) kind < 0 || kind >= YT_BROWSE_KIND_COUNT) return NULL;
    return kind_names[kind];
}

bool yt_browse_kind_parse(const char * name, yt_browse_kind_t * out) {
    if (!name) return false;
    for (int k = 0; k < YT_BROWSE_KIND_COUNT; k++) {
        if (strcmp(name, kind_names[k]) == 0) {
            if (out) *out = (yt_browse_kind_t) k;
            return true;
        }
    }
    return false;
}

static bool id_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool chars_valid(const char * s, size_t max) {
    if (!s || !s[0]) return false;
    for (size_t i = 0; s[i]; i++) {
        if (i >= max || !(id_char(s[i]) || s[i] == '%' || s[i] == '=')) return false;
    }
    return true;
}

bool yt_browse_id_valid(const char * s) {
    return chars_valid(s, YT_BROWSE_ID_MAX);
}

bool yt_browse_params_valid(const char * s) {
    return chars_valid(s, YT_BROWSE_PARAMS_MAX);
}

bool yt_browse_video_id_valid(const char * s) {
    if (!s) return false;
    size_t i = 0;
    for (; s[i]; i++) {
        if (i >= YT_BROWSE_VIDEO_ID_LEN || !id_char(s[i])) return false;
    }
    return i == YT_BROWSE_VIDEO_ID_LEN;
}

bool yt_browse_fields_valid(yt_browse_kind_t kind, const char * id, const char * params, const char * token) {
    bool has_id = id && id[0], has_params = params && params[0], has_token = token && token[0];
    switch (kind) {
    case YT_BROWSE_HOME:
    case YT_BROWSE_EXPLORE:
    case YT_BROWSE_MOODS:
    case YT_BROWSE_CHARTS:
        return !has_id && !has_params && !has_token;
    case YT_BROWSE_CATEGORY:
        return yt_browse_id_valid(id) && yt_browse_params_valid(params) && !has_token;
    case YT_BROWSE_ARTIST:
    case YT_BROWSE_ALBUM:
        return yt_browse_id_valid(id) && !has_params && !has_token;
    case YT_BROWSE_PLAYLIST:
        return yt_browse_id_valid(id) && !has_params && (!has_token || yt_search_token_valid(token));
    case YT_BROWSE_RELATED:
        return yt_browse_video_id_valid(id) && !has_params && !has_token;
    default:
        return false;
    }
}

/* The string of item into dst (size bytes), "" when item is absent or null.
 * False when item is anything else or does not fit. */
static bool optional_string(const cJSON * item, char * dst, size_t size) {
    dst[0] = '\0';
    if (!item || cJSON_IsNull(item)) return true;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= size) return false;
    strcpy(dst, item->valuestring);
    return true;
}

bool yt_browse_input_parse(const char * line, size_t len, yt_browse_input_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!line || len == 0 || len > YT_BROWSE_INPUT_MAX) return false;
    if (memchr(line, '\0', len)) return false;
    size_t body = len;
    if (line[body - 1] == '\n') body--;
    if (body == 0 || memchr(line, '\n', body)) return false;
    /* cJSON decodes an escaped \u0000 into a string that ends early, which
     * would hide what follows it from the checks below. */
    if (memmem(line, body, "\\u0000", 6)) return false;

    const char * end = NULL;
    cJSON * root = cJSON_ParseWithLengthOpts(line, body, &end, 0);
    bool ok = false;
    /* Nothing but blanks may follow the object. */
    bool tail_ok = end != NULL;
    for (const char * p = end; tail_ok && p < line + body; p++) tail_ok = *p == ' ' || *p == '\t' || *p == '\r';
    if (cJSON_IsObject(root) && tail_ok) {
        bool keys_ok = true;
        for (const cJSON * it = root->child; it && keys_ok; it = it->next) {
            keys_ok = it->string && (strcmp(it->string, "kind") == 0 || strcmp(it->string, "id") == 0 ||
                                     strcmp(it->string, "params") == 0 || strcmp(it->string, "token") == 0);
        }
        const cJSON * kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
        ok = keys_ok && cJSON_IsString(kind) && yt_browse_kind_parse(kind->valuestring, &out->kind) &&
             optional_string(cJSON_GetObjectItemCaseSensitive(root, "id"), out->id, sizeof(out->id)) &&
             optional_string(cJSON_GetObjectItemCaseSensitive(root, "params"), out->params, sizeof(out->params)) &&
             optional_string(cJSON_GetObjectItemCaseSensitive(root, "token"), out->token, sizeof(out->token)) &&
             yt_browse_fields_valid(out->kind, out->id, out->params, out->token);
    }
    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

size_t yt_browse_input_line(char * out, size_t size, yt_browse_kind_t kind, const char * id, const char * params,
                            const char * token) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!yt_browse_fields_valid(kind, id, params, token)) return 0;
    /* Valid fields have only [A-Za-z0-9_%=-]: nothing to escape. */
    const char * f[3] = {id, params, token};
    char q[3][YT_BROWSE_TOKEN_MAX + 3];
    for (int i = 0; i < 3; i++) {
        if (f[i] && f[i][0])
            snprintf(q[i], sizeof(q[i]), "\"%s\"", f[i]);
        else
            strcpy(q[i], "null");
    }
    int n = snprintf(out, size, "{\"kind\":\"%s\",\"id\":%s,\"params\":%s,\"token\":%s}\n", kind_names[kind], q[0], q[1],
                     q[2]);
    if (n < 0 || (size_t) n >= size) {
        memset(out, 0, size);
        return 0;
    }
    return (size_t) n;
}
