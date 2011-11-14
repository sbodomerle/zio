/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

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

	if (!chan->current_block)
		chan->current_block = bi->b_op->retr_block(bi);
	if (!chan->current_block)
		return 0;

	/* We have a block. So there is data and possibly control too */
	if (likely(priv->type == ZIO_CDEV_DATA))
		return can_read;

	if (!zio_is_cdone(chan->current_block))
		return POLLIN | POLLRDNORM;

	/* There's a block, but we want to re-read control. Get a new block */
	bi->b_op->free_block(bi, chan->current_block);
	chan->current_block = bi->b_op->retr_block(bi);
	if (!chan->current_block)
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
	if (!chan->current_block)
		chan->current_block = __zio_write_allocblock(bi, NULL);
	block = chan->current_block;
	if (!block)
		return 0;

	/* If the block is not full, user can write data */
	if (block->uoff < block->datalen)
		return can_write;

	/* Block is full: try to push out to the buffer */
	if (bi->b_op->store_block(bi, block) < 0)
		return 0;

	/* We sent it: get a new one for this new data */
	chan->current_block = __zio_write_allocblock(bi, NULL);
	return chan->current_block ? can_write : 0;
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
	block = chan->current_block;

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
		chan->current_block = NULL;
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
		block = chan->current_block;
		if (count > block->datalen - block->uoff)
			count =  block->datalen - block->uoff;
		if (copy_from_user(block->data + block->uoff, ubuf, count))
			return -EFAULT;
		block->uoff += count;
		if (block->uoff == block->datalen)
			if (bi->b_op->store_block(bi, block) == 0)
				chan->current_block = NULL;
		return count;
	}

	/* Control: drop the current block and create a new one */
	if (priv->type == ZIO_CDEV_CTRL && count < ZIO_CONTROL_SIZE)
		return -EINVAL;
	count = ZIO_CONTROL_SIZE;

	if (chan->current_block)
		bi->b_op->free_block(bi, chan->current_block);
	chan->current_block = NULL;
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
	return 0;
}
EXPORT_SYMBOL(zio_generic_release);
