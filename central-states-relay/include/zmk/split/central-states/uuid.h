/*
 * Copyright (c) 2026 Artem Yurov
 * SPDX-License-Identifier: MIT
 *
 * GATT service/characteristic UUIDs for central states relay.
 * Base UUID uses last byte 0x3c to distinguish from output-relay (0x2c)
 * and ZMK split service (0x2a).
 */

#pragma once

#include <zephyr/bluetooth/uuid.h>

#define ZMK_BT_SPLIT_CSR_UUID(num) BT_UUID_128_ENCODE(num, 0x0096, 0x7107, 0xc967, 0xc5cfb1c2483c)

#define ZMK_SPLIT_BT_CSR_SERVICE_UUID ZMK_BT_SPLIT_CSR_UUID(0x00000000)
#define ZMK_SPLIT_BT_CSR_CHAR_STATES_UUID ZMK_BT_SPLIT_CSR_UUID(0x00000001)
