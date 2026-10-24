#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif
#include "yt_search_input.h"

#include <string.h>

#include "cJSON.h"

/* Length of the UTF-8 sequence at s (at most n bytes), or 0 when it is not
 * a valid, shortest-form encoding of a code point up to U+10FFFF other
 * than a surrogate. *cp gets the code point. */
static size_t utf8_seq(const unsigned char * s, size_t n, unsigned long * cp) {
    unsigned char c = s[0];
    size_t len;
    unsigned long v, min;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2, v = c & 0x1F, min = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3, v = c & 0x0F, min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4, v = c & 0x07, min = 0x10000;
    } else {
        return 0;
    }
    if (len > n) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return len;
}

bool yt_search_query_valid(const char * q) {
    if (!q) return false;
    size_t n = strlen(q);
    if (n == 0 || n > YT_SEARCH_QUERY_MAX) return false;
    const unsigned char * s = (const unsigned char *) q;
    for (size_t i = 0; i < n;) {
        unsigned long cp;
        size_t len = utf8_seq(s + i, n - i, &cp);
        if (len == 0) return false;
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return false;
        i += len;
    }
    return true;
}

bool yt_search_token_valid(const char * t) {
    if (!t || !t[0]) return false;
    size_t i = 0;
    for (; t[i]; i++) {
        char c = t[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '%' || c == '=';
        if (!ok || i >= YT_SEARCH_TOKEN_MAX) return false;
    }
    return true;
}

bool yt_search_input_parse(const char * line, size_t len, yt_search_input_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!line || len == 0 || len > YT_SEARCH_INPUT_MAX) return false;
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
        const cJSON * q = cJSON_GetObjectItemCaseSensitive(root, "query");
        const cJSON * t = cJSON_GetObjectItemCaseSensitive(root, "continuation");
        bool token_ok = !t || cJSON_IsNull(t) || (cJSON_IsString(t) && yt_search_token_valid(t->valuestring));
        if (cJSON_IsString(q) && yt_search_query_valid(q->valuestring) && token_ok) {
            strcpy(out->query, q->valuestring);
            if (cJSON_IsString(t)) strcpy(out->token, t->valuestring);
            ok = true;
        }
    }
    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

size_t yt_search_input_line(char * out, size_t size, const char * query, const char * token) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    bool has_token = token && token[0];
    if (!yt_search_query_valid(query) || (has_token && !yt_search_token_valid(token))) return 0;

    static const char head[] = "{\"query\":\"";
    size_t n = 0;
#define PUT(s, l)                                \
    do {                                         \
        if (n + (l) >= size) goto too_small;     \
        memcpy(out + n, (s), (l));               \
        n += (l);                                \
    } while (0)
    PUT(head, sizeof(head) - 1);
    /* A valid query has no control character, so only '"' and '\\' need
     * escaping; UTF-8 goes through as is. */
    for (const char * p = query; *p; p++) {
        if (*p == '"' || *p == '\\') PUT("\\", 1);
        PUT(p, 1);
    }
    if (has_token) {
        PUT("\",\"continuation\":\"", 18);
        PUT(token, strlen(token));
        PUT("\"}\n", 3);
    } else {
        PUT("\",\"continuation\":null}\n", 23);
    }
#undef PUT
    out[n] = '\0';
    return n;

too_small:
    memset(out, 0, size);
    return 0;
}
