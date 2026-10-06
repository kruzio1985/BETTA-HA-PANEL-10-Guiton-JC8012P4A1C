/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Persistent, bounded system log for the BETTA panel.
 *
 * Layers:
 *  1. An esp_log_set_vprintf() hook that captures every WARN/ERROR log line
 *     into a small PSRAM ring buffer (console output is forwarded untouched).
 *  2. A low-priority task that drains the ring into /littlefs/logs/system.log.
 *  3. File rotation: when system.log reaches APP_LOG_MAX_FILE_BYTES it is
 *     renamed to system.log.1 (up to APP_LOG_MAX_ROTATED generations); the
 *     oldest generation is deleted, so storage usage is strictly bounded.
 *  4. A boot header (app version + reset reason) and a periodic heartbeat with
 *     heap/PSRAM statistics. A hard freeze (task WDT / panic) reboots the
 *     device, and the next boot header records the reset reason — the
 *     heartbeat gaps then show how much runtime was lost.
 */

#include "diag/system_log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "diag/storage_guard.h"
#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
#include "diag/data_log.h"
#include "drivers/board_extras_panel10jc.h"
#endif
#include "ui/ui_runtime.h"

#include <time.h>

#define TAG "syslog"

/* Preferred persistent log location: the microSD card (SDMMC) when mounted.
 * Writing the internal flash (LittleFS) parks both cores with the caches off,
 * which stalls the MIPI-DSI scan-out and makes the panel flash — see the
 * header comment.  LittleFS remains the fallback when no card is inserted. */
#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
#define SYSTEM_LOG_SD_DIR  "/sdcard/logs"
#define SYSTEM_LOG_SD_FILE SYSTEM_LOG_SD_DIR "/system.log"
#define SYSTEM_LOG_PATH_BYTES 96
#endif

#define SYSTEM_LOG_TASK_STACK 4096
#define SYSTEM_LOG_TASK_PRIO 1
#define SYSTEM_LOG_TASK_PERIOD_MS 2000
#define SYSTEM_LOG_CHUNK_BYTES 512
#define SYSTEM_LOG_LINE_MAX 512
#define SYSTEM_LOG_TAG_MAX 40

/* ---- Capture ring ------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t read;    /* monotonic byte counter: bytes consumed */
    size_t written; /* monotonic byte counter: bytes produced */
    SemaphoreHandle_t mutex;
} log_ring_t;

static log_ring_t s_ring;
static TaskHandle_t s_task;
static vprintf_like_t s_orig_vprintf;
static bool s_init;
static SemaphoreHandle_t s_capture_mutex;

/* UI watchdog state: last UI heartbeat value and when it last changed. */
static uint32_t s_ui_last_hb = 0;
static int64_t s_ui_last_hb_ms = 0;

/* Daily restart policy: -1 = fixed 24h-from-boot cadence (default),
 * 0..23 = restart at that local hour (falls back to 24h if time is unset). */
static int s_daily_restart_hour = -1;

/* UI heartbeat watchdog suspension (counted) and flash-writer gating.  Both
 * are driven from diag/storage_guard.c around deliberately long storage
 * operations and the MMU reprogramming inside esp_ota_end(). */
static uint32_t s_watchdog_pause_depth;
static int64_t s_watchdog_pause_start_ms;
static uint32_t s_gated_writes;

/* vprintf line-reconstruction state. With ESP_LOG_VERSION == 2 the hook is
 * called three times per line (prefix, message body, newline) and those calls
 * are serialized by esp_log's stdout lock; with version 1 the whole line
 * arrives in one call and is serialized by s_capture_mutex instead. */
static bool s_line_open;
static char s_line_level;
static char s_line_tag[SYSTEM_LOG_TAG_MAX];
static char s_line[SYSTEM_LOG_LINE_MAX];
static size_t s_line_len;

static size_t ring_avail(const log_ring_t *r)
{
    return r->written - r->read;
}

