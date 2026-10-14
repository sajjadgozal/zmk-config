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
 * close to rest, its center slowly drifts toward the current reading.
 * This absorbs small persistent calibration offsets or thermal drift
 * (common with magnetic/TMR joystick modules) that would otherwise cause
 * a constant slow cursor creep in one direction. The drift is bounded
 * (see RECENTER_WINDOW_DIV / RECENTER_MAX_DRIFT_DIV) so it can never walk
 * the center far enough away that releasing the stick reads as a held
 * deflection.
 *
 * Mouse mode uses a radial deadzone, a response curve and sub-pixel
 * accumulation - see analog_joystick_update_mouse_move(). `deadzone`,
 * `max-speed` and `curve` in the devicetree node will likely still need
 * tuning per joystick module.
 */

#define DT_DRV_COMPAT zmk_analog_joystick

#include <stdlib.h>

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
    uint16_t max_speed;
    uint8_t curve;
    uint16_t poll_interval_ms;
    uint16_t idle_poll_interval_ms;
    uint8_t arrow_layer;
};

struct analog_joystick_data {
    const struct device *dev;
    struct k_work_delayable work;
    int32_t center_x;
    int32_t center_y;
    /* Boot-calibrated center; auto-recentering stays within a bounded
     * distance of this. */
    int32_t rest_x;
    int32_t rest_y;
    int32_t last_raw_x;
    int32_t last_raw_y;
    /* Stuck-center recovery (see analog_joystick_recover_axis). */
    int32_t still_anchor_x;
    int32_t still_anchor_y;
    uint16_t still_polls_x;
    uint16_t still_polls_y;
    bool calibrated;
    bool idle_synced;
    /* Mouse mode: sub-pixel movement carried between polls, in Q16
     * fixed point (1 px = 65536). */
    int32_t mouse_rem_x;
    int32_t mouse_rem_y;
    int64_t last_poll_ms;
    uint32_t x_arrow_key;
    uint32_t y_arrow_key;

    /* Incremental, non-blocking boot calibration state. */
    int32_t calibration_sum_x;
    int32_t calibration_sum_y;
    int32_t calibration_first_x;
    int32_t calibration_first_y;
    uint8_t calibration_samples;
};

/*
 * How long after boot to wait before calibrating the resting center.
 * Deliberately generous: a short window (e.g. 100ms) risks firing while
 * the board is still being handled right after being plugged in or
 * flashed, capturing a skewed "center" that's actually the stick being
 * touched/held rather than at rest. Samples are also discarded and
 * retaken if the stick moves during them, and a center that still ends
 * up wrong is caught later by analog_joystick_recover_axis().
 */
#define CALIBRATION_DELAY_MS 2000
#define CALIBRATION_SAMPLES 8
#define CALIBRATION_SAMPLE_INTERVAL_MS 5

/*
 * Auto-recentering bounds, as divisors of the deadzone:
 * - only readings within deadzone/RECENTER_WINDOW_DIV of the current
 *   center pull it, so a thumb resting on (or slowly tilting) the stick
 *   can't drag the center along with it;
 * - the center never moves more than deadzone/RECENTER_MAX_DRIFT_DIV
 *   from the boot calibration, so a released stick always lands back
 *   inside the deadzone. Without this cap, a slow tilt could walk the
 *   center out to one side 1/32 step at a time; on release the stick
 *   then read as a large held deflection, which is outside the deadzone,
 *   so recentering never ran again and the cursor/arrow key stuck.
 */
#define RECENTER_WINDOW_DIV 4
#define RECENTER_MAX_DRIFT_DIV 2

/*
 * Arrow-mode release threshold, as a fraction of the deadzone. A key is
 * pressed past the deadzone but only released once back below this,
 * so ADC noise right at the edge can't chatter the key (each chatter is
 * a separate arrow press on the host).
 */
#define ARROW_RELEASE_NUM 3
#define ARROW_RELEASE_DEN 4

/*
 * A reading counts as "still" while it stays within STILL_TOLERANCE raw
 * counts of where it started. Used both to reject a boot calibration
 * taken while the stick was moving, and for stuck-center recovery.
 */
#define STILL_TOLERANCE 48

/*
 * Stuck-center recovery: if an axis reads past the deadzone but sits
 * perfectly still for this long, it's a candidate for "the stick is
 * actually at rest and the center is wrong" - see
 * analog_joystick_recover_axis().
 */
