#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* memmem */
#endif
#include "yt_browse_line.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#ifdef HOST_BUILD
static void * (*block_alloc)(size_t) = malloc;

void yt_browse_line_set_test_alloc(void * (*alloc)(size_t)) {
    block_alloc = alloc ? alloc : malloc;
}
#else
#define block_alloc malloc
#endif

static const char * const type_names[] = {
    [YT_ITEM_SONG] = "song",         [YT_ITEM_ALBUM] = "album",       [YT_ITEM_PLAYLIST] = "playlist",
    [YT_ITEM_ARTIST] = "artist",     [YT_ITEM_CATEGORY] = "category",
};

static bool class_valid(const char * c) {
    size_t n = 0;
    for (; c[n]; n++) {
        if (!((c[n] >= 'a' && c[n] <= 'z') || c[n] == '_') || n >= YT_PAGE_CLASS_MAX) return false;
    }
    return n > 0;
}

/* The decoder runs twice over the same tree: first to check it and measure
 * the arena (arena == NULL), then to fill the blocks. */
typedef struct {
    char * arena;     /* NULL while measuring */
    size_t arena_len; /* bytes used (or needed) */
    yt_page_t * page; /* NULL while measuring */
    size_t shelves;
    size_t items;
} fill_t;

/* A text of at most YT_PAGE_TEXT_MAX bytes with no control character (the
 * program removes them), stored in the arena. "" is offset 0. */
static bool put_text(fill_t * f, const cJSON * item, yt_page_str_t * out) {
    *out = 0;
    if (!cJSON_IsString(item)) return false;
    const char * s = item->valuestring;
    size_t n = strlen(s);
    if (n > YT_PAGE_TEXT_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char) s[i] < 0x20 || s[i] == 0x7F) return false;
    }
    if (n == 0) return true;
    if (f->arena) memcpy(f->arena + f->arena_len, s, n + 1);
    *out = (yt_page_str_t) f->arena_len;
    f->arena_len += n + 1;
    return true;
}

/* An optional text: absent is "". */
static bool put_optional_text(fill_t * f, const cJSON * item, yt_page_str_t * out) {
    *out = 0;
    return !item || put_text(f, item, out);
}

/* A browse id (or params, with params set), checked and stored. */
static bool put_id(fill_t * f, const cJSON * item, bool params, yt_page_str_t * out) {
    *out = 0;
    if (!cJSON_IsString(item)) return false;
    if (params ? !yt_browse_params_valid(item->valuestring) : !yt_browse_id_valid(item->valuestring)) return false;
    return put_text(f, item, out);
}

static bool put_optional_id(fill_t * f, const cJSON * item, yt_page_str_t * out) {
    *out = 0;
    return !item || put_id(f, item, false, out);
}

static bool read_item(fill_t * f, const cJSON * r, uint8_t shelf) {
    if (!cJSON_IsObject(r)) return false;
    yt_page_item_t it = { .shelf = shelf, .duration_s = -1 };
    const cJSON * type = cJSON_GetObjectItemCaseSensitive(r, "type");
    if (!cJSON_IsString(type)) return false;
    int t = -1;
    for (int i = 0; i < (int) (sizeof(type_names) / sizeof(type_names[0])); i++) {
        if (strcmp(type->valuestring, type_names[i]) == 0) t = i;
    }
    if (t < 0) return false;
    it.type = (uint8_t) t;
#define FIELD(name) cJSON_GetObjectItemCaseSensitive(r, name)
    if (!put_text(f, FIELD("title"), &it.title)) return false;
    if (t == YT_ITEM_SONG) {
        const cJSON * id = FIELD("video_id");
        const cJSON * dur = FIELD("duration_s");
        if (!cJSON_IsString(id) || !yt_browse_video_id_valid(id->valuestring)) return false;
        strcpy(it.video_id, id->valuestring);
        if (dur) {
            if (!cJSON_IsNumber(dur)) return false;
            double d = dur->valuedouble;
            if (!(d >= 0 && d <= YT_PAGE_DURATION_MAX_S) || d != (double) (int32_t) d) return false;
            it.duration_s = (int32_t) d;
        }
        if (!put_optional_text(f, FIELD("artist"), &it.artist) || !put_optional_text(f, FIELD("album"), &it.album) ||
            !put_optional_id(f, FIELD("artist_id"), &it.artist_id) ||
            !put_optional_id(f, FIELD("album_id"), &it.album_id))
            return false;
    } else {
        if (!put_optional_text(f, FIELD("subtitle"), &it.subtitle) || !put_id(f, FIELD("id"), false, &it.id)) return false;
        if (t == YT_ITEM_CATEGORY && !put_id(f, FIELD("params"), true, &it.params)) return false;
    }
#undef FIELD
    if (f->page) f->page->items[f->items] = it;
    f->items++;
    return true;
}