static void ring_push(log_ring_t *r, const uint8_t *data, size_t len)
{
    if (r->buf == NULL || r->mutex == NULL || data == NULL || len == 0) {
        return;
    }

    xSemaphoreTake(r->mutex, portMAX_DELAY);
    while (len > 0) {
        size_t avail = ring_avail(r);
        if (avail >= r->cap) {
            /* Drop the oldest bytes so the newest diagnostics are kept. */
            r->read += (avail - r->cap) + 1U;
            continue;
        }
        size_t space = r->cap - avail;
        size_t n = len < space ? len : space;
        size_t off = r->written % r->cap;
        size_t first = r->cap - off;
        size_t c1 = n < first ? n : first;
        memcpy(r->buf + off, data, c1);
        if (n > c1) {
            memcpy(r->buf, data + c1, n - c1);
        }
        r->written += n;
        data += n;
        len -= n;
    }
    xSemaphoreGive(r->mutex);

    /* Wake the drain task immediately so W/E lines reach flash right away
     * instead of waiting up to SYSTEM_LOG_TASK_PERIOD_MS.  A panic/reboot can
     * strike between wakeups, so minimising the in-RAM window is what makes
     * the last lines before a crash survive. */
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

static size_t ring_pop(log_ring_t *r, uint8_t *dst, size_t max)
{
    if (r->buf == NULL || r->mutex == NULL || dst == NULL || max == 0) {
        return 0;
    }

    size_t got = 0;
    xSemaphoreTake(r->mutex, portMAX_DELAY);
    size_t avail = ring_avail(r);
    if (avail > max) {
        avail = max;
    }
    size_t off = r->read % r->cap;
    size_t first = r->cap - off;
    size_t c1 = avail < first ? avail : first;
    memcpy(dst, r->buf + off, c1);
    if (avail > c1) {
        memcpy(dst + c1, r->buf, avail - c1);
    }
    r->read += avail;
    got = avail;
    xSemaphoreGive(r->mutex);
    return got;
}

/* ---- vprintf line reconstruction -------------------------------------- */

static void line_finalize(void)
{
    if (!s_line_open) {
        return;
    }
    s_line_open = false;

    if (s_line_len == 0) {
        return;
    }

    char out[SYSTEM_LOG_LINE_MAX + SYSTEM_LOG_TAG_MAX + 8];
    int n = snprintf(out, sizeof(out), "%c %s: %s\n", s_line_level, s_line_tag, s_line);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(out)) {
        len = sizeof(out) - 1U;
    }
    ring_push(&s_ring, (const uint8_t *)out, len);
    s_line_len = 0;
    s_line[0] = '\0';
}

/* Returns true when a log line at the given level/tag should be persisted:
 * errors and warnings always, plus INFO from camera-related tags so the
 * sensor/CSI bring-up sequence can be diagnosed remotely without a serial
 * console. */
