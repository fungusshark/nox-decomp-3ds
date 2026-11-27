/*
 * Modular Touch-Scrollable UI Containers, Text Panes & Clip Guards for Nox 3DS
 */

#include "platform_3ds.h"
#include "ui_scroll_3ds.h"

#include <string.h>

extern unsigned char byte_5D4594[];
extern int sub_46AA60(uint32_t *a1, uint32_t *a2, uint32_t *a3);
extern int sub_46A9B0(uint32_t *a1, int a2, int a3);
extern int sub_46B490(int a1, int a2, int a3, int a4);
extern int sub_4A2D10(int a1, int a2, int a3);

#define MAX_SCROLL_SLOTS     64
#define MAX_PANEL_CHILDREN   64
#define SCROLL_DRAG_THRESH   7
#define SCROLL_DPAD_STEP     28

enum {
    KIND_NONE    = 0,
    KIND_STATIC  = 1,
    KIND_LISTBOX = 2,
    KIND_PANEL   = 3,
    KIND_MANUAL  = 4
};

typedef struct {
    uint32_t key;
    uint8_t  kind;
    uint8_t  active_highlight;
    int16_t  abs_x, abs_y, view_w, view_h;
    int32_t  content_h;
    int32_t  scroll_y;
    uint32_t last_frame;
    void    *widget;
    void    *paired[4];
    uint8_t  paired_count;
    void    *children[MAX_PANEL_CHILDREN];
    int32_t  orig_y[MAX_PANEL_CHILDREN];
    uint8_t  child_count;
    uint8_t  shifted;
} scroll_slot_t;

static scroll_slot_t s_slots[MAX_SCROLL_SLOTS];
static uint32_t      s_frame = 1;
static uint32_t      s_last_interacted_key = 0;

/* Active drag tracking */
static uint32_t s_arm_key = 0;
static int      s_arm_x = 0;
static int      s_arm_y = 0;
static int      s_last_drag_y = 0;
static bool     s_armed = false;
static bool     s_dragging = false;
static uint32_t s_dpad_prev_held = 0;
static uint32_t s_dpad_hold_frames = 0;

