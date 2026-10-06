/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Display driver for the Guition JC8012P4A1C-I-W-Y 10.1" panel.
 *
 * The JC8012P4A1C has no upstream ESP-BSP package, so this file performs the
 * low-level init directly instead of calling bsp_display_new_with_handles():
 *   - JD9365 MIPI-DSI controller (800x1280 portrait, RGB565, 2 data lanes)
 *   - LEDC backlight on GPIO23 (channel 1 / timer 1, 10-bit @ 20 kHz)
 *   - LDO_VO3 (chan 3) @ 2500 mV for the MIPI-DSI PHY power rail
 *   - LVGL via esp_lvgl_port, software-rotated 270 deg to 1280x800 landscape
 *
 * The driver sources (esp_lcd_jd9365) come from the official Guition/Espressif
 * reference for this exact panel; only the wiring below is project-specific.
 */
#include "drivers/display_init.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "bsp/display.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/ledc.h"
#include "hal/mipi_dsi_brg_ll.h"
#include "lvgl.h"

#include "app_config.h"
#include "util/log_tags.h"

#define DISPLAY_FULL_BUFFER_PIXELS ((APP_SCREEN_WIDTH * APP_SCREEN_HEIGHT))
#define DISPLAY_POWER_EVAL_PERIOD_US (2000ULL * 1000ULL)

#define DISPLAY_NVS_NAMESPACE "display"
#define DISPLAY_NVS_KEY_POWER "power"
#define DISPLAY_NVS_MAGIC 0x44535057U /* "DSPW" */
#define DISPLAY_NVS_VERSION 4U

typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t active_percent;
    int32_t dim_percent;
    uint32_t dim_timeout_ms;
    uint32_t off_timeout_ms;
    uint32_t night_enabled;
    int32_t night_start_hour;
    int32_t night_end_hour;
    uint32_t screensaver_enabled;
    int32_t screensaver_percent;
    uint32_t screensaver_clock_enabled;
    char screensaver_wallpaper[64];
} display_power_nvs_t;

/* JC8012P4A1C panel wiring (see esp32_p4_function_ev_board.h in the
 * Guition reference).  Native panel orientation is 800x1280 portrait. */
#define JC_LCD_H_RES 800
#define JC_LCD_V_RES 1280
#define JC_LCD_BACKLIGHT_GPIO GPIO_NUM_23
#define JC_LCD_RST_GPIO GPIO_NUM_27
#define JC_LCD_DSI_LANE_NUM 2

#define JC_LCD_BACKLIGHT_LEDC_CH LEDC_CHANNEL_1
#define JC_LCD_BACKLIGHT_LEDC_TIMER LEDC_TIMER_1
#define JC_LCD_BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define JC_LCD_BACKLIGHT_LEDC_FREQ_HZ 20000
#define JC_LCD_BACKLIGHT_LEDC_MAX_DUTY ((1U << JC_LCD_BACKLIGHT_LEDC_DUTY_RES) - 1U)

static bool s_display_ready = false;
static lv_display_t *s_lv_display = NULL;
static esp_timer_handle_t s_power_timer = NULL;
static int s_display_brightness = -1;
static int s_active_brightness = APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT;
static int s_dim_brightness = APP_DISPLAY_DIM_BRIGHTNESS_PERCENT;
static uint32_t s_dim_timeout_ms = APP_DISPLAY_DIM_TIMEOUT_MS;
static uint32_t s_off_timeout_ms = APP_DISPLAY_OFF_TIMEOUT_MS;
static bool s_night_mode_enabled = APP_DISPLAY_NIGHT_MODE_ENABLED;
static int s_night_start_hour = APP_DISPLAY_NIGHT_START_HOUR;
static int s_night_end_hour = APP_DISPLAY_NIGHT_END_HOUR;
static bool s_screensaver_enabled = APP_DISPLAY_SCREENSAVER_ENABLED;
static int s_screensaver_brightness = APP_DISPLAY_SCREENSAVER_BRIGHTNESS_PERCENT;
static bool s_screensaver_clock_enabled = APP_DISPLAY_SCREENSAVER_CLOCK_ENABLED;
static char s_screensaver_wallpaper[64] = "";
static int64_t s_last_activity_us = 0;

