/*
 * Copyright (c) 2026 Artem Yurov
 * SPDX-License-Identifier: MIT
 *
 * Peripheral side: GATT server that receives central states writes
 * and raises zmk_central_states_changed ZMK events.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/split/central-states/uuid.h>
#include <zmk/split/central-states/event.h>
#include <zmk/split/central-states/cache.h>

#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

ZMK_EVENT_IMPL(zmk_central_states_changed);

/* ---- Static cache + getter functions -------------------------------------- */

static struct zmk_central_states_changed csr_cache;

void csr_cache_update(const struct zmk_central_states_changed *payload) {
    memcpy(&csr_cache, payload, sizeof(csr_cache));
}

uint8_t csr_cache_get_active_profile(void) { return csr_cache.active_profile; }
uint8_t csr_cache_get_flags(void) { return csr_cache.flags; }
bool csr_cache_get_flag(uint8_t flag) { return (csr_cache.flags & flag) != 0; }
uint8_t csr_cache_get_hid_indicators(void) { return csr_cache.hid_indicators; }
uint8_t csr_cache_get_central_battery(void) { return csr_cache.central_battery; }
uint8_t csr_cache_get_active_layer(void) { return csr_cache.active_layer; }
const char *csr_cache_get_layer_name(void) { return csr_cache.layer_name; }
uint8_t csr_cache_get_mods(void) { return csr_cache.mods; }
uint8_t csr_cache_get_wpm(void) { return csr_cache.wpm; }

/* ---- Pending payload + work ----------------------------------------------- */

static struct zmk_central_states_changed pending_payload;
static struct k_work csr_peripheral_work;

static void csr_peripheral_work_cb(struct k_work *work) {
    LOG_DBG("CSR peripheral: profile=%d flags=0x%02x battery=%d layer=%d",
            pending_payload.active_profile,
            pending_payload.flags,
            pending_payload.central_battery,
            pending_payload.active_layer);

    csr_cache_update(&pending_payload);
    raise_zmk_central_states_changed(pending_payload);
}

/* GATT write handler — called from BT RX thread, must be fast */
static ssize_t split_svc_recv_central_states(struct bt_conn *conn,
                                              const struct bt_gatt_attr *attr,
                                              const void *buf, uint16_t len,
                                              uint16_t offset, uint8_t flags) {
    if (len != sizeof(struct zmk_central_states_changed)) {
        LOG_WRN("CSR peripheral: unexpected payload size %d (expected %d)",
                len, (int)sizeof(struct zmk_central_states_changed));
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    memcpy(&pending_payload, buf, sizeof(pending_payload));
    k_work_submit(&csr_peripheral_work);

    return len;
}

/* GATT service definition */
static struct bt_uuid_128 csr_service_uuid =
    BT_UUID_INIT_128(ZMK_SPLIT_BT_CSR_SERVICE_UUID);
static struct bt_uuid_128 csr_char_uuid =
    BT_UUID_INIT_128(ZMK_SPLIT_BT_CSR_CHAR_STATES_UUID);

BT_GATT_SERVICE_DEFINE(csr_svc,
    BT_GATT_PRIMARY_SERVICE(&csr_service_uuid),
    BT_GATT_CHARACTERISTIC(&csr_char_uuid.uuid,
                           BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE_ENCRYPT,
                           NULL, split_svc_recv_central_states, NULL),
);

static int csr_peripheral_init(void) {
    k_work_init(&csr_peripheral_work, csr_peripheral_work_cb);
    LOG_DBG("CSR peripheral: GATT service registered");
    return 0;
}

SYS_INIT(csr_peripheral_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
