/*
 * Copyright (c) 2026 Artem Yurov
 * SPDX-License-Identifier: MIT
 *
 * Cache API for peripheral: getter functions for reading relay cache
 * from display widget get_state() callbacks.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zmk/split/central-states/event.h>

/* Update cache from received payload */
void csr_cache_update(const struct zmk_central_states_changed *payload);

/* Getter functions for display widgets */
uint8_t csr_cache_get_active_profile(void);
uint8_t csr_cache_get_flags(void);
bool csr_cache_get_flag(uint8_t flag);
uint8_t csr_cache_get_hid_indicators(void);
uint8_t csr_cache_get_central_battery(void);
uint8_t csr_cache_get_active_layer(void);
const char *csr_cache_get_layer_name(void);
uint8_t csr_cache_get_mods(void);
uint8_t csr_cache_get_wpm(void);
