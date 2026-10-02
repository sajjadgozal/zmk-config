/*
 * Activates one keymap layer only while a specific BLE profile is the
 * active output (see Kconfig.endpoint_layer).
 *
 * Driven by zmk_endpoint_changed, which ZMK raises whenever the
 * *connected* output changes: switching BLE profile, the selected
 * profile connecting/disconnecting, USB being plugged/unplugged, or the
 * USB/BLE output toggle. So "BLE profile N selected but not connected,
 * falling back to USB" correctly counts as USB here, not as profile N.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(endpoint_layer, CONFIG_ZMK_LOG_LEVEL);

static int endpoint_layer_listener(const zmk_event_t *eh) {
    const struct zmk_endpoint_changed *ev = as_zmk_endpoint_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_keymap_layer_id_t layer = zmk_keymap_layer_index_to_id(CONFIG_ZMK_ENDPOINT_LAYER_INDEX);
    if (layer == ZMK_KEYMAP_LAYER_ID_INVAL) {
        LOG_ERR("Layer index %d doesn't exist in the keymap", CONFIG_ZMK_ENDPOINT_LAYER_INDEX);
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool want = ev->endpoint.transport == ZMK_TRANSPORT_BLE &&
                ev->endpoint.ble.profile_index == CONFIG_ZMK_ENDPOINT_LAYER_PROFILE;

    if (want != zmk_keymap_layer_active(layer)) {
        LOG_INF("%s layer %d", want ? "Activating" : "Deactivating", layer);
        if (want) {
            zmk_keymap_layer_activate(layer, false);
        } else {
            zmk_keymap_layer_deactivate(layer, false);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(endpoint_layer, endpoint_layer_listener);
ZMK_SUBSCRIPTION(endpoint_layer, zmk_endpoint_changed);
