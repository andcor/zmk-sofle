/*
 * Copyright (c) 2026 Artem Yurov
 * SPDX-License-Identifier: MIT
 *
 * Central side: subscribes to ZMK events and writes central states
 * to all connected peripherals via bt_gatt_write_without_response.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>
#include <zmk/ble.h>
#include <zmk/battery.h>
#include <zmk/keymap.h>
#include <zmk/usb.h>
#include <zmk/hid.h>
#include <string.h>

#ifdef CONFIG_CSR_RELAY_WPM
#include <zmk/events/wpm_state_changed.h>
#include <zmk/wpm.h>
#endif

#include <zmk/split/central-states/uuid.h>
#include <zmk/split/central-states/event.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---- Peripheral slot tracking ------------------------------------------- */

#define CSR_MAX_PERIPHERALS CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS

struct csr_peripheral_slot {
    struct bt_conn *conn;
    uint16_t char_handle; /* 0 = not yet discovered */
};

static struct csr_peripheral_slot csr_peripherals[CSR_MAX_PERIPHERALS];

static struct csr_peripheral_slot *slot_for_conn(struct bt_conn *conn) {
    for (int i = 0; i < CSR_MAX_PERIPHERALS; i++) {
        if (csr_peripherals[i].conn == conn) {
            return &csr_peripherals[i];
        }
    }
    return NULL;
}

/* ---- Cached payload ------------------------------------------------------- */

static struct zmk_central_states_changed cached_payload;
static int64_t csr_last_broadcast;

/* ZMK has no caps word event; behavior_caps_word_data starts with its `active` flag */
static bool csr_caps_word_active(void) {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(caps_word), okay)
    return *(const bool *)DEVICE_DT_GET(DT_NODELABEL(caps_word))->data;
#else
    return false;
#endif
}

/* Build flags from current ZMK device state */
static uint8_t csr_update_flags(void) {
    uint8_t flags = 0;
    if (zmk_ble_active_profile_is_connected()) {
        flags |= CSR_FLAG_CONNECTED;
    }
    if (zmk_usb_is_powered()) {
        flags |= CSR_FLAG_USB;
    }
    if (!zmk_ble_active_profile_is_open()) {
        flags |= CSR_FLAG_BONDED;
    }
    if (csr_caps_word_active()) {
        flags |= CSR_FLAG_CAPS_WORD;
    }
    if (zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE) {
        flags |= CSR_FLAG_ACTIVE;
    }
    return flags;
}

/* ---- GATT discovery ------------------------------------------------------- */

static struct bt_uuid_128 csr_svc_uuid =
    BT_UUID_INIT_128(ZMK_SPLIT_BT_CSR_SERVICE_UUID);
static struct bt_uuid_128 csr_char_uuid_disc =
    BT_UUID_INIT_128(ZMK_SPLIT_BT_CSR_CHAR_STATES_UUID);

static uint8_t csr_chrc_discovery_cb(struct bt_conn *conn,
                                      const struct bt_gatt_attr *attr,
                                      struct bt_gatt_discover_params *params) {
    if (!attr) {
        LOG_DBG("CSR sender: characteristic discovery done");
        return BT_GATT_ITER_STOP;
    }

    struct csr_peripheral_slot *slot = slot_for_conn(conn);
    if (!slot) {
        return BT_GATT_ITER_STOP;
    }

    struct bt_gatt_chrc *chrc = attr->user_data;
    if (chrc->properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) {
        slot->char_handle = chrc->value_handle;
        LOG_DBG("CSR sender: characteristic found handle=0x%04x", slot->char_handle);
        /* Send current cached payload immediately */
        bt_gatt_write_without_response(conn, slot->char_handle,
                                       &cached_payload,
                                       sizeof(cached_payload), false);
    }

    return BT_GATT_ITER_STOP;
}

static struct bt_gatt_discover_params csr_disc_params[CSR_MAX_PERIPHERALS];