static bool log_line_wanted(char level, const char *tag)
{
    if (level == 'E' || level == 'W') {
        return true;
    }
    if (level != 'I') {
        return false;
    }

    static const char *const cam_tags[] = {
        "camera", "cam_sensor", "ov02c10", "xclk",
        "esp_video", "esp_video_init", "esp_video_cam", "esp_video_buffer",
        "csi_video", "isp_video", "jpeg_video", "ISP",
    };
    for (size_t i = 0; i < sizeof(cam_tags) / sizeof(cam_tags[0]); i++) {
        if (strcmp(tag, cam_tags[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void prefix_parse(const char *fmt, va_list args)
{
    if (s_line_open) {
        /* Defensive: a previous line never got its newline. */
        line_finalize();
    }

    char buf[160];
    va_list cp;
    va_copy(cp, args);
    vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);

    const char *p = buf;
    if (p[0] == '\x1b' && p[1] == '[') {
        const char *m = strchr(p, 'm');
        if (m != NULL) {
            p = m + 1;
        }
    }

    const char *tag_start = strstr(p, ") ");
    if (tag_start == NULL) {
        s_line_open = false;
        return;
    }
    tag_start += 2;
    const char *tag_end = strstr(tag_start, ": ");
    size_t tag_len = tag_end != NULL ? (size_t)(tag_end - tag_start) : strlen(tag_start);
    if (tag_len >= sizeof(s_line_tag)) {
        tag_len = sizeof(s_line_tag) - 1U;
    }
    memcpy(s_line_tag, tag_start, tag_len);
    s_line_tag[tag_len] = '\0';

    if (!log_line_wanted(*p, s_line_tag)) {
        s_line_open = false;
        return;
    }

    s_line_level = *p;
    s_line[0] = '\0';
    s_line_len = 0;
    s_line_open = true;
}

static bool is_newline_segment(const char *fmt, va_list args)
{
    if (strcmp(fmt, "%s") != 0) {
        return false;
    }

    char buf[16];
    va_list cp;
    va_copy(cp, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);

    if (n <= 0 || n >= (int)sizeof(buf)) {
        return false;
    }
    if (buf[n - 1] != '\n') {
        return false;
    }

    /* Accept only whitespace + ANSI color-reset before the newline. */
    const char *s = buf;
    while (*s != '\0') {
        if (*s == '\n' || *s == '\r' || *s == ' ') {
            s++;
            continue;
        }
        if (*s == '\x1b') {
            const char *m = strchr(s, 'm');
            if (m == NULL) {
                return false;
            }
            s = m + 1;
            continue;
        }
        return false;
    }
    return true;
}

static void line_append(const char *fmt, va_list args)
{
    if (s_line_len >= sizeof(s_line) - 1U) {
        return;
    }

    va_list cp;
    va_copy(cp, args);
    int n = vsnprintf(s_line + s_line_len, sizeof(s_line) - s_line_len, fmt, cp);
    va_end(cp);

    if (n > 0) {
        size_t add = (size_t)n;
        size_t remain = sizeof(s_line) - 1U - s_line_len;
        if (add > remain) {
            add = remain;
        }
        s_line_len += add;
        s_line[s_line_len] = '\0';
    }
}

/* ESP_LOG_VERSION == 1 path: the vprintf hook receives each log line as a
 * single call whose format is the fully-composed line (LOG_FORMAT in
 * esp_log_format.h):
 *
 *   LOG_COLOR_x "x (%" PRIu32 ") %s: " user_format LOG_RESET_COLOR "\n"
 *
 * With colors disabled (this project) the format string starts directly with
 * the level letter; with colors enabled it starts with an ANSI escape. Lines
 * are filtered by level and tag (see log_line_wanted): E/W always, plus INFO
 * from camera-related tags. Accepted lines are rendered, stripped of color
 * sequences, and re-emitted in the compact "<level> <tag>: <message>\n" form
 * used by the ring. */
static void capture_v1_line(const char *fmt, va_list args)
{
    const char *f = fmt;
    if (f[0] == '\x1b' && f[1] == '[') {
        const char *m = strchr(f, 'm');
        if (m != NULL) {
            f = m + 1;
        }
    }

    if (f[1] != ' ' || f[2] != '(') {
        return;
    }

    static char buf[SYSTEM_LOG_LINE_MAX + 160];
    va_list cp;
    va_copy(cp, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, cp);
    va_end(cp);
    if (n <= 0) {
        return;
    }

    size_t len = (size_t)n;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1U;
    }
    buf[len] = '\0';

    const char *p = buf;
    if (p[0] == '\x1b' && p[1] == '[') {
        const char *m = strchr(p, 'm');
        if (m != NULL) {
            p = m + 1;
        }
    }

    const char *tag_start = strstr(p, ") ");
    if (tag_start == NULL) {
        return;
    }
    tag_start += 2;

    const char *tag_end = strstr(tag_start, ": ");
    if (tag_end == NULL) {
        return;
    }
    const char *msg_start = tag_end + 2;

    /* Strip the trailing LOG_RESET_COLOR and newline from the message. */
    size_t msg_len = strlen(msg_start);
    while (msg_len > 0 && (msg_start[msg_len - 1] == '\n' || msg_start[msg_len - 1] == '\r')) {
        msg_len--;
    }
    static const char reset[] = "\x1b[0m";
    if (msg_len >= (sizeof(reset) - 1) &&
        memcmp(msg_start + msg_len - (sizeof(reset) - 1), reset, sizeof(reset) - 1) == 0) {
        msg_len -= sizeof(reset) - 1;
    }

    size_t tag_len = (size_t)(tag_end - tag_start);
    if (tag_len >= sizeof(s_line_tag)) {
        tag_len = sizeof(s_line_tag) - 1U;
    }
    memcpy(s_line_tag, tag_start, tag_len);
    s_line_tag[tag_len] = '\0';

    if (!log_line_wanted(*p, s_line_tag)) {
        return;
    }

    static char out[SYSTEM_LOG_LINE_MAX + SYSTEM_LOG_TAG_MAX + 8];
    int m = snprintf(out, sizeof(out), "%c %s: %.*s\n", *p, s_line_tag, (int)msg_len, msg_start);
    if (m <= 0) {
        return;
    }
    size_t out_len = (size_t)m;
    if (out_len >= sizeof(out)) {
        out_len = sizeof(out) - 1U;
    }
    ring_push(&s_ring, (const uint8_t *)out, out_len);
}

static int system_log_vprintf(const char *fmt, va_list args)
{
    int ret = 0;
    if (s_orig_vprintf != NULL) {
        va_list fwd;
        va_copy(fwd, args);
        ret = s_orig_vprintf(fmt, fwd);
        va_end(fwd);
    }

    if (fmt == NULL || !s_init) {
        return ret;
    }

    /* ESP_LOG_VERSION == 2: prefix call starts the line. */
    if (strncmp(fmt, "%s%c ", 5) == 0) {
        prefix_parse(fmt, args);
        return ret;
    }

    if (s_line_open) {
        if (is_newline_segment(fmt, args)) {
            line_finalize();
        } else {
            line_append(fmt, args);
        }
        return ret;
    }

    /* ESP_LOG_VERSION == 1: the entire line arrives as a single call. The
     * capture path renders into shared static buffers, so it is serialized by
     * s_capture_mutex (v1 has no stdout lock around the hook). */
    if (s_capture_mutex != NULL &&
        xSemaphoreTake(s_capture_mutex, portMAX_DELAY) == pdTRUE) {
        capture_v1_line(fmt, args);
        xSemaphoreGive(s_capture_mutex);
    }
    return ret;
}

/* ---- Storage ----------------------------------------------------------- */

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
static bool system_log_on_sd(void)
{
    return sdcard_is_ready();
}

/* Path of log generation `rot` (0 = the live file) on the active storage. */
static void system_log_path(int rot, char *out, size_t out_len)
{
    const char *file = system_log_on_sd() ? SYSTEM_LOG_SD_FILE : APP_LOG_FILE;
    if (rot <= 0) {
        snprintf(out, out_len, "%s", file);
    } else {
        snprintf(out, out_len, "%s.%d", file, rot);
    }
}
#else
static bool system_log_on_sd(void)
{
    return false;
}

static void system_log_path(int rot, char *out, size_t out_len)
{
    if (rot <= 0) {
        snprintf(out, out_len, "%s", APP_LOG_FILE);
    } else {
        snprintf(out, out_len, "%s.%d", APP_LOG_FILE, rot);
    }
}
#endif

static void system_log_rotate(void)
{
#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    char old_path[SYSTEM_LOG_PATH_BYTES];
    char new_path[SYSTEM_LOG_PATH_BYTES];
#else
    char old_path[sizeof(APP_LOG_FILE) + 4];
    char new_path[sizeof(APP_LOG_FILE) + 4];
#endif

    /* Delete the oldest generation first. */
    system_log_path(APP_LOG_MAX_ROTATED, old_path, sizeof(old_path));
    remove(old_path);

    for (int i = APP_LOG_MAX_ROTATED - 1; i >= 1; i--) {
        system_log_path(i, old_path, sizeof(old_path));
        system_log_path(i + 1, new_path, sizeof(new_path));
        rename(old_path, new_path);
    }

    system_log_path(1, old_path, sizeof(old_path));
    system_log_path(0, new_path, sizeof(new_path));
    rename(new_path, old_path);
}

static void system_log_file_append(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    /* While the storage guard holds the flash writers (MMU reprogramming in
     * esp_ota_end) the line must not touch the internal flash.  It stays in the
     * capture ring and reaches the file once the window closes.  SD writes are
     * safe and are not gated. */
    if (storage_guard_flash_writers_paused() && !system_log_on_sd()) {
        s_gated_writes++;
        return;
    }

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    char path[SYSTEM_LOG_PATH_BYTES];
#else
    char path[sizeof(APP_LOG_FILE) + 4];
#endif
    system_log_path(0, path, sizeof(path));

    FILE *f = fopen(path, "ab");
#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    if (f == NULL && system_log_on_sd()) {
        (void)mkdir(SYSTEM_LOG_SD_DIR, 0777);
        f = fopen(path, "ab");
    }
#endif
    if (f == NULL) {
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return;
    }
    if ((unsigned long)size + (unsigned long)len > (unsigned long)APP_LOG_MAX_FILE_BYTES) {
        fclose(f);
        system_log_rotate();
        f = fopen(path, "ab");
        if (f == NULL) {
            return;
        }
    }

    fwrite(data, 1, len, f);
    fclose(f);

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    /* The SD mirror is only needed while the primary write still lands on
     * LittleFS (no card inserted at write time).  With a card mounted the line
     * above already went straight to /sdcard/logs/system.log. */
    if (!system_log_on_sd()) {
        data_log_mirror_syslog((const char *)data, len);
    }
#endif
}

static void system_log_check_ui_watchdog(void)
{
    if (!ui_runtime_is_running()) {
        return;
    }

    /* A storage_guard window (SD format, OTA) may legitimately block the UI
     * task for tens of seconds: suppress the stale-heartbeat restart while a
     * suspension is active, but never forever (bounded expiry). */
    if (s_watchdog_pause_depth > 0) {
        int64_t held_ms = (esp_timer_get_time() / 1000) - s_watchdog_pause_start_ms;
        if (held_ms <= APP_LOG_WATCHDOG_PAUSE_MAX_MS) {
            return;
        }
        /* The suspension overstayed: drop it so a genuinely hung operation
         * cannot disable the watchdog indefinitely. */
        s_watchdog_pause_depth = 0;
        ESP_LOGW(TAG, "watchdog pause expired after %lld ms", (long long)held_ms);
    }

    uint32_t hb = ui_runtime_get_heartbeat();
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (hb != s_ui_last_hb) {
        s_ui_last_hb = hb;
        s_ui_last_hb_ms = now_ms;
        return;
    }

    if (hb == 0) {
        /* UI task hasn't produced its first heartbeat yet. */
        return;
    }

    int64_t stalled_ms = now_ms - s_ui_last_hb_ms;
    if (stalled_ms <= APP_UI_WATCHDOG_TIMEOUT_MS) {
        return;
    }

    char line[160];
    int n = snprintf(line, sizeof(line),
                     "!!! WATCHDOG: UI task stalled (no heartbeat for %lld ms), restarting !!!\n",
                     (long long)stalled_ms);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1U;
        }
        system_log_file_append((const uint8_t *)line, len);
    }
    ESP_LOGE(TAG, "UI task watchdog triggered (no heartbeat for %lld ms); restarting",
             (long long)stalled_ms);
    esp_restart();
}

