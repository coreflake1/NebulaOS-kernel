/*
 * Nsiway NS2009 touchscreen controller driver
 *
 * Copyright (C) 2017 Icenowy Zheng <icenowy@aosc.xyz>
 *
 * Some codes are from silead.c, which is
 *   Copyright (C) 2014-2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifdef CONFIG_TOUCHSCREEN_NS2009_FINAL_QUALIFICATION
/* NS2009_FINAL_QUALIFICATION (display/touch investigation mission
 * follow-on, 2026-08-02+): forward declarations only - the real driver
 * lives entirely in the new, separate ns2009_final_qualification.c (see
 * that file's own header comment for the full design). Deliberately not a
 * shared header under module_drivers/include/ for two one-line extern
 * declarations, matching this project's own established precedent for a
 * single cross-file symbol reference (pwm-ingenic-v2.c /
 * nebulaos_backlight_probe_diag.c, see
 * docs/NEBULAOS_BACKLIGHT_DIAGNOSTIC_PLAN.md) - except here both files are
 * compiled into the SAME module/built-in object (see the composite
 * ns2009-y Makefile lines this feature adds), so plain, non-exported
 * "extern" is correct and sufficient; no EXPORT_SYMBOL_GPL() is needed or
 * added anywhere by this feature.
 *
 * Deliberately placed here, at the very top of the file before any
 * #include, and never touched again below - this is the ONLY thing this
 * feature adds outside of a single opaque struct field (see struct
 * ns2009_data below) and a single two-line hook at the very end of
 * ns2009_ts_poll(). This keeps this patch's footprint far away from every
 * insertion point the completely separate, pre-existing
 * CONFIG_TOUCHSCREEN_NS2009_QUALIFICATION patch
 * (scripts/build/patches/touch-qualification-unified.patch) uses in this
 * same file, so the two patches apply cleanly in either order - see
 * scripts/build/touch-final-qualification-variant.sh's header comment for
 * the direct verification this project performed of both apply orders. */
#include <linux/types.h>

struct i2c_client;
struct input_dev;
struct gpio_desc;

void *ns2009_nfq_probe(struct i2c_client *client, struct input_dev *input,
			struct gpio_desc *pendown_gpio,
			unsigned int normal_poll_interval_ms);
void ns2009_nfq_on_poll(void *handle, bool pen_down);
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>

/* polling interval in ms */
#define POLL_INTERVAL	30

/* this driver uses 12-bit readout */
#define MAX_12BIT	0xfff

#define NS2009_TS_NAME	"ns2009_ts"

#define NS2009_READ_X_LOW_POWER_12BIT	0xc0
#define NS2009_READ_Y_LOW_POWER_12BIT	0xd0
#define NS2009_READ_Z1_LOW_POWER_12BIT	0xe0
#define NS2009_READ_Z2_LOW_POWER_12BIT	0xf0

#define NS2009_DEF_X_FUZZ	32
#define NS2009_DEF_Y_FUZZ	16

/*
 * The chip have some error in z1 value when pen is up, so the data read out
 * is sometimes not accurately 0.
 * This value is based on experiements.
 */
#define NS2009_PEN_UP_Z1_ERR	80

struct ns2009_data {
	struct i2c_client		*client;
	struct input_dev		*input;

	struct touchscreen_properties	prop;

	bool				pen_down;

#ifdef CONFIG_TOUCHSCREEN_NS2009_FINAL_QUALIFICATION
	/* NS2009_FINAL_QUALIFICATION: fully opaque handle - no struct-layout
	 * dependency between this file and ns2009_final_qualification.c
	 * (deliberate: keeps this feature's ns2009.c footprint to a single
	 * field, see the forward-declaration block near the top of this
	 * file). NULL until the first poll tick lazily initializes it (see
	 * ns2009_ts_poll() below); every ns2009_nfq_*() call is written to
	 * tolerate a NULL/inert handle safely, so this can never affect
	 * ns2009_ts_probe()'s own success or the existing poll path. */
	void				*nfq_handle;
#endif

	/* ke-mainline-klipper touch mission: optional "pendown-gpios" DT
	 * property, matching stock's real (disassembly-proven) touch-detect
	 * signal - see the halley5_v30.dts ns2009@48 node for the full
	 * evidence trail. NULL (no property present) preserves the original
	 * generic upstream Z1-threshold-only behavior unchanged for any other
	 * board using this driver. */
	struct gpio_desc		*pendown_gpio;
};

static int ns2009_ts_read_data(struct ns2009_data *data, u8 cmd, u16 *val)
{
	u8 raw_data[2];
	int error;

	error = i2c_smbus_read_i2c_block_data(data->client, cmd, 2, raw_data);
	if (error < 0)
		return error;

	if (unlikely(raw_data[1] & 0xf))
		return -EINVAL;

	*val = (raw_data[0] << 4) | (raw_data[1] >> 4);

	return 0;
}

