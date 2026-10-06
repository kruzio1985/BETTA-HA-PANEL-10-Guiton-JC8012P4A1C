/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "settings/runtime_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
#include "camera/local_camera.h"
#include "esp_video_isp_manual.h"
#endif

#include "settings/i18n_store.h"
#include "util/log_tags.h"

#define SETTINGS_NVS_NAMESPACE "runtime_sec"
#define SETTINGS_NVS_KEY_WIFI_PASSWORD "wifi_pwd"
#define SETTINGS_NVS_KEY_HA_ACCESS_TOKEN "ha_token"
#define SETTINGS_NVS_KEY_XIAOZHI_TOKEN "xz_token"

static bool is_placeholder(const char *text)
{
    return text == NULL || text[0] == '\0' || strstr(text, "YOUR_") != NULL;
}

static void normalize_ui_language(char *language, size_t language_len)
{
    if (language == NULL || language_len == 0) {
        return;
    }
    if (language[0] == '\0') {
        strlcpy(language, APP_UI_DEFAULT_LANGUAGE, language_len);
        return;
    }

    char normalized[APP_UI_LANGUAGE_MAX_LEN] = {0};
    if (!i18n_store_normalize_language_code(language, normalized, sizeof(normalized))) {
        strlcpy(language, APP_UI_DEFAULT_LANGUAGE, language_len);
        return;
    }
    strlcpy(language, normalized, language_len);
}

static void json_copy_string(cJSON *obj, const char *key, char *dst, size_t dst_len)
{
    if (obj == NULL || key == NULL || dst == NULL || dst_len == 0) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

static void json_copy_int(cJSON *obj, const char *key, int *dst, int min, int max)
{
    if (obj == NULL || key == NULL || dst == NULL) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        int value = (int)item->valuedouble;
        if (value < min) {
            value = min;
        }
        if (value > max) {
            value = max;
        }
        *dst = value;
    }
}