static void system_log_check_scheduled_restart(void)
{
    int64_t uptime_ms = esp_timer_get_time() / 1000;

    if (s_daily_restart_hour >= 0) {
        time_t now = time(NULL);
        if (now > 0) {
            /* Restart at the next occurrence of the configured local hour that
             * lies strictly in the future. Computing "strictly after now" every
             * check guarantees a reboot at 03:00:10 will not re-trigger when the
             * device comes back up at 03:00:40 the same hour. */
            struct tm tm_now = {0};
            localtime_r(&now, &tm_now);
            struct tm tm_next = tm_now;
            tm_next.tm_hour = s_daily_restart_hour;
            tm_next.tm_min = 0;
            tm_next.tm_sec = 0;
            time_t target = mktime(&tm_next);
            if (target <= now) {
                tm_next.tm_mday += 1;
                target = mktime(&tm_next);
            }
            if (now < target) {
                return; /* not yet */
            }

            char line[128];
            int n = snprintf(line, sizeof(line),
                             "!!! SCHEDULED: daily restart at local hour %d !!!\n",
                             s_daily_restart_hour);
            if (n > 0) {
                size_t len = (size_t)n;
                if (len >= sizeof(line)) {
                    len = sizeof(line) - 1U;
                }
                system_log_file_append((const uint8_t *)line, len);
            }
            ESP_LOGW(TAG, "Scheduled daily restart (local hour %d)", s_daily_restart_hour);
            esp_restart();
            return;
        }
        /* Clock has never been synced: fall through to the 24h uptime cadence. */
    }

    if (uptime_ms < APP_DAILY_RESTART_MS) {
        return;
    }

    char line[128];
    int n = snprintf(line, sizeof(line),
                     "!!! SCHEDULED: periodic 24h restart (uptime=%lld ms) !!!\n",
                     (long long)uptime_ms);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1U;
        }
        system_log_file_append((const uint8_t *)line, len);
    }
    ESP_LOGW(TAG, "Scheduled 24h restart (uptime=%lld ms)", (long long)uptime_ms);
    esp_restart();
}

