#ifndef NOX_3DS_UI_SCROLL_H
#define NOX_3DS_UI_SCROLL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nested software clip guard for Nox UI rendering (works around engine's
 * 1-slot save/restore in sub_49F7F0/sub_49F860 and narrows master clip rect
 * so sub_49F6F0 respects container viewports). */
typedef struct nox_3ds_clip_guard_t {
    uint32_t ctx_save[13];
    uint32_t slot_save[11];
    bool     active;
} nox_3ds_clip_guard_t;

bool nox_3ds_clip_push(nox_3ds_clip_guard_t *guard, int x, int y, int w, int h);
void nox_3ds_clip_pop(nox_3ds_clip_guard_t *guard);

/* Panel render guard for scrollable container windows (e.g. Server Config). */
typedef struct nox_3ds_panel_guard_t {
    int32_t              slot_idx;
    bool                 active;
    bool                 has_pinned;
    nox_3ds_clip_guard_t clip;
} nox_3ds_panel_guard_t;

/* Per-frame lifecycle: call once per Bottom-Screen GUI draw pass. */
void nox_3ds_ui_scroll_begin_frame(void);

/* Reset all scroll slots (e.g. when returning to main menu / world toggle). */
void nox_3ds_ui_scroll_reset_all(void);

/* Single-line ellipsis truncation using current Nox font metrics (native or SW).
 * Returns src directly if it already fits within max_w; otherwise writes a
 * truncated UTF-16 string ending in "..." into dst and returns dst. */
const uint16_t *nox_3ds_ui_fit_ellipsis(int font_ptr, const uint16_t *src,
                                        uint16_t *dst, int dst_cap, int max_w);

/* Bottom-Screen Takeover & Auto-Expand Helpers:
 * - expand_modal_dialog: expands modal popups (sub_449A10, Chat Lobby sub_49C560)
 *   up to 580x430 when native text or checkboxes would overflow, preserving
 *   idempotency across reused dialogs.
 * - takeover_form_dialog: full Bottom-Screen takeover (616x456) + spacious
 *   reflow with pinned header/footer and vertical scroll body for dense forms
 *   (e.g. Server Rules 10300, Access/Advanced Settings 2100).
 * - takeover_servopts: full Bottom-Screen takeover (624x472) for servopts.wnd
 *   (10100) + proportional widening and scrollable sub-panel reflow. */
void nox_3ds_ui_expand_modal_dialog(void *dlg_wnd);
void nox_3ds_ui_takeover_form_dialog(void *root_wnd);
void nox_3ds_ui_takeover_servopts(void *root_wnd);
void nox_3ds_ui_reflow_servopts_subpanel(void *panel_wnd);
void nox_3ds_ui_anchor_servopts_dropdown(void *root_wnd, int dd_id, int btn_id);
void nox_3ds_ui_takeover_advserv(void *root_wnd);
void nox_3ds_ui_reflow_advserv_subpanel(void *sub_wnd);

/* Register or update an overflowing static/description text region keyed by
 * widget pointer or fixed 32-bit key (e.g. Spellbook right pane 0x5350424Bu).
 * Returns clamped current scroll offset in pixels (>= 0). */
int  nox_3ds_ui_scroll_update_region(uint32_t key, int x, int y, int w, int h,
                                     int content_h, bool draw_scrollbar);
int  nox_3ds_ui_scroll_get(uint32_t key);
void nox_3ds_ui_scroll_set(uint32_t key, int scroll_y);
void nox_3ds_ui_scroll_step(uint32_t key, int delta_y);

/* Pair auxiliary listbox columns (e.g. Server Browser columns 10038..10042)
 * so dragging or D-pad scrolling any column scrolls the whole table together. */
void nox_3ds_ui_scroll_register_listbox_group(void *primary_lb,
                                              void *col1, void *col2,
                                              void *col3, void *col4);
void nox_3ds_ui_scroll_sync_listbox(void *lb);

/* Scrollable container panel support (Server Config panels 10150..10199, 10300, 2100). */
bool nox_3ds_ui_is_scrollable_panel_id(int id);
bool nox_3ds_ui_panel_begin(void *panel_wnd, nox_3ds_panel_guard_t *guard);
bool nox_3ds_ui_panel_child_is_pinned(const nox_3ds_panel_guard_t *guard, const void *child_wnd);
void nox_3ds_ui_panel_end(void *panel_wnd, nox_3ds_panel_guard_t *guard);

/* Input hooks inside sub_46B6A0 (src/GAME2.c):
 * 1) pump_drag: runs BEFORE captured-widget bypass so ongoing finger drags (>7px)
 *    scroll smoothly even while engine holds mouse capture on a child widget.
 *    Returns true if scroll-drag consumed the frame.
 * 2) maybe_arm: runs on initial touch press after hit-widget v1 is resolved.
 * 3) pump_dpad: scrolls the active overflowing pane on D-Pad Up/Down when no drag active. */
bool nox_3ds_ui_scroll_pump_drag(int touch_x, int touch_y,
                                 const int32_t *ev4, int32_t *captured_wnd);
void nox_3ds_ui_scroll_maybe_arm(int touch_x, int touch_y, void *hit_wnd);
void nox_3ds_ui_scroll_pump_dpad(void);
bool nox_3ds_ui_scroll_is_dragging(void);

#ifdef __cplusplus
}
#endif

#endif /* NOX_3DS_UI_SCROLL_H */
