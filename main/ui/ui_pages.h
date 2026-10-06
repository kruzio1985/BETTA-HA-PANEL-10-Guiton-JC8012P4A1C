/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "app_config.h"
#include "lvgl.h"

typedef void (*ui_pages_show_cb_t)(const char *page_id, uint16_t index);
/* Simple notification callback (no payload), e.g. topbar gear press or a
 * finished page rebuild. Runs on the LVGL UI task. */
typedef void (*ui_pages_action_cb_t)(void);

/* Runtime configuration for the top bar (editable via the web editor). */
typedef struct {
    bool show_clock;
    bool show_room_name;
    char room_name[APP_TOP_BAR_ROOM_NAME_MAX_LEN];
    bool show_status;
    bool show_brightness;
} ui_topbar_config_t;

void ui_pages_init(void);
void ui_pages_reset(void);
lv_obj_t *ui_pages_add(const char *page_id, const char *title);
bool ui_pages_show(const char *page_id);
bool ui_pages_show_index(uint16_t index);
bool ui_pages_next(void);
bool ui_pages_prev(void);
const char *ui_pages_current_id(void);
/* Register a single callback that is invoked whenever the active page
 * changes (after the new page has been made visible).  Passing NULL
 * clears the callback.  The callback runs on the LVGL UI task. */
void ui_pages_set_show_callback(ui_pages_show_cb_t cb);
/* Invoked when the topbar gear/settings button is clicked. Passing NULL
 * disables the button action. Runs on the LVGL UI task. */
void ui_pages_set_gear_callback(ui_pages_action_cb_t cb);
/* Invoked right after the screen chrome (topbar/nav/content) has been rebuilt
 * by ui_pages_init(). Lets dependent overlays drop stale object handles that
 * were destroyed by the rebuild. Passing NULL disables the hook. */
void ui_pages_set_screen_built_callback(ui_pages_action_cb_t cb);
uint16_t ui_pages_count(void);
void ui_pages_set_topbar_status(
    bool wifi_connected, bool wifi_setup_ap_active, bool api_connected, bool api_initial_sync_done);
void ui_pages_set_topbar_datetime(const struct tm *timeinfo);
/* Configure which top bar elements are shown. Must be called before
 * ui_pages_init() so the top bar is built with the right layout. */
void ui_pages_set_topbar_config(const ui_topbar_config_t *cfg);
/* Update the brightness chip label (percent value). Safe to call every second. */
void ui_pages_set_topbar_brightness(int percent);
/* Set the page wallpaper shown behind the content tiles. `path` is a PNG/JPEG
 * reachable via stdio (e.g. "/sdcard/bg/....png"); pass NULL or "" to clear.
 * Safe to call after ui_pages_init(). The decoded image is cached and freed on
 * the next call or on ui_pages_reset(). */
void ui_pages_set_wallpaper(const char *path);
