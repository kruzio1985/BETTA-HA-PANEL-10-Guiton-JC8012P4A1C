/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "api/api_routes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "bsp/display.h"
#include "diag/data_log.h"
#include "drivers/display_init.h"
#include "drivers/touch_init.h"
#include "ha/ha_client.h"
#include "net/wifi_mgr.h"
#include "settings/i18n_store.h"
#include "settings/runtime_settings.h"
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
#include "camera/local_camera.h"
#endif

static esp_timer_handle_t s_restart_timer = NULL;

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
/* Built-in camera motion-wake routes to the display's activity notifier so a
 * detected movement lights the screen back up, no matter which UI (WWW or
 * on-panel settings) started the camera. */
static void api_camera_motion_wake_cb(void *user_data)
{
    (void)user_data;
    display_note_activity();
}
#endif

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", (message != NULL) ? message : "Invalid request");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static bool has_ws_scheme(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return true;
    }
    return strncmp(url, "ws://", 5) == 0 || strncmp(url, "wss://", 6) == 0;
}

static bool is_valid_ipv4(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    int octets = 0;
    const char *p = text;
    while (*p != '\0') {
        int value = 0;
        int digits = 0;
        bool any_digit = false;
        while (*p >= '0' && *p <= '9') {
            any_digit = true;
            digits++;
            if (digits > 3) {
                return false;
            }
            value = value * 10 + (*p - '0');
            p++;
        }
        if (!any_digit || value > 255) {
            return false;
        }
        octets++;
        if (*p == '.') {
            p++;
            if (*p == '\0') {
                return false;
            }
            continue;
        }
        if (*p == '\0') {
            break;
        }
        return false;
    }
    return octets == 4;
}

