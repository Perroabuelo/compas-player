#ifndef YT_ROW_FORMAT_H
#define YT_ROW_FORMAT_H

/* Text of a result row on the YouTube Music screen (hiby-yt, design D2 of
 * youtube-music-screen). Pure: no LVGL. The first line is the title; the
 * second is "artist · album · m:ss", without the separators of a part that
 * is missing. Text that does not fit is cut at a UTF-8 character boundary,
 * never in the middle of a character. */

#include <stddef.h>
#include <stdint.h>

/* Bytes of the second line, NUL included (the compact list's own subtitle
 * size, compact_list_page_row_t). */
#define YT_ROW_SUBTITLE_MAX 256

/* "m:ss", or "h:mm:ss" from one hour. Writes "" for 0 (unknown). Returns
 * the length (0 when out is too small). */
size_t yt_row_duration(char * out, size_t size, uint32_t duration_ms);

/* The second line into out (size bytes, NUL included): artist, album and
 * duration joined by " · ", each left out when empty or 0. The duration
 * is kept whole; artist and album are cut to make room for it. Returns the
 * length. */
size_t yt_row_subtitle(char * out, size_t size, const char * artist, const char * album, uint32_t duration_ms);

/* Copies text into out (size bytes, NUL included), cut at the last whole
 * UTF-8 character that fits. Returns the length. */
size_t yt_row_cut(char * out, size_t size, const char * text);

#endif /* YT_ROW_FORMAT_H */
