/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * monitor_tile: composite server/PC monitoring tile.
 * Primary entity is rendered as a gauge (arc / arc_semi / bar) or plain
 * big number; an optional comma-separated list of sub_entity_ids renders
 * as a compact stats grid underneath (temperature, power, fan, disk, ...).
 */
#include "ui/ui_widget_factory.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_memory.h"
#include "ui/theme/theme_default.h"

#if LV_FONT_MONTSERRAT_24
#define MONITOR_VALUE_FONT_SMALL APP_FONT_TEXT_24
#elif LV_FONT_MONTSERRAT_22
#define MONITOR_VALUE_FONT_SMALL APP_FONT_TEXT_22
#elif LV_FONT_MONTSERRAT_20
#define MONITOR_VALUE_FONT_SMALL APP_FONT_TEXT_20
#else
#define MONITOR_VALUE_FONT_SMALL APP_FONT_TEXT_20
#endif

#if LV_FONT_MONTSERRAT_44
#define MONITOR_VALUE_FONT_LARGE (&lv_font_montserrat_44)
#elif LV_FONT_MONTSERRAT_40
#define MONITOR_VALUE_FONT_LARGE (&lv_font_montserrat_40)
#elif LV_FONT_MONTSERRAT_36
#define MONITOR_VALUE_FONT_LARGE (&lv_font_montserrat_36)
#elif LV_FONT_MONTSERRAT_34
#define MONITOR_VALUE_FONT_LARGE APP_FONT_TEXT_34
#elif LV_FONT_MONTSERRAT_32
#define MONITOR_VALUE_FONT_LARGE (&lv_font_montserrat_32)
#else
#define MONITOR_VALUE_FONT_LARGE MONITOR_VALUE_FONT_SMALL
#endif

#if LV_FONT_MONTSERRAT_32
#define MONITOR_VALUE_FONT_MEDIUM (&lv_font_montserrat_32)
#elif LV_FONT_MONTSERRAT_28
#define MONITOR_VALUE_FONT_MEDIUM APP_FONT_TEXT_28
#elif LV_FONT_MONTSERRAT_24
#define MONITOR_VALUE_FONT_MEDIUM APP_FONT_TEXT_24
#else
#define MONITOR_VALUE_FONT_MEDIUM MONITOR_VALUE_FONT_SMALL
#endif

typedef enum {
    MONITOR_STYLE_DEFAULT = 0,
    MONITOR_STYLE_PERCENT,
    MONITOR_STYLE_ARC,
    MONITOR_STYLE_ARC_SEMI,
    MONITOR_STYLE_GAUGE,
    MONITOR_STYLE_BARS,
    MONITOR_STYLE_HUD,
} monitor_style_t;

#define MONITOR_BAR_COUNT 8
#define MONITOR_TICK_COUNT 21

typedef enum {
    MONITOR_OPENING_LEFT = 0,
    MONITOR_OPENING_RIGHT,
    MONITOR_OPENING_TOP,
    MONITOR_OPENING_BOTTOM,
} monitor_opening_t;

typedef struct {
    char entity_id[APP_MAX_ENTITY_ID_LEN];
    lv_obj_t *name_label;
    lv_obj_t *value_label;
} monitor_sub_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *status_dot;
    lv_obj_t *value_label;
    lv_obj_t *bar;
    lv_obj_t *arc;
    lv_obj_t *glow_arc;
    lv_obj_t *ticks[MONITOR_TICK_COUNT];
    lv_obj_t *unit_label;
    lv_obj_t *needle;
    lv_obj_t *bars[MONITOR_BAR_COUNT];
    monitor_style_t style;
    monitor_opening_t opening;
    int min;
    int max;
    bool unavailable;
    int16_t arc_value;
    int32_t needle_rot;
    uint8_t sub_count;
    monitor_sub_t subs[APP_MAX_MONITOR_SUBS];
} monitor_ctx_t;

static bool monitor_state_is_unavailable(const char *state_text)
{
    if (state_text == NULL || state_text[0] == '\0') {
        return true;
    }
    return strcmp(state_text, "unavailable") == 0 || strcmp(state_text, "unknown") == 0;
}

static bool monitor_parse_float(const char *text, float *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    char *end = NULL;
    float v = strtof(text, &end);
    if (end == text) {
        return false;
    }
    *out = v;
    return true;
}

static int monitor_clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static bool monitor_unit_is_percent(const char *unit)
{
    return unit != NULL && strcmp(unit, "%") == 0;
}

static int monitor_compute_percent(float value, const char *unit, int min, int max)
{
    if (monitor_unit_is_percent(unit)) {
        return monitor_clamp_int((int)(value + 0.5f), 0, 100);
    }
    if (max > min) {
        int p = (int)(((value - (float)min) / ((float)max - (float)min)) * 100.0f + 0.5f);
        return monitor_clamp_int(p, 0, 100);
    }
    if (value >= 0.0f && value <= 100.0f) {
        return monitor_clamp_int((int)(value + 0.5f), 0, 100);
    }
    return 0;
}

