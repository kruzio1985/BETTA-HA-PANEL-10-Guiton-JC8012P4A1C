/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app_config.h"

/* Motion-detection zone in percent of the frame (0..100). */
typedef struct {
    int x;
    int y;
    int w;
    int h;
} runtime_motion_zone_t;

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_MAX_LEN];
    char wifi_password[APP_WIFI_PASSWORD_MAX_LEN];
    char wifi_country_code[APP_WIFI_COUNTRY_CODE_MAX_LEN];
    char wifi_bssid[APP_WIFI_BSSID_MAX_LEN];
    char ha_ws_url[APP_HA_WS_URL_MAX_LEN];
    char ha_access_token[APP_HA_ACCESS_TOKEN_MAX_LEN];
    bool ha_rest_enabled;
    char ntp_server[APP_NTP_SERVER_MAX_LEN];
    char time_tz[APP_TIME_TZ_MAX_LEN];
    char ui_language[APP_UI_LANGUAGE_MAX_LEN];
    char xiaozhi_server[APP_XIAOZHI_SERVER_MAX_LEN];
    char xiaozhi_device[APP_XIAOZHI_DEVICE_MAX_LEN];
    char xiaozhi_token[APP_XIAOZHI_TOKEN_MAX_LEN];
    char xiaozhi_ota_url[APP_XIAOZHI_OTA_URL_MAX_LEN];
    bool xiaozhi_enabled;
    bool sd_logging_enabled;

    /* Network: static IPv4 configuration (DHCP when disabled). */
    bool wifi_static_ip_enabled;
    char wifi_static_ip[16];
    char wifi_static_netmask[16];
    char wifi_static_gateway[16];
    char wifi_static_dns[16];

    /* Local camera (JC variant). */
    bool camera_enabled;
    bool camera_motion_wake;
    int camera_motion_threshold; /* 1..64; higher = less sensitive */
    int camera_jpeg_quality;     /* 10..95 */
    bool camera_hflip;
    bool camera_vflip;
    bool camera_stream_enabled; /* MJPEG live stream for HA (http://<ip>/api/camera/stream) */
    int camera_resolution;      /* 0 = Full HD (1920x1080), 1 = HD Ready (960x540) */

    /* Camera motion-detector tuning (zones + debounce + lighting filter). */
    int camera_motion_min_area;        /* 0..100 % of active cells, 0 = off */
    int camera_motion_min_duration_ms; /* 0..1000 ms, 0 = off */
    int camera_motion_cooldown_ms;     /* 0..30000 ms between two wake-ups */
    int camera_motion_start_delay_ms;  /* 0..10000 ms grace after start */
    bool camera_motion_ignore_lighting;
    int camera_motion_zone_count;      /* 0..4, 0 = whole frame */
    runtime_motion_zone_t camera_motion_zones[4];

    /* Local camera: manual ISP image calibration. The master switch drives the
     * basic blocks (brightness/contrast/saturation/hue/tone); the three "auto"
     * flags keep the matching block under IPA control (switch off = manual). */
    bool camera_img_manual;
    bool camera_img_wb_manual;
    bool camera_img_sharpen_manual;
    bool camera_img_denoise_manual;
    int camera_img_brightness;      /* -128..127, 0 = neutral */
    int camera_img_contrast;        /* 0..255, 128 = neutral */
    int camera_img_saturation;      /* 0..255, 128 = neutral */
    int camera_img_hue;             /* 0..360, 0 = neutral */
    int camera_img_wb_red;          /* 50..200 %, 100 = neutral */
    int camera_img_wb_blue;         /* 50..200 %, 100 = neutral */
    int camera_img_sharpen;         /* 25..300 %, 100 = neutral */
    int camera_img_denoise;         /* 25..200 %, 100 = neutral */
    int camera_img_tone_shadows;    /* -100..100 %, 0 = neutral */
    int camera_img_tone_highlights; /* -100..100 %, 0 = neutral */

    /* SD logging. */
    int sd_flush_interval_s; /* 5..300 */
    bool sd_log_system_enabled;
    bool sd_log_sensors_enabled;
    bool sd_log_camera_enabled;

    /* System. */
    int daily_restart_hour; /* -1 = fixed 24h from boot; 0..23 = local hour */
    bool touch_test;        /* Touch debug overlay (red dots) for dead-zone testing */

    /* Audio. */
    int audio_volume; /* 0..100 */

    /* Top bar (configurable via the web editor). */
    bool topbar_show_clock;
    bool topbar_show_room_name;
    char topbar_room_name[APP_TOP_BAR_ROOM_NAME_MAX_LEN];
    bool topbar_show_status;
    bool topbar_show_brightness;

    /* Appearance: page wallpaper + tile transparency. */
    char page_wallpaper[APP_MAX_IMAGE_PATH_LEN]; /* "" = no wallpaper; else /sdcard/bg/... image shown behind tiles */
    int tile_transparency;                       /* 0..100; 0 = opaque tiles, higher = more transparent */
} runtime_settings_t;

void runtime_settings_set_defaults(runtime_settings_t *out);
esp_err_t runtime_settings_init(void);
esp_err_t runtime_settings_load(runtime_settings_t *out);
esp_err_t runtime_settings_save(const runtime_settings_t *settings);
/**
 * @brief Push the manual ISP image calibration to the camera pipeline.
 *
 * Has to be called after a settings change (and once after the settings were
 * loaded), the calibration itself lives outside of the settings struct and is
 * re-applied by the ISP task on every frame. No-op when the local camera
 * feature is disabled.
 */
esp_err_t runtime_settings_apply_image_calibration(const runtime_settings_t *settings);
/**
 * @brief Push the motion-detector tuning (zones, debounce, cooldown, filters)
 * into the camera component.  Safe to call before the camera is started; the
 * values persist in the component state.  No-op when local camera is disabled.
 */
esp_err_t runtime_settings_apply_motion_config(const runtime_settings_t *settings);
bool runtime_settings_has_wifi(const runtime_settings_t *settings);
bool runtime_settings_has_ha(const runtime_settings_t *settings);
bool runtime_settings_has_xiaozhi(const runtime_settings_t *settings);