#define STUCK_RECOVER_MS 1500

/* Fixed-point scale used by the mouse-mode speed math. */
#define Q16_ONE 65536

/* SAADC hardware oversampling: 2^2 = 4 samples averaged per read. */
#define ADC_OVERSAMPLING 2

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
        .oversampling = ADC_OVERSAMPLING,
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

    /* Only count samples where both axes read, so neither average is
     * divided by a sample it never got. */
    if (analog_joystick_read(&cfg->x_channel, &x) != 0 ||
        analog_joystick_read(&cfg->y_channel, &y) != 0) {
        k_work_schedule(&data->work, K_MSEC(CALIBRATION_SAMPLE_INTERVAL_MS));
        return;
    }

    if (data->calibration_samples == 0) {
        data->calibration_first_x = x;
        data->calibration_first_y = y;
    } else if (abs(x - data->calibration_first_x) > STILL_TOLERANCE ||
               abs(y - data->calibration_first_y) > STILL_TOLERANCE) {
        /* Stick is moving - it's being touched, not at rest. Start over. */
        LOG_DBG("Stick moving during calibration; retrying");
        data->calibration_sum_x = 0;
        data->calibration_sum_y = 0;
        data->calibration_samples = 0;
        k_work_schedule(&data->work, K_MSEC(CALIBRATION_DELAY_MS / 4));
        return;
    }

    data->calibration_sum_x += x;
    data->calibration_sum_y += y;
    data->calibration_samples++;

    if (data->calibration_samples < CALIBRATION_SAMPLES) {
        k_work_schedule(&data->work, K_MSEC(CALIBRATION_SAMPLE_INTERVAL_MS));
        return;
    }

    data->center_x = data->calibration_sum_x / data->calibration_samples;
    data->center_y = data->calibration_sum_y / data->calibration_samples;
    data->rest_x = data->center_x;
    data->rest_y = data->center_y;
    data->last_raw_x = data->center_x;
    data->last_raw_y = data->center_y;
    data->still_anchor_x = data->center_x;
    data->still_anchor_y = data->center_y;
    data->still_polls_x = 0;
    data->still_polls_y = 0;
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
 * Picks one axis's arrow key with hysteresis: press past the deadzone,
 * but keep an already-held key until the deflection drops below the
 * lower release threshold.
 */
static uint32_t analog_joystick_arrow_for_axis(int32_t d, uint32_t held, uint16_t deadzone,
                                               uint32_t pos_key, uint32_t neg_key) {
    int32_t release = (int32_t)deadzone * ARROW_RELEASE_NUM / ARROW_RELEASE_DEN;

    if (d > deadzone || (held == pos_key && d > release)) {
        return pos_key;
    }
    if (d < -deadzone || (held == neg_key && d < -release)) {
        return neg_key;
    }
    return 0;
}

/*
 * Arrow-key mode: each axis holds at most one direction key, pressed
 * while tilted past the deadzone and released once back near center.
 * Only sends a report when the pressed key(s) actually change, rather
 * than every poll cycle.
 */
static void analog_joystick_update_arrow_keys(struct analog_joystick_data *data,
                                               const struct analog_joystick_config *cfg,
                                               int32_t dx, int32_t dy) {
    uint32_t new_x_key =
        analog_joystick_arrow_for_axis(dx, data->x_arrow_key, cfg->deadzone,
                                       HID_USAGE_KEY_KEYBOARD_RIGHTARROW,
                                       HID_USAGE_KEY_KEYBOARD_LEFTARROW);
    uint32_t new_y_key =
        analog_joystick_arrow_for_axis(dy, data->y_arrow_key, cfg->deadzone,
                                       HID_USAGE_KEY_KEYBOARD_DOWNARROW,
                                       HID_USAGE_KEY_KEYBOARD_UPARROW);

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
        /*
         * Arrow keys go straight to the HID report, bypassing the event
         * system ZMK's idle/sleep tracking watches, so arrow-only use
         * would otherwise power the board off mid-use after
         * CONFIG_ZMK_IDLE_SLEEP_TIMEOUT. Any input event counts as
         * activity; an unsynced EV_MSC one is ignored by the mouse
         * input listener, so it sends nothing to the host.
         */
        input_report(data->dev, INPUT_EV_MSC, INPUT_MSC_SCAN, 0, false, K_NO_WAIT);
    }
}