void system_log_set_daily_restart_hour(int hour)
{
    if (hour < -1) {
        hour = -1;
    }
    if (hour > 23) {
        hour = 23;
    }
    s_daily_restart_hour = hour;
    ESP_LOGI(TAG, "Daily restart policy: %s",
             hour >= 0 ? "fixed local hour" : "24h from boot");
}

static void system_log_heartbeat(void)
{
    system_log_check_ui_watchdog();
    system_log_check_scheduled_restart();

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    char line[256];
    int n = snprintf(line, sizeof(line),
                     "I sys: uptime=%us heap_free=%u heap_min=%u heap_largest=%u "
                     "psram_free=%u psram_largest=%u\n",
                     (unsigned)uptime_s, (unsigned)free_heap, (unsigned)min_heap,
                     (unsigned)largest_8bit, (unsigned)free_psram, (unsigned)largest_psram);
    if (n <= 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(line)) {
        len = sizeof(line) - 1U;
    }
    system_log_file_append((const uint8_t *)line, len);
}

static void system_log_boot_header(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *ver = (desc != NULL && desc->version[0] != '\0') ? desc->version : "unknown";

    static const char *const reset_names[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT",
        "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE",
        "PWR_GLITCH", "CPU_LOCKUP", "SUPER_WDT",
    };
    esp_reset_reason_t rr = esp_reset_reason();
    const char *reset_name =
        (rr >= 0 && rr < (esp_reset_reason_t)(sizeof(reset_names) / sizeof(reset_names[0])))
            ? reset_names[rr]
            : "INVALID";

    char line[256];
    int n = snprintf(line, sizeof(line),
                     "=== boot app=%s version=%s reset=%s(%d) ===\n",
                     APP_NAME, ver, reset_name, (int)rr);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1U;
        }
        system_log_file_append((const uint8_t *)line, len);
    }

    /* Make crashes impossible to miss when scrolling the web log viewer. */
    const bool crash_reset =
        rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT || rr == ESP_RST_CPU_LOCKUP;
    if (crash_reset) {
        char crash_line[128];
        int cn = snprintf(crash_line, sizeof(crash_line),
                          "!!! CRASH: previous boot ended with reset=%s(%d) !!!\n",
                          reset_name, (int)rr);
        if (cn > 0) {
            size_t clen = (size_t)cn;
            if (clen >= sizeof(crash_line)) {
                clen = sizeof(crash_line) - 1U;
            }
            system_log_file_append((const uint8_t *)crash_line, clen);
        }
    }

    /* Immediate first snapshot so a crash very early in the boot is bounded. */
    system_log_heartbeat();
}