static void display_power_config_load(void);
static void display_power_config_save(void);

static lvgl_port_cfg_t display_port_cfg(void)
{
    lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.task_priority = 20;
    cfg.task_stack = APP_LVGL_TASK_STACK;
    cfg.task_affinity = 1;
    cfg.task_max_sleep_ms = 100;
    return cfg;
}

static int display_clamp_brightness(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

static esp_err_t jc_backlight_set_duty(uint32_t duty)
{
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, JC_LCD_BACKLIGHT_LEDC_CH, duty),
        TAG_DISPLAY, "ledc_set_duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, JC_LCD_BACKLIGHT_LEDC_CH),
        TAG_DISPLAY, "ledc_update_duty failed");
    return ESP_OK;
}

static esp_err_t jc_backlight_set_percent(int percent)
{
    const uint32_t duty = (JC_LCD_BACKLIGHT_LEDC_MAX_DUTY * (uint32_t)percent) / 100U;
    return jc_backlight_set_duty(duty);
}

esp_err_t display_set_brightness_percent(int percent)
{
    const int next = display_clamp_brightness(percent);
    if (s_display_brightness == next) {
        return ESP_OK;
    }

    esp_err_t err = jc_backlight_set_percent(next);
    if (err == ESP_OK) {
        s_display_brightness = next;
    } else {
        ESP_LOGW(TAG_DISPLAY, "Could not set backlight to %d%%: %s", next, esp_err_to_name(err));
    }
    return err;
}

int display_get_brightness_percent(void)
{
    return s_display_brightness;
}

static int display_clamp_hour(int hour)
{
    if (hour < 0) {
        return 0;
    }
    if (hour > 23) {
        return 23;
    }
    return hour;
}

void display_get_power_config(display_power_config_t *out)
{
    if (out == NULL) {
        return;
    }
    out->active_brightness_percent = s_active_brightness;
    out->dim_brightness_percent = s_dim_brightness;
    out->dim_timeout_ms = s_dim_timeout_ms;
    out->off_timeout_ms = s_off_timeout_ms;
    out->night_mode_enabled = s_night_mode_enabled;
    out->night_start_hour = s_night_start_hour;
    out->night_end_hour = s_night_end_hour;
    out->screensaver_enabled = s_screensaver_enabled;
    out->screensaver_brightness_percent = s_screensaver_brightness;
    out->screensaver_clock_enabled = s_screensaver_clock_enabled;
    strlcpy(out->screensaver_wallpaper, s_screensaver_wallpaper, sizeof(out->screensaver_wallpaper));
}

void display_set_power_config(const display_power_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    s_active_brightness = display_clamp_brightness(cfg->active_brightness_percent);
    s_dim_brightness = display_clamp_brightness(cfg->dim_brightness_percent);
    s_dim_timeout_ms = cfg->dim_timeout_ms;
    s_off_timeout_ms = cfg->off_timeout_ms;
    s_night_mode_enabled = cfg->night_mode_enabled;
    s_night_start_hour = display_clamp_hour(cfg->night_start_hour);
    s_night_end_hour = display_clamp_hour(cfg->night_end_hour);
    s_screensaver_enabled = cfg->screensaver_enabled;
    s_screensaver_brightness = display_clamp_brightness(cfg->screensaver_brightness_percent);
    s_screensaver_clock_enabled = cfg->screensaver_clock_enabled;
    strlcpy(s_screensaver_wallpaper, cfg->screensaver_wallpaper, sizeof(s_screensaver_wallpaper));
    display_power_config_save();
    if (!s_display_ready) {
        return;
    }
    /* Treat a config change as user activity: apply the active brightness and
     * re-arm the inactivity timer with the new timeouts. */
    display_note_activity();
}

