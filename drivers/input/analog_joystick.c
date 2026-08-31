/*
 * Analog joystick -> relative mouse movement input driver.
 *
 * Polls two ADC channels (X, Y) on a fixed interval. Deflection from the
 * resting center position, past a configurable deadzone, is reported as
 * relative INPUT_REL_X / INPUT_REL_Y events - i.e. trackpoint-style
 * continuous cursor movement while the stick is held off-center.
 *
 * First draft: not hardware-tested. `deadzone` and `sensitivity` in the
 * devicetree node will very likely need tuning once flashed to real
 * hardware.
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
};

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

static void analog_joystick_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct analog_joystick_data *data =
        CONTAINER_OF(dwork, struct analog_joystick_data, work);
    const struct analog_joystick_config *cfg = data->dev->config;

    int32_t raw_x = analog_joystick_read(&cfg->x_channel);
    int32_t raw_y = analog_joystick_read(&cfg->y_channel);

    int32_t dx = raw_x - data->center_x;
    int32_t dy = raw_y - data->center_y;

    bool moved = false;

    if (dx > cfg->deadzone || dx < -cfg->deadzone) {
        int32_t move = dx / (int32_t)cfg->sensitivity;
        if (move != 0) {
            input_report_rel(data->dev, INPUT_REL_X, move, false, K_NO_WAIT);
            moved = true;
        }
    }

    if (dy > cfg->deadzone || dy < -cfg->deadzone) {
        int32_t move = dy / (int32_t)cfg->sensitivity;
        if (move != 0) {
            input_report_rel(data->dev, INPUT_REL_Y, move, true, K_NO_WAIT);
            moved = true;
        }
    }

    if (!moved) {
        /* Ensure a sync event is still sent so listeners don't stall. */
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
    /* Sample once at boot to establish the resting center position. */
    data->center_x = analog_joystick_read(&cfg->x_channel);
    data->center_y = analog_joystick_read(&cfg->y_channel);

    k_work_init_delayable(&data->work, analog_joystick_work_handler);
    k_work_schedule(&data->work, K_MSEC(cfg->poll_interval_ms));

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
