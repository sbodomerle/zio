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
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

/*
 * By default, run once per second, insmod can change it. If the
 * ms parameter is set to zero, the trigger fires once only, one second
 * after being created -- in any case it starts one second after creation
 */
static int ms = 2000; /* two seconds by default */
module_param(ms, int, S_IRUGO);

/* nsamples is 16 by default, should be an attribute */
static int nsamples = 16;
module_param(nsamples, int, S_IRUGO);

static struct zio_attribute zattr_dev_ext[] = {
	ZATTR_EXT_REG("ms", S_IRUGO | S_IWUGO, 0x00, 1000),
	ZATTR_EXT_REG("n_samples", S_IRUGO | S_IWUGO, 0x01, 1),
};
static int timer_set_config(struct kobject *kobj, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	zattr->value = usr_val;
	switch (zattr->priv.addr) {
	case 0:
		ms = usr_val;
		break;
	case 1:
		nsamples = usr_val;
		break;
	}
	return 0;
}
static const struct zio_sys_operations s_op = {
	.conf_set = timer_set_config,
};

struct ztt_instance {
	struct zio_ti ti;
	struct timer_list timer;
	unsigned long next_run;
	unsigned long period;
};
#define to_ztt_instance(ti) container_of(ti, struct ztt_instance, ti);

/* This runs when the timer expires */
static void ztt_fn(unsigned long arg)
{
	struct zio_ti *ti = (void *)arg;
	struct ztt_instance *ztt_instance;

	/* When a trigger fires, we must prepare our control and timestamp */
	getnstimeofday(&ti->tstamp);
	/* FIXME: where is the jiffi count placed? */

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

	if (chan->t_priv)
		return -EBUSY;
	chan->t_priv = block;
	return 0;
}

static int ztt_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	/* FIXME: config is not supported yet */

	pr_debug("%s:%d\n", __func__, __LINE__);
	return 0;
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
	ctrl->nsamples = nsamples; /* FIXME: it's moduleparam, use attrib */
	ti->current_ctrl = ctrl;

	/* Fill own fields */
	setup_timer(&ztt_instance->timer, ztt_fn,
		    (unsigned long)(&ztt_instance->ti));
	ztt_instance->next_run = jiffies + HZ;
	ztt_instance->period = msecs_to_jiffies(ms); /* module param */

	/* Start the timer (dangerous: ti is not filled) */
	mod_timer(&ztt_instance->timer, ztt_instance->next_run);

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

static const struct zio_trigger_operations ztt_trigger_ops = {
	.push_block = ztt_push_block,
	.pull_block = NULL,
	.data_done = zio_generic_data_done,
	.config = ztt_config,
	.create = ztt_create,
	.destroy = ztt_destroy,
};

static struct zio_trigger_type ztt_trigger = {
	.owner = THIS_MODULE,
	.t_op = &ztt_trigger_ops,
	.f_op = NULL, /* we use buffer fops */
};

/*
 * init and exit
 */
static int ztt_init(void)
{
	return zio_register_trig(&ztt_trigger, "timer");
}

static void ztt_exit(void)
{
	zio_unregister_trig(&ztt_trigger);
}

module_init(ztt_init);
module_exit(ztt_exit);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");