static uint8_t csr_svc_discovery_cb(struct bt_conn *conn,
                                     const struct bt_gatt_attr *attr,
                                     struct bt_gatt_discover_params *params) {
    if (!attr) {
        LOG_DBG("CSR sender: service not found on this peripheral");
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("CSR sender: service found, discovering characteristic");

    /* Re-use the same params slot for characteristic discovery */
    int idx = 0;
    for (int i = 0; i < CSR_MAX_PERIPHERALS; i++) {
        if (csr_peripherals[i].conn == conn) {
            idx = i;
            break;
        }
    }

    csr_disc_params[idx].uuid = &csr_char_uuid_disc.uuid;
    csr_disc_params[idx].func = csr_chrc_discovery_cb;
    csr_disc_params[idx].start_handle = attr->handle + 1;
    csr_disc_params[idx].end_handle = 0xFFFF;
    csr_disc_params[idx].type = BT_GATT_DISCOVER_CHARACTERISTIC;

    bt_gatt_discover(conn, &csr_disc_params[idx]);
    return BT_GATT_ITER_STOP;
}

static void csr_start_discovery(struct bt_conn *conn, int slot_idx) {
    LOG_DBG("CSR sender: starting service discovery on slot %d", slot_idx);

    csr_disc_params[slot_idx].uuid = &csr_svc_uuid.uuid;
    csr_disc_params[slot_idx].func = csr_svc_discovery_cb;
    csr_disc_params[slot_idx].start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    csr_disc_params[slot_idx].end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    csr_disc_params[slot_idx].type = BT_GATT_DISCOVER_PRIMARY;

    int err = bt_gatt_discover(conn, &csr_disc_params[slot_idx]);
    if (err) {
        LOG_WRN("CSR sender: discovery start failed (err %d)", err);
    }
}

/* ---- BT connection callbacks ---------------------------------------------- */

static void csr_bt_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }

    /* Only proceed for peripheral connections on central side */
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0) {
        return;
    }
    if (info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }

    for (int i = 0; i < CSR_MAX_PERIPHERALS; i++) {
        if (!csr_peripherals[i].conn) {
            csr_peripherals[i].conn = bt_conn_ref(conn);
            csr_peripherals[i].char_handle = 0;
            LOG_DBG("CSR sender: peripheral connected, slot %d", i);
            csr_start_discovery(conn, i);
            return;
        }
    }

    LOG_WRN("CSR sender: no free peripheral slot");
}

static void csr_bt_disconnected(struct bt_conn *conn, uint8_t reason) {
    struct csr_peripheral_slot *slot = slot_for_conn(conn);
    if (!slot) {
        return;
    }

    LOG_DBG("CSR sender: peripheral disconnected, reason %d", reason);
    bt_conn_unref(slot->conn);
    slot->conn = NULL;
    slot->char_handle = 0;
}

BT_CONN_CB_DEFINE(csr_conn_callbacks) = {
    .connected = csr_bt_connected,
    .disconnected = csr_bt_disconnected,
};

/* ---- Broadcast current payload to all peripherals ------------------------ */

static void csr_broadcast(void) {
    csr_last_broadcast = k_uptime_get();
    for (int i = 0; i < CSR_MAX_PERIPHERALS; i++) {
        if (!csr_peripherals[i].conn || !csr_peripherals[i].char_handle) {
            continue;
        }

        int err = bt_gatt_write_without_response(csr_peripherals[i].conn,
                                                 csr_peripherals[i].char_handle,
                                                 &cached_payload,
                                                 sizeof(cached_payload),
                                                 false);
        if (err) {
            LOG_WRN("CSR sender: write failed on slot %d (err %d)", i, err);
        } else {
            LOG_DBG("CSR sender: sent profile=%d flags=0x%02x battery=%d to slot %d",
                    cached_payload.active_profile,
                    cached_payload.flags,
                    cached_payload.central_battery,
                    i);
        }
    }
}

/* ---- ZMK event listeners -------------------------------------------------- */

static int csr_handle_ble_profile_changed(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *ev =
        as_zmk_ble_active_profile_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("CSR sender: BLE profile changed idx=%d", ev->index);

    cached_payload.active_profile = ev->index;
    cached_payload.flags = csr_update_flags();
    csr_broadcast();
    return ZMK_EV_EVENT_BUBBLE;
}

static int csr_handle_battery_changed(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev =
        as_zmk_battery_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("CSR sender: battery changed soc=%d", ev->state_of_charge);

    cached_payload.central_battery = ev->state_of_charge;
    csr_broadcast();
    return ZMK_EV_EVENT_BUBBLE;
}

