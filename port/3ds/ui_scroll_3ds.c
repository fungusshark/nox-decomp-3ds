/*
 * Modular Touch-Scrollable UI Containers, Text Panes & Clip Guards for Nox 3DS
 */

#include "platform_3ds.h"
#include "ui_scroll_3ds.h"

#include <string.h>

extern unsigned char byte_5D4594[];
extern int sub_46A8C0(int a1);
extern int sub_46AA60(uint32_t *a1, uint32_t *a2, uint32_t *a3);
extern uint32_t *sub_46B0C0(uint32_t *a1, int a2);
extern int sub_46A9B0(uint32_t *a1, int a2, int a3);
extern int sub_46AB20(uint32_t *a1, int a2, int a3);
extern int sub_46B490(int a1, int a2, int a3, int a4);
extern int sub_4A2D10(int a1, int a2, int a3);
extern int sub_4344A0(int a1, int a2, int a3);
extern int sub_43F320(int a1);
extern int sub_43F840(int a1, uint16_t *a2, int *a3, uint32_t *a4, int a5);
extern int sub_488160(int a1, int a2);
extern int sub_488870(int a1, int a2);
extern int sub_488D00(int a1, int a2);
extern int sub_489550(uint32_t *a1, uint32_t *a2);
extern int sub_4A3C50(uint32_t *a1, int a2);
extern int sub_4A3FC0(uint32_t *a1, int a2);
extern int sub_4A81D0(int a1, int a2);
extern int sub_4A8380(int a1, int a2);
extern int sub_4A8820(int a1, int a2);
extern int sub_4A8A20(int a1, int a2);
extern int sub_4A8EA0(int a1, int a2);
extern int sub_4A9050(int a1, int a2);

#define MAX_SCROLL_SLOTS     64
#define MAX_PANEL_CHILDREN   64
#define MAX_PANEL_METAS      24
#define MAX_MODAL_SNAPS      8
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
    int16_t  pad_top, pad_bottom;
    int32_t  content_h;
    int32_t  scroll_y;
    uint32_t last_frame;
    void    *widget;
    void    *paired[4];
    uint8_t  paired_count;
    void    *children[MAX_PANEL_CHILDREN];
    int32_t  orig_y[MAX_PANEL_CHILDREN];
    uint8_t  pinned[MAX_PANEL_CHILDREN];
    uint8_t  child_count;
    uint8_t  shifted;
    uint8_t  has_pinned;
} scroll_slot_t;

typedef struct {
    void    *panel_wnd;
    int16_t  pad_top;
    int16_t  pad_bottom;
    void    *pinned[16];
    uint8_t  pinned_count;
} panel_meta_t;

typedef struct {
    void    *wnd;
    int16_t  orig_w, orig_h;
    uint32_t orig_flags;
    uint32_t orig_style;
    uint32_t orig_bg_color;
    uint8_t  child_count;
    void    *c_ptr[MAX_PANEL_CHILDREN];
    int16_t  c_x[MAX_PANEL_CHILDREN];
    int16_t  c_y[MAX_PANEL_CHILDREN];
    int16_t  c_w[MAX_PANEL_CHILDREN];
    int16_t  c_h[MAX_PANEL_CHILDREN];
    uint32_t c_flags[MAX_PANEL_CHILDREN];
    uint32_t c_bg[MAX_PANEL_CHILDREN];
    uint32_t c_draw[MAX_PANEL_CHILDREN];
} modal_snap_t;

static scroll_slot_t s_slots[MAX_SCROLL_SLOTS];
static panel_meta_t  s_panel_metas[MAX_PANEL_METAS];
static modal_snap_t  s_modal_snaps[MAX_MODAL_SNAPS];
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
            if (!c || s->pinned[i])
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
    memset(s_panel_metas, 0, sizeof(s_panel_metas));
    memset(s_modal_snaps, 0, sizeof(s_modal_snaps));
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
    /* Server Browser multi-column listboxes: only 10042 (rightmost) draws scrollbar.
     * Also skip the native GPU scrollbar whenever the listbox already has its own
     * stock software scrollbar (v5[9] != 0) to avoid duplicate scrollbars. */
    bool hide_sb = (wid >= 10038 && wid <= 10041);
    bool has_stock_slider = (v5[9] != 0);
    if (!hide_sb && !has_stock_slider && s->content_h > s->view_h && s->view_h >= 12 && w[2] >= 20) {
        int sb_x = (int32_t)ax + (int32_t)w[2] - 6;
        bool hi = (s_dragging && s_arm_key == s->key) || (s_last_interacted_key == s->key);
        int scr = nox_3ds_is_bottom_swbuf_active() ? NOX_3DS_SWBUF_BOTTOM : NOX_3DS_SWBUF_TOP;
        nox_3ds_ui_queue_scrollbar(scr, sb_x, (int32_t)ay + 2, 5, (int32_t)w[3] - 4,
                                   s->content_h, s->view_h, s->scroll_y, hi);
    }
}

static panel_meta_t *find_panel_meta(const void *panel_wnd)
{
    if (!panel_wnd)
        return NULL;
    for (int i = 0; i < MAX_PANEL_METAS; ++i) {
        if (s_panel_metas[i].panel_wnd == panel_wnd)
            return &s_panel_metas[i];
    }
    return NULL;
}

static panel_meta_t *alloc_panel_meta(void *panel_wnd)
{
    panel_meta_t *m = find_panel_meta(panel_wnd);
    if (m)
        return m;
    for (int i = 0; i < MAX_PANEL_METAS; ++i) {
        if (!s_panel_metas[i].panel_wnd) {
            m = &s_panel_metas[i];
            memset(m, 0, sizeof(*m));
            m->panel_wnd = panel_wnd;
            return m;
        }
    }
    /* Overwrite oldest slot 0 */
    m = &s_panel_metas[0];
    memset(m, 0, sizeof(*m));
    m->panel_wnd = panel_wnd;
    return m;
}

static bool meta_is_pinned(const panel_meta_t *m, const void *child)
{
    if (!m || !child)
        return false;
    for (int i = 0; i < m->pinned_count; ++i) {
        if (m->pinned[i] == child)
            return true;
    }
    return false;
}

bool nox_3ds_ui_is_scrollable_panel_id(int id)
{
    return (id == 10151 || id == 10153 || id == 10300 || id == 10500 ||
            id == 2100);
}

bool nox_3ds_ui_panel_begin(void *panel_wnd, nox_3ds_panel_guard_t *guard)
{
    if (!guard)
        return true;
    memset(guard, 0, sizeof(*guard));
    guard->slot_idx = -1;

    uint32_t *w = (uint32_t *)panel_wnd;
    if (!w)
        return true;
    const panel_meta_t *meta = find_panel_meta(panel_wnd);
    if (!meta && !nox_3ds_ui_is_scrollable_panel_id((int)(int32_t)w[0]))
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

    int pad_top = meta ? (int)meta->pad_top : 0;
    int pad_bot = meta ? (int)meta->pad_bottom : 0;
    if (pad_top < 0) pad_top = 0;
    if (pad_bot < 0) pad_bot = 0;
    if (pad_top + pad_bot >= eff_h - 24) {
        pad_top = 0;
        pad_bot = 0;
    }
    int view_y = py + pad_top;
    int view_h = eff_h - pad_top - pad_bot;

    uint32_t key = ptr_key(panel_wnd);
    scroll_slot_t *s = alloc_slot(key, KIND_PANEL);
    s->widget = panel_wnd;
    s->abs_x = (int16_t)px;
    s->abs_y = (int16_t)view_y;
    s->view_w = (int16_t)pw;
    s->view_h = (int16_t)view_h;
    s->pad_top = (int16_t)pad_top;
    s->pad_bottom = (int16_t)pad_bot;
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
        uint8_t any_pin = 0;
        for (uint32_t *c = first_c; c && idx < MAX_PANEL_CHILDREN; c = (uint32_t *)(uintptr_t)c[97]) {
            s->children[idx] = (void *)c;
            s->orig_y[idx] = (int32_t)c[5];
            uint8_t pin = meta_is_pinned(meta, (void *)c) ? 1u : 0u;
            s->pinned[idx] = pin;
            if (pin) any_pin = 1;
            ++idx;
        }
        s->child_count = (uint8_t)idx;
        s->has_pinned = any_pin;
        s->scroll_y = 0;
        s->shifted = 1;
    }

    int max_bottom = view_h;
    for (int i = 0; i < s->child_count; ++i) {
        uint32_t *c = (uint32_t *)s->children[i];
        if (!c || (c[1] & 0x10u) || s->pinned[i])
            continue;
        int bottom = (s->orig_y[i] - pad_top) + (int32_t)c[3] + 8;
        if (bottom > max_bottom)
            max_bottom = bottom;
    }
    s->content_h = max_bottom;
    apply_slot_scroll(s, s->scroll_y);

    guard->slot_idx = (int32_t)(s - s_slots);
    guard->active = true;
    guard->has_pinned = (s->has_pinned != 0);
    return nox_3ds_clip_push(&guard->clip, px, view_y, pw, view_h);
}

