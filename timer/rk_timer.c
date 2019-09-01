#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/ioctl.h>

#ifdef CONFIG_OF
#include <linux/of_irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#endif

#define RK_DEV_MAX   1

#define RK_TIMER_START           (0x01)
#define RK_TIMER_STOP            (0x02)
#define RK_TIMER_SET_INTERVAL    (0x03)

struct rk_timer_reg {
	unsigned int load_cnt0;
	unsigned int load_cnt1;
	unsigned int curr_val0;
	unsigned int curr_val1;
	unsigned int load_cnt2;
	unsigned int load_cnt3;
	unsigned int stat;
	unsigned int ctlreg;
};

struct rk_timer {
	struct miscdevice miscdev;
	int irq;
	unsigned long interval;
	bool done;
	unsigned long dev_opened;
	/* timer->done */
	spinlock_t lock;
	struct rk_timer_reg *reg;
	struct clk *timer_clk;
	struct clk *pclk;
};

static void __iomem *timer_base;

struct rk_timer *g_ptimer;

DECLARE_WAIT_QUEUE_HEAD(rk_timer_wq);

static irqreturn_t rk_timer_interrupt(int irq, void *dev_id)
{
	unsigned long flags;

	spin_lock_irqsave(&g_ptimer->lock, flags);
	/* clear INTSTATUS */
	writel(1, &g_ptimer->reg->stat);
	g_ptimer->done = true;
	wake_up(&rk_timer_wq);

	spin_unlock_irqrestore(&g_ptimer->lock, flags);

	return IRQ_HANDLED;
}

static void timer_set_interval(struct rk_timer *timer, u64 us)
{
	struct rk_timer_reg *timer_reg = timer->reg;
	u64 count;

	if (us >= 1000 && us <= 10000000) {      /* allow: 1ms ~ 10s */
		/* clock: 24MHz */
		/* period : 1.0 / 24000000 * 1000000 = 1 / 24 us */
		count = 24 * us;

		/* load count2,3 val */
		writel(0, &timer_reg->load_cnt2);
		writel(0, &timer_reg->load_cnt3);
		/* load count1,2 val */
		writel(count & 0xFFFFFFFF, &timer_reg->load_cnt0);
		writel(count >> 32, &timer_reg->load_cnt1);
	}
}

static void timer_enable(struct rk_timer *timer)
{
	struct rk_timer_reg *timer_reg = timer->reg;
	unsigned int val;

	val = readl(&timer_reg->ctlreg);
	/* timer interrupt not mask */
	val |= (0x01 << 2);
	/* enable timer */
	val |= (0x01 << 0);
	writel(val, &timer_reg->ctlreg);
}

static void timer_disable(struct rk_timer *timer)
{
	struct rk_timer_reg *timer_reg = timer->reg;
	unsigned int val = readl(&timer_reg->ctlreg);

	val = readl(&timer_reg->ctlreg);
	/* timer interrupt mask */
	val &= ~(0x01 << 2);
	/* disable timer */
	val &= ~(0x01 << 0);
	writel(val, &timer_reg->ctlreg);
}

static void timer_init(struct rk_timer *timer)
{
	struct rk_timer_reg *timer_reg = timer->reg;
	unsigned int val = readl(&timer_reg->ctlreg);

	/* disable timer */
	val &= ~(0x01 << 0);
	/* set timer mode: free-running */
	val &= ~(0x01 << 1);
	/* timer interrupt mask */
	val &= ~(0x01 << 2);

	writel(val, &timer_reg->ctlreg);

	timer_set_interval(timer, 10000);      /* defautl: 10ms */
	timer->interval = 10000;
}

static int rk_timer_open(struct inode *inode, struct file *file)
{
	struct rk_timer *timer;

	timer = container_of(file->private_data, struct rk_timer, miscdev);

	if (test_and_set_bit(0, &timer->dev_opened))
		return -EBUSY;

	return 0;
}

static int rk_timer_release(struct inode *inode, struct file *file)
{
	struct rk_timer *timer;

	timer = container_of(file->private_data, struct rk_timer, miscdev);

	clear_bit(0, &timer->dev_opened);
	timer_disable(timer);

	return 0;
}

static unsigned int rk_timer_poll(struct file *file, poll_table *wait)
{
	struct rk_timer *timer;
	unsigned int ret;
	unsigned long flags;

	timer = container_of(file->private_data, struct rk_timer, miscdev);

	poll_wait(file, &rk_timer_wq, wait);

	spin_lock_irqsave(&timer->lock, flags);
	if (timer->done) {
		ret = POLLIN | POLLRDNORM;
		timer->done = false;
	} else {
		ret = 0;
	}
	spin_unlock_irqrestore(&timer->lock, flags);

	return ret;
}

static long
rk_timer_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct rk_timer *timer;

	timer = container_of(file->private_data, struct rk_timer, miscdev);

	switch (cmd) {
	case RK_TIMER_START:
		timer_enable(timer);
		break;
	case RK_TIMER_STOP:
		timer_disable(timer);
		break;
	case RK_TIMER_SET_INTERVAL:
	{
		unsigned int val;

		if (copy_from_user(&val, (void __user *)arg, sizeof(int)))
			return -EFAULT;

		timer->interval = val;
		timer_set_interval(timer, timer->interval);
		break;
	}
	default:
		return -ENOTTY;
	}

	return 0;
}

