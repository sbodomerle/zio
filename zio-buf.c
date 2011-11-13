/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/* FIXME: some of these headers are not needed */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

/* Helper functions to check whether read and write would block */
static int __zbg_read_mask(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;

	if (!chan->current_block)
		chan->current_block = bi->b_op->retr_block(bi);
	if (!chan->current_block)
		return 0; /* not readable */

	/* FIXME: all of this needs proper locking (mutex, probably) */
	block = chan->current_block;

	/* We have a block. So there is data and possibly control too */
	if (priv->type == ZIO_CDEV_DATA)
		return POLLIN | POLLRDNORM;

	if (!zio_is_cdone(block))
		return POLLIN | POLLRDNORM;

	/* There is a block, but control is done. Get a new block */
	chan->current_block = bi->b_op->retr_block(bi);
	bi->b_op->free_block(bi, block);
	if (!chan->current_block)
		return 0;
	return POLLIN | POLLRDNORM;
}

static int __zbg_write_mask(struct zio_f_priv *priv)
{
	/* FIXME: write_mask */
	return 0;
}

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

	if (!__zbg_read_mask(priv)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(bi->q, __zbg_read_mask(priv));
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
	block = chan->current_block;

	/* So, it's readable */
	if (priv->type == ZIO_CDEV_CTRL) {
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
		chan->current_block = NULL;
		bi->b_op->free_block(bi, block);
	}
	return count;
}
EXPORT_SYMBOL(zio_generic_read);

ssize_t zio_generic_write(struct file *f, const char __user *ubuf,
			  size_t count, loff_t *offp)
{
	/* FIXME: write still missing */
	return -ENOSYS;
}
EXPORT_SYMBOL(zio_generic_write);

unsigned int zio_generic_poll(struct file *f, struct poll_table_struct *w)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;

	poll_wait(f, &bi->q, w);
	return __zbg_read_mask(priv) | __zbg_write_mask(priv);
}
EXPORT_SYMBOL(zio_generic_poll);

int zio_generic_release(struct inode *inode, struct file *f)
{
	struct zio_f_priv *priv = f->private_data;

	/* priv is allocated by zio_f_open, must be freed */
	kfree(priv);
	return 0;
}
EXPORT_SYMBOL(zio_generic_release);
