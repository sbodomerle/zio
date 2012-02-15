/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a trigger based on an external IRQ. You can specify the IRQ
 * number or the GPIO number -- then the associated IRQ is used
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

static int zti_irq = -1;
static int zti_gpio = -1;
module_param_named(irq, zti_irq, int, 0444);
module_param_named(gpio, zti_gpio, int, 0444);

enum zti_attrs {
	ZTI_ATTR_NSAMPLES = 0,
	ZTI_ATTR_IRQ,
	ZTI_ATTR_GPIO,
};

static DEFINE_ZATTR_STD(TRIG, zti_std_attr) = {
	ZATTR_REG(trig, ZATTR_TRIG_NSAMPLES, S_IRUGO | S_IWUGO,
		  ZTI_ATTR_NSAMPLES, 16),
};

static struct zio_attribute zti_ext_attr[] = {
	ZATTR_EXT_REG("irq", S_IRUGO, ZTI_ATTR_IRQ, -1),
	ZATTR_EXT_REG("gpio", S_IRUGO, ZTI_ATTR_GPIO, -1),
};
static int zti_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	zattr->value = usr_val;

	return 0;
}

static struct zio_sysfs_operations zti_s_ops = {
	.conf_set = zti_conf_set,
};

static irqreturn_t zti_handler(int irq, void *dev_id)
{
	struct zio_ti *ti = dev_id;

	/* When a trigger fires, we must prepare our control and timestamp */
	getnstimeofday(&ti->tstamp);
	zio_fire_trigger(ti);
	return IRQ_HANDLED;
}

/*
 * The trigger operations are the core of a trigger type
 */
static int zti_push_block(struct zio_ti *ti, struct zio_channel *chan,
			  struct zio_block *block)
{
	/* software triggers must store pending stuff in chan->t_priv */
	pr_debug("%s:%d\n", __func__, __LINE__);

	if (chan->active_block)
		return -EBUSY;
	chan->active_block = block;
	return 0;
}

static int zti_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	/* FIXME: config is not supported yet */

	pr_debug("%s:%d\n", __func__, __LINE__);
	return 0;
}

static struct zio_ti *zti_create(struct zio_trigger_type *trig,
				 struct zio_cset *cset,
				 struct zio_control *ctrl, fmode_t flags)
{
	struct zio_ti *ti;

	int ret;
	pr_debug("%s:%d\n", __func__, __LINE__);

	ti = kzalloc(sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return ERR_PTR(-ENOMEM);

	ret = request_irq(zti_irq, zti_handler, IRQF_SHARED
			  | IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			  KBUILD_MODNAME, ti);
	if (ret < 0) {
		kfree(ti);
		return ERR_PTR(ret);
	}
	return ti;
}

static void zti_destroy(struct zio_ti *ti)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	free_irq(zti_irq, ti);
	kfree(ti);
}

static const struct zio_trigger_operations zti_trigger_ops = {
	.push_block = zti_push_block,
	.pull_block = NULL,
	.data_done = zio_generic_data_done,
	.config = zti_config,
	.create = zti_create,
	.destroy = zti_destroy,
};

static struct zio_trigger_type zti_trigger = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = zti_std_attr,
		.ext_zattr = zti_ext_attr,
		.n_ext_attr = ARRAY_SIZE(zti_ext_attr),
	},
	.s_op = &zti_s_ops,
	.t_op = &zti_trigger_ops,
};

/*
 * A validation function, called at insmod and at parameter change
 */
static int zti_validate(int irq, int gpio)
{
	int ret = 0;

	if (irq != -1 && gpio != -1) {
		pr_err("%s: only set irq or gpio, not both\n", KBUILD_MODNAME);
		return -EINVAL;
	}
	if (irq == -1 && gpio == -1) {
		pr_err("%s: please set irq or gpio\n", KBUILD_MODNAME);
		return -EINVAL;
	}
	if (gpio != -1) {
		irq = gpio_to_irq(gpio);
		if (irq >= 0)
			ret  = gpio_request(gpio, KBUILD_MODNAME);
		else
			ret = irq;
	}
	if (ret < 0) {
		pr_err("%s: invalid irq/gpio (%i/%i)\n", KBUILD_MODNAME,
		       gpio, irq);
		return ret;
	}
	zti_irq = irq; /* used at trigger_create time */
	return 0;
}

/*
 * init and exit
 */
static int __init zti_init(void)
{
	int ret = zti_validate(zti_irq, zti_gpio);
	if (ret)
		return ret;
	return zio_register_trig(&zti_trigger, "irq");
}

static void __exit zti_exit(void)
{
	zio_unregister_trig(&zti_trigger);
	if (zti_gpio)
		gpio_free(zti_gpio);
}

module_init(zti_init);
module_exit(zti_exit);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");
