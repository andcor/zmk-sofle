/*
 * Copyright (c) 2026 Artem Yurov
 * SPDX-License-Identifier: MIT
 *
 * Central states relay payload and ZMK event definitions.
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/**
 * ZMK event raised on peripheral when states are received from central.
 * Payload: 16 bytes, each field is a separate byte for clarity.
 */
#define CSR_LAYER_NAME_MAX 9

/* Bit flags for the flags field (device connection state) */
#define CSR_FLAG_CONNECTED  BIT(0)  /* BT profile connected to host */
#define CSR_FLAG_USB        BIT(1)  /* USB output active */
#define CSR_FLAG_BONDED     BIT(2)  /* BT profile bonded */
#define CSR_FLAG_CAPS_WORD  BIT(3)  /* caps word active */

struct zmk_central_states_changed {
    uint8_t active_profile;                /* 1 — 0..4 BT profile index */
    uint8_t flags;                         /* 1 — CSR_FLAG_* device state */
    uint8_t hid_indicators;               /* 1 — HID indicators from host (caps/num/scroll) */
    uint8_t central_battery;               /* 1 — 0..100 percent */
    uint8_t active_layer;                  /* 1 — active layer index */
    uint8_t mods;                          /* 1 — explicit modifiers (ctrl/shift/alt/gui) */
    uint8_t wpm;                           /* 1 — 0..255 words per minute */
    char layer_name[CSR_LAYER_NAME_MAX];   /* 9 — layer name, null-terminated */
    /* TODO: RAW HID fields (time, volume, layout, temperature)
     * can be added here via zmk-raw-hid module or csr_set_*() API */
} __packed;                                /* = 16 bytes */

ZMK_EVENT_DECLARE(zmk_central_states_changed);