bool nox_3ds_ui_panel_child_is_pinned(const nox_3ds_panel_guard_t *guard, const void *child_wnd)
{
    if (!guard || !guard->has_pinned || !child_wnd)
        return false;
    if (guard->slot_idx < 0 || guard->slot_idx >= MAX_SCROLL_SLOTS)
        return false;
    const scroll_slot_t *s = &s_slots[guard->slot_idx];
    for (int i = 0; i < s->child_count; ++i) {
        if (s->children[i] == child_wnd)
            return s->pinned[i] != 0;
    }
    return false;
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
            nox_3ds_ui_queue_scrollbar(scr, s->abs_x + s->view_w - 8, s->abs_y + 2, 5,
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

/* =========================================================================
 * Single-Line Ellipsis Truncation & Bottom-Screen Takeover / Auto-Expand
 * ========================================================================= */

static inline bool valid_u32_ptr(uint32_t p)
{
    return p >= 0x00100000u && (p & 3u) == 0u;
}

static bool is_static_draw(uintptr_t fn)
{
    return fn == (uintptr_t)sub_488D00 || fn == (uintptr_t)sub_489550;
}

static bool is_checkbox_or_radio(uintptr_t fn)
{
    return fn == (uintptr_t)sub_4A8820 || fn == (uintptr_t)sub_4A8A20 ||
           fn == (uintptr_t)sub_4A8EA0 || fn == (uintptr_t)sub_4A9050;
}

static bool is_pushbutton(uintptr_t fn)
{
    return fn == (uintptr_t)sub_4A81D0 || fn == (uintptr_t)sub_4A8380;
}

static bool is_editbox(uintptr_t fn)
{
    return fn == (uintptr_t)sub_488160 || fn == (uintptr_t)sub_488870;
}

static bool is_listbox(uintptr_t fn)
{
    return fn == (uintptr_t)sub_4A3C50 || fn == (uintptr_t)sub_4A3FC0;
}

static void convert_to_stone_takeover_bg(uint32_t *w)
{
    if (!w)
        return;
    /* Switch from fixed-size sprite background (0x80) to resizable solid fill +
     * Nox's native 9-slice medieval stone border (sub_4AA030 / sub_4A9E90). */
    w[1] &= ~0x80u;
    w[1] |= 0x1000u;
    w[11] |= 2048u;
    w[14] = (uint32_t)sub_4344A0(22, 18, 15);
    w[16] = (uint32_t)sub_4344A0(112, 88, 54);
}

static void convert_button_to_resizable(uint32_t *b)
{
    if (!b)
        return;
    /* sub_4A8380 always blits a fixed-size stock button image sprite at xLeft
     * while centering text at xLeft + w/2. Switching to sub_4A81D0 renders a
     * full-width medieval button box with text centered directly inside it. */
    if ((uintptr_t)b[95] == (uintptr_t)sub_4A8380)
        b[95] = (uint32_t)(uintptr_t)sub_4A81D0;
    b[1] &= ~0x80u;
    b[14] = (uint32_t)sub_4344A0(56, 40, 28);    /* enabled fill (a2+20) */
    b[16] = (uint32_t)sub_4344A0(168, 134, 82);  /* normal border (a2+28) */
    b[18] = (uint32_t)sub_4344A0(232, 196, 120); /* hover border (a2+36) */
    b[20] = (uint32_t)sub_4344A0(36, 30, 26);    /* disabled fill (a2+44) */
    b[22] = (uint32_t)sub_4344A0(92, 66, 40);    /* pressed fill (a2+52) */
}

/* Keep Westwood's authentic software-textured button sprite (sub_4A8380 / 0x80)
 * centered in [slot_x .. slot_x + slot_w] whenever its label fits inside the
 * stock sprite width. Fall back to the resizable flat medieval style only when
 * the label would genuinely clip or the button has no textured sprite. */
static void place_button_preserving_texture(uint32_t *b, int slot_x, int slot_y,
                                            int slot_w, int slot_h, int min_fallback_w)
{
    if (!b)
        return;
    int ow = (int)(int32_t)b[2];
    int oh = (int)(int32_t)b[3];
    bool is_textured = ((b[1] & 0x80u) != 0) ||
                       ((uintptr_t)b[95] == (uintptr_t)sub_4A8380);
    int lw = 0;
    uint16_t *lbl = (uint16_t *)(b + 27);
    if (*lbl)
        sub_43F840((int)b[59], lbl, &lw, NULL, 0);

    if (is_textured && ow > 0 && oh > 0 && ow >= lw + 4 && ow <= slot_w) {
        int bx = slot_x + (slot_w - ow) / 2;
        int by = slot_y + ((slot_h > oh) ? (slot_h - oh) / 2 : 0);
        sub_46A9B0(b, bx, by);
        sub_46AB20(b, ow, oh);
        return;
    }

    convert_button_to_resizable(b);
    int nw = lw + 20;
    if (nw < min_fallback_w) nw = min_fallback_w;
    if (nw > slot_w) nw = slot_w;
    int nh = (slot_h < 26) ? 26 : slot_h;
    int bx = slot_x + (slot_w - nw) / 2;
    sub_46A9B0(b, bx, slot_y);
    sub_46AB20(b, nw, nh);
}

static void scale_editbox_field_width(uint32_t *c, int new_w, float sx)
{
    if (!c || !is_editbox((uintptr_t)c[95]) || !valid_u32_ptr(c[8]))
        return;
    uint16_t *eb = (uint16_t *)(uintptr_t)c[8];
    int max_chars = (int)eb[520];
    int ebw = (int)eb[521];
    int lw = 0;
    uint16_t *lbl = (uint16_t *)(c + 27);
    if (*lbl)
        sub_43F840((int)c[59], lbl, &lw, NULL, 0);

    int max_eb = (lw > 0) ? (new_w - lw - 14) : (new_w - 4);
    if (lw > 0 && max_eb > new_w * 3 / 5)
        max_eb = new_w * 3 / 5;
    if (max_chars > 0 && max_chars <= 5 && lw > 0 && max_eb > 84)
        max_eb = 84;
    int min_eb = (max_chars > 0 && max_chars <= 5) ? 48 : 64;
    if (max_eb < min_eb)
        max_eb = (new_w > lw + 44) ? (new_w - lw - 10) : min_eb;
    if (max_eb < 36)
        max_eb = 36;

    if (ebw > 0) {
        int scaled = (int)((float)ebw * sx + 0.5f);
        if (scaled < min_eb && max_eb >= min_eb)
            scaled = min_eb;
        if (scaled > max_eb)
            scaled = max_eb;
        eb[521] = (uint16_t)scaled;
    } else if (lw > 0) {
        eb[521] = (uint16_t)max_eb;
    }
}

static void reset_listbox_metrics(uint32_t *lb)
{
    if (!lb || !is_listbox((uintptr_t)lb[95]) || !valid_u32_ptr(lb[8]))
        return;
    uint32_t *v5 = (uint32_t *)(uintptr_t)lb[8];
    if (valid_u32_ptr(v5[6]))
        *(uint8_t *)((uintptr_t)v5[6] + 521u) = 0u;
}

static void invalidate_panel_slot(const void *panel_wnd)
{
    scroll_slot_t *s = find_slot(ptr_key(panel_wnd));
    if (s) {
        s->shifted = 0;
        s->scroll_y = 0;
    }
}

const uint16_t *nox_3ds_ui_fit_ellipsis(int font_ptr, const uint16_t *src,
                                        uint16_t *dst, int dst_cap, int max_w)
{
    if (!src || !*src || !dst || dst_cap < 5 || max_w <= 0)
        return src;

    int full_w = 0;
    sub_43F840(font_ptr, (uint16_t *)src, &full_w, NULL, 0);
    if (full_w <= max_w)
        return src;

    static uint16_t s_dots[4] = { '.', '.', '.', 0 };
    int dots_w = 0;
    sub_43F840(font_ptr, s_dots, &dots_w, NULL, 0);

    int len = 0;
    while (src[len] && len < dst_cap - 4)
        ++len;

    /* Binary search longest prefix that fits alongside "..." */
    int lo = 0, hi = len, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        for (int i = 0; i < mid; ++i)
            dst[i] = src[i];
        dst[mid] = 0;
        int pw = 0;
        if (mid > 0)
            sub_43F840(font_ptr, dst, &pw, NULL, 0);
        if (pw + dots_w <= max_w) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    while (best > 0 && (src[best - 1] == ' ' || src[best - 1] == '\t'))
        --best;
    for (int i = 0; i < best; ++i)
        dst[i] = src[i];
    dst[best]     = '.';
    dst[best + 1] = '.';
    dst[best + 2] = '.';
    dst[best + 3] = 0;
    return dst;
}

static modal_snap_t *get_or_create_modal_snap(uint32_t *w)
{
    int cnt = 0;
    for (uint32_t *c = (uint32_t *)(uintptr_t)w[100]; c && cnt < MAX_PANEL_CHILDREN;
         c = (uint32_t *)(uintptr_t)c[97])
        ++cnt;

    for (int i = 0; i < MAX_MODAL_SNAPS; ++i) {
        modal_snap_t *sn = &s_modal_snaps[i];
        if (sn->wnd == (void *)w) {
            if (sn->child_count == cnt &&
                (cnt == 0 || sn->c_ptr[0] == (void *)(uintptr_t)w[100])) {
                /* Restore pristine stock layout before re-expanding */
                w[1]  = sn->orig_flags;
                w[11] = sn->orig_style;
                w[14] = sn->orig_bg_color;
                sub_46AB20(w, sn->orig_w, sn->orig_h);
                for (int k = 0; k < sn->child_count; ++k) {
                    uint32_t *c = (uint32_t *)sn->c_ptr[k];
                    if (!c) continue;
                    c[1]  = sn->c_flags[k];
                    c[14] = sn->c_bg[k];
                    c[95] = sn->c_draw[k];
                    sub_46A9B0(c, sn->c_x[k], sn->c_y[k]);
                    sub_46AB20(c, sn->c_w[k], sn->c_h[k]);
                }
                return sn;
            }
            /* Window pointer reused for a different child tree: re-snapshot */
            memset(sn, 0, sizeof(*sn));
            sn->wnd = (void *)w;
            break;
        }
    }

    modal_snap_t *sn = NULL;
    for (int i = 0; i < MAX_MODAL_SNAPS; ++i) {
        if (s_modal_snaps[i].wnd == (void *)w || !s_modal_snaps[i].wnd) {
            sn = &s_modal_snaps[i];
            break;
        }
    }
    if (!sn)
        sn = &s_modal_snaps[0];

    memset(sn, 0, sizeof(*sn));
    sn->wnd           = (void *)w;
    sn->orig_w        = (int16_t)(int32_t)w[2];
    sn->orig_h        = (int16_t)(int32_t)w[3];
    sn->orig_flags    = w[1];
    sn->orig_style    = w[11];
    sn->orig_bg_color = w[14];
    int idx = 0;
    for (uint32_t *c = (uint32_t *)(uintptr_t)w[100]; c && idx < MAX_PANEL_CHILDREN;
         c = (uint32_t *)(uintptr_t)c[97]) {
        sn->c_ptr[idx]   = (void *)c;
        sn->c_x[idx]     = (int16_t)(int32_t)c[4];
        sn->c_y[idx]     = (int16_t)(int32_t)c[5];
        sn->c_w[idx]     = (int16_t)(int32_t)c[2];
        sn->c_h[idx]     = (int16_t)(int32_t)c[3];
        sn->c_flags[idx] = c[1];
        sn->c_bg[idx]    = c[14];
        sn->c_draw[idx]  = c[95];
        ++idx;
    }
    sn->child_count = (uint8_t)idx;
    return sn;
}

void nox_3ds_ui_expand_modal_dialog(void *dlg_wnd)
{
    uint32_t *w = (uint32_t *)dlg_wnd;
    if (!w || !nox_3ds_font_is_enabled())
        return;

    modal_snap_t *sn = get_or_create_modal_snap(w);
    int orig_w = (int)sn->orig_w;
    int orig_h = (int)sn->orig_h;
    if (orig_w <= 0 || orig_h <= 0 || orig_w >= 600)
        return;

    /* Detect overflowing text/checkbox/button children */
    bool need_expand = false;
    int  want_w = orig_w;
    int  body_idx = -1;
    int  best_body_area = 0;

    for (int i = 0; i < sn->child_count; ++i) {
        uint32_t *c = (uint32_t *)sn->c_ptr[i];
        if (!c || (c[1] & 0x10u))
            continue;
        uintptr_t fn = (uintptr_t)c[95];
        int cw = (int)sn->c_w[i];
        int ch = (int)sn->c_h[i];
        int fptr = (int)c[59];

        if (fn == (uintptr_t)sub_488D00 && valid_u32_ptr(c[8])) {
            uint16_t *st = *(uint16_t **)(uintptr_t)c[8];
            if (st && *st) {
                int mw = 0;
                uint32_t mh = 0;
                sub_43F840(fptr, st, &mw, &mh, cw);
                if (ch >= 28 && cw * ch > best_body_area) {
                    best_body_area = cw * ch;
                    body_idx = i;
                }
                if ((int)mh > ch || (ch <= 24 && mw > cw)) {
                    need_expand = true;
                    if (want_w < 560)
                        want_w = 560;
                }
            }
        } else if (is_checkbox_or_radio(fn)) {
            uint16_t *lbl = (uint16_t *)(c + 27);
            if (*lbl) {
                int lw = 0;
                sub_43F840(fptr, lbl, &lw, NULL, 0);
                int req_cw = lw + 42;
                if (req_cw > cw) {
                    need_expand = true;
                    int req_dw = req_cw + 44;
                    if (req_dw > want_w)
                        want_w = req_dw;
                }
            }
        } else if (is_pushbutton(fn)) {
            uint16_t *lbl = (uint16_t *)(c + 27);
            if (*lbl) {
                int lw = 0;
                sub_43F840(fptr, lbl, &lw, NULL, 0);
                if (lw + 14 > cw) {
                    need_expand = true;
                    if (want_w < orig_w + 80)
                        want_w = orig_w + 80;
                }
            }
        }
    }

    if (!need_expand)
        return;

    if (want_w > 580) want_w = 580;
    if (want_w < orig_w) want_w = orig_w;
    int dw = want_w - orig_w;

    /* Compute needed height growth from main body static text after widening */
    int dh = 0;
    int body_bottom_orig = orig_h / 2;
    if (body_idx >= 0) {
        uint32_t *bc = (uint32_t *)sn->c_ptr[body_idx];
        int bcw_new = (int)sn->c_w[body_idx] + dw;
        int bch_orig = (int)sn->c_h[body_idx];
        body_bottom_orig = (int)sn->c_y[body_idx] + bch_orig;
        if (valid_u32_ptr(bc[8])) {
            uint16_t *st = *(uint16_t **)(uintptr_t)bc[8];
            if (st && *st) {
                uint32_t mh2 = 0;
                sub_43F840((int)bc[59], st, NULL, &mh2, bcw_new);
                if ((int)mh2 + 12 > bch_orig)
                    dh = (int)mh2 + 12 - bch_orig;
            }
        }
    }

    int want_h = orig_h + dh;
    if (want_h > 432) {
        want_h = 432;
        dh = want_h - orig_h;
        if (dh < 0) dh = 0;
    }

    if (dw > 0 || dh > 0) {
        convert_to_stone_takeover_bg(w);
        sub_46AB20(w, want_w, want_h);
        sub_46A9B0(w, (NOX_3DS_LOGICAL_W - want_w) / 2, (NOX_3DS_LOGICAL_H - want_h) / 2);

        for (int i = 0; i < sn->child_count; ++i) {
            uint32_t *c = (uint32_t *)sn->c_ptr[i];
            if (!c)
                continue;
            uintptr_t fn = (uintptr_t)c[95];
            int ox = (int)sn->c_x[i];
            int oy = (int)sn->c_y[i];
            int ow = (int)sn->c_w[i];
            int oh = (int)sn->c_h[i];

            if (i == body_idx) {
                sub_46A9B0(c, ox, oy);
                sub_46AB20(c, ow + dw, oh + dh);
            } else if (oy < body_bottom_orig - 8 && oy < orig_h / 3) {
                /* Header / title near top */
                if (is_static_draw(fn) && ow >= orig_w / 2) {
                    sub_46A9B0(c, ox, oy);
                    sub_46AB20(c, ow + dw, oh);
                } else {
                    sub_46A9B0(c, ox + dw / 2, oy);
                }
            } else if (is_checkbox_or_radio(fn)) {
                int nx = (ox > 20) ? 20 : ox;
                int nw = want_w - 2 * nx;
                if (nw < ow) nw = ow;
                sub_46A9B0(c, nx, oy + dh);
                sub_46AB20(c, nw, (oh < 22) ? 22 : oh);
            } else if (is_editbox(fn)) {
                sub_46A9B0(c, ox, oy + dh);
                sub_46AB20(c, ow + dw, oh);
            } else if (is_pushbutton(fn)) {
                int nw = ow;
                uint16_t *lbl = (uint16_t *)(c + 27);
                if (*lbl) {
                    int lw = 0;
                    sub_43F840((int)c[59], lbl, &lw, NULL, 0);
                    if (lw + 18 > nw) {
                        nw = lw + 18;
                        if (nw > want_w / 2 - 12)
                            nw = want_w / 2 - 12;
                        convert_button_to_resizable(c);
                    }
                }
                int nx = ox + dw / 2 - (nw - ow) / 2;
                if (nx < 10) nx = 10;
                if (nx + nw > want_w - 10) nx = want_w - 10 - nw;
                sub_46A9B0(c, nx, oy + dh);
                sub_46AB20(c, nw, oh);
            } else {
                /* Any other lower control */
                sub_46A9B0(c, ox + dw / 2, oy + dh);
            }
        }
    }
}

/* Collect listbox auxiliary slider/arrow pointers in a subtree so reflow
 * keeps them anchored flush against their owning listbox. */
typedef struct {
    uint32_t *lb;
    uint32_t *up;
    uint32_t *down;
    uint32_t *slider;
} lb_aux_t;

static void collect_lb_aux_rec(uint32_t *node, lb_aux_t *out, int *count, int cap)
{
    if (!node || !out || !count || *count >= cap)
        return;
    for (uint32_t *c = (uint32_t *)(uintptr_t)node[100]; c; c = (uint32_t *)(uintptr_t)c[97]) {
        if (is_listbox((uintptr_t)c[95]) && valid_u32_ptr(c[8])) {
            uint32_t *v5 = (uint32_t *)(uintptr_t)c[8];
            if (v5[7] || v5[8] || v5[9]) {
                if (*count < cap) {
                    lb_aux_t *a = &out[(*count)++];
                    a->lb     = c;
                    a->up     = valid_u32_ptr(v5[7]) ? (uint32_t *)(uintptr_t)v5[7] : NULL;
                    a->down   = valid_u32_ptr(v5[8]) ? (uint32_t *)(uintptr_t)v5[8] : NULL;
                    a->slider = valid_u32_ptr(v5[9]) ? (uint32_t *)(uintptr_t)v5[9] : NULL;
                }
            }
        }
        if (c[100])
            collect_lb_aux_rec(c, out, count, cap);
    }
}

static bool is_lb_aux_widget(const uint32_t *w, const lb_aux_t *aux, int aux_count)
{
    for (int i = 0; i < aux_count; ++i) {
        if (w == aux[i].up || w == aux[i].down || w == aux[i].slider)
            return true;
    }
    return false;
}

static void anchor_lb_aux(const lb_aux_t *a)
{
    if (!a || !a->lb)
        return;
    uint32_t *lb = a->lb;
    uint32_t lb_ax = 0, lb_ay = 0;
    sub_46AA60(lb, &lb_ax, &lb_ay);
    int lbx = (int)(int32_t)lb_ax;
    int lby = (int)(int32_t)lb_ay;
    int lbw = (int)(int32_t)lb[2];
    int lbh = (int)(int32_t)lb[3];
    if (*(const uint16_t *)(lb + 27) != 0) {
        int hdr_h = sub_43F320((int)lb[59]) + 1;
        if (lbh > hdr_h + 24) {
            lby += hdr_h;
            lbh -= hdr_h;
        }
    }

    int up_h = a->up ? (int)(int32_t)a->up[3] : 0;
    int dn_h = a->down ? (int)(int32_t)a->down[3] : 0;
    if (up_h < 12 && a->up) up_h = 16;
    if (dn_h < 12 && a->down) dn_h = 16;

    if (a->up) {
        uint32_t *p = (uint32_t *)(uintptr_t)a->up[99];
        uint32_t px = 0, py = 0;
        if (p) sub_46AA60(p, &px, &py);
        sub_46A9B0(a->up, lbx + lbw - (int)(int32_t)px, lby - (int)(int32_t)py);
    }
    if (a->down) {
        uint32_t *p = (uint32_t *)(uintptr_t)a->down[99];
        uint32_t px = 0, py = 0;
        if (p) sub_46AA60(p, &px, &py);
        sub_46A9B0(a->down, lbx + lbw - (int)(int32_t)px, lby + lbh - dn_h - (int)(int32_t)py);
    }
    if (a->slider) {
        uint32_t *p = (uint32_t *)(uintptr_t)a->slider[99];
        uint32_t px = 0, py = 0;
        if (p) sub_46AA60(p, &px, &py);
        int sh = lbh - up_h - dn_h;
        if (sh < 16) sh = 16;
        int sw = (int)(int32_t)a->slider[2];
        if (sw < 12 || sw > 24) sw = 16;
        sub_46A9B0(a->slider, lbx + lbw - (int)(int32_t)px, lby + up_h - (int)(int32_t)py);
        sub_46AB20(a->slider, sw, sh);
    }
}

static int count_non_aux_children(uint32_t *node, const lb_aux_t *aux, int aux_count)
{
    if (!node || !node[100])
        return 0;
    int scnt = 0;
    for (uint32_t *sc = (uint32_t *)(uintptr_t)node[100]; sc && scnt < MAX_PANEL_CHILDREN;
         sc = (uint32_t *)(uintptr_t)sc[97]) {
        if (!is_lb_aux_widget(sc, aux, aux_count))
            ++scnt;
    }
    return scnt;
}

/* Reflow a list of form child controls cleanly into spacious 1- or 2-column
 * rows inside [start_x .. start_x + avail_w], starting at start_y.
 * Returns total bottom Y reached. */
static int reflow_form_children(uint32_t **raw_items, int raw_count,
                                int start_x, int start_y, int avail_w,
                                const lb_aux_t *aux, int aux_count)
{
    uint32_t *items[MAX_PANEL_CHILDREN];
    int orig_xs[MAX_PANEL_CHILDREN];
    int orig_ys[MAX_PANEL_CHILDREN];
    int count = 0;

    uint32_t *dropdowns[4];
    int dd_cnt = 0;
    uint32_t *twin_10109 = NULL;
    uint32_t *ref_10105  = NULL;

    for (int i = 0; i < raw_count && count < MAX_PANEL_CHILDREN; ++i) {
        uint32_t *c = raw_items[i];
        if (!c)
            continue;
        int cid = (int)(int32_t)c[0];
        if (cid == 10120 || cid == 10317) {
            if (dd_cnt < 4)
                dropdowns[dd_cnt++] = c;
            continue;
        }
        if (cid == 10109) {
            twin_10109 = c;
            continue;
        }
        if (cid == 10105)
            ref_10105 = c;
        /* Skip permanently hidden controls (e.g. unused MinExp/MaxExp 10125..10128
         * or Ladder text 10210), while keeping quest-toggled containers 10183/10199/10122. */
        if ((c[1] & 0x10u) && cid != 10183 && cid != 10199 && cid != 10122)
            continue;
        items[count]   = c;
        orig_xs[count] = (int)(int32_t)c[4];
        orig_ys[count] = (int)(int32_t)c[5];
        ++count;
    }

    /* Sort into natural reading order: top-to-bottom, left-to-right */
    for (int i = 1; i < count; ++i) {
        uint32_t *key = items[i];
        int kx = orig_xs[i];
        int ky = orig_ys[i];
        int j = i - 1;
        while (j >= 0) {
            int jy = orig_ys[j];
            int jx = orig_xs[j];
            /* Group controls within 8px vertically as same row */
            bool after = (jy > ky + 8) || ((jy >= ky - 8 && jy <= ky + 8) && jx > kx);
            if (!after)
                break;
            items[j + 1]   = items[j];
            orig_xs[j + 1] = orig_xs[j];
            orig_ys[j + 1] = orig_ys[j];
            --j;
        }
        items[j + 1]   = key;
        orig_xs[j + 1] = kx;
        orig_ys[j + 1] = ky;
    }

    int cur_y = start_y;
    int half_w = (avail_w - 10) / 2;
    int col2_x = start_x + half_w + 10;

    for (int i = 0; i < count; ++i) {
        uint32_t *c = items[i];
        if (!c)
            continue;
        int cid = (int)(int32_t)c[0];
        uintptr_t fn = (uintptr_t)c[95];
        int ow = (int)(int32_t)c[2];
        int oh = (int)(int32_t)c[3];
        int fptr = (int)c[59];
        int sub_non_aux = count_non_aux_children(c, aux, aux_count);

        if (sub_non_aux > 0) {
            /* Nested sub-container (including SCROLLLISTBOX group boxes like
             * servopts 10183/10197/10199, lwgenerl 10310, advserv 2104):
             * always reflow its direct children across the full container width! */
            uint32_t *sub_items[MAX_PANEL_CHILDREN];
            int scnt = 0;
            for (uint32_t *sc = (uint32_t *)(uintptr_t)c[100]; sc && scnt < MAX_PANEL_CHILDREN;
                 sc = (uint32_t *)(uintptr_t)sc[97]) {
                if (!is_lb_aux_widget(sc, aux, aux_count))
                    sub_items[scnt++] = sc;
            }
            c[1] &= ~0x80u;
            sub_46A9B0(c, start_x, cur_y);
            int sub_end = reflow_form_children(sub_items, scnt, 10, 8, avail_w - 20, aux, aux_count);
            if (sub_end < 28) sub_end = 28;
            sub_46AB20(c, avail_w, sub_end + 6);
            cur_y += sub_end + 10;
        } else if (cid == 10330) {
            /* "Use Teams" push-styled checkbox uses fixed 95x20 UIButtonXlg art */
            int bw = (ow < 95) ? 95 : ow;
            int bh = (oh < 20) ? 20 : oh;
            sub_46A9B0(c, start_x + (avail_w - bw) / 2, cur_y);
            sub_46AB20(c, bw, bh);
            cur_y += bh + 6;
        } else if (is_checkbox_or_radio(fn)) {
            int lw = 0;
            uint16_t *lbl = (uint16_t *)(c + 27);
            if (*lbl)
                sub_43F840(fptr, lbl, &lw, NULL, 0);
            int need_w = lw + 36;
            int row_h = (oh < 24) ? 24 : oh;

            /* Pair [checkbox_or_radio, editbox] if they shared a row in stock .wnd
             * (e.g. Access tab Min Ping, Max Ping, Private Game + Password, AdvServ User) */
            if (half_w >= 180 && i + 1 < count &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_editbox((uintptr_t)items[i + 1][95])) {
                int dy_eb = orig_ys[i + 1] - orig_ys[i];
                if (dy_eb >= -10 && dy_eb <= 10) {
                    uint32_t *c2 = items[i + 1];
                    int ow2 = (int)(int32_t)c2[2];
                    int oh2 = (int)(int32_t)c2[3];
                    if (oh2 < 24) oh2 = 24;
                    if (oh2 > row_h) row_h = oh2;
                    float esx = (ow2 > 0) ? ((float)half_w / (float)ow2) : 1.0f;
                    if (esx < 1.0f) esx = 1.0f;
                    scale_editbox_field_width(c2, half_w, esx);
                    sub_46A9B0(c, start_x, cur_y);
                    sub_46AB20(c, half_w, row_h);
                    sub_46A9B0(c2, col2_x, cur_y);
                    sub_46AB20(c2, half_w, row_h);
                    cur_y += row_h + 6;
                    ++i;
                    continue;
                }
            }

            /* Pair two short checkboxes/radios side-by-side when both fit half_w
             * and the second one is not paired with an editbox on its own row. */
            bool next_paired_eb = (i + 2 < count &&
                                   is_editbox((uintptr_t)items[i + 2][95]) &&
                                   (orig_ys[i + 2] - orig_ys[i + 1]) >= -10 &&
                                   (orig_ys[i + 2] - orig_ys[i + 1]) <= 10);
            if (half_w >= 180 && need_w <= half_w - 4 && i + 1 < count &&
                !next_paired_eb &&
                (int)(int32_t)items[i + 1][0] != 10330 &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_checkbox_or_radio((uintptr_t)items[i + 1][95])) {
                uint32_t *c2 = items[i + 1];
                int lw2 = 0;
                uint16_t *lbl2 = (uint16_t *)(c2 + 27);
                if (*lbl2)
                    sub_43F840((int)c2[59], lbl2, &lw2, NULL, 0);
                if (lw2 + 36 <= half_w - 4) {
                    int oh2 = (int)(int32_t)c2[3];
                    if (oh2 > row_h) row_h = oh2;
                    sub_46A9B0(c, start_x, cur_y);
                    sub_46AB20(c, half_w, row_h);
                    sub_46A9B0(c2, col2_x, cur_y);
                    sub_46AB20(c2, half_w, row_h);
                    cur_y += row_h + 6;
                    ++i;
                    continue;
                }
            }

            /* Otherwise full-width row */
            if (need_w > avail_w) {
                uint32_t mh = (uint32_t)row_h;
                sub_43F840(fptr, lbl, NULL, &mh, avail_w - 34);
                if ((int)mh + 6 > row_h)
                    row_h = (int)mh + 6;
            }
            sub_46A9B0(c, start_x, cur_y);
            sub_46AB20(c, avail_w, row_h);
            cur_y += row_h + 6;
        } else if (is_editbox(fn)) {
            int row_h = (oh < 26) ? 26 : oh;
            /* Pair [editbox, pushbutton] side-by-side if they shared a row in stock .wnd
             * (e.g. servopts "Lesson [15]" + "Advanced...", "Time Limit [0]" + "Defaults") */
            if (half_w >= 180 && i + 1 < count &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_pushbutton((uintptr_t)items[i + 1][95])) {
                uint32_t *c2 = items[i + 1];
                int dy_orig = orig_ys[i + 1] - orig_ys[i];
                if (dy_orig >= -12 && dy_orig <= 12) {
                    int h2 = (int)(int32_t)c2[3];
                    if (h2 > row_h) row_h = h2;
                    float esx = (ow > 0) ? ((float)half_w / (float)ow) : 1.0f;
                    if (esx < 1.0f) esx = 1.0f;
                    scale_editbox_field_width(c, half_w, esx);
                    sub_46A9B0(c, start_x, cur_y);
                    sub_46AB20(c, half_w, row_h);
                    place_button_preserving_texture(c2, col2_x, cur_y, half_w, row_h, half_w);
                    cur_y += row_h + 8;
                    ++i;
                    continue;
                }
            }
            /* Pair [editbox, compact leaf listbox] on same stock row
             * (e.g. Access tab "Max Players [32]" + "Disallow: WARRIOR/WIZARD/CONJURER") */
            if (half_w >= 180 && i + 1 < count &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_listbox((uintptr_t)items[i + 1][95])) {
                int dy_lb = orig_ys[i + 1] - orig_ys[i];
                if (dy_lb >= -12 && dy_lb <= 12) {
                    uint32_t *c2 = items[i + 1];
                    bool has_aux2 = false;
                    for (int a = 0; a < aux_count; ++a) {
                        if (aux[a].lb == c2) { has_aux2 = true; break; }
                    }
                    int lb2_w = has_aux2 ? (half_w - 20) : half_w;
                    int lb2_h = (int)(int32_t)c2[3];
                    if (lb2_h < 62) lb2_h = 62;
                    if (lb2_h > 92) lb2_h = 92;
                    float esx = (ow > 0) ? ((float)half_w / (float)ow) : 1.0f;
                    if (esx < 1.0f) esx = 1.0f;
                    scale_editbox_field_width(c, half_w, esx);
                    sub_46A9B0(c, start_x, cur_y);
                    sub_46AB20(c, half_w, row_h);
                    sub_46A9B0(c2, col2_x, cur_y);
                    sub_46AB20(c2, lb2_w, lb2_h);
                    reset_listbox_metrics(c2);
                    cur_y += lb2_h + 6;
                    ++i;
                    continue;
                }
            }
            float esx = (ow > 0) ? ((float)avail_w / (float)ow) : 1.0f;
            if (esx < 1.0f) esx = 1.0f;
            scale_editbox_field_width(c, avail_w, esx);
            sub_46A9B0(c, start_x, cur_y);
            sub_46AB20(c, avail_w, row_h);
            cur_y += row_h + 8;
        } else if (is_static_draw(fn)) {
            /* Pair [static_text, pushbutton] when on same stock row
             * (e.g. "Current game Type:" + "[Arena]", "Connection Type:" + "[Modem]") */
            if (half_w >= 180 && i + 1 < count &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_pushbutton((uintptr_t)items[i + 1][95])) {
                int dy_btn = orig_ys[i + 1] - orig_ys[i];
                if (dy_btn >= -10 && dy_btn <= 10) {
                    uint32_t *c2 = items[i + 1];
                    int row_h = (oh < 26) ? 26 : oh;
                    int h2 = (int)(int32_t)c2[3];
                    if (h2 > row_h) row_h = h2;
                    sub_46A9B0(c, start_x, cur_y);
                    sub_46AB20(c, half_w, row_h);
                    place_button_preserving_texture(c2, col2_x, cur_y, half_w, row_h, half_w);
                    cur_y += row_h + 6;
                    ++i;
                    continue;
                }
            }
            /* Pair two short static labels on the same stock row (e.g. Slowest / Fastest) */
            if (half_w >= 180 && i + 1 < count &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_static_draw((uintptr_t)items[i + 1][95]) &&
                (orig_ys[i + 1] - orig_ys[i]) >= -4 &&
                (orig_ys[i + 1] - orig_ys[i]) <= 4 &&
                orig_xs[i + 1] > orig_xs[i] + 32) {
                uint32_t *c2 = items[i + 1];
                int row_h = (oh < 18) ? 18 : oh;
                sub_46A9B0(c, start_x, cur_y);
                sub_46AB20(c, half_w, row_h);
                sub_46A9B0(c2, col2_x, cur_y);
                sub_46AB20(c2, half_w, row_h);
                cur_y += row_h + 4;
                ++i;
                continue;
            }

            int row_h = (oh < 18) ? 18 : oh;
            if (valid_u32_ptr(c[8])) {
                uint16_t *st = *(uint16_t **)(uintptr_t)c[8];
                if (st && *st) {
                    uint32_t mh = 0;
                    sub_43F840(fptr, st, NULL, &mh, avail_w - 6);
                    if ((int)mh + 4 > row_h)
                        row_h = (int)mh + 4;
                }
            }
            sub_46A9B0(c, start_x, cur_y);
            sub_46AB20(c, avail_w, row_h);
            cur_y += row_h + 4;
        } else if (is_listbox(fn)) {
            bool has_aux = false;
            for (int a = 0; a < aux_count; ++a) {
                if (aux[a].lb == c) { has_aux = true; break; }
            }
            /* Pair two side-by-side listboxes (e.g. Access tab Current Users 10200
             * + Banned/Allowed List 10105 + Add-User input 10111) */
            if (half_w >= 180 && i + 1 < count &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_listbox((uintptr_t)items[i + 1][95]) &&
                (orig_ys[i + 1] - orig_ys[i]) >= -16 &&
                (orig_ys[i + 1] - orig_ys[i]) <= 16) {
                uint32_t *c2 = items[i + 1];
                bool has_aux2 = false;
                for (int a = 0; a < aux_count; ++a) {
                    if (aux[a].lb == c2) { has_aux2 = true; break; }
                }
                int w1 = has_aux  ? (half_w - 20) : half_w;
                int w2 = has_aux2 ? (half_w - 20) : half_w;
                if (i + 2 < count && is_editbox((uintptr_t)items[i + 2][95]) &&
                    (int)(int32_t)items[i + 2][0] == 10111) {
                    uint32_t *c3 = items[i + 2];
                    float esx = ((int32_t)c3[2] > 0) ? ((float)half_w / (float)(int32_t)c3[2]) : 1.0f;
                    scale_editbox_field_width(c3, half_w, esx);
                    sub_46A9B0(c,  start_x, cur_y);
                    sub_46AB20(c,  w1, 112);
                    sub_46A9B0(c2, col2_x, cur_y);
                    sub_46AB20(c2, w2, 82);
                    sub_46A9B0(c3, col2_x, cur_y + 86);
                    sub_46AB20(c3, half_w, 26);
                    reset_listbox_metrics(c);
                    reset_listbox_metrics(c2);
                    cur_y += 118;
                    i += 2;
                    continue;
                }
                sub_46A9B0(c,  start_x, cur_y);
                sub_46AB20(c,  w1, 96);
                sub_46A9B0(c2, col2_x, cur_y);
                sub_46AB20(c2, w2, 96);
                reset_listbox_metrics(c);
                reset_listbox_metrics(c2);
                cur_y += 102;
                ++i;
                continue;
            }
            int lb_w = has_aux ? (avail_w - 20) : avail_w;
            int lb_h = oh;
            if ((int)(int32_t)c[0] == 10114) {
                /* Tall map list so the default map set fits without scroll-within-scroll */
                lb_h = 196;
            } else if (has_aux) {
                if (lb_h < 96)  lb_h = 96;
                if (lb_h > 132) lb_h = 132;
            } else {
                if (lb_h < 62)  lb_h = 62;
                if (lb_h > 120) lb_h = 120;
            }
            sub_46A9B0(c, start_x, cur_y);
            sub_46AB20(c, lb_w, lb_h);
            reset_listbox_metrics(c);
            cur_y += lb_h + 8;
        } else if (is_pushbutton(fn)) {
            int row_h = (oh < 26) ? 26 : oh;
            /* 4-up compact button row (e.g. Access tab Kick | Ban   Add | Del) */
            if (half_w >= 180 && i + 3 < count &&
                !items[i + 1][100] && !items[i + 2][100] && !items[i + 3][100] &&
                is_pushbutton((uintptr_t)items[i + 1][95]) &&
                is_pushbutton((uintptr_t)items[i + 2][95]) &&
                is_pushbutton((uintptr_t)items[i + 3][95]) &&
                (orig_ys[i + 3] - orig_ys[i]) >= -8 &&
                (orig_ys[i + 3] - orig_ys[i]) <= 8) {
                int sub_w = (half_w - 8) / 2;
                for (int k = 0; k < 4; ++k) {
                    int bx = (k < 2) ? (start_x + k * (sub_w + 8))
                                     : (col2_x + (k - 2) * (sub_w + 8));
                    place_button_preserving_texture(items[i + k], bx, cur_y, sub_w, row_h, sub_w);
                }
                cur_y += row_h + 8;
                i += 3;
                continue;
            }
            int bw = ow;
            uint16_t *lbl = (uint16_t *)(c + 27);
            if (*lbl) {
                int lw = 0;
                sub_43F840(fptr, lbl, &lw, NULL, 0);
                if (lw + 24 > bw)
                    bw = lw + 24;
            }
            if (half_w >= 160 && bw <= half_w && i + 1 < count &&
                count_non_aux_children(items[i + 1], aux, aux_count) == 0 &&
                is_pushbutton((uintptr_t)items[i + 1][95]) &&
                (orig_ys[i + 1] - orig_ys[i]) >= -10 &&
                (orig_ys[i + 1] - orig_ys[i]) <= 10) {
                uint32_t *c2 = items[i + 1];
                int bw2 = (int)(int32_t)c2[2];
                uint16_t *lbl2 = (uint16_t *)(c2 + 27);
                if (*lbl2) {
                    int lw2 = 0;
                    sub_43F840((int)c2[59], lbl2, &lw2, NULL, 0);
                    if (lw2 + 24 > bw2)
                        bw2 = lw2 + 24;
                }
                if (bw2 <= half_w) {
                    int h2 = (int)(int32_t)c2[3];
                    if (h2 > row_h) row_h = h2;
                    place_button_preserving_texture(c, start_x, cur_y, half_w, row_h, half_w);
                    place_button_preserving_texture(c2, col2_x, cur_y, half_w, row_h, half_w);
                    cur_y += row_h + 8;
                    ++i;
                    continue;
                }
            }
            place_button_preserving_texture(c, start_x, cur_y, avail_w, row_h, 164);
            cur_y += row_h + 8;
        } else {
            int row_h = (oh < 20) ? 20 : oh;
            int nw = (ow < avail_w && !(c[1] & 0x80u)) ? avail_w : ow;
            if (nw > avail_w) nw = avail_w;
            sub_46A9B0(c, start_x, cur_y);
            sub_46AB20(c, nw, row_h);
            cur_y += row_h + 6;
        }
    }

    /* Sync toggled Allowed List (10109) onto Banned List (10105) slot */
    if (twin_10109 && ref_10105) {
        sub_46A9B0(twin_10109, (int)(int32_t)ref_10105[4], (int)(int32_t)ref_10105[5]);
        sub_46AB20(twin_10109, (int)(int32_t)ref_10105[2], (int)(int32_t)ref_10105[3]);
        reset_listbox_metrics(twin_10109);
    }

    /* Anchor popup dropdown listboxes (10120 GameType, 10317 ConnType) directly
     * beneath their trigger button with an opaque bordered backdrop. */
    for (int d = 0; d < dd_cnt; ++d) {
        uint32_t *dd = dropdowns[d];
        int want_btn = ((int)(int32_t)dd[0] == 10120) ? 10119 : 10316;
        uint32_t *btn = NULL;
        for (int i = 0; i < count; ++i) {
            if ((int)(int32_t)items[i][0] == want_btn) {
                btn = items[i];
                break;
            }
        }
        dd[1] &= ~0x80u;
        dd[1] |= (0x1000u | 0x20u);
        dd[14] = (uint32_t)sub_4344A0(18, 15, 12);
        dd[16] = (uint32_t)sub_4344A0(168, 134, 82);
        if (btn) {
            int bx = (int)(int32_t)btn[4];
            int by = (int)(int32_t)btn[5];
            int bw = (int)(int32_t)btn[2];
            if (bw < 184) bw = 184;
            int bh = 132;
            sub_46A9B0(dd, bx, by);
            sub_46AB20(dd, bw, bh);
        }
        reset_listbox_metrics(dd);
    }

    return cur_y;
}

void nox_3ds_ui_takeover_form_dialog(void *root_wnd)
{
    uint32_t *w = (uint32_t *)root_wnd;
    if (!w || !nox_3ds_font_is_enabled())
        return;

    int orig_w = (int)(int32_t)w[2];
    int orig_h = (int)(int32_t)w[3];
    if (orig_w <= 0 || orig_h <= 0)
        return;

    const int new_x = 12;
    const int new_y = 12;
    const int new_w = 616;
    const int new_h = 456;

    convert_to_stone_takeover_bg(w);
    sub_46AB20(w, new_w, new_h);
    sub_46A9B0(w, new_x, new_y);
    sub_46A8C0((int)(uintptr_t)w);

    lb_aux_t aux[12];
    int aux_count = 0;
    collect_lb_aux_rec(w, aux, &aux_count, 12);

    uint32_t *headers[8];
    int hdr_cnt = 0;
    uint32_t *footers[8];
    int ftr_cnt = 0;
    uint32_t *body[MAX_PANEL_CHILDREN];
    int body_cnt = 0;

    for (uint32_t *c = (uint32_t *)(uintptr_t)w[100]; c; c = (uint32_t *)(uintptr_t)c[97]) {
        if (is_lb_aux_widget(c, aux, aux_count))
            continue;
        uintptr_t fn = (uintptr_t)c[95];
        int oy = (int)(int32_t)c[5];
        int oh = (int)(int32_t)c[3];

        if (is_static_draw(fn) && oy < 36 && oh <= 36 && hdr_cnt < 8) {
            headers[hdr_cnt++] = c;
        } else if (is_pushbutton(fn) && oy >= orig_h - 56 && ftr_cnt < 8) {
            footers[ftr_cnt++] = c;
        } else if (body_cnt < MAX_PANEL_CHILDREN) {
            body[body_cnt++] = c;
        }
    }

    int pad_top = (hdr_cnt > 0) ? 40 : 12;
    int pad_bot = (ftr_cnt > 0) ? 48 : 12;

    panel_meta_t *meta = alloc_panel_meta(root_wnd);
    memset(meta, 0, sizeof(*meta));
    meta->panel_wnd  = root_wnd;
    meta->pad_top    = (int16_t)pad_top;
    meta->pad_bottom = (int16_t)pad_bot;

    /* Position pinned headers */
    for (int i = 0; i < hdr_cnt; ++i) {
        sub_46A9B0(headers[i], 14, 10);
        sub_46AB20(headers[i], new_w - 28, 24);
        if (meta->pinned_count < 16)
            meta->pinned[meta->pinned_count++] = headers[i];
    }

    /* Position pinned footer buttons (sorted left-to-right) */
    for (int i = 1; i < ftr_cnt; ++i) {
        uint32_t *k = footers[i];
        int j = i - 1;
        while (j >= 0 && (int32_t)footers[j][4] > (int32_t)k[4]) {
            footers[j + 1] = footers[j];
            --j;
        }
        footers[j + 1] = k;
    }
    if (ftr_cnt > 0) {
        int gap = 14;
        int btn_w = (new_w - 32 - gap * (ftr_cnt - 1)) / ftr_cnt;
        if (btn_w > 168) btn_w = 168;
        if (btn_w < 84)  btn_w = 84;
        int total_w = ftr_cnt * btn_w + (ftr_cnt - 1) * gap;
        int bx = (new_w - total_w) / 2;
        int by = new_h - pad_bot + 8;
        for (int i = 0; i < ftr_cnt; ++i) {
            uint32_t *b = footers[i];
            place_button_preserving_texture(b, bx, by, btn_w, 30, btn_w);
            bx += btn_w + gap;
            if (meta->pinned_count < 16)
                meta->pinned[meta->pinned_count++] = b;
        }
    }

    /* Reflow interior body controls into the scrollable band */
    reflow_form_children(body, body_cnt, 16, pad_top + 6, new_w - 36, aux, aux_count);

    for (int a = 0; a < aux_count; ++a)
        anchor_lb_aux(&aux[a]);

    invalidate_panel_slot(root_wnd);
}

/* Unobstructed interior viewport inside servopts.wnd (10100) Main Border (10195):
 * 10195 sits at (16, 40, 592, 382), whose +/-10px 9-slice stone bars span
 * x = 6..26, 598..618 and y = 30..50, 412..432.
 * - Game tab (10150/10153) hides Row-2 sub-tabs and uses full (28, 52, 568, 358).
 * - Server sub-tab panels (General/Access/Teams) sit below Row-2 sub-tabs
 *   (y = 50..72) at (28, 76, 568, 334). */
#define SERVOPTS_BODY_X      28
#define SERVOPTS_BODY_Y      52
#define SERVOPTS_BODY_W      568
#define SERVOPTS_BODY_H      358
#define SERVOPTS_SUB_BODY_Y  76
#define SERVOPTS_SUB_BODY_H  334

/* Position a popup dropdown listbox (e.g. 10120 Game Type, 10317 Conn Type)
 * directly over its trigger button's live screen coordinates. Overlapping the
 * trigger button is REQUIRED because sub_458500 auto-closes 10120 on the very
 * first frame if the current cursor/tap position is outside 10120's rect. */
void nox_3ds_ui_anchor_servopts_dropdown(void *root_wnd, int dd_id, int btn_id)
{
    uint32_t *root = (uint32_t *)root_wnd;
    if (!root || !nox_3ds_font_is_enabled())
        return;

    uint32_t *dd  = sub_46B0C0(root, dd_id);
    uint32_t *btn = sub_46B0C0(root, btn_id);
    if (!dd || !btn)
        return;

    uint32_t bax = 0, bay = 0;
    uint32_t pax = 0, pay = 0;
    uint32_t rax = 0, ray = 0;
    sub_46AA60(btn, &bax, &bay);
    sub_46AA60(root, &rax, &ray);
    uint32_t *dd_parent = valid_u32_ptr(dd[99]) ? (uint32_t *)(uintptr_t)dd[99] : NULL;
    if (dd_parent)
        sub_46AA60(dd_parent, &pax, &pay);

    int bw = (int)(int32_t)btn[2];
    if (bw < 184) bw = 184;
    int bh = 132;

    int dx = (int)bax - (int)pax;
    int dy = (int)bay - (int)pay;

    /* Keep dropdown inside the right/bottom edges of servopts root window */
    int root_right  = (int)rax + (int)(int32_t)root[2] - 20;
    int root_bottom = (int)ray + (int)(int32_t)root[3] - 44;
    if ((int)pax + dx + bw > root_right)
        dx = root_right - bw - (int)pax;
    if (dx < 0) dx = 0;
    if ((int)pay + dy + bh > root_bottom) {
        int avail_h = root_bottom - ((int)pay + dy);
        if (avail_h >= 72)
            bh = avail_h;
        else
            dy = root_bottom - bh - (int)pay;
    }

    dd[1] &= ~0x80u;
    dd[1] |= (0x1000u | 0x20u); /* bordered opaque box + ABOVE */
    dd[14] = (uint32_t)sub_4344A0(18, 15, 12);
    dd[16] = (uint32_t)sub_4344A0(168, 134, 82);
    sub_46A9B0(dd, dx, dy);
    sub_46AB20(dd, bw, bh);
    reset_listbox_metrics(dd);
}

void nox_3ds_ui_reflow_servopts_subpanel(void *panel_wnd)
{
    uint32_t *c = (uint32_t *)panel_wnd;
    if (!c || !nox_3ds_font_is_enabled())
        return;

    uint32_t *parent = valid_u32_ptr(c[99]) ? (uint32_t *)(uintptr_t)c[99] : NULL;
    bool inside_10150 = (parent && (int)(int32_t)parent[0] == 10150);
    int px = inside_10150 ? 0 : SERVOPTS_BODY_X;
    int py = inside_10150 ? 0 : SERVOPTS_SUB_BODY_Y;
    int ph = inside_10150 ? SERVOPTS_BODY_H : SERVOPTS_SUB_BODY_H;

    c[1] &= ~(0x80u | 0x1000u);
    c[11] &= ~2048u;
    c[14] = (uint32_t)sub_4344A0(22, 18, 15);
    c[16] = 0x80000000u;
    sub_46A9B0(c, px, py);
    sub_46AB20(c, SERVOPTS_BODY_W, ph);

    lb_aux_t aux[16];
    int aux_count = 0;
    collect_lb_aux_rec(c, aux, &aux_count, 16);

    uint32_t *sub_items[MAX_PANEL_CHILDREN];
    int scnt = 0;
    for (uint32_t *sc = (uint32_t *)(uintptr_t)c[100]; sc && scnt < MAX_PANEL_CHILDREN;
         sc = (uint32_t *)(uintptr_t)sc[97]) {
        if (!is_lb_aux_widget(sc, aux, aux_count))
            sub_items[scnt++] = sc;
    }

    if (scnt > 0) {
        reflow_form_children(sub_items, scnt, 8, 6, SERVOPTS_BODY_W - 24, aux, aux_count);
        panel_meta_t *meta = alloc_panel_meta(c);
        memset(meta, 0, sizeof(*meta));
        meta->panel_wnd  = c;
        meta->pad_top    = 0;
        meta->pad_bottom = 0;
        invalidate_panel_slot(c);
    }

    for (int a = 0; a < aux_count; ++a)
        anchor_lb_aux(&aux[a]);

    /* Anchor any popup dropdowns hosted in this subpanel or its root */
    uint32_t *root = c;
    while (valid_u32_ptr(root[99]))
        root = (uint32_t *)(uintptr_t)root[99];
    nox_3ds_ui_anchor_servopts_dropdown(root, 10317, 10316);
}

static int servopts_tab_order(int cid)
{
    switch (cid) {
        case 10160: return 0; /* Game (primary)    */
        case 10159: return 1; /* Server (primary)  */
        case 10163: return 2; /* General (sub-tab) */
        case 10161: return 3; /* Access (sub-tab)  */
        case 10162: return 4; /* Teams (sub-tab)   */
        default:    return 5;
    }
}

void nox_3ds_ui_takeover_servopts(void *root_wnd)
{
    uint32_t *w = (uint32_t *)root_wnd;
    if (!w || !nox_3ds_font_is_enabled())
        return;

    int orig_w = (int)(int32_t)w[2];
    int orig_h = (int)(int32_t)w[3];
    if (orig_w <= 0 || orig_h <= 0)
        return;

    const int new_x = 8;
    const int new_y = 4;
    const int new_w = 624;
    const int new_h = 472;

    lb_aux_t aux[16];
    int aux_count = 0;
    collect_lb_aux_rec(w, aux, &aux_count, 16);

    convert_to_stone_takeover_bg(w);
    sub_46AB20(w, new_w, new_h);
    sub_46A9B0(w, new_x, new_y);
    sub_46A8C0((int)(uintptr_t)w);

    uint32_t *tabs[8];
    int tab_cnt = 0;
    uint32_t *footers[8];
    int ftr_cnt = 0;
    uint32_t *ok_twin = NULL;
    uint32_t *panels[16];
    int pnl_cnt = 0;

    for (uint32_t *c = (uint32_t *)(uintptr_t)w[100]; c; c = (uint32_t *)(uintptr_t)c[97]) {
        if (is_lb_aux_widget(c, aux, aux_count))
            continue;
        int cid = (int)(int32_t)c[0];
        uintptr_t fn = (uintptr_t)c[95];
        int oy = (int)(int32_t)c[5];

        if (cid == 10160 || cid == 10159 || cid == 10163 || cid == 10161 || cid == 10162) {
            if (tab_cnt < 8)
                tabs[tab_cnt++] = c;
        } else if (cid == 10195) {
            /* Main Border around tab viewport (stone bars span +/-10px of edges) */
            sub_46A9B0(c, 16, 40);
            sub_46AB20(c, 592, 382);
        } else if (cid == 10196) {
            /* Suppress duplicate inner Game Border so unhiding 10196 on Server tab
             * never draws a second stone border across General/Access controls. */
            c[1] &= ~(0x80u | 0x1000u);
            c[11] &= ~2048u;
            c[14] = 0x80000000u;
            c[16] = 0x80000000u;
            sub_46A9B0(c, SERVOPTS_BODY_X, SERVOPTS_BODY_Y);
            sub_46AB20(c, SERVOPTS_BODY_W, SERVOPTS_BODY_H);
        } else if (cid == 10149) {
            /* Hidden "Ok" button shares the exact left-footer slot with "GO!" (10145) */
            ok_twin = c;
        } else if (is_pushbutton(fn) && !c[100] && oy >= orig_h - 56 && ftr_cnt < 8) {
            footers[ftr_cnt++] = c;
        } else if (c[100] && pnl_cnt < 16) {
            panels[pnl_cnt++] = c;
        }
    }

    /* Row 1 (y = 8): primary tabs (Game | Server).
     * Row 2 (y = 50, inside top of 10195): Server sub-tabs (General | Access | Teams),
     * which Nox automatically hides on Game tab and shows on Server tab. */
    for (int i = 1; i < tab_cnt; ++i) {
        uint32_t *k = tabs[i];
        int ko = servopts_tab_order((int)(int32_t)k[0]);
        int j = i - 1;
        while (j >= 0 && servopts_tab_order((int)(int32_t)tabs[j][0]) > ko) {
            tabs[j + 1] = tabs[j];
            --j;
        }
        tabs[j + 1] = k;
    }
    int row1_x = 16;
    int row2_x = SERVOPTS_BODY_X + 2;
    for (int i = 0; i < tab_cnt; ++i) {
        uint32_t *t = tabs[i];
        int tid = (int)(int32_t)t[0];
        bool is_subtab = (tid == 10163 || tid == 10161 || tid == 10162);
        int tw = (int)(int32_t)t[2];
        int th = (int)(int32_t)t[3];
        if (tw < 95) tw = 95;
        if (th < 20) th = 20;
        t[1] |= 0x20u; /* ABOVE so tabs always win child hit-testing */
        if (is_subtab) {
            sub_46A9B0(t, row2_x, 50);
            sub_46AB20(t, tw, th);
            row2_x += tw + 8;
        } else {
            sub_46A9B0(t, row1_x, 8);
            sub_46AB20(t, tw, th);
            row1_x += tw + 8;
        }
    }

    /* Sort & place footer buttons cleanly inside the bottom stone border */
    for (int i = 1; i < ftr_cnt; ++i) {
        uint32_t *k = footers[i];
        int j = i - 1;
        while (j >= 0 && (int32_t)footers[j][4] > (int32_t)k[4]) {
            footers[j + 1] = footers[j];
            --j;
        }
        footers[j + 1] = k;
    }
    int footer_y = new_h - 38;
    uint32_t *go_btn = NULL;
    if (ftr_cnt > 0) {
        int gap = 28;
        int btn_ws[8];
        int btn_hs[8];
        int total_w = (ftr_cnt - 1) * gap;
        for (int i = 0; i < ftr_cnt; ++i) {
            uint32_t *b = footers[i];
            int bw = (int)(int32_t)b[2];
            int bh = (int)(int32_t)b[3];
            bool is_textured = ((b[1] & 0x80u) != 0) ||
                               ((uintptr_t)b[95] == (uintptr_t)sub_4A8380);
            int lw = 0;
            uint16_t *lbl = (uint16_t *)(b + 27);
            if (*lbl)
                sub_43F840((int)b[59], lbl, &lw, NULL, 0);
            if (is_textured && bw >= lw + 10 && bw > 0 && bh > 0) {
                /* Keep authentic Westwood textured button sprite & exact dimensions */
                btn_ws[i] = bw;
                btn_hs[i] = bh;
            } else {
                convert_button_to_resizable(b);
                int nw = (lw + 24 > 96) ? (lw + 24) : 96;
                if (nw > 176) nw = 176;
                btn_ws[i] = nw;
                btn_hs[i] = 28;
            }
            total_w += btn_ws[i];
        }
        int bx = (new_w - total_w) / 2;
        for (int i = 0; i < ftr_cnt; ++i) {
            uint32_t *b = footers[i];
            if ((int)(int32_t)b[0] == 10145)
                go_btn = b;
            int by = footer_y + (28 - btn_hs[i]) / 2;
            sub_46A9B0(b, bx, by);
            sub_46AB20(b, btn_ws[i], btn_hs[i]);
            bx += btn_ws[i] + gap;
        }
    }
    if (ok_twin) {
        int bw = (int)(int32_t)ok_twin[2];
        int bh = (int)(int32_t)ok_twin[3];
        bool is_textured = ((ok_twin[1] & 0x80u) != 0) ||
                           ((uintptr_t)ok_twin[95] == (uintptr_t)sub_4A8380);
        int lw = 0;
        uint16_t *lbl = (uint16_t *)(ok_twin + 27);
        if (*lbl)
            sub_43F840((int)ok_twin[59], lbl, &lw, NULL, 0);
        if (!is_textured || bw < lw + 10 || bw <= 0 || bh <= 0) {
            convert_button_to_resizable(ok_twin);
            bw = (lw + 24 > 96) ? (lw + 24) : 96;
            if (bw > 176) bw = 176;
            bh = 28;
        }
        int ox = go_btn ? (int)(int32_t)go_btn[4] : ((new_w - bw) / 2);
        int oy = footer_y + (28 - bh) / 2;
        sub_46A9B0(ok_twin, ox, oy);
        sub_46AB20(ok_twin, bw, bh);
    }

    /* Reflow Game wrapper 10150 -> 10153 and any active sub-tab panels */
    for (int i = 0; i < pnl_cnt; ++i) {
        uint32_t *c = panels[i];
        int cid = (int)(int32_t)c[0];
        if (cid == 10150) {
            c[1] &= ~(0x80u | 0x1000u);
            c[14] = (uint32_t)sub_4344A0(22, 18, 15);
            c[16] = 0x80000000u;
            sub_46A9B0(c, SERVOPTS_BODY_X, SERVOPTS_BODY_Y);
            sub_46AB20(c, SERVOPTS_BODY_W, SERVOPTS_BODY_H);
            for (uint32_t *sc = (uint32_t *)(uintptr_t)c[100]; sc; sc = (uint32_t *)(uintptr_t)sc[97]) {
                if ((int)(int32_t)sc[0] == 10153)
                    nox_3ds_ui_reflow_servopts_subpanel(sc);
            }
        } else {
            nox_3ds_ui_reflow_servopts_subpanel(c);
        }
    }

    for (int a = 0; a < aux_count; ++a)
        anchor_lb_aux(&aux[a]);

    nox_3ds_ui_anchor_servopts_dropdown(w, 10120, 10119);
    nox_3ds_ui_anchor_servopts_dropdown(w, 10317, 10316);
}
