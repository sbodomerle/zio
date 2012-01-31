/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a timer-based trigger for the ZIO framework. It is not
 * specific to a low-level device (every device can use it) and clearly
 * multi-instance.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

struct ztt_instance {
	struct zio_ti ti;
	struct timer_list timer;
	unsigned long next_run;
	unsigned long period;
};
#define to_ztt_instance(ti) container_of(ti, struct ztt_instance, ti);

enum ztt_attrs { /* names for the "addr" value of sw parameters */
	ZTT_ATTR_NSAMPLES = 0,
	ZTT_ATTR_PERIOD,
};

static DEFINE_ZATTR_STD(TRIG, ztt_std_attr) = {
	ZATTR_REG(trig, ZATTR_TRIG_NSAMPLES, S_IRUGO | S_IWUGO,
		  ZTT_ATTR_NSAMPLES, 16),
};

static struct zio_attribute ztt_ext_attr[] = {
	ZATTR_EXT_REG("ms-period", S_IRUGO | S_IWUGO,
		      ZTT_ATTR_PERIOD, 2000),
};
static int ztt_conf_set(struct kobject *kobj, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	struct zio_ti *ti = to_zio_ti(kobj);
	struct ztt_instance *ztt;

	pr_debug("%s:%d\n", __func__, __LINE__);
	zattr->value = usr_val;
	switch (zattr->priv.addr) {
	case ZTT_ATTR_NSAMPLES:
		ti->current_ctrl->nsamples = usr_val;
		break;
	case ZTT_ATTR_PERIOD:
		ztt = to_ztt_instance(ti);
		ztt->period = msecs_to_jiffies(usr_val);
		break;
	default:
		pr_err("%s: unknown \"addr\" 0x%lx for configuration\n",
				__func__, zattr->priv.addr);
		return -EINVAL;
	}
	return 0;
}

static struct zio_sysfs_operations ztt_s_ops = {
	.conf_set = ztt_conf_set,
};

/* This runs when the timer expires */
static void ztt_fn(unsigned long arg)
{
	struct zio_ti *ti = (void *)arg;
	struct ztt_instance *ztt_instance;

	/* When a trigger fires, we must prepare our control and timestamp */
	getnstimeofday(&ti->tstamp);

	ztt_instance = to_ztt_instance(ti);
	zio_fire_trigger(ti);

	if (!ztt_instance->period)
		return; /* one-shot */

	ztt_instance = to_ztt_instance(ti)
	ztt_instance->next_run += ztt_instance->period;
	mod_timer(&ztt_instance->timer, ztt_instance->next_run);
}

/*
 * The trigger operations are the core of a trigger type
 */
static int ztt_push_block(struct zio_ti *ti, struct zio_channel *chan,
			  struct zio_block *block)
{
	/* software triggers must store pending stuff in chan->t_priv */
	pr_debug("%s:%d\n", __func__, __LINE__);

	if (chan->active_block)
		return -EBUSY;
	chan->active_block = block;
	return 0;
}

static int ztt_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	/* FIXME: config is not supported yet */

	pr_debug("%s:%d\n", __func__, __LINE__);
	return 0;
}
static void ztt_start_timer(struct ztt_instance *ztt_instance, uint32_t ms)
{
	ztt_instance->next_run = jiffies + HZ;
	ztt_instance->period = msecs_to_jiffies(ms);
	mod_timer(&ztt_instance->timer, ztt_instance->next_run);
}
static struct zio_ti *ztt_create(struct zio_trigger_type *trig,
				 struct zio_cset *cset,
				 struct zio_control *ctrl, fmode_t flags)
{
	struct ztt_instance *ztt_instance;
	struct zio_ti *ti;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ztt_instance = kzalloc(sizeof(struct ztt_instance), GFP_KERNEL);
	if (!ztt_instance)
		return ERR_PTR(-ENOMEM);
	ti = &ztt_instance->ti;

	/* The current control is already filled: just set nsamples */
	ctrl->nsamples = ztt_std_attr[ZATTR_TRIG_NSAMPLES].value;
	ti->current_ctrl = ctrl;

	/* Fill own fields */
	setup_timer(&ztt_instance->timer, ztt_fn,
		    (unsigned long)(&ztt_instance->ti));
	ztt_start_timer(ztt_instance, ztt_ext_attr[0].value);

	return ti;
}

static void ztt_destroy(struct zio_ti *ti)
{
	struct ztt_instance *ztt_instance;

	pr_debug("%s:%d\n", __func__, __LINE__);
	ztt_instance = to_ztt_instance(ti);
	del_timer_sync(&ztt_instance->timer);
	kfree(ti);
}

static void ztt_change_status(struct zio_ti *ti, unsigned int status)
{
	struct ztt_instance *ztt_instance;

	pr_debug("%s:%d status=%d\n", __func__, __LINE__, status);
	ztt_instance = to_ztt_instance(ti);

	if (!status) {	/* enable */
		ztt_start_timer(ztt_instance, ztt_instance->period);
	} else {	/* disable */
		/* FIXME kernel/timer.c don't use this is lock*/
		del_timer_sync(&ztt_instance->timer);
	}
}
static const struct zio_trigger_operations ztt_trigger_ops = {
	.push_block = ztt_push_block,
	.pull_block = NULL,
	.data_done = zio_generic_data_done,
	.config = ztt_config,
	.create = ztt_create,
	.destroy = ztt_destroy,
	.change_status = ztt_change_status,
};

static struct zio_trigger_type ztt_trigger = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = ztt_std_attr,
		.ext_zattr = ztt_ext_attr,
		.n_ext_attr = ARRAY_SIZE(ztt_ext_attr),
	},
	.s_op = &ztt_s_ops,
	.t_op = &ztt_trigger_ops,
	.f_op = NULL, /* we use buffer fops */
};

/*
 * init and exit
 */
static int __init ztt_init(void)
{
	return zio_register_trig(&ztt_trigger, "timer");
}

static void __exit ztt_exit(void)
{
	zio_unregister_trig(&ztt_trigger);
}

module_init(ztt_init);
module_exit(ztt_exit);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");