/* ------------------------------------------------------------------ */
/* NVS persistence (self-contained in the display driver)              */
/* ------------------------------------------------------------------ */
static void display_power_config_load(void)
{
    nvs_handle_t handle;
    if (nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    display_power_nvs_t stored = {0};
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(handle, DISPLAY_NVS_KEY_POWER, &stored, &len);
    nvs_close(handle);

    if (err != ESP_OK || stored.magic != DISPLAY_NVS_MAGIC) {
        return; /* first boot or incompatible layout: keep compile-time defaults */
    }

    /* v1 predates the screensaver fields; v2 predates the clock flag; v3
     * predates the wallpaper filename. Each older blob is an exact prefix of
     * the current struct, so the common fields are already populated above. */
    const size_t v1_len = offsetof(display_power_nvs_t, screensaver_enabled);
    const size_t v2_len = offsetof(display_power_nvs_t, screensaver_clock_enabled);
    const size_t v3_len = offsetof(display_power_nvs_t, screensaver_wallpaper);
    if (stored.version == DISPLAY_NVS_VERSION && len == sizeof(stored)) {
        s_screensaver_enabled = stored.screensaver_enabled != 0U;
        s_screensaver_brightness = display_clamp_brightness((int)stored.screensaver_percent);
        s_screensaver_clock_enabled = stored.screensaver_clock_enabled != 0U;
        strlcpy(s_screensaver_wallpaper, stored.screensaver_wallpaper, sizeof(s_screensaver_wallpaper));
    } else if (stored.version == 3U && len == v3_len) {
        s_screensaver_enabled = stored.screensaver_enabled != 0U;
        s_screensaver_brightness = display_clamp_brightness((int)stored.screensaver_percent);
        s_screensaver_clock_enabled = stored.screensaver_clock_enabled != 0U;
        s_screensaver_wallpaper[0] = '\0';
    } else if (stored.version == 2U && len == v2_len) {
        s_screensaver_enabled = stored.screensaver_enabled != 0U;
        s_screensaver_brightness = display_clamp_brightness((int)stored.screensaver_percent);
        s_screensaver_clock_enabled = APP_DISPLAY_SCREENSAVER_CLOCK_ENABLED;
        s_screensaver_wallpaper[0] = '\0';
    } else if (stored.version == 1U && len == v1_len) {
        s_screensaver_enabled = APP_DISPLAY_SCREENSAVER_ENABLED;
        s_screensaver_brightness = APP_DISPLAY_SCREENSAVER_BRIGHTNESS_PERCENT;
        s_screensaver_clock_enabled = APP_DISPLAY_SCREENSAVER_CLOCK_ENABLED;
        s_screensaver_wallpaper[0] = '\0';
    } else {
        return; /* unknown layout: keep compile-time defaults */
    }

    s_active_brightness = display_clamp_brightness((int)stored.active_percent);
    s_dim_brightness = display_clamp_brightness((int)stored.dim_percent);
    s_dim_timeout_ms = stored.dim_timeout_ms;
    s_off_timeout_ms = stored.off_timeout_ms;
    s_night_mode_enabled = stored.night_enabled != 0U;
    s_night_start_hour = display_clamp_hour((int)stored.night_start_hour);
    s_night_end_hour = display_clamp_hour((int)stored.night_end_hour);
}

static void display_power_config_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(DISPLAY_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    display_power_nvs_t stored = {
        .magic = DISPLAY_NVS_MAGIC,
        .version = DISPLAY_NVS_VERSION,
        .active_percent = s_active_brightness,
        .dim_percent = s_dim_brightness,
        .dim_timeout_ms = s_dim_timeout_ms,
        .off_timeout_ms = s_off_timeout_ms,
        .night_enabled = s_night_mode_enabled ? 1U : 0U,
        .night_start_hour = s_night_start_hour,
        .night_end_hour = s_night_end_hour,
        .screensaver_enabled = s_screensaver_enabled ? 1U : 0U,
        .screensaver_percent = s_screensaver_brightness,
        .screensaver_clock_enabled = s_screensaver_clock_enabled ? 1U : 0U,
    };
    strlcpy(stored.screensaver_wallpaper, s_screensaver_wallpaper, sizeof(stored.screensaver_wallpaper));
    if (nvs_set_blob(handle, DISPLAY_NVS_KEY_POWER, &stored, sizeof(stored)) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

/* ------------------------------------------------------------------ */
/* Inactivity state machine (periodic evaluator)                       */
/* ------------------------------------------------------------------ */
static bool display_night_window_active(void)
{
    if (!s_night_mode_enabled) {
        return false;
    }
    time_t now = 0;
    struct tm info = {0};
    time(&now);
    localtime_r(&now, &info);
    if (info.tm_year < (2016 - 1900)) {
        return false; /* clock not synced yet: never force the screen off */
    }

    const int hour = info.tm_hour;
    if (s_night_start_hour == s_night_end_hour) {
        return false;
    }
    if (s_night_start_hour < s_night_end_hour) {
        return hour >= s_night_start_hour && hour < s_night_end_hour;
    }
    return hour >= s_night_start_hour || hour < s_night_end_hour;
}

/* True when the inactivity state machine considers the screen "off" (night
 * window active, or the off timeout has elapsed). */
static bool display_power_off_condition(void)
{
    if (s_last_activity_us == 0) {
        return false;
    }
    const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s_last_activity_us) / 1000LL);

    if (s_dim_timeout_ms != 0U && elapsed_ms < s_dim_timeout_ms) {
        return false; /* still within the active window */
    }
    if (display_night_window_active()) {
        return true;
    }
    return s_off_timeout_ms != 0U && elapsed_ms >= s_off_timeout_ms;
}

static int display_power_target_brightness(void)
{
    if (s_last_activity_us == 0) {
        return s_active_brightness;
    }
    const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s_last_activity_us) / 1000LL);

    if (s_dim_timeout_ms != 0U && elapsed_ms < s_dim_timeout_ms) {
        return s_active_brightness;
    }

    /* Inactive: go OFF during the night window, otherwise dim first.  When the
     * graphical screensaver is enabled, keep the backlight at the screensaver
     * level so the static image stays visible instead of a fully dark panel. */
    if (display_power_off_condition()) {
        if (s_screensaver_enabled && s_screensaver_brightness > 0) {
            return s_screensaver_brightness;
        }
        return 0;
    }
    return s_dim_brightness;
}

