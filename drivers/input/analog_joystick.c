/*
 * Analog joystick -> relative mouse movement, or arrow keys, input driver.
 *
 * Polls two ADC channels (X, Y) on a fixed interval. Deflection from the
 * resting center position, past a configurable deadzone, is normally
 * reported as relative INPUT_REL_X / INPUT_REL_Y events - i.e.
 * trackpoint-style continuous cursor movement while the stick is held
 * off-center. While `arrow-layer` is the active ZMK keymap layer, it
 * instead sends arrow-key presses (held while tilted, released when
 * centered) - see analog_joystick_update_arrow_keys().
 *
 * The center position isn't fixed after boot: whenever a given axis is
 * within its own deadzone (i.e. reporting no movement), its center
 * slowly drifts toward the current reading. This absorbs small
 * persistent calibration offsets or thermal drift (common with
 * magnetic/TMR joystick modules) that would otherwise cause a constant
 * slow cursor creep in one direction. It never affects a genuine held
 * deflection, since that branch only runs below the deadzone threshold.
 *
 * `deadzone` and `sensitivity` in the devicetree node will likely still
 * need tuning per joystick module.
 */

#define DT_DRV_COMPAT zmk_analog_joystick

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(analog_joystick, CONFIG_INPUT_LOG_LEVEL);

struct analog_joystick_config {
    struct adc_dt_spec x_channel;
    struct adc_dt_spec y_channel;
    uint16_t deadzone;
    uint16_t sensitivity;
    uint16_t poll_interval_ms;
    uint8_t arrow_layer;
};

struct analog_joystick_data {
    const struct device *dev;
    struct k_work_delayable work;
    int32_t center_x;
    int32_t center_y;
    int32_t last_raw_x;
    int32_t last_raw_y;
    bool calibrated;
    bool idle_synced;
    uint32_t x_arrow_key;
    uint32_t y_arrow_key;

    /* Incremental, non-blocking boot calibration state. */
    int32_t calibration_sum_x;
    int32_t calibration_sum_y;
    uint8_t calibration_samples;
};

/*
 * How long after boot to wait before calibrating the resting center.
 * Deliberately generous: a short window (e.g. 100ms) risks firing while
 * the board is still being handled right after being plugged in or
 * flashed, capturing a skewed "center" that's actually the stick being
 * touched/held rather than at rest - which then gets permanently locked
 * in, since it lands far outside the deadzone and the runtime
 * auto-recentering (see below) only nudges the center when a reading is
 * already *within* the deadzone.
 */
#define CALIBRATION_DELAY_MS 2000
#define CALIBRATION_SAMPLES 8
#define CALIBRATION_SAMPLE_INTERVAL_MS 5

/*
 * Reads one ADC channel. Returns 0 and writes the raw value to *out on
 * success, or a negative error code on failure - callers should fall
 * back to the last known-good reading rather than treating a failed
 * read as a real, extreme deflection.
 */
static int analog_joystick_read(const struct adc_dt_spec *spec, int32_t *out) {
    int16_t buf = 0;
    struct adc_sequence sequence = {
        .buffer = &buf,
        .buffer_size = sizeof(buf),
    };

    int err = adc_sequence_init_dt(spec, &sequence);
    if (err < 0) {
        LOG_ERR("Failed to init ADC sequence: %d", err);
        return err;
    }

    err = adc_read(spec->dev, &sequence);
    if (err < 0) {
        LOG_ERR("ADC read failed: %d", err);
        return err;
    }

    *out = buf;
    return 0;
}

/*
 * Advances one step of the incremental boot calibration and reschedules
 * itself. Non-blocking - each call takes one sample and returns, rather
 * than blocking the shared system work queue for the whole calibration
 * window (as a k_msleep() loop would).
 */
static void analog_joystick_calibrate_step(struct analog_joystick_data *data,
                                            const struct analog_joystick_config *cfg) {
    int32_t x, y;
    bool ok_x = analog_joystick_read(&cfg->x_channel, &x) == 0;
    bool ok_y = analog_joystick_read(&cfg->y_channel, &y) == 0;

    if (ok_x) {
        data->calibration_sum_x += x;
    }
    if (ok_y) {
        data->calibration_sum_y += y;
    }
    if (ok_x || ok_y) {
        data->calibration_samples++;
    }

    if (data->calibration_samples < CALIBRATION_SAMPLES) {
        k_work_schedule(&data->work, K_MSEC(CALIBRATION_SAMPLE_INTERVAL_MS));
        return;
    }

    if (data->calibration_samples > 0) {
        data->center_x = data->calibration_sum_x / data->calibration_samples;
        data->center_y = data->calibration_sum_y / data->calibration_samples;
    } else {
        LOG_ERR("Calibration got no valid ADC samples; defaulting center to 0");
        data->center_x = 0;
        data->center_y = 0;
    }
    data->last_raw_x = data->center_x;
    data->last_raw_y = data->center_y;
    data->calibrated = true;

    LOG_INF("Calibrated center: x=%d y=%d", data->center_x, data->center_y);
    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
}