static bool normalize_country_code(char *country_code, size_t country_code_len)
{
    if (country_code == NULL || country_code_len < APP_WIFI_COUNTRY_CODE_MAX_LEN) {
        return false;
    }
    if (country_code[0] == '\0') {
        strlcpy(country_code, APP_WIFI_COUNTRY_CODE, country_code_len);
        return true;
    }
    if (strlen(country_code) != 2) {
        return false;
    }
    if (!isalpha((unsigned char)country_code[0]) || !isalpha((unsigned char)country_code[1])) {
        return false;
    }

    country_code[0] = (char)toupper((unsigned char)country_code[0]);
    country_code[1] = (char)toupper((unsigned char)country_code[1]);
    country_code[2] = '\0';
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static bool normalize_bssid(char *bssid, size_t bssid_len)
{
    if (bssid == NULL || bssid_len < APP_WIFI_BSSID_MAX_LEN) {
        return false;
    }
    if (bssid[0] == '\0') {
        return true;
    }
    if (strlen(bssid) != 17U) {
        return false;
    }

    char sep = bssid[2];
    if (sep != ':' && sep != '-') {
        return false;
    }

    for (size_t i = 0; i < 6; i++) {
        size_t idx = i * 3;
        int hi = hex_nibble(bssid[idx]);
        int lo = hex_nibble(bssid[idx + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bssid[idx] = (char)toupper((unsigned char)bssid[idx]);
        bssid[idx + 1] = (char)toupper((unsigned char)bssid[idx + 1]);
        if (i < 5) {
            if (bssid[idx + 2] != sep) {
                return false;
            }
            bssid[idx + 2] = ':';
        }
    }
    bssid[17] = '\0';
    return true;
}

static bool normalize_ui_language(char *language, size_t language_len)
{
    if (language == NULL || language_len == 0) {
        return false;
    }
    if (language[0] == '\0') {
        strlcpy(language, APP_UI_DEFAULT_LANGUAGE, language_len);
        return true;
    }

    char normalized[APP_UI_LANGUAGE_MAX_LEN] = {0};
    if (!i18n_store_normalize_language_code(language, normalized, sizeof(normalized))) {
        return false;
    }
    strlcpy(language, normalized, language_len);
    return true;
}

static void restart_timer_cb(void *arg)
{
    (void)arg;
    /* Avoid random panel colors during software reset. */
    (void)bsp_display_backlight_off();
    esp_restart();
}

static void schedule_restart(void)
{
    if (s_restart_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &restart_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "settings_restart",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&timer_args, &s_restart_timer) != ESP_OK) {
            esp_restart();
            return;
        }
    }

    if (esp_timer_is_active(s_restart_timer)) {
        (void)esp_timer_stop(s_restart_timer);
    }
    if (esp_timer_start_once(s_restart_timer, 1500ULL * 1000ULL) != ESP_OK) {
        esp_restart();
    }
}

esp_err_t api_settings_get_handler(httpd_req_t *req)
{
    runtime_settings_t *settings = calloc(1, sizeof(runtime_settings_t));
    if (settings == NULL) {
        return httpd_resp_send_500(req);
    }

    esp_err_t settings_err = runtime_settings_load(settings);
    if (settings_err != ESP_OK) {
        runtime_settings_set_defaults(settings);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON *ha = cJSON_CreateObject();
    cJSON *time_cfg = cJSON_CreateObject();
    cJSON *ui = cJSON_CreateObject();
    cJSON *xiaozhi = cJSON_CreateObject();
    cJSON *sd = cJSON_CreateObject();
    cJSON *network = cJSON_CreateObject();
    cJSON *camera = cJSON_CreateObject();
    cJSON *camera_image = cJSON_CreateObject();
    cJSON *system = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *display = cJSON_CreateObject();
    cJSON *topbar = cJSON_CreateObject();
    cJSON *appearance = cJSON_CreateObject();
    if (root == NULL || wifi == NULL || ha == NULL || time_cfg == NULL || ui == NULL || xiaozhi == NULL ||
        sd == NULL || network == NULL || camera == NULL || camera_image == NULL || system == NULL ||
        audio == NULL || display == NULL || topbar == NULL || appearance == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(wifi);
        cJSON_Delete(ha);
        cJSON_Delete(time_cfg);
        cJSON_Delete(ui);
        cJSON_Delete(xiaozhi);
        cJSON_Delete(sd);
        cJSON_Delete(network);
        cJSON_Delete(camera);
        cJSON_Delete(camera_image);
        cJSON_Delete(system);
        cJSON_Delete(audio);
        cJSON_Delete(display);
        cJSON_Delete(topbar);
        free(settings);
        return httpd_resp_send_500(req);
    }

    cJSON_AddStringToObject(wifi, "ssid", settings->wifi_ssid);
    cJSON_AddStringToObject(wifi, "country_code", settings->wifi_country_code);
    cJSON_AddStringToObject(wifi, "bssid", settings->wifi_bssid);
    cJSON_AddBoolToObject(wifi, "password_set", settings->wifi_password[0] != '\0');
    cJSON_AddBoolToObject(wifi, "configured", runtime_settings_has_wifi(settings));
    cJSON_AddBoolToObject(wifi, "connected", wifi_mgr_is_connected());
    cJSON_AddBoolToObject(wifi, "setup_ap_active", wifi_mgr_is_setup_ap_active());
    cJSON_AddStringToObject(wifi, "setup_ap_ssid", wifi_mgr_get_setup_ap_ssid());
    wifi_mgr_sta_ap_info_t sta_ap = {0};
    if (wifi_mgr_get_sta_ap_info(&sta_ap) == ESP_OK) {
        char connected_bssid[APP_WIFI_BSSID_MAX_LEN] = {0};
        snprintf(
            connected_bssid,
            sizeof(connected_bssid),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            sta_ap.bssid[0],
            sta_ap.bssid[1],
            sta_ap.bssid[2],
            sta_ap.bssid[3],
            sta_ap.bssid[4],
            sta_ap.bssid[5]);
        cJSON_AddNumberToObject(wifi, "rssi_dbm", (double)sta_ap.rssi);
        cJSON_AddStringToObject(wifi, "connected_bssid", connected_bssid);
        cJSON_AddNumberToObject(wifi, "connected_channel", (double)sta_ap.channel);
    } else {
        cJSON_AddNullToObject(wifi, "rssi_dbm");
        cJSON_AddNullToObject(wifi, "connected_bssid");
        cJSON_AddNullToObject(wifi, "connected_channel");
    }
    cJSON_AddBoolToObject(wifi, "scan_supported", true);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON_AddStringToObject(ha, "ws_url", settings->ha_ws_url);
    cJSON_AddBoolToObject(ha, "access_token_set", settings->ha_access_token[0] != '\0');
    cJSON_AddBoolToObject(ha, "rest_enabled", settings->ha_rest_enabled);
    cJSON_AddBoolToObject(ha, "configured", runtime_settings_has_ha(settings));
    cJSON_AddBoolToObject(ha, "connected", ha_client_is_connected());
    cJSON_AddItemToObject(root, "ha", ha);

    cJSON_AddStringToObject(time_cfg, "ntp_server", settings->ntp_server);
    cJSON_AddStringToObject(time_cfg, "timezone", settings->time_tz);
    cJSON_AddItemToObject(root, "time", time_cfg);

    cJSON_AddStringToObject(ui, "language", settings->ui_language);
    cJSON_AddItemToObject(root, "ui", ui);

    cJSON_AddStringToObject(xiaozhi, "server", settings->xiaozhi_server);
    cJSON_AddStringToObject(xiaozhi, "device", settings->xiaozhi_device);
    cJSON_AddStringToObject(xiaozhi, "ota_url", settings->xiaozhi_ota_url);
    cJSON_AddBoolToObject(xiaozhi, "enabled", settings->xiaozhi_enabled);
    cJSON_AddBoolToObject(xiaozhi, "access_token_set", settings->xiaozhi_token[0] != '\0');
    cJSON_AddBoolToObject(xiaozhi, "configured", runtime_settings_has_xiaozhi(settings));
    cJSON_AddItemToObject(root, "xiaozhi", xiaozhi);

    cJSON_AddBoolToObject(sd, "logging_enabled", settings->sd_logging_enabled);
    cJSON_AddNumberToObject(sd, "flush_interval_s", (double)settings->sd_flush_interval_s);
    cJSON_AddBoolToObject(sd, "log_system_enabled", settings->sd_log_system_enabled);
    cJSON_AddBoolToObject(sd, "log_sensors_enabled", settings->sd_log_sensors_enabled);
    cJSON_AddBoolToObject(sd, "log_camera_enabled", settings->sd_log_camera_enabled);
    cJSON_AddItemToObject(root, "sd", sd);

    cJSON_AddBoolToObject(network, "static_ip_enabled", settings->wifi_static_ip_enabled);
    cJSON_AddStringToObject(network, "static_ip", settings->wifi_static_ip);
    cJSON_AddStringToObject(network, "static_netmask", settings->wifi_static_netmask);
    cJSON_AddStringToObject(network, "static_gateway", settings->wifi_static_gateway);
    cJSON_AddStringToObject(network, "static_dns", settings->wifi_static_dns);
    cJSON_AddItemToObject(root, "network", network);

    cJSON_AddBoolToObject(camera, "enabled", settings->camera_enabled);
    cJSON_AddBoolToObject(camera, "motion_wake", settings->camera_motion_wake);
    cJSON_AddNumberToObject(camera, "motion_threshold", (double)settings->camera_motion_threshold);
    cJSON_AddNumberToObject(camera, "jpeg_quality", (double)settings->camera_jpeg_quality);
    cJSON_AddBoolToObject(camera, "hflip", settings->camera_hflip);
    cJSON_AddBoolToObject(camera, "vflip", settings->camera_vflip);
    cJSON_AddBoolToObject(camera, "stream_enabled", settings->camera_stream_enabled);
    cJSON_AddNumberToObject(camera, "resolution", (double)settings->camera_resolution);

    cJSON *camera_motion = cJSON_CreateObject();
    cJSON *motion_zones = cJSON_CreateArray();
    if (camera_motion != NULL && motion_zones != NULL) {
        cJSON_AddNumberToObject(camera_motion, "min_area", (double)settings->camera_motion_min_area);
        cJSON_AddNumberToObject(camera_motion, "min_duration_ms", (double)settings->camera_motion_min_duration_ms);
        cJSON_AddNumberToObject(camera_motion, "cooldown_ms", (double)settings->camera_motion_cooldown_ms);
        cJSON_AddNumberToObject(camera_motion, "start_delay_ms", (double)settings->camera_motion_start_delay_ms);
        cJSON_AddBoolToObject(camera_motion, "ignore_lighting", settings->camera_motion_ignore_lighting);
        for (int i = 0; i < settings->camera_motion_zone_count && i < 4; i++) {
            cJSON *zone = cJSON_CreateObject();
            if (zone == NULL) {
                break;
            }
            cJSON_AddNumberToObject(zone, "x", (double)settings->camera_motion_zones[i].x);
            cJSON_AddNumberToObject(zone, "y", (double)settings->camera_motion_zones[i].y);
            cJSON_AddNumberToObject(zone, "w", (double)settings->camera_motion_zones[i].w);
            cJSON_AddNumberToObject(zone, "h", (double)settings->camera_motion_zones[i].h);
            cJSON_AddItemToArray(motion_zones, zone);
        }
        cJSON_AddItemToObject(camera_motion, "zones", motion_zones);
        cJSON_AddItemToObject(camera, "motion", camera_motion);
    } else {
        cJSON_Delete(camera_motion);
        cJSON_Delete(motion_zones);
    }

    cJSON_AddBoolToObject(camera_image, "manual", settings->camera_img_manual);
    cJSON_AddBoolToObject(camera_image, "wb_manual", settings->camera_img_wb_manual);
    cJSON_AddBoolToObject(camera_image, "sharpen_manual", settings->camera_img_sharpen_manual);
    cJSON_AddBoolToObject(camera_image, "denoise_manual", settings->camera_img_denoise_manual);
    cJSON_AddNumberToObject(camera_image, "brightness", (double)settings->camera_img_brightness);
    cJSON_AddNumberToObject(camera_image, "contrast", (double)settings->camera_img_contrast);
    cJSON_AddNumberToObject(camera_image, "saturation", (double)settings->camera_img_saturation);
    cJSON_AddNumberToObject(camera_image, "hue", (double)settings->camera_img_hue);
    cJSON_AddNumberToObject(camera_image, "wb_red", (double)settings->camera_img_wb_red);
    cJSON_AddNumberToObject(camera_image, "wb_blue", (double)settings->camera_img_wb_blue);
    cJSON_AddNumberToObject(camera_image, "sharpen", (double)settings->camera_img_sharpen);
    cJSON_AddNumberToObject(camera_image, "denoise", (double)settings->camera_img_denoise);
    cJSON_AddNumberToObject(camera_image, "tone_shadows", (double)settings->camera_img_tone_shadows);
    cJSON_AddNumberToObject(camera_image, "tone_highlights", (double)settings->camera_img_tone_highlights);
    cJSON_AddItemToObject(camera, "image", camera_image);

    cJSON_AddItemToObject(root, "camera", camera);

    cJSON_AddNumberToObject(system, "daily_restart_hour", (double)settings->daily_restart_hour);
    cJSON_AddBoolToObject(system, "touch_test", settings->touch_test);
    cJSON_AddItemToObject(root, "system", system);

    cJSON_AddNumberToObject(audio, "volume", (double)settings->audio_volume);
    cJSON_AddItemToObject(root, "audio", audio);

    display_power_config_t power = {0};
    display_get_power_config(&power);
    cJSON_AddNumberToObject(display, "active_brightness", (double)power.active_brightness_percent);
    cJSON_AddNumberToObject(display, "dim_brightness", (double)power.dim_brightness_percent);
    cJSON_AddNumberToObject(display, "dim_timeout_ms", (double)power.dim_timeout_ms);
    cJSON_AddNumberToObject(display, "off_timeout_ms", (double)power.off_timeout_ms);
    cJSON_AddBoolToObject(display, "night_mode_enabled", power.night_mode_enabled);
    cJSON_AddNumberToObject(display, "night_start_hour", (double)power.night_start_hour);
    cJSON_AddNumberToObject(display, "night_end_hour", (double)power.night_end_hour);
    cJSON_AddBoolToObject(display, "screensaver_enabled", power.screensaver_enabled);
    cJSON_AddNumberToObject(display, "screensaver_brightness", (double)power.screensaver_brightness_percent);
    cJSON_AddBoolToObject(display, "screensaver_clock_enabled", power.screensaver_clock_enabled);
    cJSON_AddStringToObject(display, "screensaver_wallpaper", power.screensaver_wallpaper);
    cJSON_AddItemToObject(root, "display", display);

    cJSON_AddBoolToObject(topbar, "show_clock", settings->topbar_show_clock);
    cJSON_AddBoolToObject(topbar, "show_room_name", settings->topbar_show_room_name);
    cJSON_AddStringToObject(topbar, "room_name", settings->topbar_room_name);
    cJSON_AddBoolToObject(topbar, "show_status", settings->topbar_show_status);
    cJSON_AddBoolToObject(topbar, "show_brightness", settings->topbar_show_brightness);
    cJSON_AddItemToObject(root, "topbar", topbar);

    cJSON_AddStringToObject(appearance, "page_wallpaper", settings->page_wallpaper);
    cJSON_AddNumberToObject(appearance, "tile_transparency", settings->tile_transparency);
    cJSON_AddItemToObject(root, "appearance", appearance);

    cJSON_AddBoolToObject(root, "ok", true);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(settings);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static bool update_string_setting(
    cJSON *obj,
    const char *key,
    char *dst,
    size_t dst_len,
    bool *out_invalid_type,
    bool *out_too_long)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == NULL) {
        return false;
    }

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        if (strlen(item->valuestring) >= dst_len) {
            if (out_too_long != NULL) {
                *out_too_long = true;
            }
            return false;
        }
        strlcpy(dst, item->valuestring, dst_len);
        return true;
    }
    if (cJSON_IsNull(item)) {
        dst[0] = '\0';
        return true;
    }

    if (out_invalid_type != NULL) {
        *out_invalid_type = true;
    }
    return false;
}

