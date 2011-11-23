/* Federico Vaga and Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

static DEFINE_MUTEX(zmutex);
struct zio_status zio_global_status;
static struct zio_status *zstat = &zio_global_status; /* Always use ptr */

static ssize_t zio_show_version(struct class *class,
			struct class_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d.%d\n", ZIO_MAJOR_VERSION, ZIO_MINOR_VERSION);
}

static struct class_attribute zclass_attrs[] = {
	__ATTR(version, S_IRUGO, zio_show_version, NULL),
	__ATTR_NULL,
};

static char *zio_devnode(struct device *dev, mode_t *mode)
{
	return kasprintf(GFP_KERNEL, "zio/%s", dev_name(dev));
}

/*
 * zio_class: don't use class_create to create class because it doesn't permit
 * to insert a set of class attributes. This structure is the exact
 * reproduction of what class_create does but with some additional settings.
 */
static struct class zio_class = {
	.name		= "zio",
	.owner		= THIS_MODULE,
	.class_attrs	= zclass_attrs,
	.devnode	= zio_devnode,
};

/* Retrieve a channel from one of its minors */
static struct zio_channel *__zio_minor_to_chan(dev_t mm)
{
	struct zio_cset *zcset;
	dev_t cset_base, chan_minor;
	int found = 0;

	/* Extract cset minor base */
	chan_minor = mm & (ZIO_NMAX_CSET_MINORS-1);
	cset_base = mm & (~(ZIO_NMAX_CSET_MINORS-1));

	/* Look for this minor base*/
	list_for_each_entry(zcset, &zstat->list_cset, list_cset) {
		if (cset_base == zcset->basedev) {
			found = 1;
			break;
		}
	}
	if (!found)
		return NULL;
	return &zcset->chan[chan_minor/2];
}

static inline int zio_device_get(dev_t devt)
{
	struct zio_channel *chan;

	/*
	 * FIXME there is a little concurrency; to resolve this, get the owner
	 * from device list by searching by minor
	 */
	chan = __zio_minor_to_chan(devt);
	if (!chan) {
		pr_err("ZIO: can't retrieve channel for minor %i\n",
		       MINOR(devt));
		return -EBUSY;
	}
	return try_module_get(chan->cset->zdev->owner);
}
static inline void zio_device_put(dev_t devt)
{
	struct zio_channel *chan;

	/*
	 * FIXME there is a little concurrency; to resolve this, get the owner
	 * from device list by searching by minor
	 */
	chan = __zio_minor_to_chan(devt);
	/* it is impossbile chan = NULL because __zio_device_get() found it */
	module_put(chan->cset->zdev->owner);
}

