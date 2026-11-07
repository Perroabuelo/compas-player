/* See ui_inspect.h. */
#include "ui_inspect.h"

#include "ui_inspect_format.h"

#include "backlight.h"
#include "lvgl.h"
#include "src/core/lv_obj_class_private.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

#include <stdbool.h>
#include <string.h>

#define UI_INSPECT_DEPTH 64

typedef struct {
    lv_obj_t * obj;
    int idx;            /* this object's line */
    uint32_t next;      /* next child to visit */
    uiif_area visible;  /* clip for the children */
    bool visible_empty;
} frame_t;

typedef struct {
    FILE * f;
    int next_idx;
    char dropdown[128];
} walk_t;

static const char * obj_text(walk_t * w, lv_obj_t * obj) {
    if (lv_obj_check_type(obj, &lv_label_class)) return lv_label_get_text(obj);
    if (lv_obj_check_type(obj, &lv_textarea_class)) return lv_textarea_get_text(obj);
    if (lv_obj_check_type(obj, &lv_checkbox_class)) return lv_checkbox_get_text(obj);
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_dropdown_get_selected_str(obj, w->dropdown, sizeof(w->dropdown));
        return w->dropdown;
    }
    return NULL;
}

static uiif_area to_area(const lv_area_t * a) {
    uiif_area r = { a->x1, a->y1, a->x2, a->y2 };
    return r;
}

static bool can_scroll(lv_obj_t * obj) {
    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE)) return false;
    return lv_obj_get_scroll_top(obj) > 0 || lv_obj_get_scroll_bottom(obj) > 0 || lv_obj_get_scroll_left(obj) > 0 ||
           lv_obj_get_scroll_right(obj) > 0;
}

/* One line per visible button of a button matrix (the on-screen keyboard). */
static int write_buttons(walk_t * w, lv_obj_t * obj, int parent_idx, int layer, const frame_t * clip) {
    lv_buttonmatrix_t * btnm = (lv_buttonmatrix_t *) obj;
    if (!btnm->button_areas || !btnm->ctrl_bits) return 0;
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    bool disabled_all = lv_obj_has_state(obj, LV_STATE_DISABLED);
    for (uint32_t i = 0; i < btnm->btn_cnt; i++) {
        lv_buttonmatrix_ctrl_t ctrl = btnm->ctrl_bits[i];
        if (ctrl & LV_BUTTONMATRIX_CTRL_HIDDEN) continue;
        uiif_obj o = { 0 };
        o.idx = w->next_idx++;
        o.parent = parent_idx;
        o.layer = layer;
        o.cls = "lv_buttonmatrix_button";
        o.text = lv_buttonmatrix_get_button_text(obj, i);
        lv_area_t a = btnm->button_areas[i];
        a.x1 += coords.x1;
        a.x2 += coords.x1;
        a.y1 += coords.y1;
        a.y2 += coords.y1;
        o.area = to_area(&a);
        o.visible_empty = clip->visible_empty || !uiif_clip(&o.area, &clip->visible, &o.visible);
        o.disabled = disabled_all || (ctrl & LV_BUTTONMATRIX_CTRL_DISABLED);
        o.clickable = !o.disabled;
        o.checked = (ctrl & LV_BUTTONMATRIX_CTRL_CHECKED) != 0;
        if (uiif_write_obj(w->f, &o) != 0) return -1;
    }
    return 0;
}

/* Writes obj's line; fills the frame its children will use. */
static int write_obj(walk_t * w, lv_obj_t * obj, int parent_idx, int layer, const frame_t * parent, frame_t * self) {
    uiif_obj o = { 0 };
    o.idx = w->next_idx++;
    o.parent = parent_idx;
    o.layer = layer;
    const lv_obj_class_t * cls = lv_obj_get_class(obj);
    o.cls = cls && cls->name ? cls->name : "obj";
    o.name = lv_obj_get_name(obj);
    o.text = obj_text(w, obj);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    o.area = to_area(&coords);
    o.visible_empty = parent->visible_empty || !uiif_clip(&o.area, &parent->visible, &o.visible);
    o.clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    o.checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    o.disabled = lv_obj_has_state(obj, LV_STATE_DISABLED);
    o.scrollable = can_scroll(obj);
    if (o.scrollable) {
        o.scroll_x = lv_obj_get_scroll_x(obj);
        o.scroll_y = lv_obj_get_scroll_y(obj);
    }
    if (uiif_write_obj(w->f, &o) != 0) return -1;

    self->obj = obj;
    self->idx = o.idx;
    self->next = 0;
    self->visible = o.visible;
    self->visible_empty = o.visible_empty;
    if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) return write_buttons(w, obj, o.idx, layer, self);
    return 0;
}

/* Pre-order walk of one layer, with a fixed stack and no allocation. */
static uiif_result walk_layer(walk_t * w, lv_obj_t * root, int layer, const uiif_area * screen) {
    if (!root || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return UIIF_OK;
    frame_t stack[UI_INSPECT_DEPTH];
    frame_t top = { NULL, -1, 0, *screen, false };
    int depth = 0;
    if (write_obj(w, root, -1, layer, &top, &stack[0]) != 0) return UIIF_WRITE;
    depth = 1;
    while (depth > 0) {
        frame_t * fr = &stack[depth - 1];
        if (fr->next >= lv_obj_get_child_count(fr->obj)) {
            depth--;
            continue;
        }
        lv_obj_t * child = lv_obj_get_child(fr->obj, (int32_t) fr->next++);
        if (!child || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        if (depth == UI_INSPECT_DEPTH) return UIIF_DEPTH;
        if (write_obj(w, child, fr->idx, layer, fr, &stack[depth]) != 0) return UIIF_WRITE;
        depth++;
    }
    return UIIF_OK;
}

static bool any_scrolling(void) {
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_scroll_obj(indev)) return true;
    return false;
}

static uiif_result emit_tree(FILE * f, unsigned seq, void * ctx) {
    (void) ctx;
    lv_display_t * disp = lv_display_get_default();
    uiif_header h = {
        .seq = seq,
        .w = disp ? lv_display_get_horizontal_resolution(disp) : 0,
        .h = disp ? lv_display_get_vertical_resolution(disp) : 0,
        .anims = lv_anim_count_running(),
        .scrolling = any_scrolling(),
        .screen_on = backlight_screen_is_on(),
    };
    if (uiif_write_header(f, &h) != 0) return UIIF_WRITE;
    uiif_area screen = { 0, 0, h.w - 1, h.h - 1 };
    walk_t w = { .f = f, .next_idx = 0 };
    lv_obj_t * layers[] = { lv_screen_active(), lv_layer_top(), lv_layer_sys() };
    for (int i = 0; i < 3; i++) {
        uiif_result r = walk_layer(&w, layers[i], i, &screen);
        if (r != UIIF_OK) return r;
    }
    return UIIF_OK;
}

void ui_inspect_poll(void) {
    (void) uiif_serve(UIIF_DIR, emit_tree, NULL);
}
