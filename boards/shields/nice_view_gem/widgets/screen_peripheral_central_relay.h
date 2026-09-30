/*
 * Copyright (c) 2026 Artem Yurov
 * SPDX-License-Identifier: MIT
 *
 * Peripheral screen with central relay data — shows status of both halves.
 */

#pragma once

#include <lvgl.h>
#include <zephyr/kernel.h>
#include "util.h"

struct zmk_widget_screen {
    sys_snode_t node;
    lv_obj_t *obj;
    lv_color_t cbuf[BUFFER_SIZE * BUFFER_SIZE];
    lv_color_t cbuf2[BUFFER_SIZE * BUFFER_SIZE];
    lv_color_t cbuf3[BUFFER_SIZE * BUFFER_SIZE];
    struct status_state state;
};

int zmk_widget_screen_init(struct zmk_widget_screen *widget, lv_obj_t *parent);
lv_obj_t *zmk_widget_screen_obj(struct zmk_widget_screen *widget);