bool display_power_is_screensaver_active(void)
{
    if (!s_display_ready || !s_screensaver_enabled || s_screensaver_brightness <= 0) {
        return false;
    }
    return display_power_off_condition();
}

static void display_power_timer_cb(void *arg)
{
    (void)arg;
    if (!s_display_ready) {
        return;
    }
    (void)display_set_brightness_percent(display_power_target_brightness());
}

static esp_err_t display_power_timer_init(void)
{
    if (s_power_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = display_power_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "display_power",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_power_timer);
    if (err != ESP_OK) {
        return err;
    }
    return esp_timer_start_periodic(s_power_timer, DISPLAY_POWER_EVAL_PERIOD_US);
}

void display_note_activity(void)
{
    if (!s_display_ready) {
        return;
    }
    s_last_activity_us = esp_timer_get_time();
    (void)display_set_brightness_percent(s_active_brightness);
}

/* Minimal bsp/display.h shim: api_settings.c / api_ota.c call this before
 * reboot so the backlight is not left burning at full brightness. */
esp_err_t bsp_display_backlight_off(void)
{
    return display_set_brightness_percent(0);
}

static esp_err_t jc_backlight_init(void)
{
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = JC_LCD_BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = JC_LCD_BACKLIGHT_LEDC_TIMER,
        .freq_hz = JC_LCD_BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG_DISPLAY, "ledc_timer_config failed");

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = JC_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = JC_LCD_BACKLIGHT_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = JC_LCD_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG_DISPLAY, "ledc_channel_config failed");
    return ESP_OK;
}

