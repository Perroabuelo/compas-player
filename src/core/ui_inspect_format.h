/* ui_inspect_format: the LVGL-free half of the ui_inspect hook (hiby-yt,
 * automate-device-tests D5, spec dev/ui-inspect).
 *
 * The device runner writes a sequence number into /tmp/hiby-yt-ui/request;
 * ui_inspect.c (on the UI thread) calls uiif_serve(), which reads and
 * removes the request, writes tree-<seq>.tsv.tmp through the caller's
 * emit callback, and renames it to tree-<seq>.tsv. On failure it removes
 * the partial file and writes a fixed word to /tmp/hiby-yt-ui/error.
 *
 * Dump format, one line per entry, tab-separated:
 *
 *   #ui-inspect v=1 seq=7 w=480 h=800 anims=0 scrolling=0 screen_on=1
 *   idx parent layer class name flags x1 y1 x2 y2 vx1 vy1 vx2 vy2 scroll_x scroll_y text
 *
 * - parent is -1 for a layer root; layer is 0 (active screen), 1 (top
 *   layer) or 2 (system layer).
 * - name is "-" when the object has none.
 * - flags: c clickable, k checked, d disabled, s scrollable; "-" if none.
 * - areas are inclusive screen pixels; the visible area is clipped by the
 *   ancestors and the screen, and is "-" in all four fields when empty.
 * - scroll_x and scroll_y are "-" unless the object scrolls.
 * - name and text escape tab, newline, carriage return and backslash as
 *   \t, \n, \r and \\. Text is cut at UIIF_TEXT_MAX bytes, on a UTF-8
 *   character boundary.
 *
 * Tested on the PC by hiby-yt's tests/ui_inspect_format_test.c.
 */
#ifndef UI_INSPECT_FORMAT_H
#define UI_INSPECT_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define UIIF_DIR "/tmp/hiby-yt-ui"
#define UIIF_VERSION 1
#define UIIF_TEXT_MAX 512
#define UIIF_FIELDS 17

typedef struct {
    int32_t x1, y1, x2, y2; /* inclusive, as lv_area_t */
} uiif_area;

typedef struct {
    unsigned seq;
    int w, h;
    unsigned anims;
    bool scrolling;
    bool screen_on;
} uiif_header;

typedef struct {
    int idx;
    int parent;
    int layer;
    const char * cls;
    const char * name; /* NULL or "" when none */
    const char * text; /* NULL when none */
    uiif_area area;
    uiif_area visible;
    bool visible_empty;
    bool clickable, checked, disabled, scrollable;
    int32_t scroll_x, scroll_y;
} uiif_obj;

typedef enum {
    UIIF_NONE = 0,    /* no request */
    UIIF_OK,          /* tree written */
    UIIF_BAD_REQUEST, /* request is not 1 to 9 decimal digits */
    UIIF_OPEN,        /* the temporary file could not be created */
    UIIF_WRITE,       /* a write, the close or the rename failed */
    UIIF_DEPTH,       /* the tree is deeper than the walker's stack */
} uiif_result;

/* The fixed word written to the error file ("bad_request", "open", ...). */
const char * uiif_result_word(uiif_result r);

/* Intersection of a and clip; false (and *out untouched) when empty. */
bool uiif_clip(const uiif_area * a, const uiif_area * clip, uiif_area * out);

/* Parses a request: 1 to 9 decimal digits, optionally followed by "\n" or
 * "\r\n" (as `echo 7 >` writes it). Returns 0 and sets *seq, or -1. */
int uiif_parse_seq(const char * buf, size_t len, unsigned * seq);

/* Write one line; return 0, or -1 if the stream reports an error. */
int uiif_write_header(FILE * f, const uiif_header * h);
int uiif_write_obj(FILE * f, const uiif_obj * o);

/* Fills the open dump file; returns UIIF_OK, UIIF_WRITE or UIIF_DEPTH. */
typedef uiif_result (*uiif_emit_fn)(FILE * f, unsigned seq, void * ctx);

/* Serves the request in `dir`, if there is one. Without a request it only
 * checks for the file and returns UIIF_NONE. With one, it always removes
 * the request; on failure it also removes the partial file and writes the
 * result's word to dir/error. A stale error file is removed first. */
uiif_result uiif_serve(const char * dir, uiif_emit_fn emit, void * ctx);

#endif
