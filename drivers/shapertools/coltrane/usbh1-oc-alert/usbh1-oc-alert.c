#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#define DEBOUNCE_DELAY_MS 50

struct usbh1_oc_data {
	int gpio;
	int irq;
	struct device *dev;
	struct delayed_work debounce_work;
};

static void usbh1_oc_debounce_work(struct work_struct *work)
{
	struct usbh1_oc_data *data =
		container_of(to_delayed_work(work), struct usbh1_oc_data, debounce_work);
	int val = gpio_get_value(data->gpio);

	if (val)
		dev_info(data->dev, "USBH1 Overcurrent condition cleared!\n");
	else
		dev_warn(data->dev, "USBH1 Overcurrent condition detected!\n");
}

static irqreturn_t usbh1_oc_irq_handler(int irq, void *dev_id)
{
	struct usbh1_oc_data *data = dev_id;

	/* Schedule debounce work */
	mod_delayed_work(system_wq, &data->debounce_work,
			 msecs_to_jiffies(DEBOUNCE_DELAY_MS));

	return IRQ_HANDLED;
}

static int usbh1_oc_probe(struct platform_device *pdev)
{
	struct usbh1_oc_data *data;
	struct device_node *np = pdev->dev.of_node;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->gpio = of_get_named_gpio(np, "gpios", 0);
	if (!gpio_is_valid(data->gpio))
		return dev_err_probe(&pdev->dev, -EINVAL, "Invalid GPIO\n");

	ret = devm_gpio_request_one(&pdev->dev, data->gpio, GPIOF_IN, "usbh1_oc_gpio");
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to request GPIO\n");

	data->irq = gpio_to_irq(data->gpio);
	if (data->irq < 0)
		return dev_err_probe(&pdev->dev, data->irq, "Failed to get IRQ\n");

	INIT_DELAYED_WORK(&data->debounce_work, usbh1_oc_debounce_work);

	ret = devm_request_threaded_irq(&pdev->dev, data->irq,
					NULL, usbh1_oc_irq_handler,
					IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"usbh1_oc_irq", data);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to request IRQ\n");

	data->dev = &pdev->dev;
	platform_set_drvdata(pdev, data);

	dev_info(&pdev->dev, "USBH1 overcurrent notifier with debounce loaded\n");
	return 0;
}

static const struct of_device_id usbh1_oc_dt_ids[] = {
	{ .compatible = "shaper,usbh1-oc-alert", },
	{ }
};
MODULE_DEVICE_TABLE(of, usbh1_oc_dt_ids);

static struct platform_driver usbh1_oc_driver = {
	.probe = usbh1_oc_probe,
	.driver = {
		.name = "usbh1-oc-alert",
		.of_match_table = usbh1_oc_dt_ids,
	},
};

module_platform_driver(usbh1_oc_driver);
MODULE_DESCRIPTION("USBH1 Overcurrent GPIO Notifier with Debounce");
MODULE_AUTHOR("Jeremy Blum <jeremy@shapertools.com>");
MODULE_LICENSE("GPL");