static void json_copy_bool(cJSON *obj, const char *key, bool *dst)
{
    if (obj == NULL || key == NULL || dst == NULL) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static esp_err_t load_file_text(const char *path, size_t max_len, char **out_text)
{
    if (path == NULL || out_text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_len) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    char *buf = calloc((size_t)size + 1U, sizeof(char));
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(buf, 1U, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }

    *out_text = buf;
    return ESP_OK;
}

static esp_err_t write_public_settings_file(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
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
    cJSON *topbar = cJSON_CreateObject();
    cJSON *appearance = cJSON_CreateObject();
    if (root == NULL || wifi == NULL || ha == NULL || time_cfg == NULL || ui == NULL || xiaozhi == NULL ||
        sd == NULL || network == NULL || camera == NULL || camera_image == NULL || system == NULL || audio == NULL ||
        topbar == NULL || appearance == NULL) {
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
        cJSON_Delete(topbar);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "version", 1);

    cJSON_AddStringToObject(wifi, "ssid", settings->wifi_ssid);
    cJSON_AddStringToObject(wifi, "country_code", settings->wifi_country_code);
    cJSON_AddStringToObject(wifi, "bssid", settings->wifi_bssid);
    cJSON_AddItemToObject(root, "wifi", wifi);

    cJSON_AddStringToObject(ha, "ws_url", settings->ha_ws_url);
    cJSON_AddBoolToObject(ha, "rest_enabled", settings->ha_rest_enabled);
    cJSON_AddItemToObject(root, "ha", ha);

    cJSON_AddStringToObject(time_cfg, "ntp_server", settings->ntp_server);
    cJSON_AddStringToObject(time_cfg, "timezone", settings->time_tz);
    cJSON_AddItemToObject(root, "time", time_cfg);

    cJSON_AddStringToObject(ui, "language", settings->ui_language);
    cJSON_AddItemToObject(root, "ui", ui);

    /* The Xiaozhi token is a secret and is persisted in NVS, never here. */
    cJSON_AddStringToObject(xiaozhi, "server", settings->xiaozhi_server);
    cJSON_AddStringToObject(xiaozhi, "device", settings->xiaozhi_device);
    cJSON_AddStringToObject(xiaozhi, "ota_url", settings->xiaozhi_ota_url);
    cJSON_AddBoolToObject(xiaozhi, "enabled", settings->xiaozhi_enabled);
    cJSON_AddItemToObject(root, "xiaozhi", xiaozhi);

    cJSON_AddBoolToObject(sd, "logging_enabled", settings->sd_logging_enabled);
    cJSON_AddNumberToObject(sd, "flush_interval_s", settings->sd_flush_interval_s);
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
    cJSON_AddNumberToObject(camera, "motion_threshold", settings->camera_motion_threshold);
    cJSON_AddNumberToObject(camera, "jpeg_quality", settings->camera_jpeg_quality);
    cJSON_AddBoolToObject(camera, "hflip", settings->camera_hflip);
    cJSON_AddBoolToObject(camera, "vflip", settings->camera_vflip);
    cJSON_AddBoolToObject(camera, "stream_enabled", settings->camera_stream_enabled);
    cJSON_AddNumberToObject(camera, "resolution", settings->camera_resolution);

    cJSON *camera_motion = cJSON_CreateObject();
    cJSON *motion_zones = cJSON_CreateArray();
    if (camera_motion != NULL && motion_zones != NULL) {
        cJSON_AddNumberToObject(camera_motion, "min_area", settings->camera_motion_min_area);
        cJSON_AddNumberToObject(camera_motion, "min_duration_ms", settings->camera_motion_min_duration_ms);
        cJSON_AddNumberToObject(camera_motion, "cooldown_ms", settings->camera_motion_cooldown_ms);
        cJSON_AddNumberToObject(camera_motion, "start_delay_ms", settings->camera_motion_start_delay_ms);
        cJSON_AddBoolToObject(camera_motion, "ignore_lighting", settings->camera_motion_ignore_lighting);
        for (int i = 0; i < settings->camera_motion_zone_count && i < 4; i++) {
            cJSON *zone = cJSON_CreateObject();
            if (zone == NULL) {
                break;
            }
            cJSON_AddNumberToObject(zone, "x", settings->camera_motion_zones[i].x);
            cJSON_AddNumberToObject(zone, "y", settings->camera_motion_zones[i].y);
            cJSON_AddNumberToObject(zone, "w", settings->camera_motion_zones[i].w);
            cJSON_AddNumberToObject(zone, "h", settings->camera_motion_zones[i].h);
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
    cJSON_AddNumberToObject(camera_image, "brightness", settings->camera_img_brightness);
    cJSON_AddNumberToObject(camera_image, "contrast", settings->camera_img_contrast);
    cJSON_AddNumberToObject(camera_image, "saturation", settings->camera_img_saturation);
    cJSON_AddNumberToObject(camera_image, "hue", settings->camera_img_hue);
    cJSON_AddNumberToObject(camera_image, "wb_red", settings->camera_img_wb_red);
    cJSON_AddNumberToObject(camera_image, "wb_blue", settings->camera_img_wb_blue);
    cJSON_AddNumberToObject(camera_image, "sharpen", settings->camera_img_sharpen);
    cJSON_AddNumberToObject(camera_image, "denoise", settings->camera_img_denoise);
    cJSON_AddNumberToObject(camera_image, "tone_shadows", settings->camera_img_tone_shadows);
    cJSON_AddNumberToObject(camera_image, "tone_highlights", settings->camera_img_tone_highlights);
    cJSON_AddItemToObject(camera, "image", camera_image);

    cJSON_AddItemToObject(root, "camera", camera);

    cJSON_AddNumberToObject(system, "daily_restart_hour", settings->daily_restart_hour);
    cJSON_AddBoolToObject(system, "touch_test", settings->touch_test);
    cJSON_AddItemToObject(root, "system", system);

    cJSON_AddNumberToObject(audio, "volume", settings->audio_volume);
    cJSON_AddItemToObject(root, "audio", audio);

    cJSON_AddBoolToObject(topbar, "show_clock", settings->topbar_show_clock);
    cJSON_AddBoolToObject(topbar, "show_room_name", settings->topbar_show_room_name);
    cJSON_AddStringToObject(topbar, "room_name", settings->topbar_room_name);
    cJSON_AddBoolToObject(topbar, "show_status", settings->topbar_show_status);
    cJSON_AddBoolToObject(topbar, "show_brightness", settings->topbar_show_brightness);
    cJSON_AddItemToObject(root, "topbar", topbar);

    cJSON_AddStringToObject(appearance, "page_wallpaper", settings->page_wallpaper);
    cJSON_AddNumberToObject(appearance, "tile_transparency", settings->tile_transparency);
    cJSON_AddItemToObject(root, "appearance", appearance);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(APP_SETTINGS_PATH, "wb");
    if (f == NULL) {
        cJSON_free(payload);
        return ESP_FAIL;
    }

    size_t len = strlen(payload);
    size_t written = fwrite(payload, 1U, len, f);
    fclose(f);
    cJSON_free(payload);

    if (written != len) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t parse_settings_json(
    const char *json,
    runtime_settings_t *out,
    bool *out_legacy_wifi_password,
    bool *out_legacy_ha_access_token)
{
    if (json == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_legacy_wifi_password != NULL) {
        *out_legacy_wifi_password = false;
    }
    if (out_legacy_ha_access_token != NULL) {
        *out_legacy_ha_access_token = false;
    }

    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
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
    cJSON *topbar = cJSON_GetObjectItemCaseSensitive(root, "topbar");
    cJSON *appearance = cJSON_GetObjectItemCaseSensitive(root, "appearance");

    if (cJSON_IsObject(appearance)) {
        json_copy_string(appearance, "page_wallpaper", out->page_wallpaper, sizeof(out->page_wallpaper));
        json_copy_int(appearance, "tile_transparency", &out->tile_transparency, 0, 100);
    }

    if (cJSON_IsObject(wifi)) {
        json_copy_string(wifi, "ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
        json_copy_string(wifi, "country_code", out->wifi_country_code, sizeof(out->wifi_country_code));
        json_copy_string(wifi, "bssid", out->wifi_bssid, sizeof(out->wifi_bssid));
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(wifi, "password");
        if (pwd != NULL) {
            if (out_legacy_wifi_password != NULL) {
                *out_legacy_wifi_password = true;
            }
            if (cJSON_IsString(pwd) && pwd->valuestring != NULL) {
                strlcpy(out->wifi_password, pwd->valuestring, sizeof(out->wifi_password));
            }
        }
    } else {
        json_copy_string(root, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
        json_copy_string(root, "wifi_country_code", out->wifi_country_code, sizeof(out->wifi_country_code));
        json_copy_string(root, "wifi_bssid", out->wifi_bssid, sizeof(out->wifi_bssid));
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(root, "wifi_password");
        if (pwd != NULL) {
            if (out_legacy_wifi_password != NULL) {
                *out_legacy_wifi_password = true;
            }
            if (cJSON_IsString(pwd) && pwd->valuestring != NULL) {
                strlcpy(out->wifi_password, pwd->valuestring, sizeof(out->wifi_password));
            }
        }
    }

    if (cJSON_IsObject(ha)) {
        json_copy_string(ha, "ws_url", out->ha_ws_url, sizeof(out->ha_ws_url));
        cJSON *rest_enabled = cJSON_GetObjectItemCaseSensitive(ha, "rest_enabled");
        if (cJSON_IsBool(rest_enabled)) {
            out->ha_rest_enabled = cJSON_IsTrue(rest_enabled);
        }
        cJSON *token = cJSON_GetObjectItemCaseSensitive(ha, "access_token");
        if (token != NULL) {
            if (out_legacy_ha_access_token != NULL) {
                *out_legacy_ha_access_token = true;
            }
            if (cJSON_IsString(token) && token->valuestring != NULL) {
                strlcpy(out->ha_access_token, token->valuestring, sizeof(out->ha_access_token));
            }
        }
    } else {
        json_copy_string(root, "ha_ws_url", out->ha_ws_url, sizeof(out->ha_ws_url));
        cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "ha_access_token");
        if (token != NULL) {
            if (out_legacy_ha_access_token != NULL) {
                *out_legacy_ha_access_token = true;
            }
            if (cJSON_IsString(token) && token->valuestring != NULL) {
                strlcpy(out->ha_access_token, token->valuestring, sizeof(out->ha_access_token));
            }
        }
    }
    cJSON *rest_enabled = cJSON_GetObjectItemCaseSensitive(root, "ha_rest_enabled");
    if (cJSON_IsBool(rest_enabled)) {
        out->ha_rest_enabled = cJSON_IsTrue(rest_enabled);
    }

    if (cJSON_IsObject(time_cfg)) {
        json_copy_string(time_cfg, "ntp_server", out->ntp_server, sizeof(out->ntp_server));
        json_copy_string(time_cfg, "timezone", out->time_tz, sizeof(out->time_tz));
    } else {
        json_copy_string(root, "ntp_server", out->ntp_server, sizeof(out->ntp_server));
        json_copy_string(root, "time_tz", out->time_tz, sizeof(out->time_tz));
    }

    if (cJSON_IsObject(ui)) {
        json_copy_string(ui, "language", out->ui_language, sizeof(out->ui_language));
    } else {
        json_copy_string(root, "language", out->ui_language, sizeof(out->ui_language));
    }

    if (cJSON_IsObject(xiaozhi)) {
        json_copy_string(xiaozhi, "server", out->xiaozhi_server, sizeof(out->xiaozhi_server));
        json_copy_string(xiaozhi, "device", out->xiaozhi_device, sizeof(out->xiaozhi_device));
        json_copy_string(xiaozhi, "ota_url", out->xiaozhi_ota_url, sizeof(out->xiaozhi_ota_url));
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(xiaozhi, "enabled");
        if (cJSON_IsBool(enabled)) {
            out->xiaozhi_enabled = cJSON_IsTrue(enabled);
        }
    } else {
        json_copy_string(root, "xiaozhi_server", out->xiaozhi_server, sizeof(out->xiaozhi_server));
        json_copy_string(root, "xiaozhi_device", out->xiaozhi_device, sizeof(out->xiaozhi_device));
        json_copy_string(root, "xiaozhi_ota_url", out->xiaozhi_ota_url, sizeof(out->xiaozhi_ota_url));
    }

    if (cJSON_IsObject(sd)) {
        cJSON *logging = cJSON_GetObjectItemCaseSensitive(sd, "logging_enabled");
        if (cJSON_IsBool(logging)) {
            out->sd_logging_enabled = cJSON_IsTrue(logging);
        }
        json_copy_int(sd, "flush_interval_s", &out->sd_flush_interval_s, 5, 300);
        json_copy_bool(sd, "log_system_enabled", &out->sd_log_system_enabled);
        json_copy_bool(sd, "log_sensors_enabled", &out->sd_log_sensors_enabled);
        json_copy_bool(sd, "log_camera_enabled", &out->sd_log_camera_enabled);
    }

    if (cJSON_IsObject(network)) {
        json_copy_bool(network, "static_ip_enabled", &out->wifi_static_ip_enabled);
        json_copy_string(network, "static_ip", out->wifi_static_ip, sizeof(out->wifi_static_ip));
        json_copy_string(network, "static_netmask", out->wifi_static_netmask, sizeof(out->wifi_static_netmask));
        json_copy_string(network, "static_gateway", out->wifi_static_gateway, sizeof(out->wifi_static_gateway));
        json_copy_string(network, "static_dns", out->wifi_static_dns, sizeof(out->wifi_static_dns));
    }

    if (cJSON_IsObject(camera)) {
        json_copy_bool(camera, "enabled", &out->camera_enabled);
        json_copy_bool(camera, "motion_wake", &out->camera_motion_wake);
        json_copy_int(camera, "motion_threshold", &out->camera_motion_threshold, 1, 64);
        json_copy_int(camera, "jpeg_quality", &out->camera_jpeg_quality, 10, 95);
        json_copy_bool(camera, "hflip", &out->camera_hflip);
        json_copy_bool(camera, "vflip", &out->camera_vflip);
        json_copy_bool(camera, "stream_enabled", &out->camera_stream_enabled);
        json_copy_int(camera, "resolution", &out->camera_resolution, 0, 1);

        cJSON *motion = cJSON_GetObjectItemCaseSensitive(camera, "motion");
        if (cJSON_IsObject(motion)) {
            json_copy_int(motion, "min_area", &out->camera_motion_min_area, 0, 100);
            json_copy_int(motion, "min_duration_ms", &out->camera_motion_min_duration_ms, 0, 1000);
            json_copy_int(motion, "cooldown_ms", &out->camera_motion_cooldown_ms, 0, 30000);
            json_copy_int(motion, "start_delay_ms", &out->camera_motion_start_delay_ms, 0, 10000);
            json_copy_bool(motion, "ignore_lighting", &out->camera_motion_ignore_lighting);

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
                    json_copy_int(zone, "x", &x, 0, 100);
                    json_copy_int(zone, "y", &y, 0, 100);
                    json_copy_int(zone, "w", &w, 0, 100);
                    json_copy_int(zone, "h", &h, 0, 100);
                    if (w <= 0 || h <= 0) {
                        continue;
                    }
                    out->camera_motion_zones[count].x = x;
                    out->camera_motion_zones[count].y = y;
                    out->camera_motion_zones[count].w = w;
                    out->camera_motion_zones[count].h = h;
                    count++;
                }
                out->camera_motion_zone_count = count;
            }
        }

        cJSON *image = cJSON_GetObjectItemCaseSensitive(camera, "image");
        if (cJSON_IsObject(image)) {
            json_copy_bool(image, "manual", &out->camera_img_manual);
            json_copy_bool(image, "wb_manual", &out->camera_img_wb_manual);
            json_copy_bool(image, "sharpen_manual", &out->camera_img_sharpen_manual);
            json_copy_bool(image, "denoise_manual", &out->camera_img_denoise_manual);
            json_copy_int(image, "brightness", &out->camera_img_brightness, -128, 127);
            json_copy_int(image, "contrast", &out->camera_img_contrast, 0, 255);
            json_copy_int(image, "saturation", &out->camera_img_saturation, 0, 255);
            json_copy_int(image, "hue", &out->camera_img_hue, 0, 360);
            json_copy_int(image, "wb_red", &out->camera_img_wb_red, 50, 200);
            json_copy_int(image, "wb_blue", &out->camera_img_wb_blue, 50, 200);
            json_copy_int(image, "sharpen", &out->camera_img_sharpen, 25, 300);
            json_copy_int(image, "denoise", &out->camera_img_denoise, 25, 200);
            json_copy_int(image, "tone_shadows", &out->camera_img_tone_shadows, -100, 100);
            json_copy_int(image, "tone_highlights", &out->camera_img_tone_highlights, -100, 100);
        }
    }

    if (cJSON_IsObject(system)) {
        json_copy_int(system, "daily_restart_hour", &out->daily_restart_hour, -1, 23);
        json_copy_bool(system, "touch_test", &out->touch_test);
    }

    if (cJSON_IsObject(audio)) {
        json_copy_int(audio, "volume", &out->audio_volume, 0, 100);
    }

    if (cJSON_IsObject(topbar)) {
        json_copy_bool(topbar, "show_clock", &out->topbar_show_clock);
        json_copy_bool(topbar, "show_room_name", &out->topbar_show_room_name);
        json_copy_string(topbar, "room_name", out->topbar_room_name, sizeof(out->topbar_room_name));
        json_copy_bool(topbar, "show_status", &out->topbar_show_status);
        json_copy_bool(topbar, "show_brightness", &out->topbar_show_brightness);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t nvs_load_secret(const char *key, char *out, size_t out_len, bool *out_found)
{
    if (key == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (out_found != NULL) {
        *out_found = false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required = 0;
    err = nvs_get_str(handle, key, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (required == 0) {
        if (out_found != NULL) {
            *out_found = true;
        }
        nvs_close(handle);
        return ESP_OK;
    }
    if (required > out_len) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_str(handle, key, out, &required);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    if (out_found != NULL) {
        *out_found = true;
    }
    return ESP_OK;
}

static esp_err_t nvs_set_or_erase_secret(nvs_handle_t handle, const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (value[0] == '\0') {
        esp_err_t err = nvs_erase_key(handle, key);
        if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
            return ESP_OK;
        }
        return err;
    }

    return nvs_set_str(handle, key, value);
}

static esp_err_t nvs_save_secrets(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_or_erase_secret(handle, SETTINGS_NVS_KEY_WIFI_PASSWORD, settings->wifi_password);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_or_erase_secret(handle, SETTINGS_NVS_KEY_HA_ACCESS_TOKEN, settings->ha_access_token);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_or_erase_secret(handle, SETTINGS_NVS_KEY_XIAOZHI_TOKEN, settings->xiaozhi_token);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void runtime_settings_merge_secrets_from_nvs(runtime_settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    char wifi_password[APP_WIFI_PASSWORD_MAX_LEN] = {0};
    bool has_wifi_password = false;
    esp_err_t wifi_err =
        nvs_load_secret(SETTINGS_NVS_KEY_WIFI_PASSWORD, wifi_password, sizeof(wifi_password), &has_wifi_password);
    if (wifi_err == ESP_OK && has_wifi_password) {
        strlcpy(settings->wifi_password, wifi_password, sizeof(settings->wifi_password));
    }

    char ha_access_token[APP_HA_ACCESS_TOKEN_MAX_LEN] = {0};
    bool has_ha_access_token = false;
    esp_err_t token_err = nvs_load_secret(
        SETTINGS_NVS_KEY_HA_ACCESS_TOKEN, ha_access_token, sizeof(ha_access_token), &has_ha_access_token);
    if (token_err == ESP_OK && has_ha_access_token) {
        strlcpy(settings->ha_access_token, ha_access_token, sizeof(settings->ha_access_token));
    }

    char xiaozhi_token[APP_XIAOZHI_TOKEN_MAX_LEN] = {0};
    bool has_xiaozhi_token = false;
    esp_err_t xz_token_err = nvs_load_secret(
        SETTINGS_NVS_KEY_XIAOZHI_TOKEN, xiaozhi_token, sizeof(xiaozhi_token), &has_xiaozhi_token);
    if (xz_token_err == ESP_OK && has_xiaozhi_token) {
        strlcpy(settings->xiaozhi_token, xiaozhi_token, sizeof(settings->xiaozhi_token));
    }
}

void runtime_settings_set_defaults(runtime_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    if (!is_placeholder(APP_WIFI_SSID)) {
        strlcpy(out->wifi_ssid, APP_WIFI_SSID, sizeof(out->wifi_ssid));
    }
    if (!is_placeholder(APP_WIFI_PASSWORD)) {
        strlcpy(out->wifi_password, APP_WIFI_PASSWORD, sizeof(out->wifi_password));
    }
    strlcpy(out->wifi_country_code, APP_WIFI_COUNTRY_CODE, sizeof(out->wifi_country_code));
    if (!is_placeholder(APP_HA_WS_URL)) {
        strlcpy(out->ha_ws_url, APP_HA_WS_URL, sizeof(out->ha_ws_url));
    }
    if (!is_placeholder(APP_HA_ACCESS_TOKEN)) {
        strlcpy(out->ha_access_token, APP_HA_ACCESS_TOKEN, sizeof(out->ha_access_token));
    }
    out->ha_rest_enabled = false;
    strlcpy(out->ntp_server, APP_NTP_SERVER, sizeof(out->ntp_server));
    strlcpy(out->time_tz, APP_TIME_TZ, sizeof(out->time_tz));
    strlcpy(out->ui_language, APP_UI_DEFAULT_LANGUAGE, sizeof(out->ui_language));

    out->xiaozhi_server[0] = '\0';
    out->xiaozhi_device[0] = '\0';
    out->xiaozhi_token[0] = '\0';
    strlcpy(out->xiaozhi_ota_url, APP_XIAOZHI_OTA_URL_DEFAULT, sizeof(out->xiaozhi_ota_url));
    out->xiaozhi_enabled = false;
    out->sd_logging_enabled = true;

    /* Network: DHCP by default, empty static addresses. */
    out->wifi_static_ip_enabled = false;
    out->wifi_static_ip[0] = '\0';
    out->wifi_static_netmask[0] = '\0';
    out->wifi_static_gateway[0] = '\0';
    out->wifi_static_dns[0] = '\0';

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    out->camera_enabled = false;
    out->camera_motion_wake = CONFIG_APP_LOCAL_CAMERA_MOTION_WAKE;
    out->camera_motion_threshold = 8;
    out->camera_jpeg_quality = CONFIG_APP_LOCAL_CAMERA_JPEG_QUALITY;
#ifdef CONFIG_APP_LOCAL_CAMERA_HFLIP
    out->camera_hflip = CONFIG_APP_LOCAL_CAMERA_HFLIP;
#else
    out->camera_hflip = false;
#endif
#ifdef CONFIG_APP_LOCAL_CAMERA_VFLIP
    out->camera_vflip = CONFIG_APP_LOCAL_CAMERA_VFLIP;
#else
    out->camera_vflip = false;
#endif
    out->camera_stream_enabled = false;
    out->camera_resolution = 0;
#else
    out->camera_enabled = false;
    out->camera_motion_wake = false;
    out->camera_motion_threshold = 8;
    out->camera_jpeg_quality = 55;
    out->camera_hflip = false;
    out->camera_vflip = false;
    out->camera_stream_enabled = false;
    out->camera_resolution = 0;
#endif

    /* Motion-detector tuning: whole frame, fire immediately, 1 s cooldown,
     * 2 s start grace period, global-lighting filter on. */
    out->camera_motion_min_area = 0;
    out->camera_motion_min_duration_ms = 0;
    out->camera_motion_cooldown_ms = 1000;
    out->camera_motion_start_delay_ms = 2000;
    out->camera_motion_ignore_lighting = true;
    out->camera_motion_zone_count = 0;
    for (int i = 0; i < 4; i++) {
        out->camera_motion_zones[i].x = 0;
        out->camera_motion_zones[i].y = 0;
        out->camera_motion_zones[i].w = 0;
        out->camera_motion_zones[i].h = 0;
    }

    /* Manual ISP image calibration: disabled, every slider neutral. */
    out->camera_img_manual = false;
    out->camera_img_wb_manual = false;
    out->camera_img_sharpen_manual = false;
    out->camera_img_denoise_manual = false;
    out->camera_img_brightness = 0;
    out->camera_img_contrast = 128;
    out->camera_img_saturation = 128;
    out->camera_img_hue = 0;
    out->camera_img_wb_red = 100;
    out->camera_img_wb_blue = 100;
    out->camera_img_sharpen = 100;
    out->camera_img_denoise = 100;
    out->camera_img_tone_shadows = 0;
    out->camera_img_tone_highlights = 0;

    out->sd_flush_interval_s = 30;
    out->sd_log_system_enabled = true;
    out->sd_log_sensors_enabled = true;
    out->sd_log_camera_enabled = true;

    out->daily_restart_hour = -1;
    out->touch_test = false;
    out->audio_volume = 80;

    out->page_wallpaper[0] = '\0';
    out->tile_transparency = 0;

    out->topbar_show_clock = true;
    out->topbar_show_room_name = false;
    out->topbar_room_name[0] = '\0';
    out->topbar_show_status = true;
    out->topbar_show_brightness = true;
}

esp_err_t runtime_settings_load(runtime_settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    runtime_settings_set_defaults(out);

    char *json = NULL;
    esp_err_t file_err = load_file_text(APP_SETTINGS_PATH, APP_SETTINGS_MAX_JSON_LEN, &json);
    if (file_err != ESP_OK) {
        return file_err;
    }

    bool legacy_wifi_password = false;
    bool legacy_ha_access_token = false;
    esp_err_t parse_err = parse_settings_json(json, out, &legacy_wifi_password, &legacy_ha_access_token);
    free(json);
    if (parse_err != ESP_OK) {
        return parse_err;
    }
    if (out->wifi_country_code[0] == '\0') {
        strlcpy(out->wifi_country_code, APP_WIFI_COUNTRY_CODE, sizeof(out->wifi_country_code));
    }
    normalize_ui_language(out->ui_language, sizeof(out->ui_language));

    char nvs_wifi_password[APP_WIFI_PASSWORD_MAX_LEN] = {0};
    bool has_nvs_wifi_password = false;
    esp_err_t nvs_err =
        nvs_load_secret(SETTINGS_NVS_KEY_WIFI_PASSWORD, nvs_wifi_password, sizeof(nvs_wifi_password), &has_nvs_wifi_password);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }
    if (has_nvs_wifi_password) {
        strlcpy(out->wifi_password, nvs_wifi_password, sizeof(out->wifi_password));
    }

    char nvs_ha_access_token[APP_HA_ACCESS_TOKEN_MAX_LEN] = {0};
    bool has_nvs_ha_access_token = false;
    nvs_err = nvs_load_secret(
        SETTINGS_NVS_KEY_HA_ACCESS_TOKEN, nvs_ha_access_token, sizeof(nvs_ha_access_token), &has_nvs_ha_access_token);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }
    if (has_nvs_ha_access_token) {
        strlcpy(out->ha_access_token, nvs_ha_access_token, sizeof(out->ha_access_token));
    }

    /* The Xiaozhi token is a secret stored in NVS (never in the public JSON),
     * so restore it here alongside the other NVS secrets. Without this the
     * token is empty after every boot, which forces a cloud activation round
     * trip and races the activation task against xz_xiaozhi_init(). */
    char nvs_xiaozhi_token[APP_XIAOZHI_TOKEN_MAX_LEN] = {0};
    bool has_nvs_xiaozhi_token = false;
    nvs_err = nvs_load_secret(
        SETTINGS_NVS_KEY_XIAOZHI_TOKEN, nvs_xiaozhi_token, sizeof(nvs_xiaozhi_token), &has_nvs_xiaozhi_token);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }
    if (has_nvs_xiaozhi_token) {
        strlcpy(out->xiaozhi_token, nvs_xiaozhi_token, sizeof(out->xiaozhi_token));
    }

    bool migrate_to_nvs =
        (legacy_wifi_password && !has_nvs_wifi_password && out->wifi_password[0] != '\0') ||
        (legacy_ha_access_token && !has_nvs_ha_access_token && out->ha_access_token[0] != '\0');

    if (migrate_to_nvs) {
        nvs_err = nvs_save_secrets(out);
        if (nvs_err != ESP_OK) {
            return nvs_err;
        }
    }

    if (legacy_wifi_password || legacy_ha_access_token) {
        esp_err_t scrub_err = write_public_settings_file(out);
        if (scrub_err != ESP_OK) {
            ESP_LOGW(TAG_APP, "Failed to scrub legacy secrets from LittleFS settings: %s", esp_err_to_name(scrub_err));
        }
    }

    return ESP_OK;
}

esp_err_t runtime_settings_save(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t file_err = write_public_settings_file(settings);
    if (file_err != ESP_OK) {
        return file_err;
    }

    esp_err_t nvs_err = nvs_save_secrets(settings);
    if (nvs_err != ESP_OK) {
        return nvs_err;
    }

    ESP_LOGI(TAG_APP, "Saved runtime settings");
    return ESP_OK;
}

esp_err_t runtime_settings_init(void)
{
    runtime_settings_t *settings = calloc(1, sizeof(runtime_settings_t));
    if (settings == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = runtime_settings_load(settings);
    if (err == ESP_OK) {
        free(settings);
        return ESP_OK;
    }

    ESP_LOGW(TAG_APP, "Settings missing/invalid (%s), writing defaults", esp_err_to_name(err));
    runtime_settings_set_defaults(settings);
    runtime_settings_merge_secrets_from_nvs(settings);
    esp_err_t save_err = runtime_settings_save(settings);
    free(settings);
    return save_err;
}

bool runtime_settings_has_wifi(const runtime_settings_t *settings)
{
    return settings != NULL && settings->wifi_ssid[0] != '\0';
}

bool runtime_settings_has_ha(const runtime_settings_t *settings)
{
    return settings != NULL && settings->ha_ws_url[0] != '\0' && settings->ha_access_token[0] != '\0';
}

bool runtime_settings_has_xiaozhi(const runtime_settings_t *settings)
{
    return settings != NULL && settings->xiaozhi_enabled && settings->xiaozhi_server[0] != '\0';
}

esp_err_t runtime_settings_apply_image_calibration(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    esp_video_isp_manual_t calibration = ESP_VIDEO_ISP_MANUAL_NEUTRAL();

    if (settings->camera_img_manual) {
        calibration.blocks |= ESP_VIDEO_ISP_MANUAL_BRIGHTNESS | ESP_VIDEO_ISP_MANUAL_CONTRAST |
                              ESP_VIDEO_ISP_MANUAL_SATURATION | ESP_VIDEO_ISP_MANUAL_HUE |
                              ESP_VIDEO_ISP_MANUAL_TONE;
        calibration.brightness = settings->camera_img_brightness;
        calibration.contrast = (uint32_t)settings->camera_img_contrast;
        calibration.saturation = (uint32_t)settings->camera_img_saturation;
        calibration.hue = (uint32_t)settings->camera_img_hue;
        calibration.tone_shadows = (float)settings->camera_img_tone_shadows / 100.0f;
        calibration.tone_highlights = (float)settings->camera_img_tone_highlights / 100.0f;
    }
    if (settings->camera_img_manual && settings->camera_img_wb_manual) {
        calibration.blocks |= ESP_VIDEO_ISP_MANUAL_WB;
        calibration.wb_red_gain = (float)settings->camera_img_wb_red / 100.0f;
        calibration.wb_blue_gain = (float)settings->camera_img_wb_blue / 100.0f;
    }
    if (settings->camera_img_manual && settings->camera_img_sharpen_manual) {
        calibration.blocks |= ESP_VIDEO_ISP_MANUAL_SHARPEN;
        calibration.sharpen_gain = (float)settings->camera_img_sharpen / 100.0f;
    }
    if (settings->camera_img_manual && settings->camera_img_denoise_manual) {
        calibration.blocks |= ESP_VIDEO_ISP_MANUAL_DENOISE;
        calibration.denoise_scale = (float)settings->camera_img_denoise / 100.0f;
    }

    esp_err_t err = esp_video_isp_manual_set(&calibration);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_APP, "Failed to apply image calibration: %s", esp_err_to_name(err));
    }
    return err;
#else
    return ESP_OK;
#endif
}

esp_err_t runtime_settings_apply_motion_config(const runtime_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    local_camera_motion_config_t config;
    memset(&config, 0, sizeof(config));
    config.threshold = (uint8_t)settings->camera_motion_threshold;
    config.min_area_pct = (uint8_t)settings->camera_motion_min_area;
    config.min_duration_ms = (uint16_t)settings->camera_motion_min_duration_ms;
    config.cooldown_ms = (uint16_t)settings->camera_motion_cooldown_ms;
    config.start_delay_ms = (uint16_t)settings->camera_motion_start_delay_ms;
    config.ignore_lighting = settings->camera_motion_ignore_lighting;
    config.zone_count = (uint8_t)settings->camera_motion_zone_count;
    for (int i = 0; i < 4; i++) {
        config.zones[i].x = (uint8_t)settings->camera_motion_zones[i].x;
        config.zones[i].y = (uint8_t)settings->camera_motion_zones[i].y;
        config.zones[i].w = (uint8_t)settings->camera_motion_zones[i].w;
        config.zones[i].h = (uint8_t)settings->camera_motion_zones[i].h;
    }

    local_camera_set_motion_config(&config);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}