static int zio_f_open(struct inode *ino, struct file *f)
{
	struct zio_f_priv *priv = NULL;
	struct zio_channel *chan;
	struct zio_buffer_type *zbuf;
	const struct file_operations *old_fops, *new_fops;
	int ret = -EINVAL, minor;

	pr_debug("%s:%i\n", __func__, __LINE__);
	if (f->f_flags & FMODE_WRITE)
		goto out;

	if (!zio_device_get(ino->i_rdev))
		return -ENODEV;

	minor = iminor(ino);
	chan = __zio_minor_to_chan(ino->i_rdev);
	if (!chan) {
		pr_err("ZIO: can't retrieve channel for minor %i\n", minor);
		return -EBUSY;
	}
	zbuf = chan->cset->zbuf;
	f->private_data = NULL;
	priv = kzalloc(sizeof(struct zio_f_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* if there is no instance, then create a new one */
	if (!chan->bi)
		chan->bi = zbuf->b_op->create(zbuf, chan, FMODE_READ);
	priv->chan = chan;

	/* even number is control, odd number is data */
	if (minor & 0x1)
		priv->type = ZIO_CDEV_DATA;
	else
		priv->type = ZIO_CDEV_CTRL;
	f->private_data = priv;

	/* replace zio fops with buffer fops (FIXME: make it a lib function */
	mutex_lock(&zmutex);
	old_fops = f->f_op;
	new_fops = fops_get(zbuf->f_op);
	ret = 0;
	if (new_fops->open)
		ret = new_fops->open(ino, f);
	if (ret) {
		fops_put(zbuf->f_op);
		mutex_unlock(&zmutex);
		goto out;
	}
	fops_put(old_fops);
	f->f_op = new_fops;
	mutex_unlock(&zmutex);
	return 0;

out:
	kfree(priv);
	return ret;
}

static const struct file_operations zfops = {
	.owner = THIS_MODULE,
	.open = zio_f_open,
};

int __zio_minorbase_get(struct zio_cset *zcset)
{
	int i;

	i = find_first_zero_bit(zstat->cset_minors_mask, ZIO_CSET_MAXNUM);
	if (i >= ZIO_CSET_MAXNUM)
		return 1;
	set_bit(i, zstat->cset_minors_mask);
	/* set the base minor for a cset*/
	zcset->basedev = zstat->basedev + (i * ZIO_NMAX_CSET_MINORS);
	pr_debug("%s:%i BASEMINOR 0x%x\n", __func__, __LINE__, zcset->basedev);
	return 0;
}
void __zio_minorbase_put(struct zio_cset *zcset)
{
	int i;

	i = (zcset->basedev - zstat->basedev) / ZIO_NMAX_CSET_MINORS;
	clear_bit(i, zstat->cset_minors_mask);
}

/*
 * create control and data char devices for a channel. The even minor
 * is for control, the odd one for data.
 */
int zio_create_chan_devices(struct zio_channel *chan)
{
	int err;
	dev_t devt_c, devt_d;


	devt_c = chan->cset->basedev + chan->index * 2;
	pr_debug("%s:%d dev_t=0x%x\n", __func__, __LINE__, devt_c);
	chan->ctrl_dev = device_create(&zio_class, NULL, devt_c, NULL,
			"%s-%i-%i-ctrl",
			chan->cset->zdev->head.name,
			chan->cset->index,
			chan->index);
	if (IS_ERR(&chan->ctrl_dev)) {
		err = PTR_ERR(&chan->ctrl_dev);
		goto out;
	}

	devt_d = devt_c + 1;
	pr_debug("%s:%d dev_t=0x%x\n", __func__, __LINE__, devt_d);
	chan->data_dev = device_create(&zio_class, NULL, devt_d, NULL,
			"%s-%i-%i-data",
			chan->cset->zdev->head.name,
			chan->cset->index,
			chan->index);
	if (IS_ERR(&chan->data_dev)) {
		err = PTR_ERR(&chan->data_dev);
		goto out_data;
	}

	return 0;

out_data:
	device_destroy(&zio_class, chan->ctrl_dev->devt);
out:
	return err;
}

void zio_destroy_chan_devices(struct zio_channel *chan)
{
	pr_debug("%s\n", __func__);
	device_destroy(&zio_class, chan->data_dev->devt);
	device_destroy(&zio_class, chan->ctrl_dev->devt);
}

int __zio_register_cdev()
{
	int err;

	err = class_register(&zio_class);
	if (err) {
		pr_err("%s: unable to register class\n", __func__);
		goto out;
	}
	/* alloc to zio the maximum number of minors usable in ZIO */
	err = alloc_chrdev_region(&zstat->basedev, 0,
			ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS, "zio");
	if (err) {
		pr_err("%s: unable to allocate region for %i minors\n",
			__func__, ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
		goto out;
	}
	/* all ZIO's devices, buffers and triggers has zfops as f_op */
	cdev_init(&zstat->chrdev, &zfops);
	zstat->chrdev.owner = THIS_MODULE;
	err = cdev_add(&zstat->chrdev, zstat->basedev,
			ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
	if (err)
		goto out_cdev;
	INIT_LIST_HEAD(&zstat->list_cset);
	return 0;
out_cdev:
	unregister_chrdev_region(zstat->basedev,
			ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
out:
	class_unregister(&zio_class);
	return err;
}
void __zio_unregister_cdev()
{
	cdev_del(&zstat->chrdev);
	unregister_chrdev_region(zstat->basedev,
				ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
	class_unregister(&zio_class);
}


/*
 * Helper functions to check whether read and write would block. The
 * return value is a poll(2) mask, so the poll method just calls them.
 */

/* Read is quite straightforward, as blocks reack us already filled */
static int __zio_read_allowed(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	const int can_read =  POLLIN | POLLRDNORM;

	if (!chan->user_block)
		chan->user_block = bi->b_op->retr_block(bi);
	if (!chan->user_block)
		return 0;

	/* We have a block. So there is data and possibly control too */
	if (likely(priv->type == ZIO_CDEV_DATA))
		return can_read;

	if (!zio_is_cdone(chan->user_block))
		return POLLIN | POLLRDNORM;

	/* There's a block, but we want to re-read control. Get a new block */
	bi->b_op->free_block(bi, chan->user_block);
	chan->user_block = bi->b_op->retr_block(bi);
	if (!chan->user_block)
		return 0;
	return POLLIN | POLLRDNORM;
}

/* Write is more tricky: we need control, so we may ask it to the trigger */
static struct zio_block *__zio_write_allocblock(struct zio_bi *bi,
						 struct zio_control *ctrl)
{
	struct zio_block *block;
	size_t datalen;

	if (!ctrl) {
		ctrl = zio_alloc_control(GFP_KERNEL);
		if (!ctrl)
			return NULL;
		memcpy(ctrl, bi->cset->ti->current_ctrl, ZIO_CONTROL_SIZE);
	}
	datalen = ctrl->ssize * ctrl->nsamples;
	block = bi->b_op->alloc_block(bi, ctrl, datalen, GFP_KERNEL);
	return block;
}

static int __zio_write_allowed(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;
	const int can_write = POLLOUT | POLLWRNORM;

	if (priv->type == ZIO_CDEV_CTRL) {
		/* Control is always writeable */
		return can_write;
	}

	/* We want to write data. If we have no control, retrieve one */
	if (!chan->user_block)
		chan->user_block = __zio_write_allocblock(bi, NULL);
	block = chan->user_block;
	if (!block)
		return 0;

	/* If the block is not full, user can write data */
	if (block->uoff < block->datalen)
		return can_write;

	/* Block is full: try to push out to the buffer */
	if (bi->b_op->store_block(bi, block) < 0)
		return 0;

	/* We sent it: get a new one for this new data */
	chan->user_block = __zio_write_allocblock(bi, NULL);
	return chan->user_block ? can_write : 0;
}

/*
 * The following "generic" read and write (and poll and so on) should
 * work for most buffer types, and are exported for use in their
 * buffer operations.
 */
ssize_t zio_generic_read(struct file *f, char __user *ubuf,
			 size_t count, loff_t *offp)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;

	pr_debug("%s:%d type %s\n", __func__, __LINE__,
		priv->type == ZIO_CDEV_CTRL ? "ctrl" : "data");

	if (priv->type == ZIO_CDEV_CTRL && count < ZIO_CONTROL_SIZE)
		return -EINVAL;

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT) {
		/* FIXME: read_control for output channels is missing */
		return -EINVAL;
	}

	if (!__zio_read_allowed(priv)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(bi->q, __zio_read_allowed(priv));
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
	block = chan->user_block;

	/* So, it's readable */
	if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
		zio_set_cdone(block);
		if (copy_to_user(ubuf, zio_get_ctrl(block), ZIO_CONTROL_SIZE))
			return -EFAULT;
		*offp += ZIO_CONTROL_SIZE;
		return ZIO_CONTROL_SIZE;
	}

	/* Data file, and we have data */
	if (count > block->datalen - block->uoff)
		count = block->datalen - block->uoff;
	if (copy_to_user(ubuf, block->data + block->uoff, count))
		return -EFAULT;
	*offp += count;
	block->uoff += count;
	if (block->uoff == block->datalen) {
		chan->user_block = NULL;
		bi->b_op->free_block(bi, block);
	}
	return count;
}
EXPORT_SYMBOL(zio_generic_read);

ssize_t zio_generic_write(struct file *f, const char __user *ubuf,
			  size_t count, loff_t *offp)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;
	struct zio_control *ctrl;

	pr_debug("%s:%d type %s\n", __func__, __LINE__,
		priv->type == ZIO_CDEV_CTRL ? "ctrl" : "data");

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT) {
		/* FIXME: write_control for input channels is missing */
		return -EINVAL;
	}

	if (!__zio_write_allowed(priv)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(bi->q, __zio_write_allowed(priv));
		if (signal_pending(current))
			return -ERESTARTSYS;
	}

	if (likely(priv->type == ZIO_CDEV_DATA)) {
		/* Data is writeable, so we have space in this block */
		block = chan->user_block;
		if (count > block->datalen - block->uoff)
			count =  block->datalen - block->uoff;
		if (copy_from_user(block->data + block->uoff, ubuf, count))
			return -EFAULT;
		block->uoff += count;
		if (block->uoff == block->datalen)
			if (bi->b_op->store_block(bi, block) == 0)
				chan->user_block = NULL;
		return count;
	}

	/* Control: drop the current block and create a new one */
	if (priv->type == ZIO_CDEV_CTRL && count < ZIO_CONTROL_SIZE)
		return -EINVAL;
	count = ZIO_CONTROL_SIZE;

	if (chan->user_block)
		bi->b_op->free_block(bi, chan->user_block);
	chan->user_block = NULL;
	ctrl = zio_alloc_control(GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	if (copy_from_user(ctrl, ubuf, count))
		return -EFAULT;
	memcpy(bi->cset->ti->current_ctrl, ctrl, count);
	*offp += count;
	return count;
}
EXPORT_SYMBOL(zio_generic_write);

unsigned int zio_generic_poll(struct file *f, struct poll_table_struct *w)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;

	poll_wait(f, &bi->q, w);
	return __zio_read_allowed(priv) | __zio_write_allowed(priv);
}
EXPORT_SYMBOL(zio_generic_poll);

int zio_generic_release(struct inode *inode, struct file *f)
{
	struct zio_f_priv *priv = f->private_data;

	/* priv is allocated by zio_f_open, must be freed */
	kfree(priv);
	zio_device_put(inode->i_rdev);
	return 0;
}
EXPORT_SYMBOL(zio_generic_release);