/* ---- Task -------------------------------------------------------------- */

static void system_log_task(void *arg)
{
    (void)arg;
    uint8_t chunk[SYSTEM_LOG_CHUNK_BYTES];
    uint32_t loops = 0;
    uint32_t heartbeat_loops =
        (APP_LOG_HEARTBEAT_MS + SYSTEM_LOG_TASK_PERIOD_MS - 1U) / SYSTEM_LOG_TASK_PERIOD_MS;

    for (;;) {
        for (;;) {
            size_t n = ring_pop(&s_ring, chunk, sizeof(chunk));
            if (n == 0) {
                break;
            }
            system_log_file_append(chunk, n);
        }

        loops++;
        if (heartbeat_loops > 0 && loops % heartbeat_loops == 0) {
            system_log_heartbeat();
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SYSTEM_LOG_TASK_PERIOD_MS));
    }
}

/* ---- Public API -------------------------------------------------------- */

esp_err_t system_log_init(void)
{
    if (s_init) {
        return ESP_OK;
    }

    (void)mkdir(APP_LOG_DIR, 0755); /* ignore error: may already exist */

    s_ring.buf = (uint8_t *)heap_caps_malloc(APP_LOG_RING_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring.buf == NULL) {
        s_ring.buf = (uint8_t *)heap_caps_malloc(APP_LOG_RING_BYTES, MALLOC_CAP_8BIT);
    }
    if (s_ring.buf == NULL) {
        ESP_LOGW(TAG, "log ring alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_ring.cap = APP_LOG_RING_BYTES;

    s_ring.mutex = xSemaphoreCreateMutex();
    if (s_ring.mutex == NULL) {
        heap_caps_free(s_ring.buf);
        s_ring.buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_capture_mutex = xSemaphoreCreateMutex();
    if (s_capture_mutex == NULL) {
        vSemaphoreDelete(s_ring.mutex);
        s_ring.mutex = NULL;
        heap_caps_free(s_ring.buf);
        s_ring.buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_init = true;
    s_orig_vprintf = esp_log_set_vprintf(system_log_vprintf);

    BaseType_t ok = xTaskCreatePinnedToCore(system_log_task, TAG, SYSTEM_LOG_TASK_STACK,
                                            NULL, SYSTEM_LOG_TASK_PRIO, &s_task, 0);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "log task create failed; ring will drop oldest lines");
    }

    system_log_boot_header();
    return ESP_OK;
}

esp_err_t system_log_flush(void)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
        /* Give the low-priority task a moment to drain and append. */
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        uint8_t chunk[SYSTEM_LOG_CHUNK_BYTES];
        for (;;) {
            size_t n = ring_pop(&s_ring, chunk, sizeof(chunk));
            if (n == 0) {
                break;
            }
            system_log_file_append(chunk, n);
        }
    }
    return ESP_OK;
}