static monitor_style_t monitor_style_from_variant(const char *variant)
{
    if (variant == NULL || variant[0] == '\0' || strcmp(variant, "default") == 0) {
        return MONITOR_STYLE_DEFAULT;
    }
    if (strcmp(variant, "percent") == 0 || strcmp(variant, "percent_bar") == 0) {
        return MONITOR_STYLE_PERCENT;
    }
    if (strcmp(variant, "arc") == 0) {
        return MONITOR_STYLE_ARC;
    }
    if (strcmp(variant, "arc_semi") == 0) {
        return MONITOR_STYLE_ARC_SEMI;
    }
    if (strcmp(variant, "gauge") == 0 || strcmp(variant, "gauge_needle") == 0) {
        return MONITOR_STYLE_GAUGE;
    }
    if (strcmp(variant, "bars") == 0 || strcmp(variant, "signal_bars") == 0) {
        return MONITOR_STYLE_BARS;
    }
    if (strcmp(variant, "hud") == 0 || strcmp(variant, "ring") == 0) {
        return MONITOR_STYLE_HUD;
    }
    return MONITOR_STYLE_DEFAULT;
}

static monitor_opening_t monitor_opening_from_variant(const char *variant)
{
    if (variant != NULL) {
        if (strcmp(variant, "right") == 0) {
            return MONITOR_OPENING_RIGHT;
        }
        if (strcmp(variant, "top") == 0) {
            return MONITOR_OPENING_TOP;
        }
        if (strcmp(variant, "bottom") == 0) {
            return MONITOR_OPENING_BOTTOM;
        }
    }
    return MONITOR_OPENING_LEFT;
}

static void monitor_arc_angles(monitor_style_t style, monitor_opening_t opening, uint16_t *bg_start, uint16_t *bg_end)
{
    if (style == MONITOR_STYLE_ARC || style == MONITOR_STYLE_GAUGE || style == MONITOR_STYLE_HUD) {
        *bg_start = 135;
        *bg_end = 45;
        return;
    }
    switch (opening) {
        case MONITOR_OPENING_LEFT:
            *bg_start = 270;
            *bg_end = 90;
            break;
        case MONITOR_OPENING_RIGHT:
            *bg_start = 90;
            *bg_end = 270;
            break;
        case MONITOR_OPENING_TOP:
            *bg_start = 0;
            *bg_end = 180;
            break;
        case MONITOR_OPENING_BOTTOM:
        default:
            *bg_start = 180;
            *bg_end = 360;
            break;
    }
}

static const lv_font_t *monitor_pick_value_font(const monitor_ctx_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL) {
        return MONITOR_VALUE_FONT_MEDIUM;
    }

    lv_coord_t w = lv_obj_get_width(ctx->card);
    lv_coord_t h = lv_obj_get_height(ctx->card);
    lv_coord_t min_dim = (w < h) ? w : h;

    if (min_dim >= 260) {
        return MONITOR_VALUE_FONT_LARGE;
    }
    if (min_dim >= 160) {
        return MONITOR_VALUE_FONT_MEDIUM;
    }
    return MONITOR_VALUE_FONT_SMALL;
}

static void monitor_format_state(const ha_state_t *state, char *buf, size_t len)
{
    if (state == NULL || buf == NULL || len == 0) {
        return;
    }
    if (monitor_state_is_unavailable(state->state)) {
        snprintf(buf, len, "--");
        return;
    }

    const char *unit = NULL;
    cJSON *attrs = cJSON_Parse(state->attributes_json);
    if (attrs != NULL) {
        cJSON *unit_item = cJSON_GetObjectItemCaseSensitive(attrs, "unit_of_measurement");
        if (cJSON_IsString(unit_item) && unit_item->valuestring != NULL) {
            unit = unit_item->valuestring;
        }
    }

    float value = 0.0f;
    bool numeric = monitor_parse_float(state->state, &value);
    if (numeric && unit != NULL && unit[0] != '\0') {
        if (monitor_unit_is_percent(unit)) {
            snprintf(buf, len, "%s%%", state->state);
        } else {
            snprintf(buf, len, "%s %s", state->state, unit);
        }
    } else {
        snprintf(buf, len, "%s", state->state);
    }

    if (attrs != NULL) {
        cJSON_Delete(attrs);
    }
}

static void monitor_set_status(monitor_ctx_t *ctx, bool available)
{
    if (ctx == NULL || ctx->status_dot == NULL) {
        return;
    }
    lv_obj_set_style_bg_color(
        ctx->status_dot,
        lv_color_hex(available ? APP_UI_COLOR_STATE_ON : APP_UI_COLOR_CARD_BORDER),
        LV_PART_MAIN);
}

