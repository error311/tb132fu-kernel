// SPDX-License-Identifier: GPL-2.0
/* Lenovo TB132FU folio-cover hall sensor. */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/platform_data/i2c-hid.h>
#include <linux/workqueue.h>

#define TB132FU_KEY_HALL_OPEN	0x289
#define TB132FU_KEY_HALL_CLOSE	0x290

struct tb132fu_hall {
	struct device *dev;
	struct input_dev *input;
	struct delayed_work report_work;
	int gpio;
	int irq;
	bool closed;
	struct class *hall_class;
};

static struct tb132fu_hall *tb132fu_hall_device;

static void tb132fu_hall_report(struct tb132fu_hall *hall, bool force)
{
	bool closed = !gpio_get_value_cansleep(hall->gpio);
	unsigned int key = closed ? TB132FU_KEY_HALL_CLOSE :
					 TB132FU_KEY_HALL_OPEN;

	if (!force && closed == hall->closed)
		return;

	hall->closed = closed;
	tb132fu_i2c_hid_set_cover_state(closed);
	input_report_switch(hall->input, SW_LID, closed);
	input_report_key(hall->input, key, 1);
	input_sync(hall->input);
	input_report_key(hall->input, key, 0);
	input_sync(hall->input);
	dev_info(hall->dev, "folio cover %s\n", closed ? "closed" : "open");
}

static void tb132fu_hall_report_work(struct work_struct *work)
{
	struct tb132fu_hall *hall = container_of(to_delayed_work(work),
						 struct tb132fu_hall, report_work);

	tb132fu_hall_report(hall, false);
}

static irqreturn_t tb132fu_hall_irq(int irq, void *data)
{
	struct tb132fu_hall *hall = data;

	mod_delayed_work(system_wq, &hall->report_work, msecs_to_jiffies(150));
	return IRQ_HANDLED;
}

static ssize_t hall_state_show(struct class *class,
			       struct class_attribute *attr, char *buf)
{
	struct tb132fu_hall *hall = tb132fu_hall_device;

	if (!hall)
		return -ENODEV;
	return scnprintf(buf, PAGE_SIZE, "%d\n", hall->closed ? 2 : 3);
}
static CLASS_ATTR_RO(hall_state);

static int tb132fu_hall_probe(struct platform_device *pdev)
{
	struct tb132fu_hall *hall;
	unsigned long irqflags = IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				 IRQF_ONESHOT;
	int ret;

	hall = devm_kzalloc(&pdev->dev, sizeof(*hall), GFP_KERNEL);
	if (!hall)
		return -ENOMEM;

	hall->dev = &pdev->dev;
	hall->gpio = of_get_named_gpio(pdev->dev.of_node, "hall,irq-gpio", 0);
	if (!gpio_is_valid(hall->gpio))
		return hall->gpio < 0 ? hall->gpio : -EINVAL;

	ret = devm_gpio_request_one(&pdev->dev, hall->gpio, GPIOF_IN,
				    "hall-switch-gpio");
	if (ret)
		return ret;

	hall->input = devm_input_allocate_device(&pdev->dev);
	if (!hall->input)
		return -ENOMEM;
	hall->input->name = "hall-switch-input";
	hall->input->dev.parent = &pdev->dev;
	input_set_capability(hall->input, EV_SW, SW_LID);
	input_set_capability(hall->input, EV_KEY, TB132FU_KEY_HALL_OPEN);
	input_set_capability(hall->input, EV_KEY, TB132FU_KEY_HALL_CLOSE);
	ret = input_register_device(hall->input);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&hall->report_work, tb132fu_hall_report_work);
	hall->irq = gpio_to_irq(hall->gpio);
	if (hall->irq < 0)
		return hall->irq;
	ret = devm_request_threaded_irq(&pdev->dev, hall->irq, NULL,
					tb132fu_hall_irq, irqflags,
					"hall-switch-irq", hall);
	if (ret)
		return ret;

	device_init_wakeup(&pdev->dev, true);
	ret = enable_irq_wake(hall->irq);
	if (ret)
		dev_warn(&pdev->dev, "failed to enable hall wake IRQ: %d\n", ret);

	hall->hall_class = class_create(THIS_MODULE, "hall_class");
	if (IS_ERR(hall->hall_class)) {
		dev_warn(&pdev->dev, "failed to create hall_class\n");
		hall->hall_class = NULL;
	} else {
		ret = class_create_file(hall->hall_class, &class_attr_hall_state);
		if (ret)
			dev_warn(&pdev->dev, "failed to create hall_state: %d\n", ret);
	}

	platform_set_drvdata(pdev, hall);
	tb132fu_hall_device = hall;
	tb132fu_hall_report(hall, true);
	return 0;
}

static int tb132fu_hall_remove(struct platform_device *pdev)
{
	struct tb132fu_hall *hall = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&hall->report_work);
	disable_irq_wake(hall->irq);
	device_init_wakeup(&pdev->dev, false);
	if (hall->hall_class) {
		class_remove_file(hall->hall_class, &class_attr_hall_state);
		class_destroy(hall->hall_class);
	}
	tb132fu_hall_device = NULL;
	return 0;
}

static const struct of_device_id tb132fu_hall_of_match[] = {
	{ .compatible = "hall-switch" },
	{ .compatible = "p11,hall" },
	{ }
};
MODULE_DEVICE_TABLE(of, tb132fu_hall_of_match);

static struct platform_driver tb132fu_hall_driver = {
	.probe = tb132fu_hall_probe,
	.remove = tb132fu_hall_remove,
	.driver = {
		.name = "tb132fu-hall",
		.of_match_table = tb132fu_hall_of_match,
	},
};
module_platform_driver(tb132fu_hall_driver);

MODULE_DESCRIPTION("Lenovo TB132FU folio-cover hall sensor");
MODULE_LICENSE("GPL v2");
