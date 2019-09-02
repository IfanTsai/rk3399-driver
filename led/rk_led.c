#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>

static struct class *led_class;
#define LED_CLASS "rk_led_class"

#define LED_ON_LEVEL    (1)
#define LED_OFF_LEVEL   (0)

#define LED_MAGIC      'c'
#define IOCTL_LED_ON    _IOW(LED_MAGIC, 1, int)
#define IOCTL_LED_OFF   _IOW(LED_MAGIC, 2, int)
#define IOCTL_LED_SET_SHINE_CNT    _IOW(LED_MAGIC, 3, int)

struct led_data {
	unsigned int led_gpio;
	const char *led_name;
	dev_t led_dev;
	struct cdev led_cdev;
};

static long led_ioctl(struct file *file, unsigned int cmd, unsigned long cnt)
{
	struct led_data *led_data = file->private_data;

	switch (cmd) {
	case IOCTL_LED_ON:
		gpio_set_value(led_data->led_gpio, LED_ON_LEVEL);
		pr_err("====> %s: led_ioctl on!\n", led_data->led_name);
		break;
	case IOCTL_LED_OFF:
		gpio_set_value(led_data->led_gpio, LED_OFF_LEVEL);
		pr_err("====> %s: led_ioctl off!\n", led_data->led_name);
		break;
	case IOCTL_LED_SET_SHINE_CNT:
	{
		int i, val = gpio_get_value(led_data->led_gpio);

		pr_err("====> %s: led_ioctl set shine count %ld!\n", led_data->led_name, cnt);
		for (i = 0; i < cnt; i++) {
			gpio_set_value(led_data->led_gpio, !val);
			mdelay(500);
			gpio_set_value(led_data->led_gpio, val);
			mdelay(500);
		}
		break;
	}
	default:
		break;
	}
	return 0;
}

static ssize_t led_read(struct file *file, char __user *ubuf,
		size_t count, loff_t *offset)
{
	char kbuf[8] = { 0 };
	struct led_data *led_data = file->private_data;
	int val = gpio_get_value(led_data->led_gpio);

	if (*offset >= sizeof(min(sizeof(kbuf), count)))
		return 0;

	sprintf(kbuf, "%d", val);
	if (copy_to_user(ubuf, kbuf, min(sizeof(kbuf), count))) {
		pr_err("copy_to_user error!!!\n");
		return -EINVAL;
	}
	return min(sizeof(kbuf), count);
}

static ssize_t led_write(struct file *file, const char __user *ubuf,
		size_t count, loff_t *offset)
{
	int val;
	char kbuf[8] = { 0 };
	struct led_data *led_data = file->private_data;

	if (copy_from_user(kbuf, ubuf, min(sizeof(kbuf), count))) {
		pr_err("copy_from_user error!!!\n");
		return -EINVAL;
	}
	if (kstrtoint(kbuf, 10, &val) < 0) {
		pr_err("kstrtoint error!\n");
		return -EINVAL;
	}
	pr_err("====> %s: val = %d\n", led_data->led_name, val);
	gpio_set_value(led_data->led_gpio, val);
	return min(sizeof(kbuf), count);
}

static int led_open(struct inode *inode, struct file *file)
{
	struct led_data *led_data;

	led_data = container_of(inode->i_cdev, struct led_data, led_cdev);
	file->private_data = led_data;
	return 0;
}

static int led_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations led_fops = {
	.owner		        = THIS_MODULE,
	.open		        = led_open,
	.release	        = led_release,
	.read	            = led_read,
	.write              = led_write,
	.unlocked_ioctl     = led_ioctl,
	.compat_ioctl       = led_ioctl,
};

static int led_probe(struct platform_device *pdev)
{
	int ret;
	unsigned int led_gpio;
	enum of_gpio_flags flag;
	const char *led_name;
	dev_t led_dev;
	struct led_data *led_data = NULL;
	struct device_node *np = pdev->dev.of_node;

	of_property_read_string(np, "led_name", &led_name);

	led_gpio = of_get_named_gpio_flags(np, "led_gpio", 0, &flag);
	if (!gpio_is_valid(led_gpio)) {
		pr_err("%s: led-gpio %d is invalid\n", led_name, led_gpio);
		return -ENODEV;
	}
	if (gpio_request(led_gpio, led_name)) {
		pr_err("%s: gpio %d request failed!\n", led_name, led_gpio);
		return -ENODEV;
	}
	gpio_direction_output(led_gpio, (flag == OF_GPIO_ACTIVE_LOW) ? 0 : 1);

	ret = alloc_chrdev_region(&led_dev, 0, 1, led_name);
	if (ret < 0) {
		pr_err("register_chrdev %s failed!\n", led_name);
		goto out_alloc_chrdev;
	}

	led_data = kzalloc(sizeof(*led_data), GFP_KERNEL);
	if (!led_data) {
		pr_err("could not allocate led_data!\n");
		goto out_kzalloc;
	}

	cdev_init(&led_data->led_cdev, &led_fops);
	led_data->led_cdev.owner = THIS_MODULE;
	ret = cdev_add(&led_data->led_cdev, led_dev, 1);
	if (ret < 0) {
		pr_err("%s cdev_add error!\n", led_name);
		goto out_cdev_add;
	}

	device_create(led_class, NULL, led_dev, NULL, led_name);

	led_data->led_dev  = led_dev;
	led_data->led_gpio = led_gpio;
	led_data->led_name = led_name;
	platform_set_drvdata(pdev, led_data);

	pr_err("=========> %s probe success!\n", led_data->led_name);
	return 0;

out_cdev_add:
	kfree(led_data);
out_kzalloc:
	unregister_chrdev_region(led_dev, 1);
out_alloc_chrdev:
	gpio_free(led_gpio);
	return -EINVAL;
}

static int led_remove(struct platform_device *pdev)
{
	struct led_data *led_data = platform_get_drvdata(pdev);

	device_destroy(led_class, led_data->led_dev);
	cdev_del(&led_data->led_cdev);
	unregister_chrdev_region(led_data->led_dev, 1);
	gpio_free(led_data->led_gpio);
	kfree(led_data);

	pr_err("=========> %s remove success!\n", led_data->led_name);
	return 0;
}

static const struct of_device_id rk_led_of_match[] = {
	{ .compatible = "rockchip,led_red", },
	{ .compatible = "rockchip,led_green", },
	{ .compatible = "rockchip,led_yellow", },
	{ },
};

static struct platform_driver rk_led = {
	.probe = led_probe,
	.remove = led_remove,
	.driver = {
		.name = "rk_led",
		.owner = THIS_MODULE,
		.of_match_table = rk_led_of_match,
	}
};

static int __init rk_led_init(void)
{
	led_class = class_create(THIS_MODULE, LED_CLASS);
	if (IS_ERR(led_class)) {
		pr_err("class create %s failed!\n", LED_CLASS);
		return -EINVAL;
	}
	return platform_driver_register(&rk_led);
}

static void __exit rk_led_exit(void)
{
	platform_driver_unregister(&rk_led);
	class_destroy(led_class);
}

module_init(rk_led_init);
module_exit(rk_led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ifan Tsai");
MODULE_DESCRIPTION("led driver for rk3399");