static void monitor_set_value_text(monitor_ctx_t *ctx, const char *text)
{
    if (ctx == NULL || ctx->value_label == NULL) {
        return;
    }
    lv_label_set_text(ctx->value_label, (text != NULL && text[0] != '\0') ? text : "--");
}

static void monitor_anim_arc_cb(void *var, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)var, (int16_t)v);
}

static void monitor_anim_needle_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void monitor_animate_to(monitor_ctx_t *ctx, int16_t pct)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->arc != NULL && ctx->arc_value != pct) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ctx->arc);
        lv_anim_set_exec_cb(&a, monitor_anim_arc_cb);
        lv_anim_set_values(&a, ctx->arc_value, pct);
        lv_anim_set_time(&a, 450);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
        if (ctx->glow_arc != NULL) {
            lv_anim_t g;
            lv_anim_init(&g);
            lv_anim_set_var(&g, ctx->glow_arc);
            lv_anim_set_exec_cb(&g, monitor_anim_arc_cb);
            lv_anim_set_values(&g, ctx->arc_value, pct);
            lv_anim_set_time(&g, 450);
            lv_anim_set_path_cb(&g, lv_anim_path_ease_out);
            lv_anim_start(&g);
        }
        ctx->arc_value = pct;
    }
    if (ctx->needle != NULL) {
        int32_t target = 1350 + (int32_t)pct * 27;
        if (ctx->needle_rot != target) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ctx->needle);
            lv_anim_set_exec_cb(&a, monitor_anim_needle_cb);
            lv_anim_set_values(&a, ctx->needle_rot, target);
            lv_anim_set_time(&a, 450);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
            ctx->needle_rot = target;
        }
    }
}

static lv_color_t monitor_threshold_color(int pct)
{
    if (pct >= 85) {
        return lv_color_hex(APP_UI_COLOR_ERROR);
    }
    if (pct >= 60) {
        return lv_color_hex(0xF0A030);
    }
    return lv_color_hex(APP_UI_COLOR_STATE_ON);
}

/* Smooth cyan -> green -> amber -> red gradient for the HUD ring, instead of
 * three hard thresholds.  The stop colours match the neon theme: the ring reads
 * as "cool = fine, warm = busy, red = critical" without a visible step. */
static lv_color_t monitor_hud_gradient(int pct)
{
    const lv_color_t c_cool = lv_color_hex(APP_UI_COLOR_STATE_ON);   /* neon cyan */
    const lv_color_t c_ok   = lv_color_hex(0x3DF0A4);                /* mint green */
    const lv_color_t c_warm = lv_color_hex(0xF0A030);                /* amber */
    const lv_color_t c_hot  = lv_color_hex(APP_UI_COLOR_ERROR);      /* red */

    if (pct <= 50) {
        return lv_color_mix(c_cool, c_ok, (uint8_t)(pct * 255 / 50));
    }
    if (pct <= 80) {
        return lv_color_mix(c_ok, c_warm, (uint8_t)((pct - 50) * 255 / 30));
    }
    if (pct >= 100) {
        return c_hot;
    }
    return lv_color_mix(c_warm, c_hot, (uint8_t)((pct - 80) * 255 / 20));
}

static int monitor_sub_area_height(const monitor_ctx_t *ctx)
{
    if (ctx->sub_count == 0) {
        return 0;
    }
    if (ctx->sub_count == 1) {
        return 28;
    }
    return 56;
}

