/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a kmalloc-based buffer for the ZIO framework. It is used both
 * as a default when no buffer is selected by applications and as an
 * example about our our structures and methods are used.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

/* This is an instance of a buffer, associated to two cdevs */
struct zbk_instance {
	struct zio_bi bi;
	int nitem;
	struct list_head list; /* items list and lock */
	struct spinlock lock;
};
#define to_zbki(bi) container_of(bi, struct zbk_instance, bi)

/* The list in the structure above collects a bunch of these */
struct zbk_item {
	struct zio_block block;
	struct list_head list;	/* item list */
	struct zbk_instance *instance;
};
#define to_item(block) container_of(block, struct zbk_item, block);

static DEFINE_ZATTR_STD(ZBUF, zbk_std_zattr) = {
	ZATTR_REG(zbuf, ZATTR_ZBUF_MAXLEN, S_IRUGO | S_IWUGO, 0x0, 16),
};

int kmalloc_conf_set(struct kobject *kobj, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	zattr->value = usr_val;
	return 0;
}
struct zio_sys_operations zbk_sysfs_ops = {
	.conf_set = kmalloc_conf_set,
};


/* Alloc is called by the trigger (for input) or by f->write (for output) */
static struct zio_block *zbk_alloc_block(struct zio_bi *bi,
					 struct zio_control *ctrl,
					 size_t datalen, gfp_t gfp)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zbk_item *item;
	void *data;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* alloc item and data. Control remains null at this point */
	item = kzalloc(sizeof(*item), gfp);
	data = kmalloc(datalen, gfp);
	if (!item || !data)
		goto out_free;
	item->block.data = data;
	item->block.datalen = datalen;
	item->instance = zbki;

	zio_set_ctrl(&item->block, ctrl);

	return &item->block;

out_free:
	kfree(data);
	kfree(item);
	return ERR_PTR(-ENOMEM);
}

/* Free is called by f->read (for input) or by the trigger (for output) */
static void zbk_free_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_item *item;
	struct zbk_instance *zbki;

	pr_debug("%s:%d\n", __func__, __LINE__);

	item = to_item(block);
	zbki = item->instance;
	kfree(block->data);
	zio_free_control(zio_get_ctrl(block));
	kfree(item);
}

/* When write() stores the first block, we try pushing it */
static inline int __try_push(struct zio_ti *ti, struct zio_channel *chan,
			     struct zio_block *block)
{
	if (ti->t_op->push_block(ti, chan, block) < 0)
		return 0;
	return 1;
}

/* Store is called by the trigger (for input) or by f->write (for output) */
static int zbk_store_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zio_channel *chan = bi->chan;
	struct zbk_item *item;
	int awake = 0, pushed = 0, output;

	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, block);

	if (unlikely(!zio_get_ctrl(block))) {
		WARN_ON(1);
		return -EINVAL;
	}

	item = to_item(block);
	output = (bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT;

	/* add to the buffer instance or push to the trigger */
	spin_lock(&zbki->lock);
	if (zbki->nitem == bi->zattr_set.std_zattr[ZATTR_ZBUF_MAXLEN].value)
		goto out_unlock;
	if (!zbki->nitem) {
		if (unlikely(output))
			pushed = __try_push(chan->cset->ti, chan, block);
		else
			awake = 1;
	}
	if (likely(!pushed)) {
		zbki->nitem++;
		list_add_tail(&item->list, &zbki->list);
	}
	spin_unlock(&zbki->lock);

	/* if input, awake user space */
	if (awake && ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		wake_up_interruptible(&bi->q);
	return 0;

out_unlock:
	spin_unlock(&zbki->lock);
	return -ENOSPC;
}

/* Retr is called by f->read (for input) or by the trigger (for output) */
static struct zio_block *zbk_retr_block(struct zio_bi *bi)
{
	struct zbk_item *item;
	struct zbk_instance *zbki;
	struct zio_ti *ti;
	struct list_head *first;
	int awake = 0;

	zbki = to_zbki(bi);

	spin_lock(&zbki->lock);
	if (list_empty(&zbki->list))
		goto out_unlock;
	first = zbki->list.next;
	item = list_entry(first, struct zbk_item, list);
	list_del(&item->list);
	if (zbki->nitem == bi->zattr_set.std_zattr[ZATTR_ZBUF_MAXLEN].value)
		awake = 1;
	zbki->nitem--;
	spin_unlock(&zbki->lock);

	if (awake && ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT))
		wake_up_interruptible(&bi->q);
	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, item);
	return &item->block;

out_unlock:
	spin_unlock(&zbki->lock);
	/* There is no data in buffer, and we may pull to have data soon */
	ti = bi->cset->ti;
	if ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT && ti->t_op->pull_block)
		ti->t_op->pull_block(ti, bi->chan);
	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, NULL);
	return NULL;
}

/* Create is called by zio for each channel electing to use this buffer type */
static struct zio_bi *zbk_create(struct zio_buffer_type *zbuf,
	struct zio_channel *chan, fmode_t f_flags)
{
	struct zbk_instance *zbki;

	pr_debug("%s:%d\n", __func__, __LINE__);

	zbki = kzalloc(sizeof(*zbki), GFP_KERNEL);
	if (!zbki)
		return ERR_PTR(-ENOMEM);
	spin_lock_init(&zbki->lock);
	INIT_LIST_HEAD(&zbki->list);

	/* all the fields of zio_bi are initialied by the caller */
	return &zbki->bi;
}

/* destroy is called by zio on channel removal or if it changes buffer type */
static void zbk_destroy(struct zio_bi *bi)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zbk_item *item;
	struct list_head *pos, *tmp;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* no need to lock here, zio ensures we are not active */
	list_for_each_safe(pos, tmp, &zbki->list) {
		item = list_entry(pos, struct zbk_item, list);
		zbk_free_block(&zbki->bi, &item->block);
	}
	kfree(zbki);
}

static const struct zio_buffer_operations zbk_buffer_ops = {
	.alloc_block =	zbk_alloc_block,
	.free_block =	zbk_free_block,
	.store_block =	zbk_store_block,
	.retr_block =	zbk_retr_block,
	.create =	zbk_create,
	.destroy =	zbk_destroy,
};

/*
 * File operations. We only have read and write: mmap is definitely
 * not suitable here, and open/release are not needed.
 */

static const struct file_operations zbk_file_ops = {
	.owner =	THIS_MODULE,
	.read =		zio_generic_read,
	.write =	zio_generic_write,
	.poll =		zio_generic_poll,
	.release =	zio_generic_release,
};

static struct zio_buffer_type zbk_buffer = {
	.owner =	THIS_MODULE,
	.zattr_set = {
		.std_zattr = zbk_std_zattr,
	},
	.s_op = &zbk_sysfs_ops,
	.b_op = &zbk_buffer_ops,
	.f_op = &zbk_file_ops,
};

static int zbk_init(void)
{
	return zio_register_buf(&zbk_buffer, "kmalloc");
}

static void zbk_exit(void)
{
	zio_unregister_buf(&zbk_buffer);
	/* FIXME REMOVE all instances left */
}

module_init(zbk_init);
module_exit(zbk_exit);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");