static int csr_handle_endpoint_changed(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("CSR sender: endpoint changed");

    cached_payload.flags = csr_update_flags();
    csr_broadcast();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(csr_ble_profile, csr_handle_ble_profile_changed);
ZMK_SUBSCRIPTION(csr_ble_profile, zmk_ble_active_profile_changed);

ZMK_LISTENER(csr_battery, csr_handle_battery_changed);
ZMK_SUBSCRIPTION(csr_battery, zmk_battery_state_changed);

ZMK_LISTENER(csr_endpoint, csr_handle_endpoint_changed);
ZMK_SUBSCRIPTION(csr_endpoint, zmk_endpoint_changed);

static int csr_handle_layer_changed(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t layer = zmk_keymap_highest_layer_active();
    LOG_DBG("CSR sender: layer changed to %d", layer);

    cached_payload.active_layer = layer;
    const char *name = zmk_keymap_layer_name(layer);
    memset(cached_payload.layer_name, 0, CSR_LAYER_NAME_MAX);
    if (name) {
        strncpy(cached_payload.layer_name, name, CSR_LAYER_NAME_MAX - 1);
    }
    csr_broadcast();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(csr_layer, csr_handle_layer_changed);
ZMK_SUBSCRIPTION(csr_layer, zmk_layer_state_changed);

static int csr_handle_usb_changed(const zmk_event_t *eh) {
    cached_payload.flags = csr_update_flags();
    LOG_DBG("CSR sender: USB changed flags=0x%02x", cached_payload.flags);
    csr_broadcast();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(csr_usb, csr_handle_usb_changed);
ZMK_SUBSCRIPTION(csr_usb, zmk_usb_conn_state_changed);

static int csr_handle_hid_indicators_changed(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Direct copy — ZMK uses standard HID indicator bits:
     * bit0=NumLock, bit1=CapsLock, bit2=ScrollLock */
    cached_payload.hid_indicators = ev->indicators;
    LOG_DBG("CSR sender: HID indicators changed 0x%02x", cached_payload.hid_indicators);
    csr_broadcast();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(csr_hid_ind, csr_handle_hid_indicators_changed);
ZMK_SUBSCRIPTION(csr_hid_ind, zmk_hid_indicators_changed);

static int csr_handle_keycode_changed(const zmk_event_t *eh) {
    uint8_t mods = zmk_hid_get_explicit_mods();
    if (cached_payload.mods != mods) {
        cached_payload.mods = mods;
        LOG_DBG("CSR sender: mods changed 0x%02x", mods);
        csr_broadcast();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(csr_mods, csr_handle_keycode_changed);
ZMK_SUBSCRIPTION(csr_mods, zmk_keycode_state_changed);

/* Resend while typing so idle halves see activity from the other half */
#define CSR_ACTIVITY_HEARTBEAT_MS (CONFIG_ZMK_IDLE_TIMEOUT / 4)

/* Deferred so the check runs after caps word and activity have handled the event */
static void csr_state_check_work_cb(struct k_work *work) {
    uint8_t flags = csr_update_flags();
    bool heartbeat_due = (flags & CSR_FLAG_ACTIVE) &&
                         k_uptime_get() - csr_last_broadcast > CSR_ACTIVITY_HEARTBEAT_MS;

    if (cached_payload.flags != flags || heartbeat_due) {
        cached_payload.flags = flags;
        csr_broadcast();
    }
}

static K_WORK_DEFINE(csr_state_check_work, csr_state_check_work_cb);

static int csr_handle_state_check(const zmk_event_t *eh) {
    k_work_submit(&csr_state_check_work);
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(csr_state_check, csr_handle_state_check);
ZMK_SUBSCRIPTION(csr_state_check, zmk_position_state_changed);
ZMK_SUBSCRIPTION(csr_state_check, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(csr_state_check, zmk_activity_state_changed);

#ifdef CONFIG_CSR_RELAY_WPM
static int csr_handle_wpm_changed(const zmk_event_t *eh) {
    cached_payload.wpm = zmk_wpm_get_state();
    LOG_DBG("CSR sender: WPM changed wpm=%d", cached_payload.wpm);
    csr_broadcast();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(csr_wpm, csr_handle_wpm_changed);
ZMK_SUBSCRIPTION(csr_wpm, zmk_wpm_state_changed);
#endif

/* ---- Module init ---------------------------------------------------------- */

static int csr_sender_init(void) {
    memset(&cached_payload, 0, sizeof(cached_payload));

    /* Initialize from current ZMK state */
    cached_payload.active_profile = zmk_ble_active_profile_index();
    cached_payload.flags = csr_update_flags();
    cached_payload.central_battery = zmk_battery_state_of_charge();
    cached_payload.active_layer = zmk_keymap_highest_layer_active();
    cached_payload.mods = zmk_hid_get_explicit_mods();

    const char *name = zmk_keymap_layer_name(cached_payload.active_layer);
    if (name) {
        strncpy(cached_payload.layer_name, name, CSR_LAYER_NAME_MAX - 1);
    }

#ifdef CONFIG_CSR_RELAY_WPM
    cached_payload.wpm = zmk_wpm_get_state();
#endif

    LOG_DBG("CSR sender: init profile=%d flags=0x%02x battery=%d",
            cached_payload.active_profile,
            cached_payload.flags,
            cached_payload.central_battery);
    return 0;
}

SYS_INIT(csr_sender_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
