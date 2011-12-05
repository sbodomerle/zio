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

static DEFINE_ZATTR_STD(TRIG, ztu_std_attr) = {
	ZATTR_REG(trig, ZATTR_TRIG_NSAMPLES, S_IRUGO | S_IWUGO,
		  0 /* no addr needed */, ZTU_DEFAULT_BLOCK_SIZE),
};

int ztu_conf_set(struct kobject *kobj, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	struct zio_ti *ti = to_zio_ti(kobj);

	pr_debug("%s:%d\n", __func__, __LINE__);
	zattr->value = usr_val;
	ti->current_ctrl->nsamples = usr_val; /* We have this one only */
	return 0;
}

struct zio_sys_operations ztu_s_ops = {
	.conf_set = ztu_conf_set,
};

/* The buffer pushes a block if it has none queued and one is written */
static int ztu_push_block(struct zio_ti *ti, struct zio_channel *chan,
			  struct zio_block *block)
{
	pr_debug("%s:%d\n", __func__, __LINE__);

	if (chan->active_block)
		return -EBUSY;
	chan->active_block = block;
	getnstimeofday(&ti->tstamp);
	zio_fire_trigger(ti);
	return 0;
}

/* The buffer pulls when a user reads and it has nothing yet */
static void ztu_pull_block(struct zio_ti *ti, struct zio_channel *chan)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	getnstimeofday(&ti->tstamp);
	zio_fire_trigger(ti);
}

static int ztu_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	pr_debug("%s:%d\n", __func__, __LINE__);

	ti->current_ctrl->nsamples = ctrl->nsamples;
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
	/* The current control is already filled: just set nsamples */
	ctrl->nsamples = ztu_std_attr[ZATTR_TRIG_NSAMPLES].value;
	ti->current_ctrl = ctrl;

	return ti;
}

static void ztu_destroy(struct zio_ti *ti)
{
	kfree(ti);
}

static const struct zio_trigger_operations ztu_trigger_ops = {
	.push_block = ztu_push_block,
	.pull_block = ztu_pull_block,
	.data_done = zio_generic_data_done,
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

module_init(ztu_init);
module_exit(ztu_exit);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");