static void analog_joystick_release_arrow_keys(struct analog_joystick_data *data) {
    bool changed = false;

    if (data->x_arrow_key != 0) {
        zmk_hid_keyboard_release(data->x_arrow_key);
        data->x_arrow_key = 0;
        changed = true;
    }

    if (data->y_arrow_key != 0) {
        zmk_hid_keyboard_release(data->y_arrow_key);
        data->y_arrow_key = 0;
        changed = true;
    }

    if (changed) {
        zmk_endpoint_send_report(HID_USAGE_KEY);
    }
}

/*
 * Arrow-key mode: each axis holds at most one direction key, pressed
 * while tilted past the deadzone and released once back within it.
 * Only sends a report when the pressed key(s) actually change, rather
 * than every poll cycle.
 */
static void analog_joystick_update_arrow_keys(struct analog_joystick_data *data,
                                               const struct analog_joystick_config *cfg,
                                               int32_t dx, int32_t dy) {
    uint32_t new_x_key = 0;
    uint32_t new_y_key = 0;

    if (dx > cfg->deadzone) {
        new_x_key = HID_USAGE_KEY_KEYBOARD_RIGHTARROW;
    } else if (dx < -cfg->deadzone) {
        new_x_key = HID_USAGE_KEY_KEYBOARD_LEFTARROW;
    }

    if (dy > cfg->deadzone) {
        new_y_key = HID_USAGE_KEY_KEYBOARD_DOWNARROW;
    } else if (dy < -cfg->deadzone) {
        new_y_key = HID_USAGE_KEY_KEYBOARD_UPARROW;
    }

    bool changed = false;

    if (new_x_key != data->x_arrow_key) {
        if (data->x_arrow_key != 0) {
            zmk_hid_keyboard_release(data->x_arrow_key);
        }
        if (new_x_key != 0) {
            zmk_hid_keyboard_press(new_x_key);
        }
        data->x_arrow_key = new_x_key;
        changed = true;
    }

    if (new_y_key != data->y_arrow_key) {
        if (data->y_arrow_key != 0) {
            zmk_hid_keyboard_release(data->y_arrow_key);
        }
        if (new_y_key != 0) {
            zmk_hid_keyboard_press(new_y_key);
        }
        data->y_arrow_key = new_y_key;
        changed = true;
    }

    if (changed) {
        zmk_endpoint_send_report(HID_USAGE_KEY);
    }
}

/*
 * Mouse mode: reports relative cursor movement while deflection exceeds
 * the deadzone. Sends the zero-delta "stop" sync event only once, on the
 * transition back to rest, rather than every idle poll cycle - avoiding
 * needless HID/BLE traffic while the stick just sits centered.
 */
static void analog_joystick_update_mouse_move(struct analog_joystick_data *data,
                                               const struct analog_joystick_config *cfg,
                                               int32_t dx, int32_t dy) {
    int32_t sensitivity = cfg->sensitivity > 0 ? (int32_t)cfg->sensitivity : 1;
    bool report_x = false;
    bool report_y = false;
    int32_t move_x = 0;
    int32_t move_y = 0;

    if (dx > cfg->deadzone || dx < -cfg->deadzone) {
        move_x = dx / sensitivity;
        report_x = move_x != 0;
    }

    if (dy > cfg->deadzone || dy < -cfg->deadzone) {
        move_y = dy / sensitivity;
        report_y = move_y != 0;
    }

    if (!report_x && !report_y) {
        if (!data->idle_synced) {
            input_report_rel(data->dev, INPUT_REL_X, 0, true, K_NO_WAIT);
            data->idle_synced = true;
        }
        return;
    }

    data->idle_synced = false;

    /*
     * Exactly one call per cycle must carry sync=true - the mouse HID
     * listener accumulates dx/dy across calls and only flushes an HID
     * report on the synced call. Report Y last (if it's firing) since
     * it's naturally the synced call already; otherwise make X the
     * synced call.
     */
    if (report_x) {
        input_report_rel(data->dev, INPUT_REL_X, move_x, !report_y, K_NO_WAIT);
    }

    if (report_y) {
        input_report_rel(data->dev, INPUT_REL_Y, move_y, true, K_NO_WAIT);
    }
}