/* The DSI bridge substitutes a fixed colour while its pixel FIFO is starved.
 * ESP-IDF leaves the reserved-data register at its reset value 16383 (RGB565
 * 0x3FFF = bright cyan) and masks every underrun with discard_vcnt = h_size,
 * so a momentary DMA starvation paints a light-blue flash over the whole panel
 * - the same flash that was diagnosed and fixed on the Waveshare 7".  Set the
 * filler to black (an underrun becomes an invisible dark blink) and unmask the
 * latch.  Both registers are double-buffered: the write only takes effect after
 * dpi_config_update, which is what mipi_dsi_brg_ll_update_dpi_config() does.
 * Must run after esp_lcd_new_panel_jd9365() (it writes discard_vcnt = h_size). */
static void jc_dsi_underrun_artifact_mitigation(void)
{
    dsi_brg_dev_t *brg = MIPI_DSI_LL_GET_BRG(0);
    if (brg == NULL) {
        return;
    }
    const uint32_t rsv_was = brg->dpi_rsv_dpi_data.dpi_rsv_data;
    const uint32_t disc_was = brg->dpi_misc_config.fifo_underrun_discard_vcnt;

    brg->dpi_rsv_dpi_data.dpi_rsv_data = 0x0000;
    brg->dpi_misc_config.fifo_underrun_discard_vcnt = 0;
    mipi_dsi_brg_ll_update_dpi_config(brg);

    ESP_LOGI(TAG_DISPLAY,
        "DSI underrun artifact mitigated: rsv_data %" PRIu32 " -> 0, discard_vcnt %" PRIu32 " -> 0 lines",
        rsv_was, disc_was);
}

static esp_err_t jc_dsi_phy_power(void)
{
    /* The JC8012P4A1C board feeds the MIPI-DSI PHY from the LDO_VO3 rail
     * (channel 3) at 2500 mV.  Without this the DSI PHY stays in "No Power"
     * state and esp_lcd_new_dsi_bus() fails. */
    static esp_ldo_channel_handle_t s_phy_pwr_chan = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_phy_pwr_chan),
        TAG_DISPLAY, "Acquire LDO channel for DSI PHY failed");
    return ESP_OK;
}

static esp_err_t jc_display_new(esp_lcd_panel_io_handle_t *out_io, esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_ERROR(jc_backlight_init(), TAG_DISPLAY, "Backlight init failed");
    ESP_RETURN_ON_ERROR(jc_dsi_phy_power(), TAG_DISPLAY, "DSI PHY power failed");

    /* Create the MIPI-DSI bus first: it initializes the DSI PHY as well. */
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_config = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus),
        TAG_DISPLAY, "New DSI bus init failed");

    /* DBI (8-bit command/parameter) is used to send panel commands. */
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_config = JD9365_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io),
        TAG_DISPLAY, "New panel IO failed");

    esp_lcd_dpi_panel_config_t dpi_config =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi_config.num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS;

    jd9365_vendor_config_t vendor_config = {
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = JC_LCD_DSI_LANE_NUM,
        },
    };
    const esp_lcd_panel_dev_config_t panel_dev_config = {
        .reset_gpio_num = JC_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(io, &panel_dev_config, &panel),
        TAG_DISPLAY, "New LCD panel JD9365 failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG_DISPLAY, "LCD panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG_DISPLAY, "LCD panel init failed");

    *out_io = io;
    *out_panel = panel;
    return ESP_OK;
}