static const struct file_operations rk_timer_fops = {
	.owner   = THIS_MODULE,
	.llseek  = no_llseek,
	.open    = rk_timer_open,
	.release = rk_timer_release,
	.poll    = rk_timer_poll,
	.unlocked_ioctl = rk_timer_ioctl,
	.compat_ioctl   = rk_timer_ioctl,
};

static int rk_timer_remove(struct platform_device *pdev)
{
	struct rk_timer *timer = platform_get_drvdata(pdev);

	free_irq(timer->irq, NULL);
	misc_deregister(&timer->miscdev);
	clk_disable_unprepare(timer->timer_clk);
	clk_disable_unprepare(timer->pclk);
	iounmap(timer->reg);
	kfree(timer);

	return 0;
}

static int rk_timer_probe(struct platform_device *pdev)
{
	struct rk_timer *timer;
	int err, irq;
	struct clk *timer_clk;
	struct clk *pclk;
	struct rk_timer_reg *timer_reg;

	struct device_node *np = pdev->dev.of_node;

	timer_base = of_iomap(np, 0);
	if (!timer_base) {
		pr_err("Failed to get base address for rk_timer2\n");
		return -EINVAL;
	}
	timer_reg = (struct rk_timer_reg *)timer_base;

	pclk = of_clk_get_by_name(np, "pclk");
	if (IS_ERR(pclk)) {
		pr_err("Failed to get pclk for rk_timer2\n");
		return -EINVAL;
	}

	if (clk_prepare_enable(pclk)) {
		pr_err("Failed to enable pclk for rk_timer2\n");
		return -EINVAL;
	}

	timer_clk = of_clk_get_by_name(np, "timer");
	if (IS_ERR(timer_clk)) {
		pr_err("Failed to get timer clock for rk_timer2\n");
		return -EINVAL;
	}

	if (clk_prepare_enable(timer_clk)) {
		pr_err("Failed to enable timer clock\n");
		return -EINVAL;
	}

	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		pr_err("Failed to map interrupts for rk_timer2\n");
		return -EINVAL;
	}

	timer = kzalloc(sizeof(*timer), GFP_KERNEL);
	if (!timer) {
		err = -ENOMEM;
		goto err_return;
	}

	spin_lock_init(&timer->lock);

	timer->miscdev.minor = MISC_DYNAMIC_MINOR;
	timer->miscdev.name = "rk_timer2";
	timer->miscdev.fops = &rk_timer_fops;
	err = misc_register(&timer->miscdev);
	if (err) {
		pr_err("Register %s failed\n", timer->miscdev.name);
		goto err_free_priv;
	}
	pr_err("misc_register success\n");

	timer->reg = timer_reg;
	timer->pclk = pclk;
	timer->timer_clk = timer_clk;

	timer_init(timer);

	g_ptimer = timer;

	timer->irq = irq;
	err = request_irq(
					irq, rk_timer_interrupt,
					IRQF_TIMER, "rk_timer2", NULL);

	if (err < 0) {
		pr_err("fail to request rk_timer2 irq\n");
		goto err_misc_register;
	}

	platform_set_drvdata(pdev, timer);

	return 0;

err_misc_register:
	misc_deregister(&timer->miscdev);
err_free_priv:
	kfree(timer);
err_return:
	clk_disable_unprepare(timer->timer_clk);
	clk_disable_unprepare(timer->pclk);
	iounmap(timer->reg);
	return err;
}

#if defined CONFIG_PM
int rk_timer_suspend(struct device *dev)
{
	struct rk_timer *timer = dev_get_drvdata(dev);

	timer_disable(timer);
	return 0;
}

int rk_timer_resume(struct device *dev)
{
	struct rk_timer *timer = dev_get_drvdata(dev);

	timer_set_interval(timer, timer->interval);
	if (timer->dev_opened)
		timer_enable(timer);
	return 0;
}

static const struct dev_pm_ops rk_timer_pm_ops = {
	.suspend = rk_timer_suspend,
	.resume  = rk_timer_resume,
};

#endif

#ifdef CONFIG_OF
static const struct of_device_id rk_timer_of_ids[] = {
	{ .compatible = "rockchip,rk-timer2", },
	{}
};
#endif

static struct platform_driver rk_timer_driver = {
	.probe = rk_timer_probe,
	.remove = rk_timer_remove,
	.driver = {
		.name = "rk_timer2",
		.owner = THIS_MODULE,
#if defined CONFIG_PM
		.pm = &rk_timer_pm_ops,
#endif
#ifdef CONFIG_OF
		.of_match_table = rk_timer_of_ids,
#endif
	},
};

static int __init rk_timer_init(void)
{
	return platform_driver_register(&rk_timer_driver);
}

static void __exit rk_timer_exit(void)
{
	platform_driver_unregister(&rk_timer_driver);
}

module_init(rk_timer_init);
module_exit(rk_timer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ifan Tsai <i@caiyifan.cn>");
MODULE_DESCRIPTION("rk3399 timer2");