static void analog_joystick_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct analog_joystick_data *data =
        CONTAINER_OF(dwork, struct analog_joystick_data, work);
    const struct analog_joystick_config *cfg = data->dev->config;

    if (!data->calibrated) {
        analog_joystick_calibrate_step(data, cfg);
        return;
    }

    int32_t raw_x, raw_y;

    /* On a failed read, fall back to the last known-good value rather
     * than treating the failure as a real (and likely extreme)
     * deflection. */
    if (analog_joystick_read(&cfg->x_channel, &raw_x) != 0) {
        raw_x = data->last_raw_x;
    }
    if (analog_joystick_read(&cfg->y_channel, &raw_y) != 0) {
        raw_y = data->last_raw_y;
    }
    data->last_raw_x = raw_x;
    data->last_raw_y = raw_y;

    int32_t dx = raw_x - data->center_x;
    int32_t dy = raw_y - data->center_y;

    LOG_DBG("raw_x=%d raw_y=%d dx=%d dy=%d", raw_x, raw_y, dx, dy);

    bool arrow_mode = zmk_keymap_layer_active(cfg->arrow_layer);

    /*
     * Auto-recenter: whenever an axis is within its own deadzone (i.e.
     * reporting no movement in either mode), slowly drift the calibrated
     * center toward the current reading. Corrects small persistent
     * offsets (an imperfect boot-time calibration, thermal drift,
     * mechanical creep) that would otherwise cause a constant slow drift
     * in one direction forever - without ever affecting a genuine held
     * deflection, since this only runs while already below the deadzone.
     */
    if (dx <= cfg->deadzone && dx >= -cfg->deadzone) {
        data->center_x += (raw_x - data->center_x) / 32;
    }

    if (dy <= cfg->deadzone && dy >= -cfg->deadzone) {
        data->center_y += (raw_y - data->center_y) / 32;
    }

    if (arrow_mode) {
        analog_joystick_update_arrow_keys(data, cfg, dx, dy);
    } else {
        if (data->x_arrow_key != 0 || data->y_arrow_key != 0) {
            /* Layer changed mid-hold - don't leave an arrow key stuck down. */
            analog_joystick_release_arrow_keys(data);
        }
        analog_joystick_update_mouse_move(data, cfg, dx, dy);
    }

    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
}

static int analog_joystick_init(const struct device *dev) {
    struct analog_joystick_data *data = dev->data;
    const struct analog_joystick_config *cfg = dev->config;

    if (!adc_is_ready_dt(&cfg->x_channel) || !adc_is_ready_dt(&cfg->y_channel)) {
        LOG_ERR("ADC controller not ready");
        return -ENODEV;
    }

    int err = adc_channel_setup_dt(&cfg->x_channel);
    if (err < 0) {
        LOG_ERR("Failed to configure X ADC channel: %d", err);
        return err;
    }

    err = adc_channel_setup_dt(&cfg->y_channel);
    if (err < 0) {
        LOG_ERR("Failed to configure Y ADC channel: %d", err);
        return err;
    }

    data->dev = dev;
    data->calibrated = false;
    data->idle_synced = false;
    data->x_arrow_key = 0;
    data->y_arrow_key = 0;
    data->calibration_sum_x = 0;
    data->calibration_sum_y = 0;
    data->calibration_samples = 0;

    /*
     * Calibration happens incrementally on the work queue (see
     * analog_joystick_calibrate_step), not here - this avoids a
     * multi-sample blocking sleep in device init, which would delay
     * every other POST_KERNEL driver that initializes after this one
     * (including the BLE stack).
     */
    k_work_init_delayable(&data->work, analog_joystick_work_handler);
    k_work_schedule(&data->work, K_MSEC(CALIBRATION_DELAY_MS));

    return 0;
}

#define ANALOG_JOYSTICK_INIT(n)                                                                  \
    static struct analog_joystick_data analog_joystick_data_##n;                                 \
    static const struct analog_joystick_config analog_joystick_config_##n = {                    \
        .x_channel = ADC_DT_SPEC_INST_GET_BY_IDX(n, 0),                                          \
        .y_channel = ADC_DT_SPEC_INST_GET_BY_IDX(n, 1),                                          \
        .deadzone = DT_INST_PROP(n, deadzone),                                                   \
        .sensitivity = DT_INST_PROP(n, sensitivity),                                             \
        .poll_interval_ms = DT_INST_PROP(n, poll_interval_ms),                                   \
        .arrow_layer = DT_INST_PROP(n, arrow_layer),                                             \
    };                                                                                           \
    DEVICE_DT_INST_DEFINE(n, analog_joystick_init, NULL, &analog_joystick_data_##n,               \
                           &analog_joystick_config_##n, POST_KERNEL,                              \
                           CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ANALOG_JOYSTICK_INIT)
