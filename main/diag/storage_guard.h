/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guard window for deliberately long storage operations (microSD format,
 * firmware upload over HTTP).  Both park the CPU for long enough that the idle
 * tasks stop running and the watchdogs see "the system is stuck".  While a
 * window is open the idle tasks are unsubscribed from the task watchdog and the
 * UI heartbeat watchdog is paused; everything is restored by the last end().
 * Windows nest and are counted. */
void storage_guard_begin(const char *reason);
void storage_guard_end(void);
bool storage_guard_active(void);

/* Short window in which *no* background task may write to the internal flash.
 *
 * esp_ota_end() validates the freshly written image with esp_image_verify(),
 * which maps every segment through esp_partition_mmap()/munmap() — i.e. it
 * reprograms MMU pages, each time parking the other core with the cache off.
 * This build executes .text/.rodata from PSRAM (CONFIG_SPIRAM_XIP_FROM_PSRAM=y)
 * through that very same MMU, so a flash write landing inside the remap window
 * makes the far core fetch through a page that is momentarily being
 * reprogrammed, ending in rst:0x7 (HP_SYS_HP_WDT_RESET).
 *
 * The writers that matter (diag/system_log.c, ui/widgets/w_graph.c) hold their
 * bytes in RAM while paused and flush them once the window closes. */
void storage_guard_flash_writers_pause(void);
void storage_guard_flash_writers_resume(void);
bool storage_guard_flash_writers_paused(void);

#ifdef __cplusplus
}
#endif
