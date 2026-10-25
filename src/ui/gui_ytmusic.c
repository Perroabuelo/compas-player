/* The YouTube Music screen (hiby-yt, youtube-music-screen). See
 * gui_ytmusic.h; the design is openspec/changes/youtube-music-screen. */
#include "gui_ytmusic.h"

#include "assets.h"
#include "gui.h"
#include "gui_navigation.h"
#include "gui_theme.h"
#include "i18n.h"
#include "screen_builders.h"

#include <stdint.h>
#include <string.h>

#define YTM_TAB_COUNT 3
#define YTM_TABS_H BOARD_SCALE_PX(56)
#define YTM_BACK_ROW_H BOARD_SCALE_PX(48)

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
static int ytm_tab;
static ytm_view_t ytm_view;

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

static void prompt_search(void) {
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
    return view;
}

static lv_obj_t * build_ytmusic_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_screen_header(scr, TR("YouTube Music"), generic_back_cb, "navigation/search.png", search_click_cb);
    build_tabs(scr);

    ytm_content = lv_obj_create(scr);
    lv_obj_remove_style_all(ytm_content);
    lv_obj_set_size(ytm_content, lv_pct(100), content_height());
    lv_obj_set_pos(ytm_content, 0, content_top());
    lv_obj_remove_flag(ytm_content, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < YTM_TAB_COUNT; i++) ytm_panels[i] = build_coming_soon_panel(ytm_content);
    ytm_results_view = build_results_view(ytm_content);

    finalize_screen_navigation(scr);
    return scr;
}

void gui_ytmusic_init(void) {
    ytm_tab = 0;
    ytm_view = YTM_VIEW_TAB;
    ytm_screen = build_ytmusic_screen();
    show_view();
}

void gui_ytmusic_teardown(void) {
    if (ytm_screen) lv_obj_delete(ytm_screen);
    ytm_screen = NULL;
    ytm_content = NULL;
    ytm_results_view = NULL;
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

void poll_ytmusic(void) {
}
