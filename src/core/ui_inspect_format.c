/* See ui_inspect_format.h. */
#include "ui_inspect_format.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

const char * uiif_result_word(uiif_result r) {
    switch (r) {
    case UIIF_BAD_REQUEST: return "bad_request";
    case UIIF_OPEN: return "open";
    case UIIF_WRITE: return "write";
    case UIIF_DEPTH: return "depth";
    case UIIF_OK: return "ok";
    case UIIF_NONE: break;
    }
    return "none";
}

bool uiif_clip(const uiif_area * a, const uiif_area * clip, uiif_area * out) {
    uiif_area r = {
        a->x1 > clip->x1 ? a->x1 : clip->x1,
        a->y1 > clip->y1 ? a->y1 : clip->y1,
        a->x2 < clip->x2 ? a->x2 : clip->x2,
        a->y2 < clip->y2 ? a->y2 : clip->y2,
    };
    if (r.x1 > r.x2 || r.y1 > r.y2) return false;
    *out = r;
    return true;
}

int uiif_parse_seq(const char * buf, size_t len, unsigned * seq) {
    if (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n') len -= 2;
    else if (len >= 1 && buf[len - 1] == '\n') len -= 1;
    if (len < 1 || len > 9) return -1;
    unsigned v = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        v = v * 10 + (unsigned) (buf[i] - '0');
    }
    *seq = v;
    return 0;
}

int uiif_write_header(FILE * f, const uiif_header * h) {
    fprintf(f, "#ui-inspect v=%d seq=%u w=%d h=%d anims=%u scrolling=%d screen_on=%d\n", UIIF_VERSION, h->seq, h->w,
            h->h, h->anims, h->scrolling ? 1 : 0, h->screen_on ? 1 : 0);
    return ferror(f) ? -1 : 0;
}

/* Length of s cut to at most max bytes without splitting a UTF-8 sequence. */
static size_t utf8_cut(const char * s, size_t max) {
    size_t n = strlen(s);
    if (n <= max) return n;
    n = max;
    while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) n--;
    return n;
}

static void put_escaped(FILE * f, const char * s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
        case '\t': fputs("\\t", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\\': fputs("\\\\", f); break;
        default: fputc(s[i], f); break;
        }
    }
}

int uiif_write_obj(FILE * f, const uiif_obj * o) {
    char flags[5];
    int nf = 0;
    if (o->clickable) flags[nf++] = 'c';
    if (o->checked) flags[nf++] = 'k';
    if (o->disabled) flags[nf++] = 'd';
    if (o->scrollable) flags[nf++] = 's';
    if (nf == 0) flags[nf++] = '-';
    flags[nf] = '\0';

    fprintf(f, "%d\t%d\t%d\t", o->idx, o->parent, o->layer);
    const char * cls = o->cls && *o->cls ? o->cls : "-";
    put_escaped(f, cls, strlen(cls));
    fputc('\t', f);
    if (o->name && *o->name) put_escaped(f, o->name, strlen(o->name));
    else fputc('-', f);
    fprintf(f, "\t%s\t%d\t%d\t%d\t%d\t", flags, (int) o->area.x1, (int) o->area.y1, (int) o->area.x2, (int) o->area.y2);
    if (o->visible_empty) fputs("-\t-\t-\t-\t", f);
    else
        fprintf(f, "%d\t%d\t%d\t%d\t", (int) o->visible.x1, (int) o->visible.y1, (int) o->visible.x2,
                (int) o->visible.y2);
    if (o->scrollable) fprintf(f, "%d\t%d\t", (int) o->scroll_x, (int) o->scroll_y);
    else fputs("-\t-\t", f);
    if (o->text) put_escaped(f, o->text, utf8_cut(o->text, UIIF_TEXT_MAX));
    fputc('\n', f);
    return ferror(f) ? -1 : 0;
}

/* dir/name into buf; false when it does not fit. */
static bool join(char * buf, size_t len, const char * dir, const char * name) {
    int n = snprintf(buf, len, "%s/%s", dir, name);
    return n > 0 && (size_t) n < len;
}

static void write_error(const char * dir, uiif_result r) {
    char path[256];
    if (!join(path, sizeof(path), dir, "error")) return;
    FILE * f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", uiif_result_word(r));
    if (fclose(f) != 0) unlink(path);
}

uiif_result uiif_serve(const char * dir, uiif_emit_fn emit, void * ctx) {
    char request[256];
    if (!join(request, sizeof(request), dir, "request")) return UIIF_NONE;
    if (access(request, F_OK) != 0) return UIIF_NONE;

    char buf[16];
    ssize_t got = -1;
    int fd = open(request, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        got = read(fd, buf, sizeof(buf));
        close(fd);
    }
    unlink(request);
    char path[256];
    if (join(path, sizeof(path), dir, "error")) unlink(path);

    unsigned seq;
    if (got <= 0 || (size_t) got >= sizeof(buf) || uiif_parse_seq(buf, (size_t) got, &seq) != 0) {
        write_error(dir, UIIF_BAD_REQUEST);
        return UIIF_BAD_REQUEST;
    }

    char name[32], tmp[256], final[256];
    snprintf(name, sizeof(name), "tree-%u.tsv", seq);
    int n = join(final, sizeof(final), dir, name) ? snprintf(tmp, sizeof(tmp), "%s.tmp", final) : -1;
    if (n <= 0 || (size_t) n >= sizeof(tmp)) {
        write_error(dir, UIIF_OPEN);
        return UIIF_OPEN;
    }

    FILE * f = fopen(tmp, "w");
    if (!f) {
        write_error(dir, UIIF_OPEN);
        return UIIF_OPEN;
    }
    uiif_result r = emit(f, seq, ctx);
    if (r == UIIF_OK && (fflush(f) != 0 || ferror(f))) r = UIIF_WRITE;
    if (fclose(f) != 0 && r == UIIF_OK) r = UIIF_WRITE;
    if (r == UIIF_OK && rename(tmp, final) != 0) r = UIIF_WRITE;
    if (r != UIIF_OK) {
        unlink(tmp);
        write_error(dir, r);
    }
    return r;
}