/*
 * Pulls one axis's center toward the current reading while the stick is
 * near rest, bounded to stay close to the boot calibration.
 */
static void analog_joystick_recenter_axis(int32_t *center, int32_t rest, int32_t raw,
                                          uint16_t deadzone) {
    int32_t window = deadzone / RECENTER_WINDOW_DIV;
    int32_t max_drift = deadzone / RECENTER_MAX_DRIFT_DIV;
    int32_t d = raw - *center;

    if (d > window || d < -window) {
        return;
    }

    *center = CLAMP(*center + d / 32, rest - max_drift, rest + max_drift);
}

/*
 * Stuck-center recovery for one axis. The boot calibration only happens
 * once, 2s after power-up - including after waking from deep sleep,
 * which is a full reboot usually triggered by pressing the joystick's
 * own push button. If the stick is touched or tilted at that moment
 * (pressing it down tends to tilt it), the wrong position becomes the
 * center. On release the stick reads as a held deflection forever, and
 * the bounded auto-recentering can't fix an error that large.
 *
 * A released stick is very still; a held one jitters. So when an axis
 * reads past the deadzone but holds still for STUCK_RECOVER_MS, and that
 * reading is clearly closer to mid-scale than the current center is,
 * adopt it as the new center. The mid-scale test matters: the ADC is
 * ratiometric, so a pot/TMR stick rests near VDD/2 = mid-scale, while a
 * stick deliberately held still at a tilt reads *farther* from mid-scale
 * than a correct center does and is left alone.
 */
static void analog_joystick_recover_axis(const char *name, int32_t *center, int32_t *rest,
                                         int32_t *anchor, uint16_t *still_polls,
                                         int32_t raw, int32_t mid,
                                         const struct analog_joystick_config *cfg) {
    int32_t d = raw - *center;

    if (d <= cfg->deadzone && d >= -cfg->deadzone) {
        *still_polls = 0;
        return;
    }

    if (abs(raw - *anchor) > STILL_TOLERANCE) {
        *anchor = raw;
        *still_polls = 0;
        return;
    }

    uint32_t needed = STUCK_RECOVER_MS / MAX(cfg->poll_interval_ms, 1);
    if (++(*still_polls) < needed) {
        return;
    }
    *still_polls = 0;

    if (abs(raw - mid) + cfg->deadzone / 4 >= abs(*center - mid)) {
        return;
    }

    LOG_WRN("%s center stuck at %d, stick resting at %d; recalibrating", name, *center, raw);
    *center = raw;
    *rest = raw;
}

