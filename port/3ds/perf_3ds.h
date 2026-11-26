#ifndef NOX_PERF_3DS_H
#define NOX_PERF_3DS_H

#include <stdbool.h>
#include <stdint.h>

uint64_t nox_3ds_perf_consume_main_sleep_ticks(void);
void     nox_3ds_perf_hud_update(float fps, float cpu_ms, float gpu_ms);
bool     nox_3ds_perf_hud_is_enabled(void);
void     nox_3ds_perf_hud_toggle(void);
void     nox_3ds_perf_hud_draw_top(int target_w, int target_h);
void     nox_3ds_font_show_custom_toast(const char *msg, bool green);

#endif
