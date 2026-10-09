/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "layout/layout_validate.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "app_config.h"

#define GRAPH_POINT_COUNT_MIN 16
#define GRAPH_POINT_COUNT_MAX 64
#define GRAPH_TIME_WINDOW_MIN_MIN 1
#define GRAPH_TIME_WINDOW_MIN_MAX 1440

static const char *const GRAPH_DISPLAY_MODES[] = {
    "line",
    "line_smooth",
    "line_smooth_points",
    "bars",
};

static const int GRAPH_BAR_BUCKET_MIN_ALLOWED[] = {5, 10, 15, 30};

static bool str_in_list(const char *value, const void *list, size_t entry_size, size_t list_len)
{
    if (value == NULL || list == NULL || entry_size == 0) {
        return false;
    }
    const char *base = (const char *)list;
    for (size_t i = 0; i < list_len; i++) {
        const char *entry = base + (i * entry_size);
        if (strncmp(value, entry, entry_size) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_valid_entity_id(const char *entity_id)
{
    if (entity_id == NULL) {
        return false;
    }

    size_t len = strlen(entity_id);
    if (len < 3 || len >= APP_MAX_ENTITY_ID_LEN) {
        return false;
    }

    const char *dot = strchr(entity_id, '.');
    if (dot == NULL || dot == entity_id || *(dot + 1) == '\0') {
        return false;
    }
    if (strchr(dot + 1, '.') != NULL) {
        return false;
    }

    for (const char *p = entity_id; *p != '\0'; p++) {
        if (*p == '.') {
            continue;
        }
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')) {
            return false;
        }
    }

    return true;
}

static bool entity_in_domain(const char *entity_id, const char *domain)
{
    if (entity_id == NULL || domain == NULL) {
        return false;
    }
    size_t domain_len = strlen(domain);
    return strncmp(entity_id, domain, domain_len) == 0 && entity_id[domain_len] == '.';
}

/* Simple dotted-quad IPv4 check (used for panel-side ":ping" rows). */
static bool is_valid_ipv4(const char *ip)
{
    if (ip == NULL || ip[0] == '\0') {
        return false;
    }
    int octets = 0;
    int digits = 0;
    int value = 0;
    for (const char *p = ip; *p != '\0'; p++) {
        if (*p == '.') {
            if (digits == 0 || value > 255) {
                return false;
            }
            octets++;
            digits = 0;
            value = 0;
        } else if (isdigit((unsigned char)*p)) {
            digits++;
            value = value * 10 + (*p - '0');
            if (digits > 3 || value > 255) {
                return false;
            }
        } else {
            return false;
        }
    }
    return octets == 3 && digits > 0 && value <= 255;
}

static bool is_valid_entity_id_list(const char *list, size_t max_count, size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (list == NULL || list[0] == '\0') {
        return false;
    }

    size_t count = 0;
    const char *cursor = list;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        char token[APP_MAX_ENTITY_ID_LEN];
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
        size_t len = (size_t)(cursor - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' ||
                           start[len - 1] == '\n' || start[len - 1] == '\r')) {
            len--;
        }
        if (len == 0 || len >= APP_MAX_ENTITY_ID_LEN) {
            return false;
        }
        memcpy(token, start, len);
        token[len] = '\0';
        if (!is_valid_entity_id(token)) {
            return false;
        }
        count++;
        if (count > max_count) {
            return false;
        }
    }

    if (out_count != NULL) {
        *out_count = count;
    }
    return count > 0;
}

/* sensor_tile rows: "Label=entity.id,Label2=entity2.id". Every row needs a
 * non-empty label and an optional entity id; an empty entity hides the row.
 * An empty list (or a list whose rows all have empty entities) is allowed and
 * simply hides the whole tile. On failure, sets *reason (when non-NULL) to a
 * human readable description of the first problem found. */
static char s_labeled_list_reason[96];

static bool is_valid_labeled_entity_list(const char *list, size_t max_rows, const char **reason)
{
    if (reason != NULL) {
        *reason = NULL;
    }
    if (list == NULL || list[0] == '\0') {
        return true;
    }

    size_t count = 0;
    const char *cursor = list;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
        const char *end = cursor;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
            end--;
        }

        const char *eq = start;
        while (eq < end && *eq != '=') {
            eq++;
        }
        if (eq == end || eq == start) {
            if (reason != NULL) {
                snprintf(s_labeled_list_reason, sizeof(s_labeled_list_reason),
                    "row %d must be 'Label=entity'", (int)count + 1);
                *reason = s_labeled_list_reason;
            }
            return false; /* requires a non-empty label and '=' separator */
        }

        const char *label_start = start;
        const char *label_end = eq;
        while (label_start < label_end &&
               (*label_start == ' ' || *label_start == '\t' || *label_start == '\n' || *label_start == '\r')) {
            label_start++;
        }
        while (label_end > label_start &&
               (label_end[-1] == ' ' || label_end[-1] == '\t' || label_end[-1] == '\n' || label_end[-1] == '\r')) {
            label_end--;
        }
        size_t label_len = (size_t)(label_end - label_start);
        if (label_len == 0 || label_len >= APP_MAX_SENSOR_TILE_LABEL_LEN) {
            if (reason != NULL) {
                snprintf(s_labeled_list_reason, sizeof(s_labeled_list_reason),
                    "label length must be 1-%d characters (row %d)",
                    APP_MAX_SENSOR_TILE_LABEL_LEN - 1, (int)count + 1);
                *reason = s_labeled_list_reason;
            }
            return false;
        }

        const char *entity_start = eq + 1;
        const char *entity_end = end;
        while (entity_start < entity_end &&
               (*entity_start == ' ' || *entity_start == '\t' || *entity_start == '\n' || *entity_start == '\r')) {
            entity_start++;
        }
        while (entity_end > entity_start &&
               (entity_end[-1] == ' ' || entity_end[-1] == '\t' || entity_end[-1] == '\n' || entity_end[-1] == '\r')) {
            entity_end--;
        }
        size_t entity_len = (size_t)(entity_end - entity_start);
        if (entity_len > 0) {
            char entity[APP_MAX_ENTITY_ID_LEN + 8];
            if (entity_len >= sizeof(entity)) {
                if (reason != NULL) {
                    snprintf(s_labeled_list_reason, sizeof(s_labeled_list_reason),
                        "entity id too long in row %d", (int)count + 1);
                    *reason = s_labeled_list_reason;
                }
                return false;
            }
            memcpy(entity, entity_start, entity_len);
            entity[entity_len] = '\0';
            /* Strip auxiliary suffixes (":ping" and ":color=X"), mirroring
             * sensor_tile_strip_aux_style(). A ":ping" row uses a dotted-quad
             * IPv4 address as its token instead of an HA entity id. */
            bool is_ping = false;
            for (;;) {
                size_t alen = strlen(entity);
                if (alen >= 5 && strcmp(entity + alen - 5, ":ping") == 0) {
                    entity[alen - 5] = '\0';
                    is_ping = true;
                    continue;
                }
                char *color = strstr(entity, ":color=");
                if (color != NULL) {
                    char *tok = color + 7;
                    while (*tok != '\0' && *tok != ':') {
                        tok++;
                    }
                    memmove(color, tok, strlen(tok) + 1);
                    continue;
                }
                break;
            }
            /* Strip an optional per-row style suffix. Keep this in sync with
             * the sensor_tile widget: ":bar", ":dot", ":status", ":chart",
             * ":ports", ":ip" and ":power" with an optional colour
             * (":power:red" or ":power:#RRGGBB"). */
            size_t slen = strlen(entity);
            if (slen >= 4 && strcmp(entity + slen - 4, ":bar") == 0) {
                entity[slen - 4] = '\0';
            } else if (slen >= 4 && strcmp(entity + slen - 4, ":dot") == 0) {
                entity[slen - 4] = '\0';
            } else if (slen >= 7 && strcmp(entity + slen - 7, ":status") == 0) {
                entity[slen - 7] = '\0';
            } else if (slen >= 6 && strcmp(entity + slen - 6, ":chart") == 0) {
                entity[slen - 6] = '\0';
            } else if (slen >= 6 && strcmp(entity + slen - 6, ":ports") == 0) {
                entity[slen - 6] = '\0';
            } else if (slen >= 3 && strcmp(entity + slen - 3, ":ip") == 0) {
                entity[slen - 3] = '\0';
            } else {
                /* ":power" may be followed by ":color" or "#RRGGBB". Strip the
                 * whole suffix whenever ":power" starts a token boundary so a
                 * power row is always recognised, matching sensor_tile_strip_row_style(). */
                char *p = strstr(entity, ":power");
                if (p != NULL && (p[6] == '\0' || p[6] == ':' || p[6] == '#')) {
                    *p = '\0';
                }
            }
            if (entity[0] != '\0') {
                bool valid = is_valid_entity_id(entity);
                if (!valid && is_ping && is_valid_ipv4(entity)) {
                    valid = true;
                }
                if (!valid) {
                    if (reason != NULL) {
                        snprintf(s_labeled_list_reason, sizeof(s_labeled_list_reason),
                            "invalid entity id in row %d", (int)count + 1);
                        *reason = s_labeled_list_reason;
                    }
                    return false;
                }
            }
        }

        count++;
        if (count > max_rows) {
            if (reason != NULL) {
                snprintf(s_labeled_list_reason, sizeof(s_labeled_list_reason),
                    "too many rows (max %d)", (int)max_rows);
                *reason = s_labeled_list_reason;
            }
            return false;
        }
    }

    return true;
}