static void monitor_apply_layout(monitor_ctx_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL || ctx->title_label == NULL || ctx->value_label == NULL) {
        return;
    }

    lv_obj_t *card = ctx->card;
    lv_obj_update_layout(card);

    lv_coord_t cw = lv_obj_get_width(card) - lv_obj_get_style_pad_left(card, LV_PART_MAIN) -
                    lv_obj_get_style_pad_right(card, LV_PART_MAIN);
    lv_coord_t ch = lv_obj_get_height(card) - lv_obj_get_style_pad_top(card, LV_PART_MAIN) -
                    lv_obj_get_style_pad_bottom(card, LV_PART_MAIN);
    if (cw < 40) {
        cw = 40;
    }
    if (ch < 60) {
        ch = 60;
    }

    lv_obj_set_style_text_font(ctx->title_label, APP_FONT_TEXT_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->title_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(ctx->title_label, cw - 22);
    lv_obj_set_pos(ctx->title_label, 0, 0);
    lv_obj_update_layout(ctx->title_label);
    lv_coord_t title_h = lv_obj_get_height(ctx->title_label);
    if (title_h < 20) {
        title_h = 20;
    }

    lv_obj_set_size(ctx->status_dot, 10, 10);
    lv_obj_align(ctx->status_dot, LV_ALIGN_TOP_RIGHT, 0, 4);

    lv_coord_t sub_h = monitor_sub_area_height(ctx);
    lv_coord_t gap = (ctx->sub_count > 0) ? 4 : 0;
    lv_coord_t main_h = ch - title_h - sub_h - gap;
    if (main_h < 36) {
        main_h = 36;
    }
    lv_coord_t main_y = title_h + gap;

    if (ctx->style == MONITOR_STYLE_DEFAULT) {
        lv_obj_set_style_text_font(ctx->value_label, monitor_pick_value_font(ctx), LV_PART_MAIN);
        lv_obj_set_style_text_align(ctx->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_size(ctx->value_label, cw, main_h);
        lv_obj_set_pos(ctx->value_label, 0, main_y);
    } else if (ctx->style == MONITOR_STYLE_PERCENT) {
        lv_obj_set_style_text_font(ctx->value_label, monitor_pick_value_font(ctx), LV_PART_MAIN);
        lv_obj_set_style_text_align(ctx->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_size(ctx->value_label, cw, main_h - 22);
        lv_obj_set_pos(ctx->value_label, 0, main_y);
        if (ctx->bar != NULL) {
            lv_obj_set_size(ctx->bar, cw, 12);
            lv_obj_set_pos(ctx->bar, 0, main_y + main_h - 20);
        }
    } else if (ctx->style == MONITOR_STYLE_BARS) {
        /* Discrete signal-style bars with the big value above. */
        lv_obj_set_style_text_font(ctx->value_label, monitor_pick_value_font(ctx), LV_PART_MAIN);
        lv_obj_set_style_text_align(ctx->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_coord_t bars_h = (main_h > 70) ? 26 : 18;
        lv_coord_t bars_y = main_y + main_h - bars_h;
        lv_obj_set_size(ctx->value_label, cw, main_h - bars_h - 4);
        lv_obj_set_pos(ctx->value_label, 0, main_y);

        lv_coord_t gap = 4;
        lv_coord_t bar_w = (cw - (MONITOR_BAR_COUNT - 1) * gap) / MONITOR_BAR_COUNT;
        if (bar_w < 4) {
            bar_w = 4;
        }
        lv_coord_t total_w = bar_w * MONITOR_BAR_COUNT + gap * (MONITOR_BAR_COUNT - 1);
        lv_coord_t start_x = (cw - total_w) / 2;
        for (int i = 0; i < MONITOR_BAR_COUNT; i++) {
            lv_obj_t *b = ctx->bars[i];
            if (b == NULL) {
                continue;
            }
            lv_obj_set_size(b, bar_w, bars_h);
            lv_obj_set_pos(b, start_x + i * (bar_w + gap), bars_y);
        }
    } else if (ctx->style == MONITOR_STYLE_HUD) {
        /* Neon ring: indicator + glow arcs share one diameter, a dotted scale
         * sits inside the ring and the value/unit are stacked in the middle.
         * The 30 px glow ring overshoots the arc by ~15 px, so reserve headroom
         * below the title so the glow never overlaps the title text. */
        lv_coord_t glow_overshoot = 18;
        lv_coord_t hud_y = main_y + glow_overshoot;
        lv_coord_t hud_h = main_h - glow_overshoot;
        if (hud_h < 60) {
            hud_h = 60;
        }
        lv_coord_t diam = (cw < hud_h) ? cw : hud_h;
        diam -= 10;
        if (diam < 60) {
            diam = 60;
        }
        lv_coord_t cx = cw / 2;
        lv_coord_t cy = hud_y + hud_h / 2;
        lv_coord_t vh = (diam >= 170) ? 48 : 36;

        lv_obj_set_style_text_align(ctx->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(ctx->value_label,
            (diam >= 170) ? MONITOR_VALUE_FONT_LARGE : MONITOR_VALUE_FONT_MEDIUM, LV_PART_MAIN);
        lv_obj_set_size(ctx->value_label, diam, vh);
        lv_obj_set_pos(ctx->value_label, cx - diam / 2, cy - vh + 2);

        if (ctx->unit_label != NULL) {
            lv_obj_set_style_text_font(ctx->unit_label, APP_FONT_TEXT_16, LV_PART_MAIN);
            lv_obj_set_size(ctx->unit_label, diam, 20);
            lv_obj_set_pos(ctx->unit_label, cx - diam / 2, cy + 4);
        }
        if (ctx->glow_arc != NULL) {
            lv_obj_set_size(ctx->glow_arc, diam, diam);
            lv_obj_set_pos(ctx->glow_arc, cx - diam / 2, cy - diam / 2);
        }
        if (ctx->arc != NULL) {
            lv_obj_set_size(ctx->arc, diam, diam);
            lv_obj_set_pos(ctx->arc, cx - diam / 2, cy - diam / 2);
        }

        lv_coord_t tick_r = diam / 2 - 26;
        if (tick_r < 10) {
            tick_r = 10;
        }
        for (int i = 0; i < MONITOR_TICK_COUNT; i++) {
            lv_obj_t *t = ctx->ticks[i];
            if (t == NULL) {
                continue;
            }
            float ang = (float)(135 + i * (270 / (MONITOR_TICK_COUNT - 1))) * 3.14159265f / 180.0f;
            lv_obj_set_size(t, 5, 5);
            lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_pos(t,
                cx + (lv_coord_t)(cosf(ang) * (float)tick_r) - 2,
                cy + (lv_coord_t)(sinf(ang) * (float)tick_r) - 2);
        }
    } else {
        /* arc / arc_semi / gauge */
        lv_coord_t arc_diam = (cw < main_h) ? cw : main_h;
        if (arc_diam < 40) {
            arc_diam = 40;
        }
        lv_obj_set_style_text_align(ctx->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        if (ctx->style == MONITOR_STYLE_GAUGE) {
            /* Speedometer layout: the needle sweeps the upper area, the value
             * sits in a dedicated band below the pivot so they never overlap. */
            lv_coord_t value_band = 34;
            lv_coord_t avail_h = main_h - value_band;
            if (avail_h < 40) {
                avail_h = 40;
            }
            if (arc_diam > avail_h) {
                arc_diam = avail_h;
            }
            if (arc_diam < 40) {
                arc_diam = 40;
            }
            lv_obj_set_style_text_font(
                ctx->value_label,
                (arc_diam >= 150) ? MONITOR_VALUE_FONT_MEDIUM : MONITOR_VALUE_FONT_SMALL,
                LV_PART_MAIN);
            lv_obj_set_size(ctx->value_label, cw, value_band);
            lv_obj_set_pos(ctx->value_label, 0, main_y + main_h - value_band);
            if (ctx->arc != NULL) {
                lv_obj_set_size(ctx->arc, arc_diam, arc_diam);
                lv_obj_set_pos(ctx->arc, (cw - arc_diam) / 2, main_y + (avail_h - arc_diam) / 2);
            }
            if (ctx->needle != NULL) {
                lv_coord_t needle_len = arc_diam / 2 - 8;
                if (needle_len < 12) {
                    needle_len = 12;
                }
                lv_obj_set_size(ctx->needle, needle_len, 3);
                lv_obj_set_style_transform_pivot_x(ctx->needle, 0, LV_PART_MAIN);
                lv_obj_set_style_transform_pivot_y(ctx->needle, 1, LV_PART_MAIN);
                lv_obj_set_pos(ctx->needle, cw / 2, main_y + (avail_h - arc_diam) / 2 + arc_diam / 2 - 1);
            }
        } else {
            lv_obj_set_style_text_font(
                ctx->value_label,
                (arc_diam >= 150) ? MONITOR_VALUE_FONT_LARGE :
                    ((arc_diam >= 90) ? MONITOR_VALUE_FONT_MEDIUM : MONITOR_VALUE_FONT_SMALL),
                LV_PART_MAIN);
            lv_obj_set_size(ctx->value_label, arc_diam, arc_diam);
            lv_obj_set_pos(ctx->value_label, (cw - arc_diam) / 2, main_y + (main_h - arc_diam) / 2);
            if (ctx->arc != NULL) {
                lv_obj_set_size(ctx->arc, arc_diam, arc_diam);
                lv_obj_set_pos(ctx->arc, (cw - arc_diam) / 2, main_y + (main_h - arc_diam) / 2);
            }
        }
    }

    /* Sub-stat rows anchored to the bottom of the tile. */
    if (ctx->sub_count == 0) {
        return;
    }

    lv_coord_t sub_y = ch - sub_h;
    lv_obj_set_style_text_font(ctx->value_label, lv_obj_get_style_text_font(ctx->value_label, LV_PART_MAIN), LV_PART_MAIN);

    for (uint8_t i = 0; i < ctx->sub_count; i++) {
        monitor_sub_t *sub = &ctx->subs[i];
        if (sub->name_label == NULL || sub->value_label == NULL) {
            continue;
        }

        lv_obj_set_style_text_font(sub->name_label, APP_FONT_TEXT_12, LV_PART_MAIN);
        lv_obj_set_style_text_font(sub->value_label, APP_FONT_TEXT_16, LV_PART_MAIN);

        if (ctx->sub_count == 1) {
            lv_obj_set_style_text_align(sub->name_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
            lv_obj_set_style_text_align(sub->value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
            lv_obj_set_size(sub->name_label, cw / 2, 28);
            lv_obj_set_size(sub->value_label, cw / 2, 28);
            lv_obj_set_pos(sub->name_label, 0, sub_y);
            lv_obj_set_pos(sub->value_label, cw / 2, sub_y);
        } else {
            lv_coord_t cell_w = (cw - 10) / 2;
            lv_coord_t row_h = 28;
            lv_coord_t col = (lv_coord_t)(i % 2);
            lv_coord_t row = (lv_coord_t)(i / 2);
            lv_obj_set_style_text_align(sub->name_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
            lv_obj_set_style_text_align(sub->value_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
            lv_obj_set_size(sub->name_label, cell_w, 12);
            lv_obj_set_size(sub->value_label, cell_w, 14);
            lv_obj_set_pos(sub->name_label, col * (cell_w + 10), sub_y + row * row_h);
            lv_obj_set_pos(sub->value_label, col * (cell_w + 10), sub_y + row * row_h + 13);
        }
    }
}

static void monitor_apply_unavailable(monitor_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->unavailable = true;
    monitor_set_status(ctx, false);
    monitor_set_value_text(ctx, "--");
    if (ctx->bar != NULL) {
        lv_bar_set_value(ctx->bar, 0, LV_ANIM_OFF);
    }
    if (ctx->arc != NULL) {
        if (ctx->style == MONITOR_STYLE_GAUGE || ctx->style == MONITOR_STYLE_HUD) {
            monitor_animate_to(ctx, 0);
        } else {
            lv_arc_set_value(ctx->arc, 0);
        }
    }
    if (ctx->glow_arc != NULL) {
        lv_arc_set_value(ctx->glow_arc, 0);
    }
    if (ctx->unit_label != NULL) {
        lv_label_set_text(ctx->unit_label, "");
    }
    for (int i = 0; i < MONITOR_TICK_COUNT; i++) {
        if (ctx->ticks[i] != NULL) {
            lv_obj_set_style_bg_color(ctx->ticks[i], lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        }
    }
    for (int i = 0; i < MONITOR_BAR_COUNT; i++) {
        if (ctx->bars[i] != NULL) {
            lv_obj_set_style_bg_color(ctx->bars[i], lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        }
    }
    monitor_apply_layout(ctx);
}

static void monitor_event_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }
    monitor_ctx_t *ctx = (monitor_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        free(ctx);
    } else if (code == LV_EVENT_SIZE_CHANGED) {
        monitor_apply_layout(ctx);
    }
}

static int monitor_parse_subs(const char *list, monitor_sub_t *subs, size_t max_subs)
{
    if (list == NULL || list[0] == '\0' || subs == NULL || max_subs == 0) {
        return 0;
    }

    size_t count = 0;
    const char *cursor = list;
    while (*cursor != '\0' && count < max_subs) {
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
        size_t len = (size_t)(cursor - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t' ||
                           start[len - 1] == '\n' || start[len - 1] == '\r')) {
            len--;
        }
        if (len == 0 || len >= APP_MAX_ENTITY_ID_LEN) {
            continue;
        }
        memcpy(subs[count].entity_id, start, len);
        subs[count].entity_id[len] = '\0';
        count++;
    }
    return (int)count;
}

esp_err_t w_monitor_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    theme_default_style_card(card);
    lv_obj_set_style_pad_left(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, 10, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, def->title[0] ? def->title : def->id);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, theme_default_color_text_muted(), LV_PART_MAIN);

    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "--");
    lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(value, theme_default_color_text_primary(), LV_PART_MAIN);

    monitor_style_t style = monitor_style_from_variant(def->style_variant);
    monitor_opening_t opening = monitor_opening_from_variant(def->arc_opening);
    lv_obj_t *bar = NULL;
    lv_obj_t *arc = NULL;
    lv_obj_t *glow_arc = NULL;
    lv_obj_t *unit_label = NULL;
    lv_obj_t *ticks[MONITOR_TICK_COUNT] = {0};
    lv_obj_t *needle = NULL;
    lv_obj_t *bars[MONITOR_BAR_COUNT] = {0};

    if (style == MONITOR_STYLE_PERCENT) {
        bar = lv_bar_create(card);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    } else if (style == MONITOR_STYLE_ARC || style == MONITOR_STYLE_ARC_SEMI || style == MONITOR_STYLE_GAUGE) {
        arc = lv_arc_create(card);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 0);
        lv_arc_set_rotation(arc, 0);
        uint16_t bg_start = 0;
        uint16_t bg_end = 0;
        monitor_arc_angles(style, opening, &bg_start, &bg_end);
        lv_arc_set_bg_angles(arc, bg_start, bg_end);
        lv_obj_set_style_arc_color(arc, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);

        if (style == MONITOR_STYLE_GAUGE) {
            needle = lv_obj_create(card);
            lv_obj_clear_flag(needle, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(needle, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_pad_all(needle, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(needle, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(needle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(needle, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(needle, LV_OPA_COVER, LV_PART_MAIN);
        }
    } else if (style == MONITOR_STYLE_HUD) {
        /* Neon HUD ring: a wide translucent glow arc behind a crisp indicator
         * arc, a dotted scale inside the ring and a big centred value with a
         * small unit caption underneath. */
        glow_arc = lv_arc_create(card);
        lv_obj_clear_flag(glow_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(glow_arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_arc_set_mode(glow_arc, LV_ARC_MODE_NORMAL);
        lv_arc_set_range(glow_arc, 0, 100);
        lv_arc_set_value(glow_arc, 0);
        lv_arc_set_rotation(glow_arc, 0);
        lv_arc_set_bg_angles(glow_arc, 135, 45);
        lv_obj_set_style_arc_color(glow_arc, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(glow_arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_width(glow_arc, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(glow_arc, true, LV_PART_MAIN);
        lv_obj_set_style_arc_color(glow_arc, lv_color_hex(0x4FE3FF), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(glow_arc, LV_OPA_20, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(glow_arc, 30, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(glow_arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(glow_arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(glow_arc, 0, LV_PART_KNOB);

        arc = lv_arc_create(card);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 0);
        lv_arc_set_rotation(arc, 0);
        lv_arc_set_bg_angles(arc, 135, 45);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(0x4FE3FF), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);

        for (int i = 0; i < MONITOR_TICK_COUNT; i++) {
            lv_obj_t *t = lv_obj_create(card);
            lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(t, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(t, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
            ticks[i] = t;
        }

        unit_label = lv_label_create(card);
        lv_label_set_text(unit_label, "");
        lv_label_set_long_mode(unit_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(unit_label, theme_default_color_text_muted(), LV_PART_MAIN);
        lv_obj_set_style_text_font(unit_label, APP_FONT_TEXT_14, LV_PART_MAIN);
        lv_obj_set_style_text_align(unit_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else if (style == MONITOR_STYLE_BARS) {
        for (int i = 0; i < MONITOR_BAR_COUNT; i++) {
            bars[i] = lv_obj_create(card);
            lv_obj_clear_flag(bars[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_pad_all(bars[i], 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(bars[i], 0, LV_PART_MAIN);
            lv_obj_set_style_radius(bars[i], 2, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bars[i], lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, LV_PART_MAIN);
        }
    }

    monitor_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(monitor_ctx_t));
    if (ctx == NULL) {
        lv_obj_del(card);
        return ESP_ERR_NO_MEM;
    }

    ctx->card = card;
    ctx->title_label = title;
    ctx->status_dot = dot;
    ctx->value_label = value;
    ctx->bar = bar;
    ctx->arc = arc;
    ctx->glow_arc = glow_arc;
    ctx->unit_label = unit_label;
    memcpy(ctx->ticks, ticks, sizeof(ticks));
    ctx->needle = needle;
    memcpy(ctx->bars, bars, sizeof(bars));
    ctx->style = style;
    ctx->opening = opening;
    ctx->min = def->sensor_min;
    ctx->max = def->sensor_max;
    ctx->unavailable = false;
    ctx->arc_value = 0;
    ctx->needle_rot = 1350;
    ctx->sub_count = (uint8_t)monitor_parse_subs(def->extra_entity_ids, ctx->subs, APP_MAX_MONITOR_SUBS);

    for (uint8_t i = 0; i < ctx->sub_count; i++) {
        ctx->subs[i].name_label = lv_label_create(card);
        lv_obj_set_style_text_color(ctx->subs[i].name_label, theme_default_color_text_muted(), LV_PART_MAIN);
        lv_obj_set_style_text_font(ctx->subs[i].name_label, APP_FONT_TEXT_12, LV_PART_MAIN);
        lv_label_set_text(ctx->subs[i].name_label, "");

        ctx->subs[i].value_label = lv_label_create(card);
        lv_obj_set_style_text_color(ctx->subs[i].value_label, theme_default_color_text_primary(), LV_PART_MAIN);
        lv_obj_set_style_text_font(ctx->subs[i].value_label, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_label_set_text(ctx->subs[i].value_label, "--");
    }

    lv_obj_add_event_cb(card, monitor_event_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(card, monitor_event_cb, LV_EVENT_SIZE_CHANGED, ctx);

    monitor_apply_layout(ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

static bool monitor_entity_matches(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a, b, APP_MAX_ENTITY_ID_LEN) == 0;
}

static void monitor_update_primary(monitor_ctx_t *ctx, const ha_state_t *state)
{
    if (ctx == NULL || state == NULL) {
        return;
    }

    if (monitor_state_is_unavailable(state->state)) {
        monitor_apply_unavailable(ctx);
        return;
    }

    char value_text[96] = {0};
    monitor_format_state(state, value_text, sizeof(value_text));
    ctx->unavailable = false;
    monitor_set_status(ctx, true);
    monitor_set_value_text(ctx, value_text);

    if (ctx->bar != NULL || ctx->arc != NULL || ctx->needle != NULL ||
        ctx->style == MONITOR_STYLE_BARS) {
        float fvalue = 0.0f;
        if (monitor_parse_float(state->state, &fvalue)) {
            const char *unit = NULL;
            cJSON *attrs = cJSON_Parse(state->attributes_json);
            if (attrs != NULL) {
                cJSON *unit_item = cJSON_GetObjectItemCaseSensitive(attrs, "unit_of_measurement");
                if (cJSON_IsString(unit_item) && unit_item->valuestring != NULL) {
                    unit = unit_item->valuestring;
                }
            }
            int pct = monitor_compute_percent(fvalue, unit, ctx->min, ctx->max);
            lv_color_t pct_color = (ctx->style == MONITOR_STYLE_HUD)
                ? monitor_hud_gradient(pct)
                : monitor_threshold_color(pct);
            if (ctx->bar != NULL) {
                lv_bar_set_value(ctx->bar, pct, LV_ANIM_ON);
                lv_obj_set_style_bg_color(ctx->bar, pct_color, LV_PART_INDICATOR);
            }
            if (ctx->arc != NULL) {
                if (ctx->style == MONITOR_STYLE_GAUGE || ctx->style == MONITOR_STYLE_HUD) {
                    lv_obj_set_style_arc_color(ctx->arc, pct_color, LV_PART_INDICATOR);
                    monitor_animate_to(ctx, (int16_t)pct);
                } else {
                    lv_arc_set_value(ctx->arc, pct);
                }
            }
            if (ctx->glow_arc != NULL) {
                lv_obj_set_style_arc_color(ctx->glow_arc, pct_color, LV_PART_INDICATOR);
            }
            if (ctx->style == MONITOR_STYLE_HUD) {
                if (ctx->unit_label != NULL) {
                    lv_label_set_text(ctx->unit_label, unit != NULL ? unit : "");
                }
                int filled = (pct * (MONITOR_TICK_COUNT - 1) + 50) / 100;
                for (int i = 0; i < MONITOR_TICK_COUNT; i++) {
                    if (ctx->ticks[i] != NULL) {
                        lv_obj_set_style_bg_color(ctx->ticks[i],
                            (i <= filled) ? pct_color : lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
                    }
                }
            }
            if (ctx->needle != NULL) {
                lv_obj_set_style_bg_color(ctx->needle, pct_color, LV_PART_MAIN);
            }
            if (ctx->style == MONITOR_STYLE_BARS) {
                int filled = (pct * MONITOR_BAR_COUNT + 50) / 100;
                for (int i = 0; i < MONITOR_BAR_COUNT; i++) {
                    lv_obj_t *b = ctx->bars[i];
                    if (b == NULL) {
                        continue;
                    }
                    lv_obj_set_style_bg_color(
                        b,
                        (i < filled) ? pct_color : lv_color_hex(APP_UI_COLOR_CARD_BORDER),
                        LV_PART_MAIN);
                }
            }
            if (attrs != NULL) {
                cJSON_Delete(attrs);
            }
        }
    }

    monitor_apply_layout(ctx);
}

static void monitor_update_sub(monitor_ctx_t *ctx, const ha_state_t *state)
{
    if (ctx == NULL || state == NULL) {
        return;
    }

    for (uint8_t i = 0; i < ctx->sub_count; i++) {
        if (monitor_entity_matches(ctx->subs[i].entity_id, state->entity_id)) {
            if (ctx->subs[i].value_label != NULL) {
                char value_text[96] = {0};
                monitor_format_state(state, value_text, sizeof(value_text));
                lv_label_set_text(ctx->subs[i].value_label, value_text);
            }
            /* Derive a short friendly label from the entity id suffix. */
            if (ctx->subs[i].name_label != NULL) {
                const char *dot = strchr(ctx->subs[i].entity_id, '.');
                if (dot != NULL) {
                    lv_label_set_text(ctx->subs[i].name_label, dot + 1);
                }
            }
            return;
        }
    }
}

void w_monitor_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }

    monitor_ctx_t *ctx = (monitor_ctx_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    if (monitor_entity_matches(instance->entity_id, state->entity_id)) {
        monitor_update_primary(ctx, state);
    } else {
        monitor_update_sub(ctx, state);
    }
}

void w_monitor_tile_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }

    monitor_ctx_t *ctx = (monitor_ctx_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    monitor_apply_unavailable(ctx);
}
