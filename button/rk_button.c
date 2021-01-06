#include <linux/module.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/poll.h>
#include <linux/timer.h>

struct button_data {
    unsigned int gpio;
    unsigned int irq;
    const char *name;
    struct miscdevice misc;
    struct timer_list timer;    /* for removing shake */
    int ev_press;               /* wait's condition */
    wait_queue_head_t waitq;    /* wait queue head */
    int value;                  /* button value, to user */
};

//static DECLARE_WAIT_QUEUE_HEAD(button_waitq);

static void button_timeout_fun(unsigned long data)
{
    struct button_data *button_data = (struct button_data *)data;

    button_data->value = gpio_get_value(button_data->gpio); /* read key value */
    button_data->ev_press = 1;                              /* set wait's condition */
    wake_up_interruptible(&button_data->waitq);             /* wake up */
}

static irqreturn_t button_interrupt(int irq, void *arg)
{
    struct button_data *button_data = arg;

    /* set time and active timer */
    mod_timer(&button_data->timer, jiffies + HZ / 100);
    return IRQ_HANDLED;
}

static ssize_t button_read(struct file *file, char __user *ubuf,
                        size_t count, loff_t *offp)
{
    unsigned long err;
    char kbuf[8] = { 0 };
    struct button_data *button_data;

    button_data = container_of(file->private_data, struct button_data, misc);

    if (!button_data->ev_press) {
        if (file->f_flags & O_NONBLOCK)      /* no block */
            return -EAGAIN;
        else {                                 /* block */
            /* wait */
            DECLARE_WAITQUEUE(wait, current);
            add_wait_queue(&button_data->waitq, &wait);
            wait_event_interruptible(button_data->waitq, button_data->ev_press);
            remove_wait_queue(&button_data->waitq, &wait);
        }
    }

    button_data->ev_press = 0;    /* clear wait's condition */

    sprintf(kbuf, "%d", button_data->value);
    err = copy_to_user(ubuf, kbuf, min(sizeof(kbuf), count));

    return err ? -EFAULT : min(sizeof(kbuf), count);
}

static unsigned int button_poll(struct file *file, struct poll_table_struct *wait)
{
    unsigned int mask = 0;
    struct button_data *button_data;

    button_data = container_of(file->private_data, struct button_data, misc);
    poll_wait(file, &button_data->waitq, wait);
    if (button_data->ev_press)
        mask |= (POLLIN | POLLRDNORM);

    return mask;
}

static int button_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int button_close(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations button_misc_fops = {
    .owner   =   THIS_MODULE,
    .open    =   button_open,
    .release =   button_close,
    .read    =   button_read,
    .poll    =   button_poll,
};

static int button_probe(struct platform_device *pdev)
{
    struct device_node *np = pdev->dev.of_node;
    struct button_data *button_data = NULL;
    unsigned int button_gpio;
    unsigned int button_irq;
    enum of_gpio_flags flag;
    const char *button_name;

    of_property_read_string(np, "button_name", &button_name);

    button_gpio = of_get_named_gpio_flags(np, "button_gpio", 0, &flag);
    if (!gpio_is_valid(button_gpio)) {
        pr_err("%s: button-gpio %d is invalid\n", button_name, button_gpio);
        return -ENODEV;
    }
    if (gpio_request(button_gpio, button_name)) {
        pr_err("%s: gpio %d request failed!\n", button_name, button_gpio);
        return -ENODEV;
    }
    gpio_direction_input(button_gpio);
    button_irq = gpio_to_irq(button_gpio);

    button_data = kzalloc(sizeof(*button_data), GFP_KERNEL);
    if (!button_data) {
        pr_err("could not allocate button_data!\n");
        return -EFAULT;
    }
    init_waitqueue_head(&button_data->waitq);
    button_data->gpio = button_gpio;
    button_data->irq  = button_irq;
    button_data->name = button_name;
    button_data->ev_press   = 0;
    button_data->misc.minor = MISC_DYNAMIC_MINOR;
    button_data->misc.name  = button_name;
    button_data->misc.fops  = &button_misc_fops;

    if (request_irq(button_irq, button_interrupt, flag, button_name, button_data)) {
        pr_err("%s: request_irq %d failed\n", button_name, button_irq);
        goto out_request_irq;
    }

    if (misc_register(&button_data->misc)) {
        pr_err("%s: misc_register error!\n", button_name);
        goto out_misc_register;
    }

    init_timer(&button_data->timer);
    button_data->timer.function = button_timeout_fun;
    button_data->timer.data     = (unsigned long)button_data;

    platform_set_drvdata(pdev, button_data);
    pr_err("===> %s: probe success\n", button_name);
    return 0;

out_misc_register:
    free_irq(button_irq, button_data);
out_request_irq:
    kfree(button_data);
    gpio_free(button_gpio);
    return -EINVAL;
}

static int button_remove(struct platform_device *pdev)
{
    struct button_data *button_data = platform_get_drvdata(pdev);

    del_timer(&button_data->timer);
    misc_deregister(&button_data->misc);
    free_irq(button_data->irq, button_data);
    gpio_free(button_data->gpio);
    kfree(button_data);
    return 0;
}

static const struct of_device_id rk_button_of_match[] = {
    { .compatible = "rockchip,button_blue", },
    { },
};

static struct platform_driver rk_button = {
    .probe  = button_probe,
    .remove = button_remove,
    .driver = {
        .name   = "rk_button",
        .owner  = THIS_MODULE,
        .of_match_table = rk_button_of_match,
    }
};

static int __init rk_button_init(void)
{
    return platform_driver_register(&rk_button);
}

static void __exit rk_button_exit(void)
{
    platform_driver_unregister(&rk_button);
}

module_init(rk_button_init);
module_exit(rk_button_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ifan Tsai <i@caiyifan.cn>");
MODULE_DESCRIPTION("button driver for rk3399");