int system_log_read_tail(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return 0;
    }

    system_log_flush();

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    char path[SYSTEM_LOG_PATH_BYTES];
#else
    char path[sizeof(APP_LOG_FILE) + 4];
#endif
    system_log_path(0, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        buf[0] = '\0';
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        buf[0] = '\0';
        return 0;
    }
    long size = ftell(f);
    size_t want = buf_len - 1U;
    if (size > (long)want) {
        if (fseek(f, size - (long)want, SEEK_SET) != 0) {
            fclose(f);
            buf[0] = '\0';
            return 0;
        }
    } else {
        rewind(f);
    }

    size_t got = fread(buf, 1, want, f);
    fclose(f);
    buf[got] = '\0';
    return (int)got;
}

esp_err_t system_log_clear(void)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Drain the capture ring first so a clear request can't race pending
     * lines back into the file after we truncate it. */
    system_log_flush();

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    char path[SYSTEM_LOG_PATH_BYTES];
#else
    char path[sizeof(APP_LOG_FILE) + 4];
#endif
    for (int i = 0; i <= APP_LOG_MAX_ROTATED; i++) {
        system_log_path(i, path, sizeof(path));
        remove(path);
    }

    /* Leave a fresh line so the viewer shows a start point, not "(no logs)". */
    system_log_heartbeat();
    return ESP_OK;
}

void system_log_watchdog_suspend(void)
{
    if (s_watchdog_pause_depth == 0) {
        s_watchdog_pause_start_ms = esp_timer_get_time() / 1000;
    }
    s_watchdog_pause_depth++;
}

void system_log_watchdog_resume(void)
{
    if (s_watchdog_pause_depth > 0) {
        s_watchdog_pause_depth--;
    }
}

uint32_t system_log_gated_writes(void)
{
    return s_gated_writes;
}

void system_log_gated_writes_reset(void)
{
    s_gated_writes = 0;
}

void system_log_write(const char *tag, const char *fmt, ...)
{
    if (fmt == NULL) {
        return;
    }

    char body[SYSTEM_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }

    char out[SYSTEM_LOG_LINE_MAX + SYSTEM_LOG_TAG_MAX + 8];
    int m = snprintf(out, sizeof(out), "E %s: %s\n",
                     (tag != NULL && tag[0] != '\0') ? tag : "sys", body);
    if (m <= 0) {
        return;
    }
    size_t len = (size_t)m;
    if (len >= sizeof(out)) {
        len = sizeof(out) - 1U;
    }
    ring_push(&s_ring, (const uint8_t *)out, len);
}
