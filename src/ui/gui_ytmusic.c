/* The YouTube Music screen (hiby-yt, youtube-music-screen). See
 * gui_ytmusic.h; the design is openspec/changes/youtube-music-screen. */
#include "gui_ytmusic.h"

#include "assets.h"
#include "gui.h"
#include "gui_navigation.h"
#include "gui_notifications.h"
#include "gui_text_input.h"
#include "gui_theme.h"
#include "i18n.h"
#include "screen_builders.h"
#include "stream_log.h"
#include "yt_resolver_proc.h"
#include "yt_results.h"
#include "yt_row_format.h"
#include "yt_search_line.h"
#include "yt_search_mailbox.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YTM_TAB_COUNT 3
#define YTM_TABS_H BOARD_SCALE_PX(56)
#define YTM_BACK_ROW_H BOARD_SCALE_PX(48)
/* The cover slot of a row (D4): the same box in every row, with or without
 * an image, so youtube-music-covers only changes what it shows. */
#define YTM_ART_PX BOARD_SCALE_PX(56)
#define YTM_ART_INSET 14 /* the list's own leading image offset */
#define YTM_ART_ICON "launcher/music.png"
#define YTM_MORE_ICON "navigation/more.png"

typedef enum {
    YTM_VIEW_TAB,     /* the selected tab's panel */
    YTM_VIEW_RESULTS, /* the search results */
} ytm_view_t;

static const char * const ytm_tab_names[YTM_TAB_COUNT] = { N_("Home"), N_("Explore"), N_("Library") };

static lv_obj_t * ytm_screen;
static lv_obj_t * ytm_tab_buttons[YTM_TAB_COUNT];
static lv_obj_t * ytm_panels[YTM_TAB_COUNT];
static lv_obj_t * ytm_content;      /* panels and the results view */
static lv_obj_t * ytm_results_view; /* back row and result list */
static lv_obj_t * ytm_status;       /* "Searching…" */
static lv_obj_t * ytm_list;
/* Second lines of the rows: the list borrows its strings (titles point
 * into the store). Allocated with the first results. */
static char (*ytm_subtitles)[YT_ROW_SUBTITLE_MAX];
static int ytm_tab;
static ytm_view_t ytm_view;

/* The search (design D3). The mailbox outlives the screen: its mutex is
 * set up once, and a rebuild only reopens it. */
static yt_search_mailbox_t ytm_mailbox;
static bool ytm_mailbox_ready;
static char ytm_query[YT_SEARCH_QUERY_MAX + 1]; /* the last query, for the keyboard */
static bool ytm_searching;
static bool ytm_search_more;   /* the running search asks for the next page */
static uint16_t ytm_search_gen; /* yt_resolver_proc generation, for cancel and wait */
static yt_results_t ytm_results;

static void start_search(bool more);

static int32_t screen_height(void) {
    return lv_display_get_vertical_resolution(lv_display_get_default());
}

static int32_t content_top(void) {
    return STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + YTM_TABS_H;
}

static int32_t content_height(void) {
    return screen_height() - content_top() - HOME_INDICATOR_CONTENT_INSET;
}

