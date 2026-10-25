#ifndef YT_BROWSE_INPUT_H
#define YT_BROWSE_INPUT_H

/* What a YouTube Music content request accepts (hiby-yt, spec
 * youtube/content-browse, design D2 and D3 of youtube-music-browse). Pure,
 * shared by the player (it checks before it launches anything) and by the
 * resolver's content mode (hiby-yt-resolve --browse reads the request from
 * stdin).
 *
 * The request is one JSON line on stdin, never arguments, so identifiers do
 * not show in ps or in /proc/<pid>/cmdline:
 *
 *   {"kind":"<kind>","id":"<id>"|null,"params":"<params>"|null,
 *    "token":"<token>"|null}
 *
 * Each kind has its fields (the spec): home, explore, moods and charts take
 * none; category takes an id and its params; artist, album and playlist take
 * a browse id, and only playlist a continuation token; related takes a
 * videoId. A field a kind does not take must be null or absent, and no other
 * key is accepted.
 *
 * The limits come from docs/mediciones/2026-10-25-browse-spike-2.md: the
 * longest browse id seen was 49 bytes, a category's params 71 and a
 * playlist's token 196. The token limit is the search's. */

#include <stdbool.h>
#include <stddef.h>

#include "yt_search_input.h"

typedef enum {
    YT_BROWSE_HOME = 0,
    YT_BROWSE_EXPLORE,
    YT_BROWSE_MOODS,
    YT_BROWSE_CHARTS,
    YT_BROWSE_CATEGORY,
    YT_BROWSE_ARTIST,
    YT_BROWSE_ALBUM,
    YT_BROWSE_PLAYLIST,
    YT_BROWSE_RELATED,
    YT_BROWSE_KIND_COUNT
} yt_browse_kind_t;

#define YT_BROWSE_ID_MAX 128
#define YT_BROWSE_PARAMS_MAX 256
#define YT_BROWSE_TOKEN_MAX YT_SEARCH_TOKEN_MAX
#define YT_BROWSE_VIDEO_ID_LEN 11
/* The whole request line, newline included. */
#define YT_BROWSE_INPUT_MAX 4096

typedef struct {
    yt_browse_kind_t kind;
    char id[YT_BROWSE_ID_MAX + 1];         /* "" when the kind takes none */
    char params[YT_BROWSE_PARAMS_MAX + 1]; /* "" unless a category */
    char token[YT_BROWSE_TOKEN_MAX + 1];   /* "" unless the next page of a playlist */
} yt_browse_input_t;

/* The name of kind in the request line and in the logs ("home", "album",
 * ...), or NULL when kind is out of range. */
const char * yt_browse_kind_name(yt_browse_kind_t kind);

/* The kind called name. False when there is none. */
bool yt_browse_kind_parse(const char * name, yt_browse_kind_t * out);

/* True when s is 1..YT_BROWSE_ID_MAX characters of [A-Za-z0-9_%=-]. */
bool yt_browse_id_valid(const char * s);

/* True when s is 1..YT_BROWSE_PARAMS_MAX characters of [A-Za-z0-9_%=-]. */
bool yt_browse_params_valid(const char * s);

/* True when s is exactly YT_BROWSE_VIDEO_ID_LEN characters of
 * [A-Za-z0-9_-]. */
bool yt_browse_video_id_valid(const char * s);

/* True when id, params and token (NULL or "" for none) are what kind takes,
 * each valid. */
bool yt_browse_fields_valid(yt_browse_kind_t kind, const char * id, const char * params, const char * token);

/* Parses the request line (len bytes, with or without its final newline).
 * Rejects anything longer than YT_BROWSE_INPUT_MAX, with a NUL byte, or with
 * more than one line. On success fills *out; on failure *out is zeroed. */
bool yt_browse_input_parse(const char * line, size_t len, yt_browse_input_t * out);

/* Writes the request line for kind, id, params and token (NULL or "" for
 * none), newline included, NUL-terminated. Returns its length, or 0 when the
 * fields are not valid for kind or out is too small. */
size_t yt_browse_input_line(char * out, size_t size, yt_browse_kind_t kind, const char * id, const char * params,
                            const char * token);

#endif /* YT_BROWSE_INPUT_H */