static bool is_supported_widget_type(const char *type)
{
    if (type == NULL) {
        return false;
    }
    return (strcmp(type, "sensor") == 0) || (strcmp(type, "button") == 0) || (strcmp(type, "slider") == 0) ||
           (strcmp(type, "graph") == 0) || (strcmp(type, "empty_tile") == 0) || (strcmp(type, "light_tile") == 0) ||
           (strcmp(type, "heating_tile") == 0) || (strcmp(type, "weather_tile") == 0) ||
           (strcmp(type, "weather_3day") == 0) || (strcmp(type, "todo_list") == 0) ||
           (strcmp(type, "media_player") == 0) || (strcmp(type, "roborock_tile") == 0) ||
           (strcmp(type, "binary_sensor") == 0) || (strcmp(type, "presence") == 0) ||
           (strcmp(type, "cover") == 0) ||
           (strcmp(type, "lock") == 0) || (strcmp(type, "fan") == 0) || (strcmp(type, "select") == 0) ||
           (strcmp(type, "number") == 0) || (strcmp(type, "monitor_tile") == 0) ||
           (strcmp(type, "entity_list") == 0) || (strcmp(type, "sensor_tile") == 0) ||
           (strcmp(type, "temp_tile") == 0);
}

static bool is_supported_page_type(const char *type)
{
    if (type == NULL || type[0] == '\0') {
        return true;
    }
    return strcmp(type, "dashboard") == 0 || strcmp(type, "energy_dashboard") == 0 || strcmp(type, "xiaozhi") == 0 ||
           strcmp(type, "cameras") == 0;
}

typedef struct {
    int min_w;
    int min_h;
    int max_w;
    int max_h;
} widget_size_limits_t;