/* Shows the panel of the selected tab or the results, and marks the tab. */
static void show_view(void) {
    for (int i = 0; i < YTM_TAB_COUNT; i++) {
        if (ytm_view == YTM_VIEW_TAB && i == ytm_tab) lv_obj_remove_flag(ytm_panels[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ytm_panels[i], LV_OBJ_FLAG_HIDDEN);
        if (i == ytm_tab) lv_obj_add_state(ytm_tab_buttons[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(ytm_tab_buttons[i], LV_STATE_CHECKED);
    }
    if (ytm_view == YTM_VIEW_RESULTS) lv_obj_remove_flag(ytm_results_view, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ytm_results_view, LV_OBJ_FLAG_HIDDEN);
}

/* Changing tabs makes no request (spec «Recorrer las pestañas»). */
static void tab_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ytm_tab = (int) (intptr_t) lv_event_get_user_data(e);
    ytm_view = YTM_VIEW_TAB;
    show_view();
}

static void set_searching(bool searching) {
    ytm_searching = searching;
    if (!ytm_status) return;
    if (searching) {
        lv_obj_remove_flag(ytm_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ytm_status);
    } else {
        lv_obj_add_flag(ytm_status, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The toast of a search that did not give a page: busy, or the class. */
static void show_search_failure(const char * cls) {
    if (strcmp(cls, "busy") == 0) {
        show_info_toast(TR("Try again in a moment"));
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), TR("Search failed: %s"), cls);
    show_info_toast(msg);
}

static void clear_results_list(void) {
    if (ytm_list) compact_list_set_items(ytm_list, NULL, 0);
}

/* Fills the result list from the store, with "More results" at the end
 * while there is a next page. keep_scroll for a next page: the list grows
 * without jumping to the top. */
static void update_results_list(bool keep_scroll) {
    if (!ytm_list) return;
    int32_t scroll_y = keep_scroll ? lv_obj_get_scroll_y(ytm_list) : 0;
    int n = (int) ytm_results.count;
    int total = n + (yt_results_has_more(&ytm_results) ? 1 : 0);
    if (total == 0) {
        clear_results_list();
        return;
    }
    if (!ytm_subtitles) ytm_subtitles = malloc(sizeof(*ytm_subtitles) * YT_RESULTS_MAX);
    compact_list_item_t * items = calloc((size_t) total, sizeof(*items));
    if (!ytm_subtitles || !items) {
        free(items);
        clear_results_list();
        show_search_failure("internal");
        return;
    }
    for (int i = 0; i < n; i++) {
        const yt_song_t * s = &ytm_results.songs[i];
        yt_row_subtitle(ytm_subtitles[i], YT_ROW_SUBTITLE_MAX, s->artist, s->album, s->duration_ms);
        items[i].label = s->title;
        items[i].subtitle = ytm_subtitles[i];
        items[i].trailing_asset = YTM_MORE_ICON;
        items[i].identity = i + 1;
        items[i].artwork_key = (uint64_t) i + 1;
    }
    if (total > n) {
        items[n].label = TR("More results");
        items[n].is_action = true;
    }
    /* The list copies the array, not the strings. */
    compact_list_set_items(ytm_list, items, total);
    free(items);
    if (scroll_y > 0) lv_obj_scroll_to_y(ytm_list, scroll_y, LV_ANIM_OFF);
}

/* The fixed cover slot with the note icon; "More results" has none. */
static void row_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image, int logical_index, int pool_slot,
                          int64_t identity, uint64_t artwork_key, const char * artwork_name, void * ctx) {
    (void) list;
    (void) logical_index;
    (void) pool_slot;
    (void) identity;
    (void) artwork_name;
    (void) ctx;
    if (artwork_key == 0) {
        lv_obj_set_style_pad_left(row, LIST_ROW_LABEL_INSET, 0);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_pad_left(row, YTM_ART_INSET + YTM_ART_PX + YTM_ART_INSET, 0);
    /* The source first: an empty image has no size to fit. */
    lv_image_set_src(image, asset_path(YTM_ART_ICON));
    lv_obj_set_size(image, YTM_ART_PX, YTM_ART_PX);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
}

static void open_row_menu(int index) {
    (void) index;
}

static void play_from(int index) {
    (void) index;
}

static void row_click_cb(int index) {
    if (index == (int) ytm_results.count) {
        if (yt_results_has_more(&ytm_results)) start_search(true);
        return;
    }
    if (index >= 0 && index < (int) ytm_results.count) play_from(index);
}

static void row_menu_cb(int index) {
    if (index >= 0 && index < (int) ytm_results.count) open_row_menu(index);
}

/* On the search service's worker thread: decodes the line off the UI
 * thread and leaves the page (or the class) in the mailbox. */
static void search_done(const yt_search_result_t * r, void * arg) {
    uint32_t ticket = (uint32_t) (uintptr_t) arg;
    if (r->cancelled) {
        yt_search_mailbox_deliver(&ytm_mailbox, ticket, NULL, "cancelled");
        return;
    }
    if (r->fail_class) {
        yt_search_mailbox_deliver(&ytm_mailbox, ticket, NULL, r->fail_class);
        return;
    }
    yt_search_line_page_t * page = malloc(sizeof(*page));
    if (!page) {
        yt_search_mailbox_deliver(&ytm_mailbox, ticket, NULL, "internal");
        return;
    }
    char cls[YT_SEARCH_LINE_CLASS_MAX + 1];
    switch (yt_search_line_parse(r->output, r->output_len, page)) {
        case YT_SEARCH_LINE_OK:
            yt_search_mailbox_deliver(&ytm_mailbox, ticket, page, NULL);
            return;
        case YT_SEARCH_LINE_FAILED:
            memcpy(cls, page->fail_class, sizeof(cls));
            break;
        default:
            strcpy(cls, "malformed");
            break;
    }
    free(page);
    yt_search_mailbox_deliver(&ytm_mailbox, ticket, NULL, cls);
}

/* Starts the search for ytm_query: page 1, or the next page of the
 * results. busy and bad_input come back at once (D3). */
static void start_search(bool more) {
    if (ytm_searching) {
        show_info_toast(TR("Try again in a moment"));
        return;
    }
    uint32_t ticket = yt_search_mailbox_expect(&ytm_mailbox);
    const char * why = "internal";
    uint16_t gen = yt_resolver_start_search(ytm_query, more ? ytm_results.token : NULL, search_done,
                                            (void *) (uintptr_t) ticket, &why);
    if (gen == 0) {
        yt_search_mailbox_forget(&ytm_mailbox);
        show_search_failure(why);
        return;
    }
    ytm_search_gen = gen;
    ytm_search_more = more;
    set_searching(true);
}

static void query_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    /* An empty query, or only blanks, starts nothing (spec «Consulta vacía»). */
    const char * p = text;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return;
    snprintf(ytm_query, sizeof(ytm_query), "%s", text);
    start_search(false);
}

/* The keyboard, with the text of the last search. */
static void prompt_search(void) {
    show_text_entry(TR("Search YouTube Music"), ytm_query, false, false, query_entered_cb, NULL);
}

/* A finished search, on the UI thread. A new search replaces the list; a
 * next page is appended to it. */
static void take_search_result(void) {
    yt_search_line_page_t * page = NULL;
    char cls[YT_SEARCH_LINE_CLASS_MAX + 1];
    if (!yt_search_mailbox_take(&ytm_mailbox, &page, cls)) return;
    /* The worker is about to leave the service: let it, so the next search
     * is not refused as busy. */
    yt_resolver_wait_ended(ytm_search_gen);
    bool more = ytm_search_more;
    set_searching(false);
    if (!page) {
        if (more) stream_log_ytm_ui("more", 0, cls);
        if (strcmp(cls, "cancelled") != 0) show_search_failure(cls);
        return;
    }
    if (!yt_results_reserve(&ytm_results)) {
        free(page);
        show_search_failure("internal");
        return;
    }
    if (more) {
        size_t added = yt_results_add_page(&ytm_results, page);
        stream_log_ytm_ui("more", (int) added, "ok");
    } else {
        /* The list points into the store: empty it before the store. */
        clear_results_list();
        yt_results_clear(&ytm_results);
        yt_results_add_page(&ytm_results, page);
        if (ytm_results.count == 0) show_info_toast(TR("No results"));
        ytm_view = YTM_VIEW_RESULTS;
        show_view();
    }
    free(page);
    update_results_list(more);
}

static void search_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    prompt_search();
}

static void results_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    ytm_view = YTM_VIEW_TAB;
    show_view();
}

static lv_obj_t * build_tabs(lv_obj_t * scr) {
    lv_obj_t * row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), YTM_TABS_H);
    lv_obj_set_pos(row, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < YTM_TAB_COUNT; i++) {
        lv_obj_t * btn = lv_obj_create(row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, YTM_TABS_H - BOARD_SCALE_PX(12));
        lv_obj_set_style_pad_hor(btn, BOARD_SCALE_PX(18), 0);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(GUI_COLOR_PANEL), 0);
        lv_obj_add_style(btn, gui_theme_accent_style(), LV_STATE_CHECKED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, tab_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text(label, TR(ytm_tab_names[i]));
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_center(label);
        ytm_tab_buttons[i] = btn;
    }
    return row;
}

