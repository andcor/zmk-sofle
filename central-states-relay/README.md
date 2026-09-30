# zmk-central-states-relay

Out-of-tree ZMK module that relays central keyboard states (BT profile,
connection/USB/bonded flags, HID indicators, battery, layer, modifiers, WPM)
to peripheral halves over a sideloaded GATT characteristic on the existing
split BLE link.

## What it does

- **Central side:** subscribes to ZMK events and writes a 16-byte payload
  to all connected peripherals via `bt_gatt_write_without_response`.
- **Peripheral side:** hosts a GATT characteristic, receives writes from
  central, caches the payload, and raises `zmk_central_states_changed` ZMK
  event for consumption by display widgets.

## Payload format (16 bytes)

```c
struct zmk_central_states_changed {
    uint8_t active_profile;    /* 0..4 BT profile index */
    uint8_t flags;             /* device state: connected, USB, bonded */
    uint8_t hid_indicators;    /* from host: caps/num/scroll lock */
    uint8_t central_battery;   /* 0..100 percent */
    uint8_t active_layer;      /* active layer index */
    uint8_t mods;              /* explicit modifiers (ctrl/shift/alt/gui) */
    uint8_t wpm;               /* words per minute (optional, see below) */
    char layer_name[9];        /* layer name, null-terminated */
} __packed;
```

## Kconfig options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_ZMK_CENTRAL_STATES_RELAY` | auto (from DTS) | Master toggle, enabled by DTS compatible node |
| `CONFIG_CSR_RELAY_WPM` | n | Relay WPM to peripheral. Generates frequent BLE traffic during typing — disable to save power |

## Integration

Add to your `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: central-states-relay
      url-base: https://github.com/ArtemYurov
  projects:
    - name: zmk-central-states-relay
      remote: central-states-relay
      revision: main
```

Add a DTS node in your overlay (both halves):

```dts
/ {
    central_states_relay {
        compatible = "zmk,central-states-relay";
    };
};
```

Optional: enable WPM relay in your `.conf`:

```
CONFIG_CSR_RELAY_WPM=y
```

## Cache API (peripheral side)

Display widgets read cached values via getter functions:

```c
#include <zmk/split/central-states/cache.h>

uint8_t profile = csr_cache_get_active_profile();
uint8_t battery = csr_cache_get_central_battery();
uint8_t mods    = csr_cache_get_mods();
uint8_t hid_ind = csr_cache_get_hid_indicators();
const char *name = csr_cache_get_layer_name();
```

## License

MIT