static widget_size_limits_t widget_size_limits_for_type(const char *type)
{
    widget_size_limits_t limits = {
        .min_w = 60,
        .min_h = 60,
        .max_w = APP_CONTENT_BOX_WIDTH,
        .max_h = APP_CONTENT_BOX_HEIGHT,
    };

    if (type == NULL) {
        return limits;
    }

    if (strcmp(type, "sensor") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 90;
        limits.min_h = 60;
#else
        limits.min_w = 120;
        limits.min_h = 80;
#endif
    } else if (strcmp(type, "binary_sensor") == 0 || strcmp(type, "presence") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 90;
        limits.min_h = 60;
#else
        limits.min_w = 120;
        limits.min_h = 80;
#endif
    } else if (strcmp(type, "button") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 82;
        limits.min_h = 82;
        limits.max_w = 320;
        limits.max_h = 260;
#else
        limits.min_w = 100;
        limits.min_h = 100;
        limits.max_w = 480;
        limits.max_h = 320;
#endif
    } else if (strcmp(type, "slider") == 0) {
        limits.min_w = 100;
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_h = 80;
#else
        limits.min_h = 100;
#endif
    } else if (strcmp(type, "graph") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 150;
        limits.min_h = 100;
#else
        limits.min_w = 220;
        limits.min_h = 140;
#endif
    } else if (strcmp(type, "empty_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 100;
        limits.min_h = 70;
#else
        limits.min_w = 120;
        limits.min_h = 80;
#endif
    } else if (strcmp(type, "light_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 140;
        limits.min_h = 140;
#else
        limits.min_w = 150;
        limits.min_h = 150;
#endif
        limits.max_w = 480;
        limits.max_h = 480;
    } else if (strcmp(type, "heating_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 150;
        limits.min_h = 150;
#else
        limits.min_w = 220;
        limits.min_h = 200;
#endif
        limits.max_w = 480;
        limits.max_h = 480;
    } else if (strcmp(type, "weather_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 160;
        limits.min_h = 150;
#else
        limits.min_w = 220;
        limits.min_h = 200;
#endif
        limits.max_w = 480;
        limits.max_h = 480;
    } else if (strcmp(type, "weather_3day") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 280;
        limits.min_h = 180;
#else
        limits.min_w = 260;
        limits.min_h = 220;
#endif
        limits.max_w = 640;
        limits.max_h = 480;
    } else if (strcmp(type, "todo_list") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 180;
        limits.min_h = 160;
#else
        limits.min_w = 220;
        limits.min_h = 200;
#endif
        limits.max_w = 640;
        limits.max_h = 640;
    } else if (strcmp(type, "media_player") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 200;
        limits.min_h = 170;
#else
        limits.min_w = 260;
        limits.min_h = 220;
#endif
        limits.max_w = APP_CONTENT_BOX_WIDTH;
        limits.max_h = APP_CONTENT_BOX_HEIGHT;
    } else if (strcmp(type, "roborock_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 220;
        limits.min_h = 190;
#else
        limits.min_w = 240;
        limits.min_h = 220;
#endif
        limits.max_w = APP_CONTENT_BOX_WIDTH;
        limits.max_h = APP_CONTENT_BOX_HEIGHT;
    } else if (strcmp(type, "lock") == 0 || strcmp(type, "fan") == 0 || strcmp(type, "cover") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 100;
        limits.min_h = 90;
#else
        limits.min_w = 140;
        limits.min_h = 120;
#endif
        limits.max_w = 480;
        limits.max_h = 480;
    } else if (strcmp(type, "select") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 140;
        limits.min_h = 80;
#else
        limits.min_w = 180;
        limits.min_h = 100;
#endif
        limits.max_w = 480;
        limits.max_h = 300;
    } else if (strcmp(type, "number") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 100;
        limits.min_h = 90;
#else
        limits.min_w = 140;
        limits.min_h = 120;
#endif
        limits.max_w = 480;
        limits.max_h = 480;
    } else if (strcmp(type, "monitor_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 170;
        limits.min_h = 150;
#else
        limits.min_w = 200;
        limits.min_h = 180;
#endif
        limits.max_w = APP_CONTENT_BOX_WIDTH;
        limits.max_h = APP_CONTENT_BOX_HEIGHT;
    } else if (strcmp(type, "temp_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 130;
        limits.min_h = 140;
#else
        limits.min_w = 150;
        limits.min_h = 160;
#endif
        limits.max_w = APP_CONTENT_BOX_WIDTH;
        limits.max_h = APP_CONTENT_BOX_HEIGHT;
    } else if (strcmp(type, "entity_list") == 0 || strcmp(type, "sensor_tile") == 0) {
#if defined(CONFIG_APP_PANEL_VARIANT_S3_480)
        limits.min_w = 180;
        limits.min_h = 140;
#else
        limits.min_w = 200;
        limits.min_h = 160;
#endif
        limits.max_w = APP_CONTENT_BOX_WIDTH;
        limits.max_h = APP_CONTENT_BOX_HEIGHT;
    }

    if (limits.max_w > APP_CONTENT_BOX_WIDTH) {
        limits.max_w = APP_CONTENT_BOX_WIDTH;
    }
    if (limits.max_h > APP_CONTENT_BOX_HEIGHT) {
        limits.max_h = APP_CONTENT_BOX_HEIGHT;
    }
    return limits;
}

static const char *required_domain_for_widget_type(const char *type)
{
    if (type == NULL) {
        return NULL;
    }
    if (strcmp(type, "sensor") == 0) {
        return "sensor";
    }
    if (strcmp(type, "binary_sensor") == 0) {
        return "binary_sensor";
    }
    if (strcmp(type, "light_tile") == 0) {
        return "light";
    }
    if (strcmp(type, "heating_tile") == 0) {
        return "climate";
    }
    if (strcmp(type, "weather_tile") == 0 || strcmp(type, "weather_3day") == 0) {
        return "weather";
    }
    if (strcmp(type, "todo_list") == 0) {
        return "todo";
    }
    if (strcmp(type, "media_player") == 0) {
        return "media_player";
    }
    if (strcmp(type, "roborock_tile") == 0) {
        return "vacuum";
    }
    if (strcmp(type, "lock") == 0) {
        return "lock";
    }
    if (strcmp(type, "cover") == 0) {
        return "cover";
    }
    if (strcmp(type, "fan") == 0) {
        return "fan";
    }
    if (strcmp(type, "select") == 0) {
        return "select";
    }
    if (strcmp(type, "number") == 0) {
        return "number";
    }
    if (strcmp(type, "monitor_tile") == 0) {
        return "sensor";
    }
    if (strcmp(type, "temp_tile") == 0) {
        return "sensor";
    }
    return NULL;
}

static bool widget_entity_domain_valid(const char *type, const char *entity_id)
{
    if (type == NULL || entity_id == NULL) {
        return false;
    }

    if (strcmp(type, "sensor") == 0) {
        return entity_in_domain(entity_id, "sensor") || entity_in_domain(entity_id, "binary_sensor");
    }
    if (strcmp(type, "binary_sensor") == 0) {
        return entity_in_domain(entity_id, "binary_sensor");
    }
    if (strcmp(type, "presence") == 0) {
        return entity_in_domain(entity_id, "device_tracker") || entity_in_domain(entity_id, "person");
    }
    if (strcmp(type, "button") == 0) {
        return entity_in_domain(entity_id, "switch") || entity_in_domain(entity_id, "media_player") ||
               entity_in_domain(entity_id, "button") || entity_in_domain(entity_id, "scene") ||
               entity_in_domain(entity_id, "script") || entity_in_domain(entity_id, "automation") ||
               entity_in_domain(entity_id, "input_boolean");
    }
    if (strcmp(type, "select") == 0) {
        return entity_in_domain(entity_id, "select") || entity_in_domain(entity_id, "input_select");
    }
    if (strcmp(type, "number") == 0) {
        return entity_in_domain(entity_id, "number") || entity_in_domain(entity_id, "input_number");
    }
    if (strcmp(type, "monitor_tile") == 0) {
        return entity_in_domain(entity_id, "sensor") || entity_in_domain(entity_id, "binary_sensor");
    }
    if (strcmp(type, "temp_tile") == 0) {
        return entity_in_domain(entity_id, "sensor");
    }
    if (strcmp(type, "media_player") == 0) {
        return entity_in_domain(entity_id, "media_player");
    }
    if (strcmp(type, "roborock_tile") == 0) {
        return entity_in_domain(entity_id, "vacuum");
    }
    if (strcmp(type, "empty_tile") == 0) {
        return true;
    }

    const char *required_domain = required_domain_for_widget_type(type);
    if (required_domain == NULL) {
        return true;
    }
    return entity_in_domain(entity_id, required_domain);
}

static bool widget_requires_primary_entity(const char *type)
{
    return type == NULL || (strcmp(type, "empty_tile") != 0 && strcmp(type, "entity_list") != 0 &&
                            strcmp(type, "sensor_tile") != 0);
}

static bool is_hex_digit_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_valid_hex_rgb_color(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    const char *p = text;
    if (p[0] == '#') {
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (strlen(p) != 6) {
        return false;
    }

    for (size_t i = 0; i < 6; i++) {
        if (!is_hex_digit_char(p[i])) {
            return false;
        }
    }
    return true;
}

static bool is_valid_slider_direction(const char *direction)
{
    if (direction == NULL || direction[0] == '\0') {
        return false;
    }
    return strcmp(direction, "auto") == 0 || strcmp(direction, "left_to_right") == 0 ||
           strcmp(direction, "right_to_left") == 0 || strcmp(direction, "bottom_to_top") == 0 ||
           strcmp(direction, "top_to_bottom") == 0;
}

static bool is_valid_button_mode(const char *mode)
{
    if (mode == NULL || mode[0] == '\0') {
        return false;
    }
    return strcmp(mode, "auto") == 0 || strcmp(mode, "play_pause") == 0 || strcmp(mode, "stop") == 0 ||
           strcmp(mode, "next") == 0 || strcmp(mode, "previous") == 0;
}

static bool button_mode_requires_media_player(const char *mode)
{
    if (mode == NULL || mode[0] == '\0') {
        return false;
    }
    return strcmp(mode, "play_pause") == 0 || strcmp(mode, "stop") == 0 || strcmp(mode, "next") == 0 ||
           strcmp(mode, "previous") == 0;
}

static bool is_valid_energy_source(const char *source)
{
    if (source == NULL || source[0] == '\0') {
        return false;
    }
    return strcmp(source, "ha_energy") == 0 || strcmp(source, "manual_live") == 0;
}

static void validate_energy_entity_field(cJSON *energy, const char *key, const char *page_id,
    layout_validation_result_t *result)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(energy, key);
    if (item == NULL) {
        return;
    }

    char msg[128];
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        snprintf(msg, sizeof(msg), "page %s energy.%s must be a string", page_id != NULL ? page_id : "?", key);
        layout_validation_add(result, msg);
        return;
    }

    if (item->valuestring[0] == '\0') {
        return;
    }

    if (!is_valid_entity_id(item->valuestring) || !entity_in_domain(item->valuestring, "sensor")) {
        snprintf(msg, sizeof(msg), "page %s energy.%s must be sensor.*", page_id != NULL ? page_id : "?", key);
        layout_validation_add(result, msg);
    }
}

static void validate_energy_page(cJSON *page, const char *page_id, layout_validation_result_t *result)
{
    cJSON *energy = cJSON_GetObjectItemCaseSensitive(page, "energy");
    if (energy == NULL) {
        return;
    }
    if (!cJSON_IsObject(energy)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "page %s: energy must be object", page_id != NULL ? page_id : "?");
        layout_validation_add(result, msg);
        return;
    }

    cJSON *source = cJSON_GetObjectItemCaseSensitive(energy, "source");
    if (source != NULL) {
        char msg[128];
        if (!cJSON_IsString(source) || source->valuestring == NULL) {
            snprintf(msg, sizeof(msg), "page %s energy.source must be a string", page_id != NULL ? page_id : "?");
            layout_validation_add(result, msg);
        } else if (source->valuestring[0] != '\0' && !is_valid_energy_source(source->valuestring)) {
            snprintf(msg,
                sizeof(msg),
                "page %s energy.source must be ha_energy or manual_live",
                page_id != NULL ? page_id : "?");
            layout_validation_add(result, msg);
        }
    }

    static const char *keys[] = {
        "home_power_entity_id",
        "solar_power_entity_id",
        "grid_power_entity_id",
        "grid_import_power_entity_id",
        "grid_export_power_entity_id",
        "battery_power_entity_id",
        "battery_charge_power_entity_id",
        "battery_discharge_power_entity_id",
        "battery_soc_entity_id",
    };
    for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); i++) {
        validate_energy_entity_field(energy, keys[i], page_id, result);
    }
}

void layout_validation_clear(layout_validation_result_t *result)
{
    if (result == NULL) {
        return;
    }
    result->count = 0;
    memset(result->messages, 0, sizeof(result->messages));
}

void layout_validation_add(layout_validation_result_t *result, const char *msg)
{
    if (result == NULL || msg == NULL) {
        return;
    }
    if (result->count >= APP_LAYOUT_MAX_ERRORS) {
        return;
    }
    snprintf(result->messages[result->count], sizeof(result->messages[result->count]), "%s", msg);
    result->count++;
}

static bool validate_widget(cJSON *widget, const char *known_widget_ids, size_t known_ids_len,
    size_t page_index, size_t widget_index, layout_validation_result_t *result)
{
    char msg[96];

    cJSON *id = cJSON_GetObjectItemCaseSensitive(widget, "id");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(widget, "type");
    cJSON *entity_id = cJSON_GetObjectItemCaseSensitive(widget, "entity_id");
    cJSON *secondary_entity_id = cJSON_GetObjectItemCaseSensitive(widget, "secondary_entity_id");
    cJSON *slider_direction = cJSON_GetObjectItemCaseSensitive(widget, "slider_direction");
    cJSON *slider_accent_color = cJSON_GetObjectItemCaseSensitive(widget, "slider_accent_color");
    cJSON *button_accent_color = cJSON_GetObjectItemCaseSensitive(widget, "button_accent_color");
    cJSON *button_mode = cJSON_GetObjectItemCaseSensitive(widget, "button_mode");
    cJSON *graph_line_color = cJSON_GetObjectItemCaseSensitive(widget, "graph_line_color");
    cJSON *graph_point_count = cJSON_GetObjectItemCaseSensitive(widget, "graph_point_count");
    cJSON *graph_time_window_min = cJSON_GetObjectItemCaseSensitive(widget, "graph_time_window_min");
    cJSON *graph_display_mode = cJSON_GetObjectItemCaseSensitive(widget, "graph_display_mode");
    cJSON *graph_bar_bucket_min = cJSON_GetObjectItemCaseSensitive(widget, "graph_bar_bucket_min");
    cJSON *card_bg_color = cJSON_GetObjectItemCaseSensitive(widget, "card_bg_color");
    cJSON *label_color = cJSON_GetObjectItemCaseSensitive(widget, "label_color");
    cJSON *bg_image = cJSON_GetObjectItemCaseSensitive(widget, "bg_image");
    cJSON *rect = cJSON_GetObjectItemCaseSensitive(widget, "rect");

    if (!cJSON_IsString(id) || id->valuestring == NULL || strlen(id->valuestring) == 0U) {
        snprintf(msg, sizeof(msg), "page[%u] widget[%u]: invalid id", (unsigned)page_index, (unsigned)widget_index);
        layout_validation_add(result, msg);
    } else if (strlen(id->valuestring) >= APP_MAX_WIDGET_ID_LEN) {
        snprintf(msg, sizeof(msg), "widget id too long: %s", id->valuestring);
        layout_validation_add(result, msg);
    } else if (str_in_list(id->valuestring, known_widget_ids, APP_MAX_WIDGET_ID_LEN, known_ids_len)) {
        snprintf(msg, sizeof(msg), "duplicate widget id: %s", id->valuestring);
        layout_validation_add(result, msg);
    }

    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        snprintf(msg, sizeof(msg), "widget %s: missing type", cJSON_IsString(id) ? id->valuestring : "?");
        layout_validation_add(result, msg);
    } else if (!is_supported_widget_type(type->valuestring)) {
        snprintf(msg, sizeof(msg), "widget %s: unsupported type %s", cJSON_IsString(id) ? id->valuestring : "?",
            type->valuestring);
        layout_validation_add(result, msg);
    }

    bool requires_entity = true;
    if (cJSON_IsString(type) && type->valuestring != NULL) {
        requires_entity = widget_requires_primary_entity(type->valuestring);
    }

    if (requires_entity) {
        if (!cJSON_IsString(entity_id) || !is_valid_entity_id(entity_id->valuestring)) {
            snprintf(msg, sizeof(msg), "widget %s: invalid entity_id", cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }

        if (cJSON_IsString(type) && type->valuestring != NULL && cJSON_IsString(entity_id) && entity_id->valuestring != NULL) {
            if (!widget_entity_domain_valid(type->valuestring, entity_id->valuestring)) {
                if (strcmp(type->valuestring, "sensor") == 0) {
                    snprintf(msg, sizeof(msg), "widget %s: entity_id must be sensor.* or binary_sensor.*",
                        cJSON_IsString(id) ? id->valuestring : "?");
                } else if (strcmp(type->valuestring, "button") == 0) {
                    snprintf(msg, sizeof(msg), "widget %s: entity_id must be switch.* or media_player.*",
                        cJSON_IsString(id) ? id->valuestring : "?");
                } else {
                    const char *required_domain = required_domain_for_widget_type(type->valuestring);
                    snprintf(msg, sizeof(msg), "widget %s: entity_id must be %s.*",
                        cJSON_IsString(id) ? id->valuestring : "?", required_domain != NULL ? required_domain : "?");
                }
                layout_validation_add(result, msg);
            }
        }
    } else if (cJSON_IsString(entity_id) && entity_id->valuestring != NULL && entity_id->valuestring[0] != '\0' &&
               !is_valid_entity_id(entity_id->valuestring)) {
        snprintf(msg, sizeof(msg), "widget %s: invalid entity_id", cJSON_IsString(id) ? id->valuestring : "?");
        layout_validation_add(result, msg);
    }

    if (card_bg_color != NULL && cJSON_IsString(card_bg_color) && card_bg_color->valuestring != NULL &&
        card_bg_color->valuestring[0] != '\0' && !is_valid_hex_rgb_color(card_bg_color->valuestring)) {
        snprintf(msg, sizeof(msg), "widget %s: card_bg_color must be hex RGB", cJSON_IsString(id) ? id->valuestring : "?");
        layout_validation_add(result, msg);
    }
    if (label_color != NULL && cJSON_IsString(label_color) && label_color->valuestring != NULL &&
        label_color->valuestring[0] != '\0' && !is_valid_hex_rgb_color(label_color->valuestring)) {
        snprintf(msg, sizeof(msg), "widget %s: label_color must be hex RGB", cJSON_IsString(id) ? id->valuestring : "?");
        layout_validation_add(result, msg);
    }
    if (bg_image != NULL && cJSON_IsString(bg_image) && bg_image->valuestring != NULL &&
        strlen(bg_image->valuestring) >= APP_MAX_IMAGE_PATH_LEN) {
        snprintf(msg, sizeof(msg), "widget %s: bg_image path too long", cJSON_IsString(id) ? id->valuestring : "?");
        layout_validation_add(result, msg);
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "heating_tile") == 0) {
        if (cJSON_IsString(secondary_entity_id) && secondary_entity_id->valuestring != NULL &&
            secondary_entity_id->valuestring[0] != '\0') {
            if (!is_valid_entity_id(secondary_entity_id->valuestring) ||
                !entity_in_domain(secondary_entity_id->valuestring, "sensor")) {
                snprintf(msg, sizeof(msg), "widget %s: invalid secondary_entity_id", cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }

        cJSON *style_variant_heating = cJSON_GetObjectItemCaseSensitive(widget, "style_variant");
        if (cJSON_IsString(style_variant_heating) && style_variant_heating->valuestring != NULL &&
            style_variant_heating->valuestring[0] != '\0') {
            if (strcmp(style_variant_heating->valuestring, "default") != 0 &&
                strcmp(style_variant_heating->valuestring, "arc_semi") != 0) {
                snprintf(msg, sizeof(msg), "widget %s: style_variant must be default|arc_semi",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
        cJSON *arc_opening_heating = cJSON_GetObjectItemCaseSensitive(widget, "arc_opening");
        if (cJSON_IsString(arc_opening_heating) && arc_opening_heating->valuestring != NULL &&
            arc_opening_heating->valuestring[0] != '\0') {
            const char *v = arc_opening_heating->valuestring;
            if (strcmp(v, "left") != 0 && strcmp(v, "right") != 0 && strcmp(v, "top") != 0 && strcmp(v, "bottom") != 0) {
                snprintf(msg, sizeof(msg), "widget %s: arc_opening must be left|right|top|bottom",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "sensor") == 0) {
        cJSON *style_variant = cJSON_GetObjectItemCaseSensitive(widget, "style_variant");
        cJSON *arc_opening = cJSON_GetObjectItemCaseSensitive(widget, "arc_opening");
        cJSON *sensor_min = cJSON_GetObjectItemCaseSensitive(widget, "sensor_min");
        cJSON *sensor_max = cJSON_GetObjectItemCaseSensitive(widget, "sensor_max");
        if (cJSON_IsString(style_variant) && style_variant->valuestring != NULL &&
            style_variant->valuestring[0] != '\0') {
            const char *v = style_variant->valuestring;
            if (strcmp(v, "default") != 0 && strcmp(v, "percent") != 0 &&
                strcmp(v, "percent_bar") != 0 && strcmp(v, "arc") != 0 && strcmp(v, "arc_semi") != 0) {
                snprintf(msg, sizeof(msg), "widget %s: style_variant must be default|percent|percent_bar|arc|arc_semi",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
        if (cJSON_IsString(arc_opening) && arc_opening->valuestring != NULL &&
            arc_opening->valuestring[0] != '\0') {
            const char *v = arc_opening->valuestring;
            if (strcmp(v, "left") != 0 && strcmp(v, "right") != 0 && strcmp(v, "top") != 0 && strcmp(v, "bottom") != 0) {
                snprintf(msg, sizeof(msg), "widget %s: arc_opening must be left|right|top|bottom",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
        if (sensor_min != NULL && !cJSON_IsNumber(sensor_min)) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_min must be a number",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (sensor_max != NULL && !cJSON_IsNumber(sensor_max)) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_max must be a number",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (cJSON_IsNumber(sensor_min) && cJSON_IsNumber(sensor_max) && sensor_max->valueint <= sensor_min->valueint) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_max must be greater than sensor_min",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "monitor_tile") == 0) {
        cJSON *style_variant = cJSON_GetObjectItemCaseSensitive(widget, "style_variant");
        cJSON *arc_opening = cJSON_GetObjectItemCaseSensitive(widget, "arc_opening");
        cJSON *sensor_min = cJSON_GetObjectItemCaseSensitive(widget, "sensor_min");
        cJSON *sensor_max = cJSON_GetObjectItemCaseSensitive(widget, "sensor_max");
        cJSON *sub_entity_ids = cJSON_GetObjectItemCaseSensitive(widget, "sub_entity_ids");

        if (cJSON_IsString(style_variant) && style_variant->valuestring != NULL &&
            style_variant->valuestring[0] != '\0') {
            const char *v = style_variant->valuestring;
            if (strcmp(v, "default") != 0 && strcmp(v, "percent") != 0 &&
                strcmp(v, "percent_bar") != 0 && strcmp(v, "arc") != 0 &&
                strcmp(v, "arc_semi") != 0 && strcmp(v, "gauge") != 0 &&
                strcmp(v, "gauge_needle") != 0 && strcmp(v, "bars") != 0 &&
                strcmp(v, "signal_bars") != 0 && strcmp(v, "hud") != 0 &&
                strcmp(v, "ring") != 0) {
                snprintf(msg, sizeof(msg), "widget %s: bad style_variant",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
        if (cJSON_IsString(arc_opening) && arc_opening->valuestring != NULL &&
            arc_opening->valuestring[0] != '\0') {
            const char *v = arc_opening->valuestring;
            if (strcmp(v, "left") != 0 && strcmp(v, "right") != 0 && strcmp(v, "top") != 0 && strcmp(v, "bottom") != 0) {
                snprintf(msg, sizeof(msg), "widget %s: arc_opening must be left|right|top|bottom",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
        if (sensor_min != NULL && !cJSON_IsNumber(sensor_min)) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_min must be a number",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (sensor_max != NULL && !cJSON_IsNumber(sensor_max)) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_max must be a number",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (cJSON_IsNumber(sensor_min) && cJSON_IsNumber(sensor_max) && sensor_max->valueint <= sensor_min->valueint) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_max must be greater than sensor_min",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (cJSON_IsString(sub_entity_ids) && sub_entity_ids->valuestring != NULL &&
            sub_entity_ids->valuestring[0] != '\0') {
            size_t count = 0;
            if (!is_valid_entity_id_list(sub_entity_ids->valuestring, APP_MAX_MONITOR_SUBS, &count)) {
                snprintf(msg, sizeof(msg), "widget %s: sub_entity_ids must list up to %d valid entity ids",
                    cJSON_IsString(id) ? id->valuestring : "?", APP_MAX_MONITOR_SUBS);
                layout_validation_add(result, msg);
            }
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "temp_tile") == 0) {
        cJSON *style_variant = cJSON_GetObjectItemCaseSensitive(widget, "style_variant");
        cJSON *sensor_min = cJSON_GetObjectItemCaseSensitive(widget, "sensor_min");
        cJSON *sensor_max = cJSON_GetObjectItemCaseSensitive(widget, "sensor_max");

        if (cJSON_IsString(style_variant) && style_variant->valuestring != NULL &&
            style_variant->valuestring[0] != '\0') {
            const char *v = style_variant->valuestring;
            if (strcmp(v, "graphic") != 0 && strcmp(v, "text") != 0) {
                snprintf(msg, sizeof(msg), "widget %s: style_variant must be graphic|text",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
        if (sensor_min != NULL && !cJSON_IsNumber(sensor_min)) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_min must be a number",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (sensor_max != NULL && !cJSON_IsNumber(sensor_max)) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_max must be a number",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (cJSON_IsNumber(sensor_min) && cJSON_IsNumber(sensor_max) && sensor_max->valueint <= sensor_min->valueint) {
            snprintf(msg, sizeof(msg), "widget %s: sensor_max must be greater than sensor_min",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "entity_list") == 0) {
        cJSON *entity_ids = cJSON_GetObjectItemCaseSensitive(widget, "entity_ids");
        size_t count = 0;
        if (!cJSON_IsString(entity_ids) || entity_ids->valuestring == NULL ||
            !is_valid_entity_id_list(entity_ids->valuestring, APP_MAX_ENTITY_LIST_ROWS, &count)) {
            snprintf(msg, sizeof(msg), "widget %s: entity_ids must list up to %d valid entity ids",
                cJSON_IsString(id) ? id->valuestring : "?", APP_MAX_ENTITY_LIST_ROWS);
            layout_validation_add(result, msg);
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "sensor_tile") == 0) {
        cJSON *entity_ids = cJSON_GetObjectItemCaseSensitive(widget, "entity_ids");
        if (entity_ids != NULL &&
            (!cJSON_IsString(entity_ids) || entity_ids->valuestring == NULL)) {
            snprintf(msg, sizeof(msg),
                "widget %s: entity_ids must list up to %d 'Label=entity' pairs (empty entity hides a row)",
                cJSON_IsString(id) ? id->valuestring : "?", APP_MAX_SENSOR_TILE_ROWS);
            layout_validation_add(result, msg);
        } else if (entity_ids->valuestring != NULL) {
            const char *reason = NULL;
            if (!is_valid_labeled_entity_list(entity_ids->valuestring, APP_MAX_SENSOR_TILE_ROWS, &reason)) {
                snprintf(msg, sizeof(msg),
                    "widget %s: entity_ids: %s",
                    cJSON_IsString(id) ? id->valuestring : "?",
                    reason != NULL ? reason : "must list up to 24 'Label=entity' pairs");
                layout_validation_add(result, msg);
            }
        }

        static const char *const font_keys[] = {
            "sensor_tile_title_font_px", "sensor_tile_row_font_px",
            "sensor_tile_ip_font_px", "sensor_tile_power_font_px",
            "sensor_tile_ports_font_px",
        };
        for (size_t k = 0; k < sizeof(font_keys) / sizeof(font_keys[0]); k++) {
            cJSON *font_px = cJSON_GetObjectItemCaseSensitive(widget, font_keys[k]);
            if (font_px != NULL && (!cJSON_IsNumber(font_px) || font_px->valueint < 0 || font_px->valueint > 64)) {
                snprintf(msg, sizeof(msg), "widget %s: %s must be a number from 0 to 64 (0 = auto)",
                    cJSON_IsString(id) ? id->valuestring : "?", font_keys[k]);
                layout_validation_add(result, msg);
            }
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "roborock_tile") == 0) {
        if (cJSON_IsString(secondary_entity_id) && secondary_entity_id->valuestring != NULL &&
            secondary_entity_id->valuestring[0] != '\0') {
            if (!is_valid_entity_id(secondary_entity_id->valuestring) ||
                !entity_in_domain(secondary_entity_id->valuestring, "image")) {
                snprintf(msg, sizeof(msg), "widget %s: secondary_entity_id must be image.*",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "slider") == 0) {
        if (slider_direction != NULL) {
            if (!cJSON_IsString(slider_direction) || slider_direction->valuestring == NULL ||
                !is_valid_slider_direction(slider_direction->valuestring)) {
                snprintf(msg, sizeof(msg),
                    "widget %s: slider_direction must be auto|left_to_right|right_to_left|bottom_to_top|top_to_bottom",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }

        if (slider_accent_color != NULL) {
            if (!cJSON_IsString(slider_accent_color) || slider_accent_color->valuestring == NULL ||
                !is_valid_hex_rgb_color(slider_accent_color->valuestring)) {
                snprintf(msg, sizeof(msg), "widget %s: slider_accent_color must be hex RGB",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "binary_sensor") == 0) {
        cJSON *binary_color_on = cJSON_GetObjectItemCaseSensitive(widget, "binary_color_on");
        cJSON *binary_color_off = cJSON_GetObjectItemCaseSensitive(widget, "binary_color_off");
        cJSON *binary_show_title = cJSON_GetObjectItemCaseSensitive(widget, "binary_show_title");
        if (binary_color_on != NULL && !cJSON_IsString(binary_color_on)) {
            snprintf(msg, sizeof(msg), "widget %s: binary_color_on must be a string", cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (binary_color_off != NULL && !cJSON_IsString(binary_color_off)) {
            snprintf(msg, sizeof(msg), "widget %s: binary_color_off must be a string", cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (binary_color_on != NULL && cJSON_IsString(binary_color_on) && binary_color_on->valuestring != NULL &&
            binary_color_on->valuestring[0] != '\0' && !is_valid_hex_rgb_color(binary_color_on->valuestring)) {
            snprintf(msg, sizeof(msg), "widget %s: binary_color_on must be hex RGB", cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (binary_color_off != NULL && cJSON_IsString(binary_color_off) && binary_color_off->valuestring != NULL &&
            binary_color_off->valuestring[0] != '\0' && !is_valid_hex_rgb_color(binary_color_off->valuestring)) {
            snprintf(msg, sizeof(msg), "widget %s: binary_color_off must be hex RGB", cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
        if (binary_show_title != NULL && !cJSON_IsBool(binary_show_title)) {
            snprintf(msg, sizeof(msg), "widget %s: binary_show_title must be a boolean", cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "button") == 0) {
        if (button_accent_color != NULL) {
            if (!cJSON_IsString(button_accent_color) || button_accent_color->valuestring == NULL ||
                !is_valid_hex_rgb_color(button_accent_color->valuestring)) {
                snprintf(msg, sizeof(msg), "widget %s: button_accent_color must be hex RGB",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }

        if (button_mode != NULL) {
            if (!cJSON_IsString(button_mode) || button_mode->valuestring == NULL ||
                !is_valid_button_mode(button_mode->valuestring)) {
                snprintf(msg, sizeof(msg), "widget %s: button_mode must be auto|play_pause|stop|next|previous",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            } else if (button_mode_requires_media_player(button_mode->valuestring) &&
                       cJSON_IsString(entity_id) && entity_id->valuestring != NULL &&
                       !entity_in_domain(entity_id->valuestring, "media_player")) {
                snprintf(msg, sizeof(msg), "widget %s: button_mode %s requires media_player.* entity_id",
                    cJSON_IsString(id) ? id->valuestring : "?",
                    button_mode->valuestring);
                layout_validation_add(result, msg);
            }
        }
    }

    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "graph") == 0) {
        if (graph_line_color != NULL) {
            if (!cJSON_IsString(graph_line_color) || graph_line_color->valuestring == NULL ||
                !is_valid_hex_rgb_color(graph_line_color->valuestring)) {
                snprintf(msg, sizeof(msg), "widget %s: graph_line_color must be hex RGB",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }

        if (graph_point_count != NULL) {
            if (!cJSON_IsNumber(graph_point_count) ||
                graph_point_count->valuedouble < (double)GRAPH_POINT_COUNT_MIN ||
                graph_point_count->valuedouble > (double)GRAPH_POINT_COUNT_MAX ||
                (double)graph_point_count->valueint != graph_point_count->valuedouble) {
                snprintf(msg, sizeof(msg), "widget %s: graph_point_count must be integer %d..%d",
                    cJSON_IsString(id) ? id->valuestring : "?",
                    GRAPH_POINT_COUNT_MIN,
                    GRAPH_POINT_COUNT_MAX);
                layout_validation_add(result, msg);
            }
        }

        if (graph_time_window_min != NULL) {
            if (!cJSON_IsNumber(graph_time_window_min) ||
                graph_time_window_min->valuedouble < (double)GRAPH_TIME_WINDOW_MIN_MIN ||
                graph_time_window_min->valuedouble > (double)GRAPH_TIME_WINDOW_MIN_MAX ||
                (double)graph_time_window_min->valueint != graph_time_window_min->valuedouble) {
                snprintf(msg, sizeof(msg), "widget %s: graph_time_window_min must be integer %d..%d",
                    cJSON_IsString(id) ? id->valuestring : "?",
                    GRAPH_TIME_WINDOW_MIN_MIN,
                    GRAPH_TIME_WINDOW_MIN_MAX);
                layout_validation_add(result, msg);
            }
        }

        if (graph_display_mode != NULL) {
            bool valid_mode = false;
            if (cJSON_IsString(graph_display_mode) && graph_display_mode->valuestring != NULL) {
                for (size_t i = 0; i < sizeof(GRAPH_DISPLAY_MODES) / sizeof(GRAPH_DISPLAY_MODES[0]); ++i) {
                    if (strcmp(graph_display_mode->valuestring, GRAPH_DISPLAY_MODES[i]) == 0) {
                        valid_mode = true;
                        break;
                    }
                }
            }
            if (!valid_mode) {
                snprintf(msg, sizeof(msg),
                    "widget %s: graph_display_mode must be line|line_smooth|line_smooth_points|bars",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }

        if (graph_bar_bucket_min != NULL) {
            bool valid_bucket = false;
            if (cJSON_IsNumber(graph_bar_bucket_min) &&
                (double)graph_bar_bucket_min->valueint == graph_bar_bucket_min->valuedouble) {
                for (size_t i = 0; i < sizeof(GRAPH_BAR_BUCKET_MIN_ALLOWED) / sizeof(GRAPH_BAR_BUCKET_MIN_ALLOWED[0]); ++i) {
                    if (graph_bar_bucket_min->valueint == GRAPH_BAR_BUCKET_MIN_ALLOWED[i]) {
                        valid_bucket = true;
                        break;
                    }
                }
            }
            if (!valid_bucket) {
                snprintf(msg, sizeof(msg),
                    "widget %s: graph_bar_bucket_min must be one of 5|10|15|30",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            }
        }
    }

    if (!cJSON_IsObject(rect)) {
        snprintf(msg, sizeof(msg), "widget %s: missing rect", cJSON_IsString(id) ? id->valuestring : "?");
        layout_validation_add(result, msg);
    } else {
        cJSON *x = cJSON_GetObjectItemCaseSensitive(rect, "x");
        cJSON *y = cJSON_GetObjectItemCaseSensitive(rect, "y");
        cJSON *w = cJSON_GetObjectItemCaseSensitive(rect, "w");
        cJSON *h = cJSON_GetObjectItemCaseSensitive(rect, "h");
        if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(w) || !cJSON_IsNumber(h)) {
            snprintf(msg, sizeof(msg), "widget %s: rect values must be numbers",
                cJSON_IsString(id) ? id->valuestring : "?");
            layout_validation_add(result, msg);
        } else {
            int rx = x->valueint;
            int ry = y->valueint;
            int rw = w->valueint;
            int rh = h->valueint;
            widget_size_limits_t limits = widget_size_limits_for_type(cJSON_IsString(type) ? type->valuestring : NULL);
            if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 || (rx + rw) > APP_CONTENT_BOX_WIDTH ||
                (ry + rh) > APP_CONTENT_BOX_HEIGHT) {
                snprintf(msg, sizeof(msg), "widget %s: rect out of bounds for content box",
                    cJSON_IsString(id) ? id->valuestring : "?");
                layout_validation_add(result, msg);
            } else if (rw < limits.min_w || rh < limits.min_h || rw > limits.max_w || rh > limits.max_h) {
                snprintf(msg, sizeof(msg), "widget %s: size must be %dx%d..%dx%d",
                    cJSON_IsString(id) ? id->valuestring : "?",
                    limits.min_w,
                    limits.min_h,
                    limits.max_w,
                    limits.max_h);
                layout_validation_add(result, msg);
            }
        }
    }

    return true;
}

bool layout_validate_json(const char *json, layout_validation_result_t *result)
{
    layout_validation_clear(result);
    if (json == NULL) {
        layout_validation_add(result, "layout json is null");
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        layout_validation_add(result, "layout json parse error");
        return false;
    }

    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");

    if (!cJSON_IsNumber(version) || version->valueint != 1) {
        layout_validation_add(result, "layout version must be 1");
    }

    if (!cJSON_IsArray(pages)) {
        layout_validation_add(result, "pages must be an array");
        cJSON_Delete(root);
        return false;
    }

    int page_count = cJSON_GetArraySize(pages);
    if (page_count <= 0) {
        layout_validation_add(result, "at least one page required");
    }
    if (page_count > APP_MAX_PAGES) {
        layout_validation_add(result, "too many pages");
    }

    char *known_page_ids = calloc(APP_MAX_PAGES, APP_MAX_PAGE_ID_LEN);
    char *known_widget_ids = calloc(APP_MAX_WIDGETS_TOTAL, APP_MAX_WIDGET_ID_LEN);
    if (known_page_ids == NULL || known_widget_ids == NULL) {
        free(known_page_ids);
        free(known_widget_ids);
        layout_validation_add(result, "out of memory during validation");
        cJSON_Delete(root);
        return false;
    }

    size_t known_page_ids_len = 0;
    size_t known_widget_ids_len = 0;

    for (int i = 0; i < page_count; i++) {
        cJSON *page = cJSON_GetArrayItem(pages, i);
        if (!cJSON_IsObject(page)) {
            layout_validation_add(result, "page entry must be object");
            continue;
        }

        cJSON *page_id = cJSON_GetObjectItemCaseSensitive(page, "id");
        cJSON *page_type = cJSON_GetObjectItemCaseSensitive(page, "type");
        cJSON *widgets = cJSON_GetObjectItemCaseSensitive(page, "widgets");
        char msg[96];
        bool is_energy_dashboard_page = false;
        bool is_xiaozhi_page = false;
        bool is_cameras_page = false;

        if (!cJSON_IsString(page_id) || page_id->valuestring == NULL || strlen(page_id->valuestring) == 0U) {
            snprintf(msg, sizeof(msg), "page[%u]: invalid id", (unsigned)i);
            layout_validation_add(result, msg);
        } else if (strlen(page_id->valuestring) >= APP_MAX_PAGE_ID_LEN) {
            snprintf(msg, sizeof(msg), "page id too long: %s", page_id->valuestring);
            layout_validation_add(result, msg);
        } else if (str_in_list(page_id->valuestring, known_page_ids, APP_MAX_PAGE_ID_LEN, known_page_ids_len)) {
            snprintf(msg, sizeof(msg), "duplicate page id: %s", page_id->valuestring);
            layout_validation_add(result, msg);
        } else if (known_page_ids_len < APP_MAX_PAGES) {
            char *dst = known_page_ids + (known_page_ids_len * APP_MAX_PAGE_ID_LEN);
            snprintf(dst, APP_MAX_PAGE_ID_LEN, "%s", page_id->valuestring);
            known_page_ids_len++;
        }

        if (page_type != NULL) {
            if (!cJSON_IsString(page_type) || page_type->valuestring == NULL ||
                !is_supported_page_type(page_type->valuestring)) {
                snprintf(msg, sizeof(msg), "page %s: unsupported type", cJSON_IsString(page_id) ? page_id->valuestring : "?");
                layout_validation_add(result, msg);
            } else if (strcmp(page_type->valuestring, "energy_dashboard") == 0) {
                is_energy_dashboard_page = true;
            } else if (strcmp(page_type->valuestring, "xiaozhi") == 0) {
                is_xiaozhi_page = true;
            } else if (strcmp(page_type->valuestring, "cameras") == 0) {
                is_cameras_page = true;
            }
        }

        if (is_energy_dashboard_page) {
            validate_energy_page(page, cJSON_IsString(page_id) ? page_id->valuestring : "?", result);
            if (widgets != NULL && !cJSON_IsArray(widgets)) {
                snprintf(msg, sizeof(msg), "page %s: widgets must be array", cJSON_IsString(page_id) ? page_id->valuestring : "?");
                layout_validation_add(result, msg);
            } else if (cJSON_IsArray(widgets) && cJSON_GetArraySize(widgets) > 0) {
                snprintf(msg, sizeof(msg), "page %s: energy_dashboard pages cannot contain widgets",
                    cJSON_IsString(page_id) ? page_id->valuestring : "?");
                layout_validation_add(result, msg);
            }
            continue;
        }

        if (is_xiaozhi_page) {
            if (widgets != NULL && !cJSON_IsArray(widgets)) {
                snprintf(msg, sizeof(msg), "page %s: widgets must be array", cJSON_IsString(page_id) ? page_id->valuestring : "?");
                layout_validation_add(result, msg);
            } else if (cJSON_IsArray(widgets) && cJSON_GetArraySize(widgets) > 0) {
                snprintf(msg, sizeof(msg), "page %s: xiaozhi pages cannot contain widgets",
                    cJSON_IsString(page_id) ? page_id->valuestring : "?");
                layout_validation_add(result, msg);
            }
            continue;
        }

        if (is_cameras_page) {
            if (widgets != NULL && !cJSON_IsArray(widgets)) {
                snprintf(msg, sizeof(msg), "page %s: widgets must be array", cJSON_IsString(page_id) ? page_id->valuestring : "?");
                layout_validation_add(result, msg);
            } else if (cJSON_IsArray(widgets) && cJSON_GetArraySize(widgets) > 0) {
                snprintf(msg, sizeof(msg), "page %s: cameras pages cannot contain widgets",
                    cJSON_IsString(page_id) ? page_id->valuestring : "?");
                layout_validation_add(result, msg);
            }
            continue;
        }

        if (!cJSON_IsArray(widgets)) {
            snprintf(msg, sizeof(msg), "page %s: widgets must be array", cJSON_IsString(page_id) ? page_id->valuestring : "?");
            layout_validation_add(result, msg);
            continue;
        }

        int widget_count = cJSON_GetArraySize(widgets);
        if (widget_count > APP_MAX_WIDGETS_PER_PAGE) {
            snprintf(msg, sizeof(msg), "page %s: too many widgets", cJSON_IsString(page_id) ? page_id->valuestring : "?");
            layout_validation_add(result, msg);
        }

        for (int w = 0; w < widget_count; w++) {
            cJSON *widget = cJSON_GetArrayItem(widgets, w);
            if (!cJSON_IsObject(widget)) {
                layout_validation_add(result, "widget entry must be object");
                continue;
            }
            validate_widget(widget, known_widget_ids, known_widget_ids_len, (size_t)i, (size_t)w, result);
            cJSON *id = cJSON_GetObjectItemCaseSensitive(widget, "id");
            if (cJSON_IsString(id) && id->valuestring != NULL && strlen(id->valuestring) > 0 &&
                strlen(id->valuestring) < APP_MAX_WIDGET_ID_LEN &&
                !str_in_list(id->valuestring, known_widget_ids, APP_MAX_WIDGET_ID_LEN, known_widget_ids_len) &&
                known_widget_ids_len < APP_MAX_WIDGETS_TOTAL) {
                char *dst = known_widget_ids + (known_widget_ids_len * APP_MAX_WIDGET_ID_LEN);
                snprintf(dst, APP_MAX_WIDGET_ID_LEN, "%s", id->valuestring);
                known_widget_ids_len++;
            }
        }
    }

    free(known_page_ids);
    free(known_widget_ids);
    cJSON_Delete(root);
    return result->count == 0;
}