/* A tab without content: "Coming soon" and a way into the search. */
static lv_obj_t * build_coming_soon_panel(lv_obj_t * parent) {
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(panel, BOARD_SCALE_PX(24), 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(panel);
    lv_label_set_text(label, TR("Coming soon"));
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);

    lv_obj_t * btn = lv_obj_create(panel);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, BOARD_SCALE_PX(56));
    lv_obj_set_style_pad_hor(btn, BOARD_SCALE_PX(32), 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_style(btn, gui_theme_accent_style(), 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, search_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * text = lv_label_create(btn);
    lv_label_set_text(text, TR("Search"));
    lv_obj_add_style(text, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(text, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_center(text);
    return panel;
}

/* The results: a row back to the tab, then the list. */
static lv_obj_t * build_results_view(lv_obj_t * parent) {
    lv_obj_t * view = lv_obj_create(parent);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * back = lv_obj_create(view);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, lv_pct(100), YTM_BACK_ROW_H);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(back, results_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * label = lv_label_create(back);
    lv_label_set_text_fmt(label, LV_SYMBOL_LEFT "  %s", TR("Back to tab"));
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, 0);

    /* Tap plays; the "…" button and a long press open the row menu. */
    ytm_list = build_compact_list_widget(view, NULL, 0, row_click_cb, row_menu_cb, LIST_ROW_WIDTH_WIDE, false, NULL);
    compact_list_set_row_height(ytm_list, MUSIC_LIST_ROW_HEIGHT);
    compact_list_set_row_decorator(ytm_list, row_decorator, NULL);
    compact_list_set_trailing_click(ytm_list, row_menu_cb);
    lv_obj_set_size(ytm_list, lv_pct(100), content_height() - YTM_BACK_ROW_H);
    lv_obj_align(ytm_list, LV_ALIGN_TOP_MID, 0, YTM_BACK_ROW_H);
    return view;
}

static lv_obj_t * build_ytmusic_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_screen_header(scr, TR("YouTube Music"), generic_back_cb, asset_path("navigation/search.png"),
                        search_click_cb);
    build_tabs(scr);

    ytm_content = lv_obj_create(scr);
    lv_obj_remove_style_all(ytm_content);
    lv_obj_set_size(ytm_content, lv_pct(100), content_height());
    lv_obj_set_pos(ytm_content, 0, content_top());
    lv_obj_remove_flag(ytm_content, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < YTM_TAB_COUNT; i++) ytm_panels[i] = build_coming_soon_panel(ytm_content);
    ytm_results_view = build_results_view(ytm_content);

    ytm_status = lv_label_create(scr);
    lv_label_set_text(ytm_status, TR("Searching…"));
    lv_obj_add_style(ytm_status, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(ytm_status, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_bg_opa(ytm_status, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ytm_status, lv_color_hex(GUI_COLOR_PANEL), 0);
    lv_obj_set_style_radius(ytm_status, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(ytm_status, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_ver(ytm_status, BOARD_SCALE_PX(8), 0);
    lv_obj_align(ytm_status, LV_ALIGN_TOP_MID, 0, content_top() + BOARD_SCALE_PX(4));
    lv_obj_add_flag(ytm_status, LV_OBJ_FLAG_HIDDEN);

    finalize_screen_navigation(scr);
    return scr;
}

void gui_ytmusic_init(void) {
    if (!ytm_mailbox_ready) {
        yt_search_mailbox_init(&ytm_mailbox);
        ytm_mailbox_ready = true;
    } else {
        yt_search_mailbox_reopen(&ytm_mailbox);
    }
    yt_results_init(&ytm_results);
    ytm_query[0] = '\0';
    ytm_searching = false;
    ytm_tab = 0;
    ytm_view = YTM_VIEW_TAB;
    ytm_screen = build_ytmusic_screen();
    show_view();
}

void gui_ytmusic_teardown(void) {
    /* A search in flight is killed and reaped; its late result, if any,
     * is freed by the closed mailbox and never touches the screen. */
    if (ytm_searching) {
        yt_resolver_cancel(ytm_search_gen);
        yt_resolver_wait_ended(ytm_search_gen);
    }
    ytm_searching = false;
    if (ytm_mailbox_ready) yt_search_mailbox_close(&ytm_mailbox);
    /* The list (and its copy of the items) goes with the screen, before the
     * strings it points at. */
    if (ytm_screen) lv_obj_delete(ytm_screen);
    yt_results_free(&ytm_results);
    free(ytm_subtitles);
    ytm_subtitles = NULL;
    ytm_screen = NULL;
    ytm_content = NULL;
    ytm_results_view = NULL;
    ytm_status = NULL;
    ytm_list = NULL;
    memset(ytm_tab_buttons, 0, sizeof(ytm_tab_buttons));
    memset(ytm_panels, 0, sizeof(ytm_panels));
}

void gui_ytmusic_open(void) {
    if (ytm_screen) nav_push(ytm_screen);
}

void ytmusic_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_ytmusic_open();
}

/* Runs whether or not the screen is visible: a result that arrives while
 * the user is elsewhere is shown when the screen opens again. */
void poll_ytmusic(void) {
    if (!ytm_screen || !ytm_searching) return;
    take_search_result();
}