static int ns2009_ts_report(struct ns2009_data *data)
{
	u16 x, y, z1;
	int ret;
	bool pen_is_down;

	/* ke-mainline-klipper touch mission: when a pendown-gpios property is
	 * present, use it as the pen-down signal instead of the Z1 pressure
	 * threshold - proven on this exact board that the touch driver never
	 * generated coordinate events via Z1 polling, while stock's own
	 * driver never uses Z1 at all (interrupt-driven off this exact GPIO
	 * instead - see halley5_v30.dts ns2009@48 for the full evidence
	 * trail). Physically confirmed fixed: all corners and center
	 * activate accurately. Boards without the property keep the
	 * original upstream Z1-only behavior unchanged. */
	if (data->pendown_gpio) {
		pen_is_down = gpiod_get_value_cansleep(data->pendown_gpio);
	} else {
		/*
		 * NS2009 chip supports pressure measurement, but currently it
		 * needs more investigation, so we only use z1 axis to detect
		 * pen down here.
		 */
		ret = ns2009_ts_read_data(data, NS2009_READ_Z1_LOW_POWER_12BIT, &z1);
		if (ret)
			return ret;
		pen_is_down = z1 >= NS2009_PEN_UP_Z1_ERR;
	}

	if (pen_is_down) {
		ret = ns2009_ts_read_data(data, NS2009_READ_X_LOW_POWER_12BIT,
					  &x);
		if (ret)
			return ret;

		ret = ns2009_ts_read_data(data, NS2009_READ_Y_LOW_POWER_12BIT,
					  &y);
		if (ret)
			return ret;

		if (!data->pen_down) {
			input_report_key(data->input, BTN_TOUCH, 1);
			data->pen_down = true;
		}

		input_report_abs(data->input, ABS_X, x);
		input_report_abs(data->input, ABS_Y, y);
		input_sync(data->input);
	} else if (data->pen_down) {
		input_report_key(data->input, BTN_TOUCH, 0);
		input_sync(data->input);
		data->pen_down = false;
	}
	return 0;
}

static void ns2009_ts_poll(struct input_dev *input_dev)
{
	struct ns2009_data *data = input_get_drvdata(input_dev);
	int ret;

	ret = ns2009_ts_report(data);
	if (ret)
		dev_err(&input_dev->dev, "Poll touch data failed: %d\n", ret);

#ifdef CONFIG_TOUCHSCREEN_NS2009_FINAL_QUALIFICATION
	/* Lazy first-tick init - input registration has already succeeded
	 * by the time any poll tick can run, so data->input is always valid
	 * here. Boot-time behavior is unaffected either way: no GPIO IRQ is
	 * ever requested until a debugfs client explicitly asks for
	 * "irq-assist" (see ns2009_final_qualification.c). */
	if (unlikely(!data->nfq_handle))
		data->nfq_handle = ns2009_nfq_probe(data->client, data->input,
						     data->pendown_gpio, POLL_INTERVAL);
	ns2009_nfq_on_poll(data->nfq_handle, data->pen_down);
#endif
}

static void ns2009_ts_config_input_dev(struct ns2009_data *data)
{
	struct input_dev *input = data->input;

	input_set_abs_params(input, ABS_X, 0, MAX_12BIT, NS2009_DEF_X_FUZZ, 0);
	input_set_abs_params(input, ABS_Y, 0, MAX_12BIT, NS2009_DEF_Y_FUZZ, 0);
	touchscreen_parse_properties(input, false, &data->prop);

	input->name = NS2009_TS_NAME;
	input->phys = "input/ts";
	input->id.bustype = BUS_I2C;
	input_set_capability(input, EV_KEY, BTN_TOUCH);
}

static int ns2009_ts_request_polled_input_dev(struct ns2009_data *data)
{
	struct device *dev = &data->client->dev;
	int error;

	data->input = devm_input_allocate_device(dev);
	if (!data->input) {
		dev_err(dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}
	input_set_drvdata(data->input, data);

	ns2009_ts_config_input_dev(data);

	error = input_setup_polling(data->input, ns2009_ts_poll);
	if (error) {
		dev_err(dev, "Failed to set up polling: %d\n", error);
		return error;
	}
	input_set_poll_interval(data->input, POLL_INTERVAL);

	error = input_register_device(data->input);
	if (error) {
		dev_err(dev, "Failed to register input device: %d\n", error);
		return error;
	}

	return 0;
}

static int ns2009_ts_probe(struct i2c_client *client)
{
	struct ns2009_data *data;
	struct device *dev = &client->dev;
	int error;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_READ_I2C_BLOCK |
				     I2C_FUNC_SMBUS_WRITE_I2C_BLOCK)) {
		dev_err(dev, "I2C functionality check failed\n");
		return -ENXIO;
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	i2c_set_clientdata(client, data);
	data->client = client;

	/* ke-mainline-klipper touch mission: optional, absent on any board
	 * that doesn't declare "pendown-gpios" in its DT node - see the
	 * struct field comment and halley5_v30.dts for the full evidence
	 * trail behind this exact property on this exact board. */
	data->pendown_gpio = devm_gpiod_get_optional(dev, "pendown", GPIOD_IN);
	if (IS_ERR(data->pendown_gpio)) {
		dev_err(dev, "Failed to get pendown-gpios: %ld\n",
			PTR_ERR(data->pendown_gpio));
		return PTR_ERR(data->pendown_gpio);
	}

	error = ns2009_ts_request_polled_input_dev(data);
	if (error)
		return error;

	return 0;
};

static const struct i2c_device_id ns2009_ts_id[] = {
	{ "ns2009", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ns2009_ts_id);

#ifdef CONFIG_OF
static const struct of_device_id ns2009_ts_of_match[] = {
	{ .compatible = "nsiway,ns2009", },
	{ }
};
MODULE_DEVICE_TABLE(of, ns2009_ts_of_match);
#endif

static struct i2c_driver ns2009_ts_driver = {
	.probe = ns2009_ts_probe,
	.id_table = ns2009_ts_id,
	.driver = {
		.name = NS2009_TS_NAME,
		.of_match_table = of_match_ptr(ns2009_ts_of_match),
	},
};
module_i2c_driver(ns2009_ts_driver);

MODULE_AUTHOR("Icenowy Zheng <icenowy@aosc.xyz>");
MODULE_DESCRIPTION("Nsiway NS2009 touchscreen controller driver");
MODULE_LICENSE("GPL");
