/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t api_routes_register(httpd_handle_t server);

esp_err_t api_layout_get_handler(httpd_req_t *req);
esp_err_t api_layout_put_handler(httpd_req_t *req);
esp_err_t api_entities_get_handler(httpd_req_t *req);
esp_err_t api_light_entities_get_handler(httpd_req_t *req);
esp_err_t api_light_entities_delete_handler(httpd_req_t *req);
esp_err_t api_ha_energy_get_handler(httpd_req_t *req);
esp_err_t api_state_get_handler(httpd_req_t *req);
esp_err_t api_settings_get_handler(httpd_req_t *req);
esp_err_t api_settings_put_handler(httpd_req_t *req);
esp_err_t api_i18n_languages_get_handler(httpd_req_t *req);
esp_err_t api_i18n_effective_get_handler(httpd_req_t *req);
esp_err_t api_i18n_custom_put_handler(httpd_req_t *req);
esp_err_t api_wifi_scan_get_handler(httpd_req_t *req);
esp_err_t api_version_get_handler(httpd_req_t *req);
esp_err_t api_screenshot_bmp_get_handler(httpd_req_t *req);
esp_err_t api_ui_tree_get_handler(httpd_req_t *req);
esp_err_t api_ota_status_get_handler(httpd_req_t *req);
esp_err_t api_ota_url_post_handler(httpd_req_t *req);
esp_err_t api_ota_upload_post_handler(httpd_req_t *req);
esp_err_t api_ota_switch_slot_post_handler(httpd_req_t *req);
esp_err_t api_ha_diagnostics_get_handler(httpd_req_t *req);
esp_err_t api_cameras_get_handler(httpd_req_t *req);
esp_err_t api_cameras_put_handler(httpd_req_t *req);
esp_err_t api_logs_get_handler(httpd_req_t *req);
esp_err_t api_logs_delete_handler(httpd_req_t *req);

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
esp_err_t api_sd_status_get_handler(httpd_req_t *req);
esp_err_t api_sd_format_post_handler(httpd_req_t *req);
esp_err_t api_sd_bg_list_get_handler(httpd_req_t *req);
esp_err_t api_sd_bg_upload_post_handler(httpd_req_t *req);
esp_err_t api_sd_bg_delete_handler(httpd_req_t *req);
esp_err_t api_sd_list_get_handler(httpd_req_t *req);
esp_err_t api_sd_file_get_handler(httpd_req_t *req);
esp_err_t api_backup_post_handler(httpd_req_t *req);
esp_err_t api_restore_post_handler(httpd_req_t *req);
#endif

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
esp_err_t api_camera_local_snapshot_get_handler(httpd_req_t *req);
esp_err_t api_camera_local_status_get_handler(httpd_req_t *req);
esp_err_t api_camera_local_motion_get_handler(httpd_req_t *req);
esp_err_t api_camera_local_stream_get_handler(httpd_req_t *req);
void api_camera_local_set_stream_enabled(bool enabled);
bool api_camera_local_get_stream_enabled(void);
#endif

esp_err_t api_themes_list_get_handler(httpd_req_t *req);
esp_err_t api_themes_active_get_handler(httpd_req_t *req);
esp_err_t api_themes_active_put_handler(httpd_req_t *req);
esp_err_t api_themes_get_handler(httpd_req_t *req);
esp_err_t api_themes_custom_put_handler(httpd_req_t *req);
esp_err_t api_themes_custom_delete_handler(httpd_req_t *req);

esp_err_t api_themes_list_get_handler(httpd_req_t *req);
esp_err_t api_themes_active_get_handler(httpd_req_t *req);
esp_err_t api_themes_active_put_handler(httpd_req_t *req);
esp_err_t api_themes_get_handler(httpd_req_t *req);
esp_err_t api_themes_custom_put_handler(httpd_req_t *req);
esp_err_t api_themes_custom_delete_handler(httpd_req_t *req);