static bool update_bool_setting(cJSON *obj, const char *key, bool *dst, bool *out_invalid_type)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == NULL) {
        return false;
    }

    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
        return true;
    }

    if (out_invalid_type != NULL) {
        *out_invalid_type = true;
    }
    return false;
}

static bool update_int_setting(cJSON *obj, const char *key, int *dst, int min, int max, bool *out_invalid_type)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == NULL) {
        return false;
    }

    if (cJSON_IsNumber(item)) {
        int value = (int)item->valuedouble;
        if (value < min) {
            value = min;
        }
        if (value > max) {
            value = max;
        }
        *dst = value;
        return true;
    }

    if (out_invalid_type != NULL) {
        *out_invalid_type = true;
    }
    return false;
}

/* Screensaver wallpapers live under /sdcard/bg and are decoded by the
 * PNG-only loader, so accept only a plain basename ending in .png. Rejecting
 * path separators also blocks traversal outside the wallpaper directory. */
static bool screensaver_wallpaper_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return true; /* empty = default /sdcard/bg/screensaver.png */
    }
    const size_t n = strlen(name);
    if (n == 0 || n >= 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)name[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            continue;
        }
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    return strcasecmp(dot, ".png") == 0;
}

esp_err_t api_settings_put_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > APP_SETTINGS_MAX_JSON_LEN) {
        return send_json_error(req, "400 Bad Request", "Invalid payload size");
    }

    char *buf = calloc((size_t)req->content_len + 1U, sizeof(char));
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            return send_json_error(req, "400 Bad Request", "Failed to read request body");
        }
        received += r;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Invalid JSON");
    }

    runtime_settings_t *settings = calloc(1, sizeof(runtime_settings_t));
    if (settings == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_500(req);
    }

    esp_err_t load_err = runtime_settings_load(settings);
    if (load_err != ESP_OK) {
        runtime_settings_set_defaults(settings);
    }

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "ha");
    cJSON *time_cfg = cJSON_GetObjectItemCaseSensitive(root, "time");
    cJSON *ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
    cJSON *xiaozhi = cJSON_GetObjectItemCaseSensitive(root, "xiaozhi");
    cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "sd");
    cJSON *network = cJSON_GetObjectItemCaseSensitive(root, "network");
    cJSON *camera = cJSON_GetObjectItemCaseSensitive(root, "camera");
    cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
    cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    cJSON *topbar = cJSON_GetObjectItemCaseSensitive(root, "topbar");
    cJSON *appearance = cJSON_GetObjectItemCaseSensitive(root, "appearance");
    if (wifi != NULL && !cJSON_IsObject(wifi)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "wifi must be an object");
    }
    if (ha != NULL && !cJSON_IsObject(ha)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "ha must be an object");
    }
    if (time_cfg != NULL && !cJSON_IsObject(time_cfg)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "time must be an object");
    }
    if (ui != NULL && !cJSON_IsObject(ui)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "ui must be an object");
    }
    if (xiaozhi != NULL && !cJSON_IsObject(xiaozhi)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "xiaozhi must be an object");
    }
    if (sd != NULL && !cJSON_IsObject(sd)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "sd must be an object");
    }
    if (network != NULL && !cJSON_IsObject(network)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "network must be an object");
    }
    if (camera != NULL && !cJSON_IsObject(camera)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "camera must be an object");
    }
    if (system != NULL && !cJSON_IsObject(system)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "system must be an object");
    }
    if (audio != NULL && !cJSON_IsObject(audio)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "audio must be an object");
    }
    if (display != NULL && !cJSON_IsObject(display)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "display must be an object");
    }
    if (topbar != NULL && !cJSON_IsObject(topbar)) {
        cJSON_Delete(root);
        free(settings);
        return send_json_error(req, "400 Bad Request", "topbar must be an object");
    }

    bool invalid_type = false;
    bool too_long = false;
    if (cJSON_IsObject(wifi)) {
        (void)update_string_setting(
            wifi, "ssid", settings->wifi_ssid, sizeof(settings->wifi_ssid), &invalid_type, &too_long);
        (void)update_string_setting(
            wifi, "password", settings->wifi_password, sizeof(settings->wifi_password), &invalid_type, &too_long);
        (void)update_string_setting(
            wifi, "country_code", settings->wifi_country_code, sizeof(settings->wifi_country_code), &invalid_type, &too_long);
        (void)update_string_setting(
            wifi, "bssid", settings->wifi_bssid, sizeof(settings->wifi_bssid), &invalid_type, &too_long);
    }
    if (cJSON_IsObject(ha)) {
        (void)update_string_setting(
            ha, "ws_url", settings->ha_ws_url, sizeof(settings->ha_ws_url), &invalid_type, &too_long);
        (void)update_string_setting(
            ha, "access_token", settings->ha_access_token, sizeof(settings->ha_access_token), &invalid_type, &too_long);
        (void)update_bool_setting(ha, "rest_enabled", &settings->ha_rest_enabled, &invalid_type);
    }
    if (cJSON_IsObject(time_cfg)) {
        (void)update_string_setting(
            time_cfg, "ntp_server", settings->ntp_server, sizeof(settings->ntp_server), &invalid_type, &too_long);
        (void)update_string_setting(
            time_cfg, "timezone", settings->time_tz, sizeof(settings->time_tz), &invalid_type, &too_long);
    }
    if (cJSON_IsObject(ui)) {
        (void)update_string_setting(
            ui, "language", settings->ui_language, sizeof(settings->ui_language), &invalid_type, &too_long);
    }
    if (cJSON_IsObject(xiaozhi)) {
        (void)update_string_setting(
            xiaozhi, "server", settings->xiaozhi_server, sizeof(settings->xiaozhi_server), &invalid_type, &too_long);
        (void)update_string_setting(
            xiaozhi, "device", settings->xiaozhi_device, sizeof(settings->xiaozhi_device), &invalid_type, &too_long);
        (void)update_string_setting(
            xiaozhi, "ota_url", settings->xiaozhi_ota_url, sizeof(settings->xiaozhi_ota_url), &invalid_type, &too_long);
        (void)update_string_setting(
            xiaozhi, "access_token", settings->xiaozhi_token, sizeof(settings->xiaozhi_token), &invalid_type, &too_long);
        (void)update_bool_setting(xiaozhi, "enabled", &settings->xiaozhi_enabled, &invalid_type);
    }
    if (cJSON_IsObject(sd)) {
        (void)update_bool_setting(sd, "logging_enabled", &settings->sd_logging_enabled, &invalid_type);
        (void)update_int_setting(sd, "flush_interval_s", &settings->sd_flush_interval_s, 5, 300, &invalid_type);
        (void)update_bool_setting(sd, "log_system_enabled", &settings->sd_log_system_enabled, &invalid_type);
        (void)update_bool_setting(sd, "log_sensors_enabled", &settings->sd_log_sensors_enabled, &invalid_type);
        (void)update_bool_setting(sd, "log_camera_enabled", &settings->sd_log_camera_enabled, &invalid_type);
    }
    if (cJSON_IsObject(network)) {
        (void)update_bool_setting(network, "static_ip_enabled", &settings->wifi_static_ip_enabled, &invalid_type);
        (void)update_string_setting(network, "static_ip", settings->wifi_static_ip, sizeof(settings->wifi_static_ip), &invalid_type, &too_long);
        (void)update_string_setting(network, "static_netmask", settings->wifi_static_netmask, sizeof(settings->wifi_static_netmask), &invalid_type, &too_long);
        (void)update_string_setting(network, "static_gateway", settings->wifi_static_gateway, sizeof(settings->wifi_static_gateway), &invalid_type, &too_long);
        (void)update_string_setting(network, "static_dns", settings->wifi_static_dns, sizeof(settings->wifi_static_dns), &invalid_type, &too_long);
    }
    if (cJSON_IsObject(camera)) {
        (void)update_bool_setting(camera, "enabled", &settings->camera_enabled, &invalid_type);
        (void)update_bool_setting(camera, "motion_wake", &settings->camera_motion_wake, &invalid_type);
        (void)update_int_setting(camera, "motion_threshold", &settings->camera_motion_threshold, 1, 64, &invalid_type);
        (void)update_int_setting(camera, "jpeg_quality", &settings->camera_jpeg_quality, 10, 95, &invalid_type);
        (void)update_bool_setting(camera, "hflip", &settings->camera_hflip, &invalid_type);
        (void)update_bool_setting(camera, "vflip", &settings->camera_vflip, &invalid_type);
        (void)update_bool_setting(camera, "stream_enabled", &settings->camera_stream_enabled, &invalid_type);
        (void)update_int_setting(camera, "resolution", &settings->camera_resolution, 0, 1, &invalid_type);

        cJSON *motion = cJSON_GetObjectItemCaseSensitive(camera, "motion");
        if (cJSON_IsObject(motion)) {
            (void)update_int_setting(motion, "min_area", &settings->camera_motion_min_area, 0, 100, &invalid_type);
            (void)update_int_setting(motion, "min_duration_ms", &settings->camera_motion_min_duration_ms, 0, 1000, &invalid_type);
            (void)update_int_setting(motion, "cooldown_ms", &settings->camera_motion_cooldown_ms, 0, 30000, &invalid_type);
            (void)update_int_setting(motion, "start_delay_ms", &settings->camera_motion_start_delay_ms, 0, 10000, &invalid_type);
            (void)update_bool_setting(motion, "ignore_lighting", &settings->camera_motion_ignore_lighting, &invalid_type);

            cJSON *zones = cJSON_GetObjectItemCaseSensitive(motion, "zones");
            if (cJSON_IsArray(zones)) {
                int count = 0;
                const int total = cJSON_GetArraySize(zones);
                for (int i = 0; i < total && count < 4; i++) {
                    cJSON *zone = cJSON_GetArrayItem(zones, i);
                    if (!cJSON_IsObject(zone)) {
                        continue;
                    }
                    int x = 0, y = 0, w = 0, h = 0;
                    (void)update_int_setting(zone, "x", &x, 0, 100, &invalid_type);
                    (void)update_int_setting(zone, "y", &y, 0, 100, &invalid_type);
                    (void)update_int_setting(zone, "w", &w, 0, 100, &invalid_type);
                    (void)update_int_setting(zone, "h", &h, 0, 100, &invalid_type);
                    if (w <= 0 || h <= 0) {
                        continue;
                    }
                    settings->camera_motion_zones[count].x = x;
                    settings->camera_motion_zones[count].y = y;
                    settings->camera_motion_zones[count].w = w;
                    settings->camera_motion_zones[count].h = h;
                    count++;
                }
                settings->camera_motion_zone_count = count;
            }
        }

        cJSON *image = cJSON_GetObjectItemCaseSensitive(camera, "image");
        if (cJSON_IsObject(image)) {
            (void)update_bool_setting(image, "manual", &settings->camera_img_manual, &invalid_type);
            (void)update_bool_setting(image, "wb_manual", &settings->camera_img_wb_manual, &invalid_type);
            (void)update_bool_setting(image, "sharpen_manual", &settings->camera_img_sharpen_manual, &invalid_type);
            (void)update_bool_setting(image, "denoise_manual", &settings->camera_img_denoise_manual, &invalid_type);
            (void)update_int_setting(image, "brightness", &settings->camera_img_brightness, -128, 127, &invalid_type);
            (void)update_int_setting(image, "contrast", &settings->camera_img_contrast, 0, 255, &invalid_type);
            (void)update_int_setting(image, "saturation", &settings->camera_img_saturation, 0, 255, &invalid_type);
            (void)update_int_setting(image, "hue", &settings->camera_img_hue, 0, 360, &invalid_type);
            (void)update_int_setting(image, "wb_red", &settings->camera_img_wb_red, 50, 200, &invalid_type);
            (void)update_int_setting(image, "wb_blue", &settings->camera_img_wb_blue, 50, 200, &invalid_type);
            (void)update_int_setting(image, "sharpen", &settings->camera_img_sharpen, 25, 300, &invalid_type);
            (void)update_int_setting(image, "denoise", &settings->camera_img_denoise, 25, 200, &invalid_type);
            (void)update_int_setting(image, "tone_shadows", &settings->camera_img_tone_shadows, -100, 100, &invalid_type);
            (void)update_int_setting(image, "tone_highlights", &settings->camera_img_tone_highlights, -100, 100, &invalid_type);
        }
    }
    if (cJSON_IsObject(system)) {
        (void)update_int_setting(system, "daily_restart_hour", &settings->daily_restart_hour, -1, 23, &invalid_type);
        (void)update_bool_setting(system, "touch_test", &settings->touch_test, &invalid_type);
    }
    if (cJSON_IsObject(audio)) {
        (void)update_int_setting(audio, "volume", &settings->audio_volume, 0, 100, &invalid_type);
    }
    if (cJSON_IsObject(topbar)) {
        (void)update_bool_setting(topbar, "show_clock", &settings->topbar_show_clock, &invalid_type);
        (void)update_bool_setting(topbar, "show_room_name", &settings->topbar_show_room_name, &invalid_type);
        (void)update_string_setting(
            topbar, "room_name", settings->topbar_room_name, sizeof(settings->topbar_room_name), &invalid_type, &too_long);
        (void)update_bool_setting(topbar, "show_status", &settings->topbar_show_status, &invalid_type);
        (void)update_bool_setting(topbar, "show_brightness", &settings->topbar_show_brightness, &invalid_type);
    }
    if (cJSON_IsObject(appearance)) {
        (void)update_string_setting(
            appearance, "page_wallpaper", settings->page_wallpaper, sizeof(settings->page_wallpaper), &invalid_type, &too_long);
        (void)update_int_setting(appearance, "tile_transparency", &settings->tile_transparency, 0, 100, &invalid_type);
    }
    if (cJSON_IsObject(display)) {
        /* The display power policy lives in its own NVS (display driver), so it
         * is applied immediately here and does not need the settings struct. */
        display_power_config_t power = {0};
        display_get_power_config(&power);
        int value = 0;
        if (update_int_setting(display, "active_brightness", &value, 0, 100, &invalid_type)) {
            power.active_brightness_percent = value;
        }
        value = 0;
        if (update_int_setting(display, "dim_brightness", &value, 0, 100, &invalid_type)) {
            power.dim_brightness_percent = value;
        }
        value = 0;
        if (update_int_setting(display, "dim_timeout_ms", &value, 0, 86400000, &invalid_type)) {
            power.dim_timeout_ms = (uint32_t)value;
        }
        value = 0;
        if (update_int_setting(display, "off_timeout_ms", &value, 0, 86400000, &invalid_type)) {
            power.off_timeout_ms = (uint32_t)value;
        }
        (void)update_bool_setting(display, "night_mode_enabled", &power.night_mode_enabled, &invalid_type);
        value = 0;
        if (update_int_setting(display, "night_start_hour", &value, 0, 23, &invalid_type)) {
            power.night_start_hour = value;
        }
        value = 0;
        if (update_int_setting(display, "night_end_hour", &value, 0, 23, &invalid_type)) {
            power.night_end_hour = value;
        }
        (void)update_bool_setting(display, "screensaver_enabled", &power.screensaver_enabled, &invalid_type);
        value = 0;
        if (update_int_setting(display, "screensaver_brightness", &value, 0, 100, &invalid_type)) {
            power.screensaver_brightness_percent = value;
        }
        (void)update_bool_setting(display, "screensaver_clock_enabled", &power.screensaver_clock_enabled, &invalid_type);
        char wallpaper[64] = {0};
        if (update_string_setting(display, "screensaver_wallpaper", wallpaper, sizeof(wallpaper), &invalid_type, &too_long)) {
            if (screensaver_wallpaper_name_valid(wallpaper)) {
                strlcpy(power.screensaver_wallpaper, wallpaper, sizeof(power.screensaver_wallpaper));
            } else {
                invalid_type = true;
            }
        }
        display_set_power_config(&power);
    }

    (void)update_string_setting(
        root, "wifi_ssid", settings->wifi_ssid, sizeof(settings->wifi_ssid), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "wifi_password", settings->wifi_password, sizeof(settings->wifi_password), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "wifi_country_code", settings->wifi_country_code, sizeof(settings->wifi_country_code), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "wifi_bssid", settings->wifi_bssid, sizeof(settings->wifi_bssid), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "ha_ws_url", settings->ha_ws_url, sizeof(settings->ha_ws_url), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "ha_access_token", settings->ha_access_token, sizeof(settings->ha_access_token), &invalid_type, &too_long);
    (void)update_bool_setting(root, "ha_rest_enabled", &settings->ha_rest_enabled, &invalid_type);
    (void)update_string_setting(
        root, "ntp_server", settings->ntp_server, sizeof(settings->ntp_server), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "time_tz", settings->time_tz, sizeof(settings->time_tz), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "language", settings->ui_language, sizeof(settings->ui_language), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "xiaozhi_server", settings->xiaozhi_server, sizeof(settings->xiaozhi_server), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "xiaozhi_device", settings->xiaozhi_device, sizeof(settings->xiaozhi_device), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "xiaozhi_ota_url", settings->xiaozhi_ota_url, sizeof(settings->xiaozhi_ota_url), &invalid_type, &too_long);
    (void)update_string_setting(
        root, "xiaozhi_access_token", settings->xiaozhi_token, sizeof(settings->xiaozhi_token), &invalid_type, &too_long);
    (void)update_bool_setting(root, "xiaozhi_enabled", &settings->xiaozhi_enabled, &invalid_type);

    bool reboot = true;
    cJSON *reboot_item = cJSON_GetObjectItemCaseSensitive(root, "reboot");
    if (reboot_item != NULL) {
        if (cJSON_IsBool(reboot_item)) {
            reboot = cJSON_IsTrue(reboot_item);
        } else {
            cJSON_Delete(root);
            free(settings);
            return send_json_error(req, "400 Bad Request", "reboot must be boolean");
        }
    }

    cJSON_Delete(root);

    if (invalid_type) {
        free(settings);
        return send_json_error(req, "400 Bad Request", "One or more settings fields have invalid type");
    }
    if (too_long) {
        free(settings);
        return send_json_error(
            req,
            "400 Bad Request",
            "One or more settings values are too long (ssid<=32, wifi_password<=64, country_code<=2, bssid<=17, ws_url<=255, token<=511, ntp<=127, timezone<=127, language<=15, room_name<=31)");
    }
    if (!has_ws_scheme(settings->ha_ws_url)) {
        free(settings);
        return send_json_error(req, "400 Bad Request", "ha.ws_url must start with ws:// or wss://");
    }
    if (!normalize_country_code(settings->wifi_country_code, sizeof(settings->wifi_country_code))) {
        free(settings);
        return send_json_error(req, "400 Bad Request", "wifi.country_code must be a 2-letter ISO code (e.g. US, DE)");
    }
    if (!normalize_bssid(settings->wifi_bssid, sizeof(settings->wifi_bssid))) {
        free(settings);
        return send_json_error(req, "400 Bad Request", "wifi.bssid must be empty or MAC format AA:BB:CC:DD:EE:FF");
    }
    if (settings->wifi_ssid[0] == '\0') {
        settings->wifi_password[0] = '\0';
        settings->wifi_bssid[0] = '\0';
    }
    if (settings->ntp_server[0] == '\0') {
        strlcpy(settings->ntp_server, APP_NTP_SERVER, sizeof(settings->ntp_server));
    }
    if (settings->time_tz[0] == '\0') {
        strlcpy(settings->time_tz, APP_TIME_TZ, sizeof(settings->time_tz));
    }
    if (!normalize_ui_language(settings->ui_language, sizeof(settings->ui_language))) {
        free(settings);
        return send_json_error(req, "400 Bad Request", "ui.language must use [a-z0-9_-] and be 2-15 chars");
    }
    if (settings->wifi_static_ip_enabled) {
        if (!is_valid_ipv4(settings->wifi_static_ip) ||
            !is_valid_ipv4(settings->wifi_static_netmask) ||
            !is_valid_ipv4(settings->wifi_static_gateway) ||
            !is_valid_ipv4(settings->wifi_static_dns)) {
            free(settings);
            return send_json_error(
                req,
                "400 Bad Request",
                "network.static_ip_* must all be valid IPv4 addresses when static_ip_enabled is true");
        }
    }

    esp_err_t save_err = runtime_settings_save(settings);
    bool sd_logging_enabled = settings->sd_logging_enabled;
    int sd_flush_interval_s = settings->sd_flush_interval_s;
    bool sd_log_system_enabled = settings->sd_log_system_enabled;
    bool sd_log_sensors_enabled = settings->sd_log_sensors_enabled;
    bool sd_log_camera_enabled = settings->sd_log_camera_enabled;
    bool touch_test = settings->touch_test;
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    bool camera_stream_enabled = settings->camera_stream_enabled;
    bool camera_enabled = settings->camera_enabled;
    bool camera_motion_wake = settings->camera_motion_wake;
    uint8_t camera_motion_threshold = (uint8_t)settings->camera_motion_threshold;
    uint8_t camera_jpeg_quality = (uint8_t)settings->camera_jpeg_quality;
    bool camera_hflip = settings->camera_hflip;
    bool camera_vflip = settings->camera_vflip;
    int camera_resolution = settings->camera_resolution;