static uint32_t ptr_key(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

bool nox_3ds_clip_push(nox_3ds_clip_guard_t *guard, int x, int y, int w, int h)
{
    if (!guard)
        return false;
    memset(guard, 0, sizeof(*guard));

    uint32_t *ctx = (uint32_t *)(uintptr_t)(*(uint32_t *)&byte_5D4594[3799572]);
    if (!ctx)
        return false;

    for (int i = 0; i < 13; ++i)
        guard->ctx_save[i] = ctx[i];
    for (int i = 0; i < 11; ++i)
        guard->slot_save[i] = *(uint32_t *)&byte_5D4594[1305732 + i * 4];
    *(uint32_t *)&byte_5D4594[1305748] = 0;
    guard->active = true;

    int cl = (int32_t)ctx[1];
    int ct = (int32_t)ctx[2];
    int cr = (int32_t)ctx[3];
    int cb = (int32_t)ctx[4];

    int ml = (int32_t)ctx[9];
    int mt = (int32_t)ctx[10];
    int mr = (int32_t)ctx[11];
    int mb = (int32_t)ctx[12];

    int nl = x;
    int nt = y;
    int nr = x + w;
    int nb = y + h;

    if (cl > nl) nl = cl;
    if (ml > nl) nl = ml;
    if (ct > nt) nt = ct;
    if (mt > nt) nt = mt;
    if (cr < nr) nr = cr;
    if (mr < nr) nr = mr;
    if (cb < nb) nb = cb;
    if (mb < nb) nb = mb;

    if (nl < nr && nt < nb) {
        ctx[1] = (uint32_t)(int32_t)nl;
        ctx[2] = (uint32_t)(int32_t)nt;
        ctx[3] = (uint32_t)(int32_t)nr;
        ctx[4] = (uint32_t)(int32_t)nb;

        ctx[5] = (uint32_t)(int32_t)nl;
        ctx[6] = (uint32_t)(int32_t)nt;
        ctx[7] = (uint32_t)(int32_t)(nr - 1);
        ctx[8] = (uint32_t)(int32_t)(nb - 1);

        ctx[9]  = (uint32_t)(int32_t)nl;
        ctx[10] = (uint32_t)(int32_t)nt;
        ctx[11] = (uint32_t)(int32_t)nr;
        ctx[12] = (uint32_t)(int32_t)nb;
        return true;
    }

    for (int i = 1; i <= 12; ++i)
        ctx[i] = 0;
    return false;
}

void nox_3ds_clip_pop(nox_3ds_clip_guard_t *guard)
{
    if (!guard || !guard->active)
        return;
    uint32_t *ctx = (uint32_t *)(uintptr_t)(*(uint32_t *)&byte_5D4594[3799572]);
    if (ctx) {
        for (int i = 0; i < 13; ++i)
            ctx[i] = guard->ctx_save[i];
    }
    for (int i = 0; i < 11; ++i)
        *(uint32_t *)&byte_5D4594[1305732 + i * 4] = guard->slot_save[i];
    guard->active = false;
}

static scroll_slot_t *find_slot(uint32_t key)
{
    if (!key)
        return NULL;
    for (int i = 0; i < MAX_SCROLL_SLOTS; ++i) {
        if (s_slots[i].key == key && s_slots[i].kind != KIND_NONE)
            return &s_slots[i];
    }
    return NULL;
}

static scroll_slot_t *alloc_slot(uint32_t key, uint8_t kind)
{
    scroll_slot_t *s = find_slot(key);
    if (s) {
        s->kind = kind;
        return s;
    }
    int best = -1;
    uint32_t oldest = s_frame;
    for (int i = 0; i < MAX_SCROLL_SLOTS; ++i) {
        if (s_slots[i].kind == KIND_NONE) {
            best = i;
            break;
        }
        if (s_slots[i].last_frame < oldest) {
            oldest = s_slots[i].last_frame;
            best = i;
        }
    }
    if (best < 0)
        best = 0;
    s = &s_slots[best];
    memset(s, 0, sizeof(*s));
    s->key = key;
    s->kind = kind;
    return s;
}

static int clamp_scroll(int sy, int content_h, int view_h)
{
    int max_s = content_h - view_h;
    if (max_s < 0)
        max_s = 0;
    if (sy < 0)
        return 0;
    if (sy > max_s)
        return max_s;
    return sy;
}

static void apply_listbox_scroll_px(uint32_t *lb, int scroll_px)
{
    if (!lb)
        return;
    uint32_t *v5 = (uint32_t *)(uintptr_t)lb[8];
    if (!v5)
        return;
    int total_h = (int)(int32_t)v5[10];
    int view_h  = (int)((const int16_t *)v5)[26];
    int clamped = clamp_scroll(scroll_px, total_h, view_h);
    ((int16_t *)v5)[27] = (int16_t)clamped;

    uint32_t slider_ptr = v5[9];
    if (slider_ptr) {
        sub_4A2D10((int)(uintptr_t)lb, 0, 0);
        uint32_t *sd = (uint32_t *)(uintptr_t)(((uint32_t *)(uintptr_t)slider_ptr)[8]);
        if (sd) {
            int max_range = (int)(int32_t)sd[1];
            if (max_range < 0) max_range = 0;
            int slider_val = max_range - clamped;
            if (slider_val < 0) slider_val = 0;
            sub_46B490((int)(uintptr_t)slider_ptr, 16394, slider_val, 0);
        }
    }
}

static void apply_slot_scroll(scroll_slot_t *s, int new_scroll_y)
{
    if (!s)
        return;
    int clamped = clamp_scroll(new_scroll_y, s->content_h, s->view_h);
    s->scroll_y = clamped;

    if (s->kind == KIND_LISTBOX && s->widget) {
        apply_listbox_scroll_px((uint32_t *)s->widget, clamped);
        for (int i = 0; i < s->paired_count; ++i) {
            if (s->paired[i])
                apply_listbox_scroll_px((uint32_t *)s->paired[i], clamped);
        }
    } else if (s->kind == KIND_PANEL && s->widget) {
        for (int i = 0; i < s->child_count; ++i) {
            uint32_t *c = (uint32_t *)s->children[i];
            if (!c)
                continue;
            int target_y = s->orig_y[i] - clamped;
            if ((int32_t)c[5] != target_y)
                sub_46A9B0(c, (int)(int32_t)c[4], target_y);
        }
    }
}

static int read_slot_scroll(const scroll_slot_t *s)
{
    if (!s)
        return 0;
    if (s->kind == KIND_LISTBOX && s->widget) {
        const uint32_t *lb = (const uint32_t *)s->widget;
        const uint32_t *v5 = (const uint32_t *)(uintptr_t)lb[8];
        if (v5)
            return (int)((const int16_t *)v5)[27];
    }
    return s->scroll_y;
}

void nox_3ds_ui_scroll_begin_frame(void)
{
    ++s_frame;
    for (int i = 0; i < MAX_SCROLL_SLOTS; ++i) {
        scroll_slot_t *s = &s_slots[i];
        if (s->kind == KIND_NONE)
            continue;
        if (s_frame - s->last_frame > 16) {
            if (s->key == s_arm_key) {
                s_armed = false;
                s_dragging = false;
                s_arm_key = 0;
            }
            if (s->key == s_last_interacted_key)
                s_last_interacted_key = 0;
            memset(s, 0, sizeof(*s));
        }
    }
}

void nox_3ds_ui_scroll_reset_all(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_arm_key = 0;
    s_armed = false;
    s_dragging = false;
    s_last_interacted_key = 0;
}

int nox_3ds_ui_scroll_update_region(uint32_t key, int x, int y, int w, int h,
                                    int content_h, bool draw_scrollbar)
{
    scroll_slot_t *s = alloc_slot(key, KIND_STATIC);
    s->abs_x = (int16_t)x;
    s->abs_y = (int16_t)y;
    s->view_w = (int16_t)w;
    s->view_h = (int16_t)h;
    s->content_h = content_h;
    s->last_frame = s_frame;
    s->scroll_y = clamp_scroll(s->scroll_y, content_h, h);

    if (draw_scrollbar && content_h > h && h >= 12 && w >= 16) {
        bool hi = (s_dragging && s_arm_key == key) || (s_last_interacted_key == key);
        int scr = nox_3ds_is_bottom_swbuf_active() ? NOX_3DS_SWBUF_BOTTOM : NOX_3DS_SWBUF_TOP;
        nox_3ds_ui_queue_scrollbar(scr, x + w - 5, y + 1, 4, h - 2,
                                   content_h, h, s->scroll_y, hi);
    }
    return s->scroll_y;
}

int nox_3ds_ui_scroll_get(uint32_t key)
{
    scroll_slot_t *s = find_slot(key);
    return s ? read_slot_scroll(s) : 0;
}

void nox_3ds_ui_scroll_set(uint32_t key, int scroll_y)
{
    scroll_slot_t *s = find_slot(key);
    if (s)
        apply_slot_scroll(s, scroll_y);
}

void nox_3ds_ui_scroll_step(uint32_t key, int delta_y)
{
    scroll_slot_t *s = find_slot(key);
    if (!s)
        return;
    int cur = read_slot_scroll(s);
    apply_slot_scroll(s, cur + delta_y);
    s_last_interacted_key = key;
}

static void add_lb_pair(void *lb, void *other)
{
    if (!lb || !other || lb == other)
        return;
    scroll_slot_t *s = alloc_slot(ptr_key(lb), KIND_LISTBOX);
    s->widget = lb;
    for (int i = 0; i < s->paired_count; ++i) {
        if (s->paired[i] == other)
            return;
    }
    if (s->paired_count < 4)
        s->paired[s->paired_count++] = other;
}

void nox_3ds_ui_scroll_register_listbox_group(void *primary_lb,
                                              void *col1, void *col2,
                                              void *col3, void *col4)
{
    void *cols[5] = { primary_lb, col1, col2, col3, col4 };
    for (int i = 0; i < 5; ++i) {
        if (!cols[i]) continue;
        for (int j = 0; j < 5; ++j) {
            if (cols[j] && i != j)
                add_lb_pair(cols[i], cols[j]);
        }
    }
}

void nox_3ds_ui_scroll_sync_listbox(void *lb)
{
    if (!lb)
        return;
    uint32_t *w = (uint32_t *)lb;
    uint32_t *v5 = (uint32_t *)(uintptr_t)w[8];
    if (!v5)
        return;

    uint32_t ax = 0, ay = 0;
    sub_46AA60(w, &ax, &ay);

    scroll_slot_t *s = alloc_slot(ptr_key(lb), KIND_LISTBOX);
    s->widget = lb;
    s->abs_x = (int16_t)(int32_t)ax;
    s->abs_y = (int16_t)(int32_t)ay;
    s->view_w = (int16_t)(int32_t)w[2];
    s->view_h = ((const int16_t *)v5)[26];
    s->content_h = (int32_t)v5[10];
    s->scroll_y = clamp_scroll((int)((const int16_t *)v5)[27], s->content_h, s->view_h);
    s->last_frame = s_frame;

    /* Sync paired columns if one of them is ahead/behind due to arrow keys or row click */
    if (s->paired_count > 0) {
        for (int i = 0; i < s->paired_count; ++i) {
            uint32_t *peer = (uint32_t *)s->paired[i];
            if (!peer) continue;
            uint32_t *pv5 = (uint32_t *)(uintptr_t)peer[8];
            if (!pv5) continue;
            if ((int)((const int16_t *)pv5)[27] != s->scroll_y)
                apply_listbox_scroll_px(peer, s->scroll_y);
        }
    }

    int wid = (int)(int32_t)w[0];
    /* Server Browser multi-column listboxes: only 10042 (rightmost) draws scrollbar */
    bool hide_sb = (wid >= 10038 && wid <= 10041);
    if (!hide_sb && s->content_h > s->view_h && s->view_h >= 12 && w[2] >= 20) {
        bool has_stock_slider = (v5[9] != 0);
        int sb_x = (int32_t)ax + (int32_t)w[2] - (has_stock_slider ? 14 : 6);
        bool hi = (s_dragging && s_arm_key == s->key) || (s_last_interacted_key == s->key);
        int scr = nox_3ds_is_bottom_swbuf_active() ? NOX_3DS_SWBUF_BOTTOM : NOX_3DS_SWBUF_TOP;
        nox_3ds_ui_queue_scrollbar(scr, sb_x, (int32_t)ay + 2, 5, (int32_t)w[3] - 4,
                                   s->content_h, s->view_h, s->scroll_y, hi);
    }
}

bool nox_3ds_ui_is_scrollable_panel_id(int id)
{
    return (id == 10151 || id == 10300 || id == 2100 || id == 10153);
}

bool nox_3ds_ui_panel_begin(void *panel_wnd, nox_3ds_panel_guard_t *guard)
{
    if (!guard)
        return true;
    memset(guard, 0, sizeof(*guard));
    guard->slot_idx = -1;

    uint32_t *w = (uint32_t *)panel_wnd;
    if (!w || !nox_3ds_ui_is_scrollable_panel_id((int)(int32_t)w[0]))
        return true;

    uint32_t ax = 0, ay = 0;
    sub_46AA60(w, &ax, &ay);
    int px = (int)(int32_t)ax;
    int py = (int)(int32_t)ay;
    int pw = (int)(int32_t)w[2];
    int ph = (int)(int32_t)w[3];
    int eff_h = ph;
    if (py + eff_h > NOX_3DS_LOGICAL_H)
        eff_h = NOX_3DS_LOGICAL_H - py;
    if (eff_h < 24)
        eff_h = ph;

    uint32_t key = ptr_key(panel_wnd);
    scroll_slot_t *s = alloc_slot(key, KIND_PANEL);
    s->widget = panel_wnd;
    s->abs_x = (int16_t)px;
    s->abs_y = (int16_t)py;
    s->view_w = (int16_t)pw;
    s->view_h = (int16_t)eff_h;
    s->last_frame = s_frame;

    /* Count direct children and check whether this slot owns the exact same chain */
    uint32_t *first_c = (uint32_t *)(uintptr_t)w[100];
    int cnt = 0;
    for (uint32_t *c = first_c; c && cnt < MAX_PANEL_CHILDREN; c = (uint32_t *)(uintptr_t)c[97])
        ++cnt;

    bool same_layout = (s->shifted && s->child_count == cnt &&
                        (cnt == 0 || s->children[0] == (void *)first_c));
    if (!same_layout) {
        int idx = 0;
        for (uint32_t *c = first_c; c && idx < MAX_PANEL_CHILDREN; c = (uint32_t *)(uintptr_t)c[97]) {
            s->children[idx] = (void *)c;
            s->orig_y[idx] = (int32_t)c[5];
            ++idx;
        }
        s->child_count = (uint8_t)idx;
        s->scroll_y = 0;
        s->shifted = 1;
    }

    int max_bottom = eff_h;
    for (int i = 0; i < s->child_count; ++i) {
        uint32_t *c = (uint32_t *)s->children[i];
        if (!c || (c[1] & 0x10u))
            continue;
        int bottom = s->orig_y[i] + (int32_t)c[3] + 6;
        if (bottom > max_bottom)
            max_bottom = bottom;
    }
    s->content_h = max_bottom;
    apply_slot_scroll(s, s->scroll_y);

    guard->slot_idx = (int32_t)(s - s_slots);
    guard->active = true;
    return nox_3ds_clip_push(&guard->clip, px, py, pw, eff_h);
}

void nox_3ds_ui_panel_end(void *panel_wnd, nox_3ds_panel_guard_t *guard)
{
    (void)panel_wnd;
    if (!guard || !guard->active)
        return;

    nox_3ds_clip_pop(&guard->clip);

    if (guard->slot_idx >= 0 && guard->slot_idx < MAX_SCROLL_SLOTS) {
        scroll_slot_t *s = &s_slots[guard->slot_idx];
        if (s->content_h > s->view_h && s->view_h >= 24 && s->view_w >= 24) {
            bool hi = (s_dragging && s_arm_key == s->key) || (s_last_interacted_key == s->key);
            int scr = nox_3ds_is_bottom_swbuf_active() ? NOX_3DS_SWBUF_BOTTOM : NOX_3DS_SWBUF_TOP;
            nox_3ds_ui_queue_scrollbar(scr, s->abs_x + s->view_w - 6, s->abs_y + 2, 5,
                                       s->view_h - 4, s->content_h, s->view_h,
                                       s->scroll_y, hi);
        }
    }
    guard->active = false;
}

static bool point_in_slot(const scroll_slot_t *s, int x, int y)
{
    return (s->kind != KIND_NONE &&
            s->content_h > s->view_h &&
            (s_frame - s->last_frame) <= 3 &&
            x >= s->abs_x && x < (s->abs_x + s->view_w) &&
            y >= s->abs_y && y < (s->abs_y + s->view_h));
}

static bool is_widget_descendant(const uint32_t *w, const uint32_t *ancestor)
{
    if (!w || !ancestor)
        return false;
    for (int depth = 0; w && depth < 16; ++depth) {
        if (w == ancestor)
            return true;
        w = (const uint32_t *)(uintptr_t)w[99];
    }
    return false;
}

static bool is_listbox_slider_part(const uint32_t *hit, const uint32_t *lb)
{
    if (!hit || !lb || !lb[8])
        return false;
    const uint32_t *v5 = (const uint32_t *)(uintptr_t)lb[8];
    const uint32_t *slider = (const uint32_t *)(uintptr_t)v5[9];
    if (slider && is_widget_descendant(hit, slider))
        return true;
    /* Up/down arrows belong to parent container next to listbox; check slider id +/- 2 */
    if (slider && hit[0] != 0 && slider[0] != 0) {
        int diff = (int)(int32_t)hit[0] - (int)(int32_t)slider[0];
        if (diff >= -2 && diff <= 2 && hit[2] <= 24 && hit[3] <= 24)
            return true;
    }
    return false;
}

void nox_3ds_ui_scroll_maybe_arm(int touch_x, int touch_y, void *hit_wnd)
{
    const uint32_t *hit = (const uint32_t *)hit_wnd;
    int best_area = 0x7FFFFFFF;
    scroll_slot_t *best = NULL;

    for (int i = 0; i < MAX_SCROLL_SLOTS; ++i) {
        scroll_slot_t *s = &s_slots[i];
        if (!point_in_slot(s, touch_x, touch_y))
            continue;
        if (s->kind == KIND_LISTBOX && s->widget) {
            if (is_listbox_slider_part(hit, (const uint32_t *)s->widget))
                continue;
        }
        int area = (int)s->view_w * (int)s->view_h;
        if (hit && s->widget && is_widget_descendant(hit, (const uint32_t *)s->widget))
            area /= 4; /* prefer slot containing hit control over parent panel */
        if (area < best_area) {
            best_area = area;
            best = s;
        }
    }

    if (best) {
        s_arm_key = best->key;
        s_arm_x = touch_x;
        s_arm_y = touch_y;
        s_last_drag_y = touch_y;
        s_armed = true;
        s_dragging = false;
        s_last_interacted_key = best->key;
    } else {
        s_armed = false;
        s_dragging = false;
        s_arm_key = 0;
    }
}

bool nox_3ds_ui_scroll_pump_drag(int touch_x, int touch_y,
                                 const int32_t *ev4, int32_t *captured_wnd)
{
    (void)touch_x;
    bool released = false;
    bool pressed = false;
    if (ev4) {
        for (int i = 0; i < 4; ++i) {
            if (ev4[i] == 5) pressed = true;
            if (ev4[i] == 6 || ev4[i] == 7) released = true;
        }
    }

    if (!s_armed || pressed) {
        if (pressed)
            s_dragging = false;
        return false;
    }

    scroll_slot_t *s = find_slot(s_arm_key);
    if (!s || s->content_h <= s->view_h) {
        s_armed = false;
        s_dragging = false;
        s_arm_key = 0;
        return false;
    }

    bool active_touch = nox_3ds_input_is_bottom_touch_active();
    if (!s_dragging) {
        int dy = touch_y - s_arm_y;
        if (dy < 0) dy = -dy;
        if (active_touch && !released && dy >= SCROLL_DRAG_THRESH) {
            s_dragging = true;
            s_last_drag_y = s_arm_y;
            if (captured_wnd)
                *captured_wnd = 0;
        }
    }

    if (s_dragging) {
        int step = s_last_drag_y - touch_y;
        if (step != 0) {
            int cur = read_slot_scroll(s);
            apply_slot_scroll(s, cur + step);
            s_last_drag_y = touch_y;
            nox_3ds_swbuf_invalidate_bottom();
        }
        if (captured_wnd)
            *captured_wnd = 0;

        if (released || !active_touch) {
            s_armed = false;
            s_dragging = false;
            s_arm_key = 0;
        }
        return true; /* swallow release / move so child widgets don't trigger click */
    }

    if (released || !active_touch) {
        s_armed = false;
        s_arm_key = 0;
    }
    return false;
}

void nox_3ds_ui_scroll_pump_dpad(void)
{
    const nox_3ds_input_t *inp = nox_3ds_input_get();
    if (!inp || s_dragging)
        return;

    uint32_t held = inp->keys_held & (NOX_3DS_KEY_DUP | NOX_3DS_KEY_DDOWN);
    if (!held) {
        s_dpad_prev_held = 0;
        s_dpad_hold_frames = 0;
        return;
    }

    bool fire = false;
    if (held != s_dpad_prev_held) {
        fire = true;
        s_dpad_hold_frames = 0;
    } else {
        ++s_dpad_hold_frames;
        if (s_dpad_hold_frames >= 12 && (s_dpad_hold_frames % 3) == 0)
            fire = true;
    }
    s_dpad_prev_held = held;
    if (!fire)
        return;

    scroll_slot_t *target = find_slot(s_last_interacted_key);
    if (!target || target->content_h <= target->view_h || (s_frame - target->last_frame) > 3) {
        target = NULL;
        int best_area = -1;
        for (int i = 0; i < MAX_SCROLL_SLOTS; ++i) {
            scroll_slot_t *s = &s_slots[i];
            if (s->kind != KIND_NONE && s->content_h > s->view_h && (s_frame - s->last_frame) <= 3) {
                int area = (int)s->view_w * (int)s->view_h;
                if (area > best_area) {
                    best_area = area;
                    target = s;
                }
            }
        }
    }

    if (!target)
        return;

    int delta = (held & NOX_3DS_KEY_DDOWN) ? SCROLL_DPAD_STEP : -SCROLL_DPAD_STEP;
    int cur = read_slot_scroll(target);
    int next = clamp_scroll(cur + delta, target->content_h, target->view_h);
    if (next != cur) {
        apply_slot_scroll(target, next);
        nox_3ds_swbuf_invalidate_bottom();
    }
}

bool nox_3ds_ui_scroll_is_dragging(void)
{
    return s_dragging;
}
