// SPDX-License-Identifier: GPL-2.0
/*
 * The built-in pad of Anbernic's H700 handhelds as one input device: the buttons
 * (a GPIO each, on its interrupt) and the sticks where the board has them (IIO
 * channels, polled). Every board reports the same buttons, whichever it has, so
 * a button's number is the same on all of them.
 *
 * Child nodes: a button has gpios and linux,code; an axis has reg (its index in
 * io-channels), linux,code, abs-range (high to low inverts it), abs-fuzz and abs-flat.
 */
#include <linux/gpio/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/workqueue.h>

/* Every board's buttons (Documentation/input/gamepad.rst) */
static const unsigned int pad_codes[] = {
	BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
	BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR,
	BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
};

/* As gpio-keys' default */
#define PAD_DEBOUNCE_MS 5
#define PAD_POLL_MS 16

struct pad_button {
	struct gpio_desc *gpio;
	unsigned int code;
};

struct pad_axis {
	unsigned int code;
	int min, max;
	bool inverted, set;
};

struct pad {
	struct input_dev *input;
	struct delayed_work debounce;
	struct pad_button *buttons;
	int nbuttons;
	struct iio_channel *chans;
	struct pad_axis *axes;
	int naxes;
};

static void pad_report_buttons(struct work_struct *work)
{
	struct pad *pad = container_of(work, struct pad, debounce.work);
	int i, v;

	for (i = 0; i < pad->nbuttons; i++) {
		v = gpiod_get_value_cansleep(pad->buttons[i].gpio);
		if (v >= 0)
			input_report_key(pad->input, pad->buttons[i].code, v);
	}
	input_sync(pad->input);
}

/* Any button's edge: all of them read once they have settled */
static irqreturn_t pad_irq(int irq, void *data)
{
	struct pad *pad = data;

	mod_delayed_work(system_wq, &pad->debounce, msecs_to_jiffies(PAD_DEBOUNCE_MS));
	return IRQ_HANDLED;
}

static void pad_poll(struct input_dev *input)
{
	struct pad *pad = input_get_drvdata(input);
	int i, val;

	for (i = 0; i < pad->naxes; i++) {
		if (iio_read_channel_raw(&pad->chans[i], &val) < 0)
			return;
		if (pad->axes[i].inverted)
			val = pad->axes[i].min + pad->axes[i].max - val;
		input_report_abs(input, pad->axes[i].code, val);
	}
	input_sync(input);
}

static void pad_cancel(void *data)
{
	struct pad *pad = data;

	cancel_delayed_work_sync(&pad->debounce);
}

static int pad_add_button(struct device *dev, struct pad *pad, struct fwnode_handle *child)
{
	struct pad_button *b = &pad->buttons[pad->nbuttons];
	int irq, err;

	if (fwnode_property_read_u32(child, "linux,code", &b->code))
		return dev_err_probe(dev, -EINVAL, "%pfwP: no linux,code\n", child);
	b->gpio = devm_fwnode_gpiod_get(dev, child, NULL, GPIOD_IN, fwnode_get_name(child));
	if (IS_ERR(b->gpio))
		return dev_err_probe(dev, PTR_ERR(b->gpio), "%pfwP: no gpio\n", child);
	irq = gpiod_to_irq(b->gpio);
	if (irq < 0)
		return dev_err_probe(dev, irq, "%pfwP: no interrupt\n", child);
	err = devm_request_irq(dev, irq, pad_irq, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       "anbernic-gamepad", pad);
	if (err)
		return dev_err_probe(dev, err, "%pfwP: interrupt\n", child);
	input_set_capability(pad->input, EV_KEY, b->code);
	pad->nbuttons++;
	return 0;
}