#endif
    if (save_err == ESP_OK) {
        /* The manual ISP calibration lives in the camera component, not in the
         * settings struct, so it has to be handed over before the copy is freed. */
        (void)runtime_settings_apply_image_calibration(settings);
        /* Motion tuning (zones/debounce/cooldown) likewise lives in the camera
         * component; it is safe to apply before the pipeline is (re)started. */
        (void)runtime_settings_apply_motion_config(settings);
    }
    free(settings);
    if (save_err != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    data_log_set_enabled(sd_logging_enabled);
    data_log_set_flush_interval_s(sd_flush_interval_s);
    data_log_set_log_system_enabled(sd_log_system_enabled);
    data_log_set_log_sensors_enabled(sd_log_sensors_enabled);
    data_log_set_log_camera_enabled(sd_log_camera_enabled);
    touch_debug_set_enabled(touch_test);
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    api_camera_local_set_stream_enabled(camera_stream_enabled);
    /* Keep the motion-wake callback registered regardless of who starts the
     * pipeline, then apply the whole camera configuration atomically. */
    (void)local_camera_register_motion_cb(api_camera_motion_wake_cb, NULL);
    (void)local_camera_apply_settings(camera_enabled, camera_motion_wake, camera_motion_threshold,
                                      camera_jpeg_quality, camera_hflip, camera_vflip,
                                      camera_resolution);
#endif

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "rebooting", reboot);
    char *payload = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t send_err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);

    if (send_err == ESP_OK && reboot) {
        schedule_restart();
    }
    return send_err;
}
