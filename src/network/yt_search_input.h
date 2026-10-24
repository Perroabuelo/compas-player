#ifndef YT_SEARCH_INPUT_H
#define YT_SEARCH_INPUT_H

/* What a YouTube Music search accepts (hiby-yt, spec youtube/track-search,
 * design D1, D2 and D6). Pure, shared by the player (plugin.youtube_search
 * checks before it launches anything) and by the resolver's search mode
 * (hiby-yt-resolve --search reads the request from stdin).
 *
 * The request is one JSON line on stdin, never arguments, so the query does
 * not show in ps or in /proc/<pid>/cmdline:
 *
 *   {"query":"<1-200 bytes of UTF-8>","continuation":"<token>"|null}
 *
 * The limits come from docs/mediciones/2026-10-24-busqueda-spike.md: the
 * longest token seen was 592 bytes; the limit is four times that. */

#include <stdbool.h>
#include <stddef.h>

#define YT_SEARCH_QUERY_MAX 200
#define YT_SEARCH_TOKEN_MAX 2368
/* The whole request line, newline included. */
#define YT_SEARCH_INPUT_MAX 4096

typedef struct {
    char query[YT_SEARCH_QUERY_MAX + 1];
    char token[YT_SEARCH_TOKEN_MAX + 1]; /* "" when asking for page 1 */
} yt_search_input_t;

/* True when q is valid UTF-8 (no overlong forms, surrogates or code points
 * past U+10FFFF), 1..YT_SEARCH_QUERY_MAX bytes, with no control character
 * (U+0000-U+001F, U+007F-U+009F). */
bool yt_search_query_valid(const char * q);

/* True when t is 1..YT_SEARCH_TOKEN_MAX characters of [A-Za-z0-9_%=-]. */
bool yt_search_token_valid(const char * t);

/* Parses the request line (len bytes, with or without its final newline):
 * a JSON object with a valid "query" and a "continuation" that is null,
 * absent or a valid token. Rejects anything longer than
 * YT_SEARCH_INPUT_MAX, with a NUL byte, or with more than one line. On
 * success fills *out; on failure *out is zeroed. */
bool yt_search_input_parse(const char * line, size_t len, yt_search_input_t * out);

/* Writes the request line for query and token (NULL or "" for page 1),
 * newline included, NUL-terminated. Returns its length, or 0 when the query
 * or the token is invalid or out is too small. */
size_t yt_search_input_line(char * out, size_t size, const char * query, const char * token);

#endif /* YT_SEARCH_INPUT_H */