esp_err_t display_init(void)
{
    if (s_display_ready) {
        return ESP_OK;
    }

    lvgl_port_cfg_t lvgl_cfg = display_port_cfg();
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DISPLAY, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    err = jc_display_new(&io, &panel);
    if (err != ESP_OK || panel == NULL) {
        ESP_LOGE(TAG_DISPLAY, "jc_display_new failed: %s", esp_err_to_name(err));
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    err = esp_lcd_panel_disp_on_off(panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not enable LCD panel output: %s", esp_err_to_name(err));
    }

    display_power_config_load();
    ESP_LOGI(TAG_DISPLAY,
        "Power config: active=%d%% dim=%d%% dim_after=%" PRIu32 "ms off_after=%" PRIu32 "ms night=%s (%d:00-%d:00)",
        s_active_brightness, s_dim_brightness, s_dim_timeout_ms, s_off_timeout_ms,
        s_night_mode_enabled ? "on" : "off", s_night_start_hour, s_night_end_hour);

    err = display_set_brightness_percent(s_active_brightness);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not enable backlight: %s", esp_err_to_name(err));
    }

    err = display_power_timer_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG_DISPLAY, "Could not create display power timer: %s", esp_err_to_name(err));
    }

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .control_handle = NULL,
        .buffer_size = DISPLAY_FULL_BUFFER_PIXELS / 5U,
        .double_buffer = true,
        .hres = JC_LCD_H_RES,
        .vres = JC_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
#if LV_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dsi_cfg = {
        .flags = {
            .avoid_tearing = false,
        },
    };

    static const uint8_t draw_buf_divisors[] = {5U, 8U, 10U, 12U};
    uint8_t used_divisor = 0U;
    uint32_t used_buffer_pixels = 0U;
    for (size_t i = 0; i < (sizeof(draw_buf_divisors) / sizeof(draw_buf_divisors[0])); i++) {
        uint8_t divisor = draw_buf_divisors[i];
        if (divisor == 0U) {
            continue;
        }
        disp_cfg.buffer_size = DISPLAY_FULL_BUFFER_PIXELS / divisor;
        s_lv_display = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
        if (s_lv_display != NULL) {
            used_divisor = divisor;
            used_buffer_pixels = disp_cfg.buffer_size;
            break;
        }
        ESP_LOGW(TAG_DISPLAY, "lvgl_port_add_disp_dsi failed with draw_buf=1/%u (%u px), trying smaller buffer",
            (unsigned)divisor, (unsigned)disp_cfg.buffer_size);
    }

    if (s_lv_display == NULL) {
        ESP_LOGE(TAG_DISPLAY, "lvgl_port_add_disp_dsi failed");
        return ESP_FAIL;
    }

    if (lvgl_port_lock(2000)) {
        lv_display_set_antialiasing(s_lv_display, APP_LVGL_ANTIALIASING != 0);
        lv_display_set_rotation(s_lv_display, LV_DISPLAY_ROTATION_270);
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG_DISPLAY, "Could not lock LVGL for rotation setup");
        lv_display_set_antialiasing(s_lv_display, APP_LVGL_ANTIALIASING != 0);
    }
    ESP_LOGI(TAG_DISPLAY, "LVGL antialiasing: %s", (APP_LVGL_ANTIALIASING != 0) ? "on" : "off");

    /* Suppress the cyan/blue underrun flash: this must run after the DPI panel
     * has been created (esp_lcd_new_panel_jd9365 wrote discard_vcnt = h_size)
     * and after the LVGL DSI display exists, so nothing re-writes it later. */
    jc_dsi_underrun_artifact_mitigation();

    s_display_ready = true;
    ESP_LOGI(TAG_DISPLAY,
        "Display initialized (JD9365 + DSI 2ch, 800x1280@270deg, avoid_tearing=0, direct_mode=0, double_buffer=1, draw_buf=1/%u, %u px)",
        (unsigned)used_divisor, (unsigned)used_buffer_pixels);
    display_note_activity();
    return ESP_OK;
}

bool display_is_ready(void)
{
    return s_display_ready;
}

bool display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void display_unlock(void)
{
    lvgl_port_unlock();
}
