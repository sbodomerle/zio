/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a transparent trigger, driven by user requests. It is best
 * for streaming devices or one-shot transfers. It implements the pull
 * method to request data to hardware if none is queued and the push
 * method to pass data to the device.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#define ZTU_DEFAULT_BLOCK_SIZE 16

static ZIO_ATTR_DEFINE_STD(ZIO_TRG, ztu_std_attr) = {
	ZIO_ATTR(trig, ZIO_ATTR_TRIG_POST_SAMP, ZIO_RW_PERM,
		 0 /* no addr needed */, ZTU_DEFAULT_BLOCK_SIZE),
};

int ztu_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	zattr->value = usr_val;
	return 0;
}

struct zio_sysfs_operations ztu_s_ops = {
	.conf_set = ztu_conf_set,
};

/* The buffer pushes a block if it has none queued and one is written */
static int ztu_push_block(struct zio_ti *ti, struct zio_channel *chan,
			  struct zio_block *block)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	struct zio_cset *cset = chan->cset;

	if (chan->active_block)
		return -EBUSY;
	chan->active_block = block;

	/* If all enabled channels are ready, tell hardware we are ready */
	chan_for_each(chan, cset)
		if (!chan->active_block)
			return 0;
	getnstimeofday(&ti->tstamp);
	zio_arm_trigger(ti);
	return 0;
}

/* The buffer pulls when a user reads and it has nothing yet */
static void ztu_pull_block(struct zio_ti *ti, struct zio_channel *chan)
{
	pr_debug("%s:%d\n", __func__, __LINE__);

	/* For self-timed devices, we have no pull, as it's already armed */
	if (zio_cset_early_arm(ti->cset))
		return;
	/* Otherwise, the user sets the input timing by reading */
	getnstimeofday(&ti->tstamp);
	zio_arm_trigger(ti);
}

static int ztu_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	pr_debug("%s:%d\n", __func__, __LINE__);

	return 0;
}

static struct zio_ti *ztu_create(struct zio_trigger_type *trig,
				 struct zio_cset *cset,
				 struct zio_control *ctrl, fmode_t flags)
{
	struct zio_ti *ti;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ti = kzalloc(sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return ERR_PTR(-ENOMEM);
	ti->flags = ZIO_DISABLED;
	ti->cset = cset;

	return ti;
}

static void ztu_destroy(struct zio_ti *ti)
{
	kfree(ti);
}

static const struct zio_trigger_operations ztu_trigger_ops = {
	.push_block = ztu_push_block,
	.pull_block = ztu_pull_block,
	.config = ztu_config,
	.create = ztu_create,
	.destroy = ztu_destroy,
};

static struct zio_trigger_type ztu_trigger = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = ztu_std_attr,
	},
	.s_op = &ztu_s_ops,
	.t_op = &ztu_trigger_ops,
};

/*
 * init and exit
 */
static int __init ztu_init(void)
{
	return zio_register_trig(&ztu_trigger, "user");
}

static void __exit ztu_exit(void)
{
	zio_unregister_trig(&ztu_trigger);
}


/* This is the default trigger, and is part of zio-core: no module init/exit */
int __init __attribute__((alias("ztu_init"))) zio_default_trigger_init(void);
void __exit __attribute__((alias("ztu_exit"))) zio_default_trigger_exit(void);
