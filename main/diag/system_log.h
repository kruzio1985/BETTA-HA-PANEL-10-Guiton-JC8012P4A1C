/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the persistent system log:
 *  - installs an esp_log vprintf hook that captures WARN/ERROR lines,
 *  - starts a low-priority task that flushes the capture ring to LittleFS,
 *  - writes a boot header (version + reset reason) and periodic heap/heartbeat
 *    snapshots so freezes, panics and memory-exhaustion trends survive a reboot.
 *
 * Must be called after the LittleFS filesystem is mounted. */
esp_err_t system_log_init(void);

/* Flush any pending buffered log lines to storage (best effort). */
esp_err_t system_log_flush(void);

/* Read the tail of the active log file into buf (NUL-terminated).
 * Returns the number of bytes copied (excluding the NUL), or 0 if empty. */
int system_log_read_tail(char *buf, size_t buf_len);

/* Delete the active log file and all rotated generations, then write a fresh
 * heartbeat line so the log viewer is not left empty. */
esp_err_t system_log_clear(void);

/* Suspend/resume the UI heartbeat watchdog around a deliberately long storage
 * operation (microSD format, firmware upload).  The suspension is counted
 * (nested operations are safe) and expires on its own after
 * APP_LOG_WATCHDOG_PAUSE_MAX_MS so a genuinely hung operation cannot disable
 * the watchdog forever. */
void system_log_watchdog_suspend(void);
void system_log_watchdog_resume(void);

/* Log-file appends held back in RAM while the storage guard paused the flash
 * writers (see diag/storage_guard.h).  The counter proves the pause window
 * actually covered the MMU reprogramming inside esp_ota_end(). */
uint32_t system_log_gated_writes(void);
void system_log_gated_writes_reset(void);

/* Explicitly record a diagnostic error line. Use from error paths that would
 * otherwise fail silently. */
void system_log_write(const char *tag, const char *fmt, ...);

/* Configure the daily scheduled restart.
 * hour == -1: fixed 24h-from-boot cadence (default).
 * hour == 0..23: restart once per day at that local hour (requires synced
 * time; falls back to the 24h cadence if the clock has never been set). */
void system_log_set_daily_restart_hour(int hour);

#ifdef __cplusplus
}
#endif