static bool read_page(fill_t * f, const cJSON * root) {
    const cJSON * shelves = cJSON_GetObjectItemCaseSensitive(root, "shelves");
    const cJSON * t = cJSON_GetObjectItemCaseSensitive(root, "continuation");
    yt_page_str_t title, token = 0;
    if (!put_text(f, cJSON_GetObjectItemCaseSensitive(root, "title"), &title)) return false;
    if (!cJSON_IsArray(shelves) || cJSON_GetArraySize(shelves) > YT_PAGE_MAX_SHELVES) return false;
    if (cJSON_IsString(t)) {
        if (!yt_search_token_valid(t->valuestring) || !put_text(f, t, &token)) return false;
    } else if (!cJSON_IsNull(t)) {
        return false;
    }
    if (f->page) {
        f->page->title = title;
        f->page->token = token;
    }
    const cJSON * s;
    cJSON_ArrayForEach(s, shelves) {
        const cJSON * items = cJSON_GetObjectItemCaseSensitive(s, "items");
        yt_page_str_t shelf_title;
        if (!cJSON_IsObject(s) || !put_text(f, cJSON_GetObjectItemCaseSensitive(s, "title"), &shelf_title) ||
            !cJSON_IsArray(items))
            return false;
        if (f->items + (size_t) cJSON_GetArraySize(items) > YT_PAGE_MAX_ITEMS) return false;
        size_t first = f->items;
        const cJSON * r;
        cJSON_ArrayForEach(r, items) {
            if (!read_item(f, r, (uint8_t) f->shelves)) return false;
        }
        if (f->page) {
            f->page->shelves[f->shelves] =
                (yt_page_shelf_t) { .title = shelf_title, .first = (uint16_t) first, .count = (uint16_t) (f->items - first) };
        }
        f->shelves++;
    }
    return true;
}

yt_browse_line_status_t yt_browse_line_parse(const char * line, size_t len, yt_page_t ** out, char * fail_class) {
    if (fail_class) fail_class[0] = '\0';
    if (!out) return YT_BROWSE_LINE_MALFORMED;
    *out = NULL;
    if (!line || len == 0) return YT_BROWSE_LINE_MALFORMED;
    size_t body = len;
    if (line[body - 1] == '\n') body--;
    if (body == 0 || memchr(line, '\0', body) || memchr(line, '\n', body)) return YT_BROWSE_LINE_MALFORMED;
    /* cJSON ends a string early at an escaped \u0000. */
    if (memmem(line, body, "\\u0000", 6)) return YT_BROWSE_LINE_MALFORMED;

    const char * end = NULL;
    cJSON * root = cJSON_ParseWithLengthOpts(line, body, &end, 0);
    yt_browse_line_status_t st = YT_BROWSE_LINE_MALFORMED;
    bool tail_ok = end != NULL;
    for (const char * p = end; tail_ok && p < line + body; p++) tail_ok = *p == ' ' || *p == '\t' || *p == '\r';
    if (!cJSON_IsObject(root) || !tail_ok) goto done;
    const cJSON * ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const cJSON * cls = cJSON_GetObjectItemCaseSensitive(root, "class");
    if (cJSON_IsFalse(ok) && cJSON_IsString(cls) && class_valid(cls->valuestring)) {
        if (fail_class) strcpy(fail_class, cls->valuestring);
        st = YT_BROWSE_LINE_FAILED;
        goto done;
    }
    if (!cJSON_IsTrue(ok)) goto done;

    /* Measure: offset 0 is the arena's leading NUL, the empty string. */
    fill_t m = { .arena_len = 1 };
    if (!read_page(&m, root)) goto done;
    size_t head = sizeof(yt_page_t) + m.shelves * sizeof(yt_page_shelf_t) + m.items * sizeof(yt_page_item_t);
    yt_page_t * page = block_alloc(head);
    char * arena = page ? block_alloc(m.arena_len) : NULL;
    if (!page || !arena) {
        free(page);
        st = YT_BROWSE_LINE_NO_MEMORY;
        goto done;
    }
    memset(page, 0, head);
    page->items = (yt_page_item_t *) (page + 1);
    page->shelves = (yt_page_shelf_t *) (page->items + m.items);
    page->arena = arena;
    page->arena_len = m.arena_len;
    page->shelf_count = m.shelves;
    page->item_count = m.items;
    arena[0] = '\0';
    fill_t f = { .arena = arena, .arena_len = 1, .page = page };
    read_page(&f, root); /* the same tree: it reads as it measured */
    *out = page;
    st = YT_BROWSE_LINE_OK;
done:
    cJSON_Delete(root);
    return st;
}

const char * yt_page_str(const yt_page_t * page, yt_page_str_t s) {
    if (!page || s == 0 || s >= page->arena_len) return "";
    return page->arena + s;
}

void yt_page_free(yt_page_t * page) {
    if (!page) return;
    free(page->arena);
    free(page);
}