static int pad_add_axis(struct device *dev, struct pad *pad, struct fwnode_handle *child)
{
	struct pad_axis *a;
	u32 reg, range[2], fuzz = 0, flat = 0;

	if (fwnode_property_read_u32(child, "reg", &reg) || reg >= pad->naxes)
		return dev_err_probe(dev, -EINVAL, "%pfwP: reg past io-channels\n", child);
	a = &pad->axes[reg];
	a->set = true;
	if (fwnode_property_read_u32(child, "linux,code", &a->code) ||
	    fwnode_property_read_u32_array(child, "abs-range", range, 2))
		return dev_err_probe(dev, -EINVAL, "%pfwP: no linux,code or abs-range\n", child);
	fwnode_property_read_u32(child, "abs-fuzz", &fuzz);
	fwnode_property_read_u32(child, "abs-flat", &flat);
	a->inverted = range[0] > range[1];
	a->min = min(range[0], range[1]);
	a->max = max(range[0], range[1]);
	input_set_abs_params(pad->input, a->code, a->min, a->max, fuzz, flat);
	return 0;
}

static int pad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pad *pad;
	int i, err, n = device_get_child_node_count(dev);

	pad = devm_kzalloc(dev, sizeof(*pad), GFP_KERNEL);
	if (!pad)
		return -ENOMEM;
	pad->buttons = devm_kcalloc(dev, n, sizeof(*pad->buttons), GFP_KERNEL);
	pad->input = devm_input_allocate_device(dev);
	if (!pad->buttons || !pad->input)
		return -ENOMEM;
	INIT_DELAYED_WORK(&pad->debounce, pad_report_buttons);
	/* Before the interrupts: freed after them */
	err = devm_add_action(dev, pad_cancel, pad);
	if (err)
		return err;

	pad->input->name = "Anbernic Gamepad";
	pad->input->phys = "anbernic-gamepad/input0";
	/* gpio-keys' ids: SDL's GUID for the pad as it was */
	pad->input->id.bustype = BUS_HOST;
	pad->input->id.vendor = 0x0001;
	pad->input->id.product = 0x0001;
	pad->input->id.version = 0x0100;
	input_set_drvdata(pad->input, pad);
	for (i = 0; i < ARRAY_SIZE(pad_codes); i++)
		input_set_capability(pad->input, EV_KEY, pad_codes[i]);

	if (device_property_present(dev, "io-channels")) {
		pad->chans = devm_iio_channel_get_all(dev);
		if (IS_ERR(pad->chans))
			return dev_err_probe(dev, PTR_ERR(pad->chans), "sticks' channels\n");
		while (pad->chans[pad->naxes].indio_dev)
			pad->naxes++;
		pad->axes = devm_kcalloc(dev, pad->naxes, sizeof(*pad->axes), GFP_KERNEL);
		if (!pad->axes)
			return -ENOMEM;
	}

	device_for_each_child_node_scoped(dev, child) {
		err = fwnode_property_present(child, "reg") ? pad_add_axis(dev, pad, child)
							    : pad_add_button(dev, pad, child);
		if (err)
			return err;
	}
	for (i = 0; i < pad->naxes; i++)
		if (!pad->axes[i].set)
			return dev_err_probe(dev, -EINVAL, "no axis for channel %d\n", i);

	if (pad->naxes) {
		err = input_setup_polling(pad->input, pad_poll);
		if (err)
			return err;
		input_set_poll_interval(pad->input, PAD_POLL_MS);
	}
	err = input_register_device(pad->input);
	if (err)
		return err;
	/* What is held already */
	mod_delayed_work(system_wq, &pad->debounce, 0);
	return 0;
}

static const struct of_device_id pad_of_match[] = {
	{ .compatible = "anbernic,gamepad" },
	{ }
};
MODULE_DEVICE_TABLE(of, pad_of_match);

static struct platform_driver pad_driver = {
	.probe = pad_probe,
	.driver = {
		.name = "anbernic-gamepad",
		.of_match_table = pad_of_match,
	},
};
module_platform_driver(pad_driver);

MODULE_DESCRIPTION("Anbernic H700 handhelds' built-in pad");
MODULE_LICENSE("GPL");