static uint32_t isqrt32(uint32_t v) {
    uint32_t root = 0;
    uint32_t bit = 1UL << 30;

    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (v >= root + bit) {
            v -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/*
 * Mouse mode: turns deflection into cursor velocity.
 *
 * - Radial deadzone: the deadzone is a circle around the center, not a
 *   square per axis, so diagonals near the center don't snap to an axis.
 * - Deadzone-rescaled: speed starts from 0 at the deadzone edge instead
 *   of jumping straight to a sizeable speed.
 * - Response curve: normalized deflection t (0..1) is raised to `curve`
 *   (1 = linear, 2 = quadratic, 3 = cubic), giving fine control near the
 *   center and full `max-speed` (px/s) at the edge.
 * - Time-based: movement per poll scales with the real time since the
 *   last poll, so speed doesn't depend on the (adaptive) poll rate.
 * - Sub-pixel accumulation: fractional pixels carry over to the next
 *   poll instead of being truncated, so slow speeds move smoothly
 *   rather than stalling or stepping.
 *
 * Sends the zero-delta "stop" sync event only once, on the transition
 * back to rest, rather than every idle poll cycle.
 */
static void analog_joystick_update_mouse_move(struct analog_joystick_data *data,
                                               const struct analog_joystick_config *cfg,
                                               int32_t dx, int32_t dy, int32_t dt_ms) {
    int32_t mag = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
    int32_t out_x = 0;
    int32_t out_y = 0;

    if (mag > cfg->deadzone) {
        /* Full deflection is roughly half the ADC range from center. */
        int32_t half = BIT(cfg->x_channel.resolution - 1);
        int32_t span = MAX(half - cfg->deadzone, 1);
        uint64_t t = MIN((uint64_t)(mag - cfg->deadzone) * Q16_ONE / span, Q16_ONE);

        uint64_t f = t;
        for (uint8_t i = 1; i < CLAMP(cfg->curve, 1, 3); i++) {
            f = f * t / Q16_ONE;
        }

        /* Pixels to move this poll, Q16, split along the stick direction. */
        int64_t step = (int64_t)cfg->max_speed * (int64_t)f * dt_ms / 1000;
        data->mouse_rem_x += (int32_t)(step * dx / mag);
        data->mouse_rem_y += (int32_t)(step * dy / mag);

        out_x = data->mouse_rem_x / Q16_ONE;
        out_y = data->mouse_rem_y / Q16_ONE;
        data->mouse_rem_x -= out_x * Q16_ONE;
        data->mouse_rem_y -= out_y * Q16_ONE;
    } else {
        data->mouse_rem_x = 0;
        data->mouse_rem_y = 0;
    }

    if (out_x == 0 && out_y == 0) {
        /* Still accumulating sub-pixel motion counts as moving, so don't
         * send the stop sync until the stick is actually back at rest. */
        if (mag <= cfg->deadzone && !data->idle_synced) {
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
    if (out_x != 0) {
        input_report_rel(data->dev, INPUT_REL_X, out_x, out_y == 0, K_NO_WAIT);
    }

    if (out_y != 0) {
        input_report_rel(data->dev, INPUT_REL_Y, out_y, true, K_NO_WAIT);
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

    int64_t now = k_uptime_get();
    int32_t dt_ms = CLAMP((int32_t)(now - data->last_poll_ms), 1,
                          (int32_t)cfg->idle_poll_interval_ms * 2);
    data->last_poll_ms = now;

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
     * Auto-recenter: corrects small persistent offsets (thermal drift,
     * mechanical creep) that would otherwise cause a constant slow drift
     * in one direction. Bounded - see RECENTER_WINDOW_DIV and
     * RECENTER_MAX_DRIFT_DIV for why.
     */
    analog_joystick_recenter_axis(&data->center_x, data->rest_x, raw_x, cfg->deadzone);
    analog_joystick_recenter_axis(&data->center_y, data->rest_y, raw_y, cfg->deadzone);

    analog_joystick_recover_axis("X", &data->center_x, &data->rest_x, &data->still_anchor_x,
                                 &data->still_polls_x, raw_x, BIT(cfg->x_channel.resolution - 1),
                                 cfg);
    analog_joystick_recover_axis("Y", &data->center_y, &data->rest_y, &data->still_anchor_y,
                                 &data->still_polls_y, raw_y, BIT(cfg->y_channel.resolution - 1),
                                 cfg);

    if (arrow_mode) {
        analog_joystick_update_arrow_keys(data, cfg, dx, dy);
    } else {
        if (data->x_arrow_key != 0 || data->y_arrow_key != 0) {
            /* Layer changed mid-hold - don't leave an arrow key stuck down. */
            analog_joystick_release_arrow_keys(data);
        }
        analog_joystick_update_mouse_move(data, cfg, dx, dy, dt_ms);
    }

    /*
     * Adaptive polling: fast while the stick is (starting to be) tilted
     * or an arrow key is held, slower at rest to save power. The
     * threshold is well inside the deadzone, so the fast rate is already
     * running by the time movement starts.
     */
    int32_t wake = cfg->deadzone / 4;
    bool active = dx > wake || dx < -wake || dy > wake || dy < -wake ||
                  data->x_arrow_key != 0 || data->y_arrow_key != 0;
    k_work_schedule(&data->work,
                    K_MSEC(active ? cfg->poll_interval_ms : cfg->idle_poll_interval_ms));
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
    data->mouse_rem_x = 0;
    data->mouse_rem_y = 0;
    data->last_poll_ms = k_uptime_get();
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
        .max_speed = DT_INST_PROP(n, max_speed),                                                 \
        .curve = DT_INST_PROP(n, curve),                                                         \
        .poll_interval_ms = DT_INST_PROP(n, poll_interval_ms),                                   \
        .idle_poll_interval_ms = DT_INST_PROP(n, idle_poll_interval_ms),                         \
        .arrow_layer = DT_INST_PROP(n, arrow_layer),                                             \
    };                                                                                           \
    DEVICE_DT_INST_DEFINE(n, analog_joystick_init, NULL, &analog_joystick_data_##n,               \
                           &analog_joystick_config_##n, POST_KERNEL,                              \
                           CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ANALOG_JOYSTICK_INIT)
