#include "yt_page_stack.h"

#include <stdio.h>
#include <string.h>

void yt_page_view_clear(yt_page_view_t * v) {
    for (size_t i = 0; i < v->page_count; i++) yt_page_free(v->pages[i]);
    memset(v, 0, sizeof(*v));
}

bool yt_page_view_add(yt_page_view_t * v, yt_page_t * page) {
    if (!page) return false;
    if (v->page_count >= YT_VIEW_MAX_PAGES) {
        yt_page_free(page);
        return false;
    }
    v->pages[v->page_count++] = page;
    return true;
}

size_t yt_page_view_items(const yt_page_view_t * v) {
    size_t n = 0;
    for (size_t i = 0; i < v->page_count; i++) n += v->pages[i]->item_count;
    return n;
}

static bool has_header(const yt_page_t * p, size_t shelf) {
    return p->shelves[shelf].title != 0;
}

size_t yt_page_view_rows(const yt_page_view_t * v) {
    size_t n = 0;
    for (size_t i = 0; i < v->page_count; i++) {
        const yt_page_t * p = v->pages[i];
        for (size_t s = 0; s < p->shelf_count; s++) n += has_header(p, s) + p->shelves[s].count;
    }
    return n;
}

bool yt_page_view_row(const yt_page_view_t * v, size_t index, yt_page_row_t * out) {
    for (size_t i = 0; i < v->page_count; i++) {
        const yt_page_t * p = v->pages[i];
        for (size_t s = 0; s < p->shelf_count; s++) {
            size_t header = has_header(p, s);
            size_t rows = header + p->shelves[s].count;
            if (index >= rows) {
                index -= rows;
                continue;
            }
            *out = (yt_page_row_t) { .page = p, .shelf = s };
            if (index >= header) {
                out->item_index = p->shelves[s].first + (index - header);
                out->item = &p->items[out->item_index];
            }
            return true;
        }
    }
    return false;
}

void yt_page_stack_init(yt_page_stack_t * s) {
    memset(s, 0, sizeof(*s));
}

static bool entry_loaded(const yt_page_entry_t * e) {
    return e->view.page_count > 0;
}

size_t yt_page_stack_loaded(const yt_page_stack_t * s) {
    size_t n = 0;
    for (size_t i = 0; i < s->depth; i++) n += entry_loaded(&s->entries[i]);
    return n;
}

yt_page_entry_t * yt_page_stack_push(yt_page_stack_t * s, yt_browse_kind_t kind, const char * id, const char * params,
                                     yt_page_entry_t ** released, void ** dropped_ui) {
    if (released) *released = NULL;
    if (dropped_ui) *dropped_ui = NULL;
    if (kind == YT_BROWSE_HOME || kind == YT_BROWSE_EXPLORE || kind == YT_BROWSE_MOODS ||
        kind == YT_BROWSE_CHARTS || kind == YT_BROWSE_RELATED || !yt_browse_fields_valid(kind, id, params, NULL))
        return NULL;
    if (s->depth == YTM_STACK_DEPTH) {
        yt_page_entry_t * oldest = &s->entries[0];
        if (dropped_ui) *dropped_ui = oldest->ui;
        yt_page_view_clear(&oldest->view);
        memmove(&s->entries[0], &s->entries[1], (YTM_STACK_DEPTH - 1) * sizeof(s->entries[0]));
        s->depth--;
    }
    /* The new page will keep a page too: free the oldest beyond the limit. */
    if (yt_page_stack_loaded(s) >= YTM_PAGES_MAX) {
        for (size_t i = 0; i < s->depth; i++) {
            yt_page_entry_t * e = &s->entries[i];
            if (!entry_loaded(e)) continue;
            yt_page_view_clear(&e->view);
            e->released = true;
            if (released) *released = e;
            break;
        }
    }
    yt_page_entry_t * e = &s->entries[s->depth++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    snprintf(e->id, sizeof(e->id), "%s", id ? id : "");
    snprintf(e->params, sizeof(e->params), "%s", params ? params : "");
    return e;
}

yt_page_entry_t * yt_page_stack_top(yt_page_stack_t * s) {
    return s->depth ? &s->entries[s->depth - 1] : NULL;
}

bool yt_page_stack_pop(yt_page_stack_t * s, void ** ui, uint32_t * ticket) {
    if (ui) *ui = NULL;
    if (ticket) *ticket = 0;
    if (s->depth == 0) return false;
    yt_page_entry_t * e = &s->entries[--s->depth];
    if (ui) *ui = e->ui;
    if (ticket) *ticket = e->ticket;
    yt_page_view_clear(&e->view);
    memset(e, 0, sizeof(*e));
    return true;
}

bool yt_page_entry_set_page(yt_page_entry_t * e, yt_page_t * page) {
    if (!yt_page_view_add(&e->view, page)) return false;
    e->released = false;
    return true;
}

void yt_page_stack_clear(yt_page_stack_t * s) {
    for (size_t i = 0; i < s->depth; i++) yt_page_view_clear(&s->entries[i].view);
    memset(s, 0, sizeof(*s));
}
