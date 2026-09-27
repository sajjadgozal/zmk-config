/*
 * Analog joystick -> relative mouse movement input driver.
 *
 * Polls two ADC channels (X, Y) on a fixed interval. Deflection from the
 * resting center position, past a configurable deadzone, is reported as
 * relative INPUT_REL_X / INPUT_REL_Y events - i.e. trackpoint-style
 * continuous cursor movement while the stick is held off-center.
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

LOG_MODULE_REGISTER(analog_joystick, CONFIG_INPUT_LOG_LEVEL);

struct analog_joystick_config {
    struct adc_dt_spec x_channel;
    struct adc_dt_spec y_channel;
    uint16_t deadzone;
    uint16_t sensitivity;
    uint16_t poll_interval_ms;
};

struct analog_joystick_data {
    const struct device *dev;
    struct k_work_delayable work;
    int32_t center_x;
    int32_t center_y;
    bool calibrated;
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

static int32_t analog_joystick_read(const struct adc_dt_spec *spec) {
    int16_t buf = 0;
    struct adc_sequence sequence = {
        .buffer = &buf,
        .buffer_size = sizeof(buf),
    };

    int err = adc_sequence_init_dt(spec, &sequence);
    if (err < 0) {
        LOG_ERR("Failed to init ADC sequence: %d", err);
        return 0;
    }

    err = adc_read(spec->dev, &sequence);
    if (err < 0) {
        LOG_ERR("ADC read failed: %d", err);
        return 0;
    }

    return buf;
}

static void analog_joystick_calibrate(struct analog_joystick_data *data,
                                       const struct analog_joystick_config *cfg) {
    int32_t sum_x = 0;
    int32_t sum_y = 0;
    const int samples = 8;

    for (int i = 0; i < samples; i++) {
        sum_x += analog_joystick_read(&cfg->x_channel);
        sum_y += analog_joystick_read(&cfg->y_channel);
        k_msleep(5);
    }

    data->center_x = sum_x / samples;
    data->center_y = sum_y / samples;
    data->calibrated = true;

    LOG_INF("Calibrated center: x=%d y=%d", data->center_x, data->center_y);
}

static void analog_joystick_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct analog_joystick_data *data =
        CONTAINER_OF(dwork, struct analog_joystick_data, work);
    const struct analog_joystick_config *cfg = data->dev->config;

    if (!data->calibrated) {
        analog_joystick_calibrate(data, cfg);
        k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));
        return;
    }

    int32_t raw_x = analog_joystick_read(&cfg->x_channel);
    int32_t raw_y = analog_joystick_read(&cfg->y_channel);

    int32_t dx = raw_x - data->center_x;
    int32_t dy = raw_y - data->center_y;

    LOG_DBG("raw_x=%d raw_y=%d dx=%d dy=%d", raw_x, raw_y, dx, dy);

    int32_t move_x = 0;
    int32_t move_y = 0;
    bool report_x = false;
    bool report_y = false;

    if (dx > cfg->deadzone || dx < -cfg->deadzone) {
        move_x = dx / (int32_t)cfg->sensitivity;
        report_x = move_x != 0;
    } else {
        /*
         * Within the deadzone: slowly drift the calibrated center toward
         * the current reading. Corrects small persistent offsets (an
         * imperfect boot-time calibration, thermal drift, mechanical
         * creep) that would otherwise cause a slow constant drift in one
         * direction forever - without ever affecting a genuine held
         * deflection, since this branch only runs while already below
         * the deadzone threshold.
         */
        data->center_x += (raw_x - data->center_x) / 32;
    }

    if (dy > cfg->deadzone || dy < -cfg->deadzone) {
        move_y = dy / (int32_t)cfg->sensitivity;
        report_y = move_y != 0;
    } else {
        data->center_y += (raw_y - data->center_y) / 32;
    }

    /*
     * Exactly one call per cycle must carry sync=true - the mouse HID
     * listener accumulates dx/dy across calls and only flushes an HID
     * report on the synced call. Previously X was always reported with
     * sync=false, and the fallback sync-only event only fired when
     * *nothing* moved - so an X-only movement (Y within its deadzone)
     * queued a delta that never got flushed. Report Y last (if it's
     * firing) since it's naturally the synced call already; otherwise
     * make X the synced call; otherwise send a zero-delta synced event
     * so listeners don't stall.
     */
    if (report_x) {
        input_report_rel(data->dev, INPUT_REL_X, move_x, !report_y, K_NO_WAIT);
    }

    if (report_y) {
        input_report_rel(data->dev, INPUT_REL_Y, move_y, true, K_NO_WAIT);
    }

    if (!report_x && !report_y) {
        input_report_rel(data->dev, INPUT_REL_X, 0, true, K_NO_WAIT);
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

    /*
     * Calibration happens on the work queue's first run (see
     * analog_joystick_work_handler), not here - this avoids a multi-
     * second blocking sleep in device init, which would delay every
     * other POST_KERNEL driver that initializes after this one
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
    };                                                                                           \
    DEVICE_DT_INST_DEFINE(n, analog_joystick_init, NULL, &analog_joystick_data_##n,               \
                           &analog_joystick_config_##n, POST_KERNEL,                              \
                           CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ANALOG_JOYSTICK_INIT)
