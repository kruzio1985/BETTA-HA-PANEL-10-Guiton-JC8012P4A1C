/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include "esp_err.h"

/* Decode the screensaver wallpaper early in boot, while PSRAM is still at its
 * post-boot maximum. Call this right after display_init() (and the boot
 * splash), before the layout tiles, camera and HA tasks consume PSRAM.
 * The decoded image is cached for every later screensaver cycle. */
void ui_screensaver_preload(void);

esp_err_t ui_screensaver_init(void);
void ui_screensaver_deinit(void);
