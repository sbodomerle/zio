/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#define ZOBJ_SYSFS_NAME "name"
#define ZOBJ_SYSFS_ENABLE "enable"
#define CSET_SYSFS_BUFFER "current_buffer"
#define CSET_SYSFS_TRIGGER "current_trigger"

static struct zio_status *zstat = &zio_global_status; /* Always use ptr */

const char zio_zdev_attr_names[ZATTR_STD_NUM_ZDEV][ZIO_NAME_LEN] = {
	[ZATTR_GAIN]		= "gain_factor",
	[ZATTR_OFFSET]		= "offset",
	[ZATTR_NBITS]		= "resolution-bits",
	[ZATTR_MAXRATE]		= "max-sample-rate",
	[ZATTR_VREFTYPE]	= "vref-src",
};
EXPORT_SYMBOL(zio_zdev_attr_names);
const char zio_trig_attr_names[ZATTR_STD_NUM_TRIG][ZIO_NAME_LEN] = {
	[ZATTR_TRIG_REENABLE]	= "re-enable",
	[ZATTR_TRIG_NSAMPLES]	= "nsamples",
};
EXPORT_SYMBOL(zio_trig_attr_names);
const char zio_zbuf_attr_names[ZATTR_STD_NUM_ZBUF][ZIO_NAME_LEN] = {
	[ZATTR_ZBUF_MAXLEN]	= "max-buffer-len",
	[ZATTR_ZBUF_MAXKB]	= "max-buffer-kb",
};
EXPORT_SYMBOL(zio_zbuf_attr_names);

/* buffer instance prototype */
static struct zio_bi *__bi_create_and_init(struct zio_buffer_type *zbuf,
					   struct zio_channel *chan);
static void __bi_destroy(struct zio_buffer_type *zbuf, struct zio_bi *bi);
static int __bi_register(struct zio_buffer_type *zbuf, struct zio_channel *chan,
			 struct zio_bi *bi, const char *name);
static void __bi_unregister(struct zio_buffer_type *zbuf, struct zio_bi *bi);
/* trigger instance prototype */
static struct zio_ti *__ti_create_and_init(struct zio_trigger_type *trig,
					   struct zio_cset *cset);
static void __ti_destroy(struct zio_trigger_type *trig, struct zio_ti *ti);
static int __ti_register(struct zio_trigger_type *trig, struct zio_cset *cset,
			 struct zio_ti *ti, const char *name);
static void __ti_unregister(struct zio_trigger_type *trig, struct zio_ti *ti);
/* Attributes initlialization */
static void __zattr_trig_init_ctrl(struct zio_ti *ti, struct zio_control *ctrl);

/*
 * Top-level ZIO objects has a unique name.
 * You can find a particular object by searching its name.
 */
static inline struct zio_object_list_item *__find_by_name(
			struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *cur;

	if (!name)
		return NULL;
	list_for_each_entry(cur, &zobj_list->list, list) {
		pr_debug("%s:%d %s=%s\n", __func__, __LINE__, cur->name, name);
		if (strcmp(cur->name, name) == 0)
			return cur; /* object found */
	}
	return NULL;
}

static inline struct zio_object_list_item *__zio_object_get(
	struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *list_item;

	/* search for default trigger */
	list_item = __find_by_name(zobj_list, name);
	if (!list_item)
		return NULL;
	/* increment trigger usage to prevent rmmod */
	if (!try_module_get(list_item->owner))
		return NULL;
	return list_item;
}
static struct zio_buffer_type *zio_buffer_get(char *name)
{
	struct zio_object_list_item *list_item;

	list_item = __zio_object_get(&zstat->all_buffer_types, name);
	if (!list_item)
		return ERR_PTR(-ENODEV);
	return container_of(list_item->obj_head, struct zio_buffer_type, head);
}
static inline void zio_buffer_put(struct zio_buffer_type *zbuf)
{
	pr_debug("%s:%d %p\n", __func__, __LINE__, zbuf->owner);
	module_put(zbuf->owner);
}
static struct zio_trigger_type *zio_trigger_get(char *name)
{
	struct zio_object_list_item *list_item;

	list_item = __zio_object_get(&zstat->all_trigger_types, name);
	if (!list_item)
		return ERR_PTR(-ENODEV);
	return container_of(list_item->obj_head, struct zio_trigger_type, head);
}
static inline void zio_trigger_put(struct zio_trigger_type *trig)
{
	pr_debug("%s:%d %p\n", __func__, __LINE__, trig->owner);
	module_put(trig->owner);
}

/* data_done is called by the driver, after {in,out}put_cset */
void zio_generic_data_done(struct zio_cset *cset)
{
	struct zio_buffer_type *zbuf;
	struct zio_device *zdev;
	struct zio_channel *chan;
	struct zio_block *block;
	struct zio_ti *ti;
	struct zio_bi *bi;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ti = cset->ti;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	if (unlikely((ti->flags & ZIO_DIR) == ZIO_DIR_OUTPUT)) {
		cset_for_each(cset, chan) {
			bi = chan->bi;
			block = chan->active_block;
			if (block)
				zbuf->b_op->free_block(chan->bi, block);
			/* We may have a new block ready, or not */
			chan->active_block = zbuf->b_op->retr_block(chan->bi);
		}
		goto out;
	}
	/* DIR_INPUT */
	cset_for_each(cset, chan) {
		bi = chan->bi;
		block = chan->active_block;
		if (!block)
			continue;
		if (zbuf->b_op->store_block(bi, block)) /* may fail, no prob */
			zbuf->b_op->free_block(bi, block);
	}
out:
	spin_lock(&cset->lock);
	ti->flags &= (~ZTI_BUSY); /* Reset busy, now is idle */
	spin_unlock(&cset->lock);
}
EXPORT_SYMBOL(zio_generic_data_done);

static void __zio_fire_input_trigger(struct zio_ti *ti)
{
	struct zio_buffer_type *zbuf;
	struct zio_block *block;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_channel *chan;
	struct zio_control *ctrl;
	int errdone = 0;

	cset = ti->cset;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* Allocate the buffer for the incoming sample, in active channels */
	cset_for_each(cset, chan) {
		ctrl = zio_alloc_control(GFP_ATOMIC);
		if (!ctrl) {
			if (!errdone++)
				pr_err("%s: can't alloc control\n", __func__);
			continue;
		}
		/*
		 * Update sequence number too (first returned seq is 1).
		 * Sequence number is always increased to identify un-stored
		 * blocks or other errors in trigger activation.
		 */
		chan->current_ctrl->seq_num++;
		/* Copy the stamp (we are software driven anyways) */
		chan->current_ctrl->tstamp.secs = ti->tstamp.tv_sec;
		chan->current_ctrl->tstamp.ticks = ti->tstamp.tv_nsec;
		chan->current_ctrl->tstamp.bins = ti->tstamp_extra;
		memcpy(ctrl, chan->current_ctrl, ZIO_CONTROL_SIZE);

		block = zbuf->b_op->alloc_block(chan->bi, ctrl,
						ctrl->ssize * ctrl->nsamples,
						GFP_ATOMIC);
		if (IS_ERR(block)) {
			/* Remove the following print, it's common */
			if (0 && !errdone++)
				pr_err("%s: can't alloc block\n", __func__);
			zio_free_control(ctrl);
			chan->active_block = NULL;
			continue;
		}
		chan->active_block = block;
	}
	if (!cset->raw_io(cset)) {
		/* It succeeded immediately */
		ti->t_op->data_done(cset);
	}
}

static void __zio_fire_output_trigger(struct zio_ti *ti)
{
	struct zio_cset *cset = ti->cset;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* We are expected to already have a block in active channels */
	if (!cset->raw_io(cset)) {
		/* It succeeded immediately */
		ti->t_op->data_done(cset);
	}
}

/*
 * When a software trigger fires, it should call this function. Hw ones don't
 */
void zio_fire_trigger(struct zio_ti *ti)
{
	/* If the trigger runs too early, ti->cset is still NULL */
	if (!ti->cset)
		return;

	/* check if trigger is disabled or previous fire is still running */
	if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED ||
			(ti->flags & ZTI_BUSY)))
		return;
	spin_lock(&ti->cset->lock);
	ti->flags |= ZTI_BUSY;
	spin_unlock(&ti->cset->lock);

	if (likely((ti->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		__zio_fire_input_trigger(ti);
	else
		__zio_fire_output_trigger(ti);
}
EXPORT_SYMBOL(zio_fire_trigger);

static int __has_auto_index(char *s)
{
	int i = 0;
	for (i = 0; i < ZIO_NAME_LEN-1; i++) {
		if (s[i] != '%')
			continue;
		i++;
		if (s[i] == 'd')
			return 1;
	}
	return 0;
}
static int __next_strlen(char *str)
{
	int increment = 0, i;

	for (i = strlen(str)-1; i > 0; i--) {
		/* if is an ascii number */
		if (str[i] >= '0' && str[i] <= '9') {
			if (str[i] == '9')
				continue;
			else
				break;
		} else {
			increment++;
			break;
		}
	}
	return strlen(str) + increment;
}

/*
 * The zio device name must be unique. If it is not unique, a busy error is
 * returned.
 */
static int zobj_unique_name(struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *cur;
	struct zio_obj_head *tmp;
	unsigned int counter = 0, again, len;
	char name_to_check[ZIO_NAME_LEN];
	int auto_index = __has_auto_index(name);

	pr_debug("%s\n", __func__);

	if (!name)
		return -EINVAL;

	len = strlen(name);
	if (!len)
		return -EINVAL;

	strncpy(name_to_check, name, ZIO_NAME_LEN);
	do {
		again = 0;
		if (auto_index) { /* TODO when zio become bus, it is useless */
			sprintf(name_to_check, name, counter++);
			len = strlen(name_to_check);
		}

		list_for_each_entry(cur, &zobj_list->list, list) {
			tmp = cur->obj_head;
			if (strcmp(tmp->name, name_to_check))
				continue; /* no conflict */
			/* conflict found */

			/* if not auto-assigned, then error */
			if (!auto_index) {
				pr_err("ZIO: name \"%s\" is already taken\n",
					name);
				return -EBUSY;
			}
			/* build sequential name */
			if (__next_strlen(name_to_check) > ZIO_NAME_LEN) {
				pr_err("ZIO: invalid name \"%s\"\n", name);
				return -EINVAL;
			}
			again = 1;
			break;
		}
	} while (again);
	strncpy(name, name_to_check, ZIO_NAME_LEN);
	return 0;
}

static struct zio_attribute *__zattr_clone(const struct zio_attribute *src,
		unsigned int n)
{
	struct zio_attribute *dest = NULL;
	unsigned int size;

	if (!src)
		return NULL;
	size = n * sizeof(struct zio_attribute);
	dest = kmalloc(size, GFP_KERNEL);
	if (!dest)
		return NULL;

	dest = memcpy(dest, src, size);

	return dest;
}

static void __zattr_unclone(struct zio_attribute *zattr)
{
	kfree(zattr);
}

static int __zattr_set_copy(struct zio_attribute_set *dest,
				     struct zio_attribute_set *src)
{
	if (!dest || !src)
		return -EINVAL;
	dest->n_std_attr = src->n_std_attr;
	dest->n_ext_attr = src->n_ext_attr;
	dest->std_zattr = __zattr_clone(src->std_zattr, dest->n_std_attr);
	dest->ext_zattr = __zattr_clone(src->ext_zattr, dest->n_ext_attr);

	return 0;
}
static void __zattr_set_free(struct zio_attribute_set *zattr_set)
{
	if (!zattr_set)
		return;
	__zattr_unclone(zattr_set->ext_zattr);
	__zattr_unclone(zattr_set->std_zattr);
}

/* When touching attributes, we always use the spinlock for the hosting dev */
static spinlock_t *__get_spinlock(struct zio_obj_head *head)
{
	spinlock_t *lock;

	switch (head->zobj_type) {
	case ZDEV:
		lock = &to_zio_dev(&head->dev)->lock;
		break;
	case ZCSET:
		lock = &to_zio_cset(&head->dev)->zdev->lock;
		break;
	case ZCHAN:
		lock = &to_zio_chan(&head->dev)->cset->zdev->lock;
		break;
	case ZTI: /* we might not want to take a lock but... */
		lock = &to_zio_ti(&head->dev)->cset->zdev->lock;
		break;
	case ZBI:
		lock = &to_zio_bi(&head->dev)->cset->zdev->lock;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return lock;
}

static int zio_change_current_trigger(struct zio_cset *cset, char *name)
{
	struct zio_trigger_type *trig, *trig_old = cset->trig;
	struct zio_channel *chan;
	struct zio_ti *ti, *ti_old = cset->ti;
	int err, i;

	pr_debug("%s\n", __func__);
	spin_lock(&cset->lock);
	if (ti_old->flags & ZTI_BUSY) {
		spin_unlock(&cset->lock);
		return -EBUSY;
	}
	/* Set ti BUSY, so it cannot fire */
	ti_old->flags |= ZTI_BUSY;
	spin_unlock(&cset->lock);

	if (strlen(name) > ZIO_OBJ_NAME_LEN)
		return -EINVAL; /* name too long */
	if (unlikely(strcmp(name, trig_old->head.name) == 0))
		return 0; /* is the current trigger */

	/* get the new trigger */
	trig = zio_trigger_get(name);
	if (IS_ERR(trig))
		return PTR_ERR(trig);
	/* Create and register the new trigger instance */
	ti = __ti_create_and_init(trig, cset);
	if (IS_ERR(ti)) {
		err = PTR_ERR(ti);
		goto out;
	}
	err = __ti_register(trig, cset, ti, "trigger-tmp");
	if (err)
		goto out_reg;
	/* New ti successful created, remove the old ti */
	__ti_unregister(trig_old, ti_old);
	__ti_destroy(trig_old, ti_old);
	zio_trigger_put(trig_old);
	/* Set new trigger*/
	mb();
	cset->trig = trig;
	cset->ti = ti;
	/* Rename trigger-tmp to trigger */
	err = device_rename(&ti->head.dev, "trigger");
	if (err)
		WARN(1, "%s: cannot rename trigger folder for"
			" cset%d\n", __func__, cset->index);
	/* Update channel current controls */
	for (i = 0; i < cset->n_chan; ++i) {
		chan = &cset->chan[i];
		__zattr_trig_init_ctrl(ti, chan->current_ctrl);
	}
	return 0;

out_reg:
	__ti_destroy(trig, ti);
out:
	zio_trigger_put(trig);
	return err;
}

static int zio_change_current_buffer(struct zio_cset *cset, char *name)
{
	struct zio_buffer_type *zbuf, *zbuf_old = cset->zbuf;
	struct zio_bi **bi_vector;
	int i, j, err;

	pr_debug("%s\n", __func__);
	if (strlen(name) > ZIO_OBJ_NAME_LEN)
		return -EINVAL; /* name too long */
	if (unlikely(strcmp(name, cset->zbuf->head.name) == 0))
		return 0; /* is the current buffer */

	zbuf = zio_buffer_get(name);
	if (IS_ERR(zbuf))
		return PTR_ERR(zbuf);

	bi_vector = kzalloc(sizeof(struct zio_bi *) * cset->n_chan,
			     GFP_KERNEL);
	if (!bi_vector) {
		err = -ENOMEM;
		goto out;
	}
	/* Create a new buffer instance for each channel of the cset */
	for (i = 0; i < cset->n_chan; ++i) {
		bi_vector[i] = __bi_create_and_init(zbuf, &cset->chan[i]);
		if (IS_ERR(bi_vector[i])) {
			pr_err("%s can't create buffer instance\n", __func__);
			err = PTR_ERR(bi_vector[i]);
			goto out_create;
		}
		err = __bi_register(zbuf, &cset->chan[i], bi_vector[i],
				    "buffer-tmp");
		if (err) {
			pr_err("%s can't register buffer instance\n", __func__);
			__bi_destroy(zbuf, bi_vector[i]);
			goto out_create;
		}
	}

	for (i = 0; i < cset->n_chan; ++i) {
		/* Delete old buffer instance */
		__bi_unregister(zbuf_old, cset->chan[i].bi);
		__bi_destroy(zbuf_old, cset->chan[i].bi);
		/* Assign new buffer instance */
		cset->chan[i].bi = bi_vector[i];
		/* Rename buffer-tmp to trigger */
		err = device_rename(&cset->chan[i].bi->head.dev, "buffer");
		if (err)
			WARN(1, "%s: cannot rename buffer folder for"
				" cset%d:chan%d\n", __func__, cset->index, i);
	}

	kfree(bi_vector);
	cset->zbuf = zbuf;
	zio_buffer_put(zbuf_old);

	return 0;

out_create:
	for (j = i-1; j >= 0; --j) {
		__bi_unregister(zbuf, bi_vector[j]);
		__bi_destroy(zbuf, bi_vector[j]);
	}
	kfree(bi_vector);
out:
	zio_buffer_put(zbuf);
	return err;
}

/*
 * used to init and update sysfs attribute value into a control.
 * The bit mask is set also during update to make the code simple, but
 * this does not decrease performance
 */
static inline void __zattr_valcpy(struct zio_ctrl_attr *ctrl,
				  struct zio_attribute *zattr)
{
	pr_debug("%s\n", __func__);
	if ((zattr->flags & ZATTR_TYPE) == ZATTR_TYPE_EXT) {
		ctrl->ext_mask |= (1 << zattr->index);
		ctrl->ext_val[zattr->index] = zattr->value;
	} else {
		if (zattr->index == ZATTR_INDEX_NONE)
			return;
		ctrl->std_mask |= (1 << zattr->index);
		ctrl->std_val[zattr->index] = zattr->value;
	}
}
static void __zattr_propagate_value(struct zio_obj_head *head,
			       struct zio_attribute *zattr)
{
	int i, j;
	struct zio_ti *ti;
	struct zio_device *zdev;
	struct zio_channel *chan;
	struct zio_cset *cset;
	struct zio_control *ctrl;

	pr_debug("%s\n", __func__);
	switch (head->zobj_type) {
	case ZDEV:
		zdev = to_zio_dev(&head->dev);
		for (i = 0; i < zdev->n_cset; ++i) {
			cset = &zdev->cset[i];
			for (j = 0; j < cset->n_chan; ++j) {
				ctrl = cset->chan[j].current_ctrl;
				__zattr_valcpy(&ctrl->attr_channel, zattr);
			}
		}
		break;
	case ZCSET:
		cset = to_zio_cset(&head->dev);
		for (i = 0; i < cset->n_chan; ++i) {
			ctrl = cset->chan[i].current_ctrl;
			__zattr_valcpy(&ctrl->attr_channel, zattr);
		}
		break;
	case ZCHAN:
		ctrl = to_zio_chan(&head->dev)->current_ctrl;
		__zattr_valcpy(&ctrl->attr_channel, zattr);
		break;
	case ZTI:
		ti = to_zio_ti(&head->dev);
		/* Update all channel current control */
		for (i = 0; i < ti->cset->n_chan; ++i) {
			chan = &ti->cset->chan[i];
			ctrl = chan->current_ctrl;
			__zattr_valcpy(&ctrl->attr_trigger, zattr);
			if (zattr->index == ZATTR_TRIG_NSAMPLES &&
				(zattr->flags & ZATTR_TYPE) == ZATTR_TYPE_STD)
				chan->current_ctrl->nsamples = zattr->value;
		}
		break;
	default:
		return;
	}
}

static void __zattr_trig_init_ctrl(struct zio_ti *ti, struct zio_control *ctrl)
{
	int i;
	struct zio_ctrl_attr *ctrl_attr_trig = &ctrl->attr_trigger;

	strncpy(ctrl->triggername, ti->cset->trig->head.name, ZIO_OBJ_NAME_LEN);
	/* Copy trigger value */
	for (i = 0; i < ti->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_trig, &ti->zattr_set.std_zattr[i]);
	for (i = 0; i < ti->zattr_set.n_ext_attr; ++i)
		__zattr_valcpy(ctrl_attr_trig, &ti->zattr_set.ext_zattr[i]);
}
static int __zattr_chan_init_ctrl(struct zio_channel *chan, unsigned int start)
{
	struct zio_ctrl_attr*ctrl_attr_chan;
	struct zio_control *ctrl;
	struct zio_device *zdev;
	struct zio_cset *cset;
	int i;

	cset = chan->cset;
	zdev = cset->zdev;
	ctrl = chan->current_ctrl;
	ctrl_attr_chan = &chan->current_ctrl->attr_channel;
	if (!(start + chan->zattr_set.n_ext_attr < 32)) {
		pr_err("%s: too many extended attribute in %s",
			__func__, chan->cset->zdev->head.name);
		return -EINVAL;
	}

	pr_debug("%s copy device values\n", __func__);
	/* Copy channel attributes */
	for (i = 0; i < chan->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &chan->zattr_set.std_zattr[i]);
	for (i = 0; i < chan->zattr_set.n_ext_attr; ++i) {
		/* Fix channel extended attribute index */
		chan->zattr_set.ext_zattr[i].index = start + i;
		__zattr_valcpy(ctrl_attr_chan, &chan->zattr_set.ext_zattr[i]);
	}

	/* Copy cset attributes */
	for (i = 0; i < cset->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &cset->zattr_set.std_zattr[i]);
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &cset->zattr_set.ext_zattr[i]);

	/* Copy device attributes */
	for (i = 0; i < zdev->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &zdev->zattr_set.std_zattr[i]);
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &zdev->zattr_set.ext_zattr[i]);

	pr_debug("%s copy trigger values\n", __func__);
	__zattr_trig_init_ctrl(cset->ti, chan->current_ctrl);

	return 0;
}
static int __zattr_cset_init_ctrl(struct zio_cset *cset, unsigned int start)
{
	int i, err, start_c;

	/* Fix cset extended attribute index */
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		cset->zattr_set.ext_zattr[i].index = start + i;
	start_c = start + i;
	for (i = 0; i < cset->n_chan; ++i) {
		err = __zattr_chan_init_ctrl(&cset->chan[i], start_c);
		if (err)
			return err;
	}
	return 0;
}
/*
 * fix the zio attribute index for the extended attribute within device
 * and set the attribute value into the current control of each channel
 */
static int __zattr_dev_init_ctrl(struct zio_device *zdev)
{
	int i, err, start;

	pr_debug("%s\n", __func__);
	/* Device level */
	/* Fix device extended attribute index */
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		zdev->zattr_set.ext_zattr[i].index = i;

	start = i;
	for (i = 0; i < zdev->n_cset; ++i) {
		err = __zattr_cset_init_ctrl(&zdev->cset[i], start);
		if (err)
			return err;
	}
	return 0;
}

/*
 * enable/disable a zio object
 * must be called while holding the zio_device spinlock
 */
static void __zobj_enable(struct device *dev, unsigned int enable)
{
	unsigned long *flags;
	int i, status;
	struct zio_obj_head *head;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_ti *ti;

	pr_debug("%s\n", __func__);
	head = to_zio_head(dev);

	flags = __get_from_zobj(to_zio_head(dev), flags);
	status = !((*flags) & ZIO_STATUS);
	/* if the status is not changing */
	if (!(enable ^ status))
		return;
	/* change status */
	*flags = (*flags | ZIO_STATUS) & status;
	switch (head->zobj_type) {
	case ZDEV:
		pr_debug("%s: zdev\n", __func__);

		zdev = to_zio_dev(dev);
		/* enable/disable all cset */
		for (i = 0; i < zdev->n_cset; ++i)
			__zobj_enable(&zdev->cset[i].head.dev, enable);
		/* device callback */
		break;
	case ZCSET:
		pr_debug("%s: zcset\n", __func__);

		cset = to_zio_cset(dev);
		/* enable/disable trigger instance */
		__zobj_enable(&cset->ti->head.dev, enable);
		/* enable/disable all channel*/
		for (i = 0; i < cset->n_chan; ++i)
			__zobj_enable(&cset->chan[i].head.dev, enable);
		/* cset callback */
		break;
	case ZCHAN:
		pr_debug("%s: zchan\n", __func__);
		/* channel callback */
		break;
	case ZTI:
		pr_debug("%s: zti\n", __func__);

		ti = to_zio_ti(dev);
		/* if trigger is running, abort it*/
		spin_lock(&ti->cset->lock);
		if (*flags & ZTI_BUSY) {
			if(ti->t_op->abort)
				ti->t_op->abort(ti->cset);
			*flags &= ~ZTI_BUSY; /* when disabled is not busy */
		}
		spin_unlock(&ti->cset->lock);
		/* trigger instance callback */
		if (ti->t_op->change_status)
			ti->t_op->change_status(ti, status);
		break;
	/* following objects can't be enabled/disabled */
	case ZBUF:
	case ZTRIG:
	case ZBI:
		pr_debug("%s: others\n", __func__);
		/* buffer instance callback */
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
	}
}

/* Print the name of a zio object */
static ssize_t zobj_show_name(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_head(dev)->name);
}
/* Print the current trigger name */
static ssize_t zobj_show_cur_trig(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_cset(dev)->trig->head.name);
}
/* Change the current trigger */
static ssize_t zobj_store_cur_trig(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char buf_tmp[ZIO_OBJ_NAME_LEN];
	struct zio_cset *cset;
	int err = 0;

	if (strlen(buf) > ZIO_OBJ_NAME_LEN+1)
		return -EINVAL; /* name too long */
	sscanf(buf, "%s\n", buf_tmp);
	cset = to_zio_cset(dev);
	/* change trigger only if is different then current */
	if (strcmp(buf_tmp, cset->trig->head.name))
		err = zio_change_current_trigger(cset, buf_tmp);
	return err ? err : count;
}
/* Print the current buffer name */
static ssize_t zobj_show_cur_zbuf(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_cset(dev)->zbuf->head.name);
}
/* Change the current buffer */
static ssize_t zobj_store_cur_zbuf(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char buf_tmp[ZIO_OBJ_NAME_LEN];
	struct zio_cset *cset;
	int err = 0;

	if (strlen(buf) > ZIO_OBJ_NAME_LEN+1)
		return -EINVAL; /* name too long */
	sscanf(buf, "%s\n", buf_tmp);
	cset = to_zio_cset(dev);
	/* change buffer only if is different then current */
	if (strcmp(buf_tmp, cset->trig->head.name))
		err = zio_change_current_buffer(cset, buf_tmp);
	return err ? err : count;
}
/* Print the current enable status */
static ssize_t zobj_show_enable(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	unsigned long *flags;
	int status;

	flags = __get_from_zobj(to_zio_head(dev), flags);
	status = !(*flags & ZIO_DISABLED);
	return sprintf(buf, "%d\n", status);
}
/* Change the current enable status */
static ssize_t zobj_store_enable(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	long val;
	int err;
	spinlock_t *lock;

	err = strict_strtol(buf, 0, &val);
	if (err)
		return -EINVAL;

	lock = __get_spinlock(to_zio_head(dev));
	/* change enable status */
	if (unlikely(strcmp(attr->attr.name, ZOBJ_SYSFS_ENABLE) == 0 &&
	    (val == 0 || val == 1))) {
		spin_lock(lock);
		__zobj_enable(dev, val);
		spin_unlock(lock);
		return count;
	}

	return count;
}
/*
 * Zio objects all handle uint32_t values. So the show and store
 * are centralized here, and each device has its own get_info and set_conf
 * which handle binary 32-bit numbers. Both the function are locked to prevent
 * concurrency issue when editing device register.
 */
static ssize_t zattr_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct zio_attribute *zattr = to_zio_zattr(attr);
	ssize_t len = 0;


	pr_debug("%s\n", __func__);
	if (!zattr->s_op)
		goto out;
	if (zattr->s_op->info_get) {
		struct zio_obj_head *head = to_zio_head(dev);
		spinlock_t *lock;
		int err = 0;

		lock = __get_spinlock(head);
		spin_lock(lock);
		err = zattr->s_op->info_get(dev, zattr, &zattr->value);
		spin_unlock(lock);
		if (err)
			return err;
	}
out:
	len = sprintf(buf, "%i\n", zattr->value);
	return len;
}

static ssize_t zattr_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct zio_attribute *zattr = to_zio_zattr(attr);
	struct zio_obj_head *head = to_zio_head(dev);
	spinlock_t *lock;
	long val;
	int err;

	pr_debug("%s\n", __func__);

	if (strict_strtol(buf, 0, &val))
		return -EINVAL;

	/* device attributes */
	if (!zattr->s_op->conf_set)
		return -EINVAL;

	lock = __get_spinlock(head);
	spin_lock(lock);
	err = zattr->s_op->conf_set(dev, zattr, (uint32_t)val);
	if (err) {
		spin_unlock(lock);
		return err;
	}
	zattr->value = (uint32_t)val;
	__zattr_propagate_value(head, zattr);
	spin_unlock(lock);

	return count;
}

static ssize_t zio_show_version(struct bus_type *bus, char *buf)
{
	return sprintf(buf, "%d.%d\n", ZIO_MAJOR_VERSION, ZIO_MINOR_VERSION);
}
static ssize_t zio_show_buffers(struct bus_type *bus, char *buf)
{
	struct zio_object_list_item *cur;
	ssize_t len = 0;

	spin_lock(&zstat->lock);
	list_for_each_entry(cur, &zstat->all_buffer_types.list, list)
		len = sprintf(buf, "%s%s\n", buf, cur->name);
	spin_unlock(&zstat->lock);

	return len;
}
static ssize_t zio_show_triggers(struct bus_type *bus, char *buf)
{
	struct zio_object_list_item *cur;
	ssize_t len = 0;

	spin_lock(&zstat->lock);
	list_for_each_entry(cur, &zstat->all_trigger_types.list, list)
		len = sprintf(buf, "%s%s\n", buf, cur->name);
	spin_unlock(&zstat->lock);

	return len;
}

/* Attributes to change buffer and trigger instance*/
static DEVICE_ATTR(current_trigger, 0666, zobj_show_cur_trig,
					  zobj_store_cur_trig);
static DEVICE_ATTR(current_buffer, 0666, zobj_show_cur_zbuf,
					 zobj_store_cur_zbuf);

/* Bus definition */
static struct bus_attribute def_bus_attrs[] = {
	__ATTR(version, 0444, zio_show_version, NULL),
	__ATTR(available_buffers, 0444, zio_show_buffers, NULL),
	__ATTR(available_triggers, 0444, zio_show_triggers, NULL),
	__ATTR_NULL,
};
static struct device_attribute def_device_attrs[] = {
	__ATTR(name, 0444, zobj_show_name, NULL),
	__ATTR(enable, 0666, zobj_show_enable, zobj_store_enable),
	__ATTR_NULL,
};
static struct attribute *def_device_attrs_ptr[] = {
	&def_device_attrs[0].attr,
	&def_device_attrs[1].attr,
	NULL,
};
static const struct attribute_group def_device_groups[] = {
	{
		.attrs = def_device_attrs_ptr,
	},
};
static const struct attribute_group *def_device_groups_ptr[] = {
	&def_device_groups[0],
	NULL,
};
static int zio_match(struct device *dev, struct device_driver *drv)
{
	pr_debug("%s %s\n", __func__, dev->init_name);
	return 0;
}
struct bus_type zio_bus_type = {
	.name = "zio",
	.bus_attrs = def_bus_attrs,
	//.dev_attrs = def_device_attrs,
	.match = zio_match,
};

/* Device types */
void zio_device_release(struct device *dev)
{
	return;
}

struct device_type zobj_device_type = {
	.name = "zio object device type",
	.release = zio_device_release,
	.groups = def_device_groups_ptr,
};


static int __check_dev_zattr(struct zio_attribute_set *parent,
			     struct zio_attribute_set *this)
{
	int i, j;

	pr_debug("%s %d\n", __func__, this->n_std_attr);
	/* verify standard attribute */
	for (i = 0; i < this->n_std_attr; ++i) {
		if (this->std_zattr[i].index == ZATTR_INDEX_NONE)
			continue; /* next attribute */
		for (j = 0; j < parent->n_std_attr; ++j) {
			/*
			 * a standard attribute must be unique from a child
			 * to the root. This allow to create a consistent
			 * vector of value in control structure
			 */
			if (this->std_zattr[i].index ==
						parent->std_zattr[j].index) {
				pr_err("ZIO: attribute conflict for %s\n",
				       this->std_zattr[i].attr.attr.name);
				return -EINVAL;
			}
		}
	}
	return 0;
}
static int __check_attr(struct attribute *attr,
			const struct zio_sysfs_operations *s_op)
{
	/* check name*/
	if (!attr->name)
		return -EINVAL;

	/* check mode */
	if ((attr->mode & S_IWUGO) == S_IWUGO && !s_op->conf_set) {
		pr_err("ZIO: %s: attribute %s has write permission but "
			"no write function\n", __func__, attr->name);
		return -ENOSYS;
	}
	return 0;
}

static struct attribute_group *__allocate_group(int n_attr)
{
	struct attribute_group *group;

	group = kzalloc(sizeof(struct attribute_group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);
	group->attrs = kzalloc(sizeof(struct attribute) * n_attr, GFP_KERNEL);
	if (!group->attrs)
		return ERR_PTR(-ENOMEM);
	return group;
}

/* create a set of zio attributes: the standard one and the extended one */
static int zattr_set_create(struct zio_obj_head *head,
			    const struct zio_sysfs_operations *s_op)
{
	int i, err, a_count, g_count = 0, g = 0;
	const struct attribute_group **groups;
	struct zio_attribute_set *zattr_set;
	struct attribute *attr;

	pr_debug("%s\n", __func__);
	zattr_set = __get_from_zobj(head, zattr_set);
	if (!zattr_set)
		return -EINVAL; /* message already printed */

	if (zattr_set->std_zattr && zattr_set->n_std_attr)
		++g_count;	/* There are standard attributes */
	else
		zattr_set->n_std_attr = 0;
	if (zattr_set->ext_zattr && zattr_set->n_ext_attr)
		++g_count;	/* There are extended attributes */
	else
		zattr_set->n_ext_attr = 0;
	if (head->zobj_type == ZCSET)
		++g_count;	/* cset need current_(trigger|buffer)*/

	if (!g_count)
		goto out;

	/* Allocate needed groups. dev->groups is null ended */
	groups = kzalloc(sizeof(struct attribute_group*) * (g_count + 1),
			 GFP_KERNEL);
	if (!groups)
		return -ENOMEM;

	/* Allocate standard attribute group */
	if (!zattr_set->std_zattr || !zattr_set->n_std_attr)
		goto ext;
	groups[g] = __allocate_group(zattr_set->n_std_attr);
	if (IS_ERR(groups[0]))
		return PTR_ERR(groups[g]);
	for (i = 0, a_count = 0; i < zattr_set->n_std_attr; ++i) {
		attr = &zattr_set->std_zattr[i].attr.attr;
		err = __check_attr(attr, s_op);
		switch (err) {
		case 0:
			/* valid attribute */
			groups[g]->attrs[a_count++] = attr;
			zattr_set->std_zattr[i].attr.show = zattr_show;
			zattr_set->std_zattr[i].attr.store = zattr_store;
			zattr_set->std_zattr[i].s_op = s_op;
			zattr_set->std_zattr[i].index = i;
			break;
		case -EINVAL: /* unused std attribute */
			zattr_set->std_zattr[i].index = ZATTR_INDEX_NONE;
			break;
		default:
			return err;
		}
	}
	++g;
ext:
	/* Allocate extended attribute group */
	if (!zattr_set->ext_zattr || !zattr_set->n_ext_attr)
		goto cset;
	groups[g] = __allocate_group(zattr_set->n_ext_attr);
	if (IS_ERR(groups[g]))
		return PTR_ERR(groups[g]);
	pr_debug("%s %p", __func__, groups[g]);
	for (i = 0, a_count = 0; i < zattr_set->n_ext_attr; ++i) {
		attr = &zattr_set->ext_zattr[i].attr.attr;
		err = __check_attr(attr, s_op);
		if (err)
			return err;
		/* valid attribute */
		groups[1]->attrs[a_count++] = attr;
		zattr_set->ext_zattr[i].attr.show = zattr_show;
		zattr_set->ext_zattr[i].attr.store = zattr_store;
		zattr_set->ext_zattr[i].s_op = s_op;
		zattr_set->ext_zattr[i].index = i; /* FIXME useless for zdev*/
		zattr_set->ext_zattr[i].flags |= ZATTR_TYPE_EXT;
	}
	++g;
cset:
	/* Allocate cset special attributes */
	if (head->zobj_type != ZCSET)
		goto out_assign;
	groups[g] = __allocate_group(2);
	if (IS_ERR(groups[2]))
		return PTR_ERR(groups[g]);
	groups[g]->attrs[0] = &dev_attr_current_buffer.attr;
	groups[g]->attrs[1] = &dev_attr_current_trigger.attr;
	++g;
out_assign:
	groups[g] = NULL;
	head->dev.groups = groups;
out:
	return 0;
}
/* Remove an existent set of attributes */
static void zattr_set_remove(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;
	int i;

	zattr_set = __get_from_zobj(head, zattr_set);
	if (!zattr_set)
		return;
	if (! head->dev.groups)
		return;
	for (i = 0; head->dev.groups[i]; ++i) {
		kfree(head->dev.groups[i]->attrs);
		kfree(head->dev.groups[i]);
	}
}

/* create and initialize a new buffer instance */
static struct zio_bi *__bi_create_and_init(struct zio_buffer_type *zbuf,
					   struct zio_channel *chan)
{
	struct zio_bi *bi;
	int err;

	pr_debug("%s\n", __func__);
	/* Create buffer */
	bi = zbuf->b_op->create(zbuf, chan);
	if (IS_ERR(bi)) {
		pr_err("ZIO %s: can't create buffer, error %ld\n",
		       __func__, PTR_ERR(bi));
		goto out;
	}
	/* Initialize buffer */
	bi->b_op = zbuf->b_op;
	bi->f_op = zbuf->f_op;
	bi->v_op = zbuf->v_op;
	bi->flags |= (chan->flags & ZIO_DIR);
	/* Initialize head */
	bi->head.dev.type = &zobj_device_type;
	bi->head.dev.parent = &chan->head.dev;
	bi->head.zobj_type = ZBI;
	snprintf(bi->head.name, ZIO_NAME_LEN, "%s-%s-%d-%d",
		 zbuf->head.name, chan->cset->zdev->head.name,
		 chan->cset->index, chan->index);
	init_waitqueue_head(&bi->q);
	/* Copy sysfs attribute from buffer type */
	err = __zattr_set_copy(&bi->zattr_set, &zbuf->zattr_set);
	if (err) {
		zbuf->b_op->destroy(bi);
		bi = ERR_PTR(err);
	}
out:
	return bi;
}
static void __bi_destroy(struct zio_buffer_type *zbuf, struct zio_bi *bi)
{
	pr_debug("%s\n", __func__);
	zbuf->b_op->destroy(bi);
	__zattr_set_free(&bi->zattr_set);
}
static int __bi_register(struct zio_buffer_type *zbuf,
			 struct zio_channel *chan,
			 struct zio_bi *bi, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	bi->head.dev.init_name = name;
	/* Create attributes */
	err = zattr_set_create(&bi->head, zbuf->s_op);
	if (err)
		goto out;
	/* Register buffer instance */
	err = device_register(&bi->head.dev);
	if (err)
		goto out_reg;

	/* Add to buffer instance list */
	spin_lock(&zbuf->lock);
	list_add(&bi->list, &zbuf->list);
	spin_unlock(&zbuf->lock);
	bi->cset = chan->cset;
	bi->chan = chan;
	/* Done. This bi->chan marks everything is running */

	return 0;

out_reg:
	zattr_set_remove(&bi->head);
out:
	return err;
}
static void __bi_unregister(struct zio_buffer_type *zbuf, struct zio_bi *bi)
{
	pr_debug("%s\n", __func__);
	/* Remove from buffer instance list */
	spin_lock(&zbuf->lock);
	list_del(&bi->list);
	spin_unlock(&zbuf->lock);
	device_unregister(&bi->head.dev);
	/* Remove zio attribute */
	zattr_set_remove(&bi->head);
}

/* create and initialize a new trigger instance */
static struct zio_ti *__ti_create_and_init(struct zio_trigger_type *trig,
					   struct zio_cset *cset)
{
	int err;
	struct zio_ti *ti;

	pr_debug("%s\n", __func__);
	/* Create trigger */
	ti = trig->t_op->create(trig, cset, NULL, 0/*FIXME*/);
	if (IS_ERR(ti)) {
		pr_err("ZIO %s: can't create trigger, error %ld\n",
		       __func__, PTR_ERR(ti));
		goto out;
	}
	/* Initialize trigger */
	ti->t_op = trig->t_op;
	ti->flags |= cset->flags & ZIO_DIR;
	/* Initialize head */
	ti->head.dev.type = &zobj_device_type;
	ti->head.dev.parent = &cset->head.dev;
	ti->head.zobj_type = ZTI;
	snprintf(ti->head.name, ZIO_NAME_LEN, "%s-%s-%d",
		 trig->head.name, cset->zdev->head.name, cset->index);
	/* Copy sysfs attribute from trigger type */
	err = __zattr_set_copy(&ti->zattr_set, &trig->zattr_set);
	if (err) {
		trig->t_op->destroy(ti);
		return ERR_PTR(err);
	}

out:
	return ti;

}
static void __ti_destroy(struct zio_trigger_type *trig, struct zio_ti *ti)
{
	pr_debug("%s\n", __func__);
	trig->t_op->destroy(ti);
	__zattr_set_free(&ti->zattr_set);
}
static int __ti_register(struct zio_trigger_type *trig, struct zio_cset *cset,
			 struct zio_ti *ti, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	ti->head.dev.init_name = name;
	/* Create attributes */
	err = zattr_set_create(&ti->head, trig->s_op);
	if (err)
		goto out;
	/* Register trigger instance */
	err = device_register(&ti->head.dev);
	if (err)
		goto out_reg;
	/* Add to trigger instance list */
	spin_lock(&trig->lock);
	list_add(&ti->list, &trig->list);
	spin_unlock(&trig->lock);
	ti->cset = cset;
	/* Done. This ti->cset marks everything is running */

	return 0;

out_reg:
	zattr_set_remove(&ti->head);
out:
	return err;
}
static void __ti_unregister(struct zio_trigger_type *trig, struct zio_ti *ti)
{
	pr_debug("%s\n", __func__);
	/* Remove from trigger instance list */
	spin_lock(&trig->lock);
	list_del(&ti->list);
	spin_unlock(&trig->lock);
	device_unregister(&ti->head.dev);
	/* Remove zio attributes */
	zattr_set_remove(&ti->head);
}

/*
 * Return the resolution bit of the zio device. The function look in each
 * hierarchy level to find this value
 */
static uint16_t __get_nbits(struct zio_channel *chan)
{
	struct zio_device *zdev;
	struct zio_cset *cset;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (chan->zattr_set.std_zattr)
		if (chan->zattr_set.std_zattr[ZATTR_NBITS].value)
			return chan->zattr_set.std_zattr[ZATTR_NBITS].value;
	cset = chan->cset;
	if (cset->zattr_set.std_zattr)
		if (cset->zattr_set.std_zattr[ZATTR_NBITS].value)
			return cset->zattr_set.std_zattr[ZATTR_NBITS].value;
	zdev = cset->zdev;
	if (zdev->zattr_set.std_zattr)
		if (zdev->zattr_set.std_zattr[ZATTR_NBITS].value)
			return zdev->zattr_set.std_zattr[ZATTR_NBITS].value;

	pr_err("%s: device \"%s\" lacks mandatory \"resolution bit\" attribute",
		__func__, chan->cset->zdev->head.name);
	return 0;
}
/*
 * chan_register registers one channel.  It is important to register
 * or unregister all the channels of a cset at the same time to prevent
 * overlaps in the minors.
 */
static int chan_register(struct zio_channel *chan)
{
	struct zio_control *ctrl;
	struct zio_bi *bi;
	int err;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!chan)
		return -EINVAL;

	/* Allocate, initialize and assign a current control for channel */
	ctrl = zio_alloc_control(GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->cset_i = chan->cset->index;
	ctrl->chan_i = chan->index;
	strncpy(ctrl->devname, chan->cset->zdev->head.name, ZIO_NAME_LEN);
	ctrl->nbits = __get_nbits(chan);
	if (!ctrl->nbits) {
		err = -EINVAL; /* message already printed */
		goto out;
	}
	ctrl->ssize = chan->cset->ssize;
	/* Trigger instance is already assigned so */
	ctrl->nsamples =
		chan->cset->ti->zattr_set.std_zattr[ZATTR_TRIG_NSAMPLES].value;
	chan->current_ctrl = ctrl;

	/* Initialize head */
	if (strlen(chan->head.name) == 0)
		snprintf(chan->head.name, ZIO_NAME_LEN, "chan%i", chan->index);
	chan->head.dev.init_name = chan->head.name;
	chan->head.dev.type = &zobj_device_type;
	chan->head.dev.parent = &chan->cset->head.dev;
	chan->head.zobj_type = ZCHAN;

	/* Create sysfs channel attributes */
	if (chan->zattr_set.std_zattr)
		chan->zattr_set.n_std_attr = ZATTR_STD_NUM_ZDEV;
	err = zattr_set_create(&chan->head, chan->cset->zdev->s_op);
	if (err)
		goto out;
	/* Register channel */
	err = device_register(&chan->head.dev);
	if (err)
		goto out_reg;
	/* Check attribute hierarchy */
	err = __check_dev_zattr(&chan->cset->zattr_set, &chan->zattr_set);
	if (err)
		goto out_remove_sys;
	pr_debug("%s:%d\n", __func__, __LINE__);
	err = __check_dev_zattr(&chan->cset->zdev->zattr_set, &chan->zattr_set);
	if (err)
		goto out_remove_sys;

	/* Create buffer */
	bi = __bi_create_and_init(chan->cset->zbuf, chan);
	if (IS_ERR(bi)) {
		err = PTR_ERR(bi);
		goto out_remove_sys;
	}
	err = __bi_register(chan->cset->zbuf, chan, bi, "buffer");
	if (err)
		goto out_bi_destroy;
	/* Assign the buffer instance to this channel */
	chan->bi = bi;
	/* Create channel char devices*/
	err = zio_create_chan_devices(chan);
	if (err)
		goto out_create;
	/*
	 * If no name was assigned, ZIO assigns it.  channel name is
	 * set to the kobject name. kobject name has no length limit,
	 * so the channel name is the first ZIO_NAME_LEN characters of
	 * kobject name. A duplicate channel name is not a problem
	 * anyways.
	 */
	if (!strlen(chan->head.name))
		strncpy(chan->head.name, dev_name(&chan->head.dev),
			ZIO_NAME_LEN);
	return 0;

out_create:
	__bi_unregister(chan->cset->zbuf, bi);
out_bi_destroy:
	__bi_destroy(chan->cset->zbuf, bi);
out_remove_sys:
	device_unregister(&chan->head.dev);
out_reg:
	zattr_set_remove(&chan->head);
out:
	zio_free_control(ctrl);
	return err;
}

static void chan_unregister(struct zio_channel *chan)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!chan)
		return;
	zio_destroy_chan_devices(chan);
	/* destroy buffer instance */
	__bi_unregister(chan->cset->zbuf, chan->bi);
	__bi_destroy(chan->cset->zbuf, chan->bi);
	device_unregister(&chan->head.dev);
	zattr_set_remove(&chan->head);
	zio_free_control(chan->current_ctrl);
}

/*
 * @cset_alloc_chan: low-level drivers can avoid allocating their channels,
 * they say how many are there and ZIO allocates them.
 * @cset_free_chan: if ZIO allocated channels, then it frees them; otherwise
 * it does nothing.
 */
static struct zio_channel *cset_alloc_chan(struct zio_cset *cset)
{
	int i;

	pr_debug("%s:%d\n", __func__, __LINE__);
	/*if no static channels, then ZIO must alloc them */
	if (cset->chan)
		return cset->chan;

	/* initialize memory to zero to have correct flags and attrs */
	cset->chan = kzalloc(sizeof(struct zio_channel) * cset->n_chan,
			     GFP_KERNEL);
	if (!cset->chan)
		return ERR_PTR(-ENOMEM);
	cset->flags |= ZCSET_CHAN_ALLOC;

	if (!cset->chan_template)
		return cset->chan;

	cset->chan_template->zattr_set.n_std_attr = ZATTR_STD_NUM_ZDEV;
	/* apply template on channels */
	for (i = 0; i < cset->n_chan; ++i) {
		memcpy(cset->chan + i, cset->chan_template,
		       sizeof(struct zio_channel));
		__zattr_set_copy(&cset->chan[i].zattr_set,
				 &cset->chan_template->zattr_set);
	}

	return cset->chan;
}
static inline void cset_free_chan(struct zio_cset *cset)
{
	int i;

	pr_debug("%s:%d\n", __func__, __LINE__);
	/* Only allocated channels need to be freed */
	if (!(cset->flags & ZCSET_CHAN_ALLOC))
		return;
	if(cset->chan_template)
		for (i = 0; i < cset->n_chan; ++i)
			__zattr_set_free(&cset->chan[i].zattr_set);
	kfree(cset->chan);
}

static int cset_register(struct zio_cset *cset)
{
	int i, j, err = 0;
	struct zio_buffer_type *zbuf = NULL;
	struct zio_trigger_type *trig = NULL;
	struct zio_ti *ti = NULL;
	char *name;

	pr_debug("%s\n", __func__);
	if (!cset)
		return -EINVAL;

	if (!cset->n_chan) {
		pr_err("ZIO: no channels in cset%i\n", cset->index);
		return -EINVAL;
	}

	if (!cset->ssize) {
		pr_err("ZIO: ssize can not be 0 in cset%i\n", cset->index);
		return -EINVAL;
	}

	/* Get an available minor base */
	err = __zio_minorbase_get(cset);
	if (err) {
		pr_err("ZIO: no minors available\n");
		return -EBUSY;
	}
	/* Initialize head */
	if (strlen(cset->head.name) == 0)
		snprintf(cset->head.name, ZIO_NAME_LEN, "cset%i", cset->index);
	cset->head.dev.init_name = cset->head.name;
	cset->head.dev.type = &zobj_device_type;
	cset->head.dev.parent = &cset->zdev->head.dev;
	cset->head.zobj_type = ZCSET;
	/* Create sysfs cset attributes */
	if (cset->zattr_set.std_zattr)
		cset->zattr_set.n_std_attr = ZATTR_STD_NUM_ZDEV;
	err = zattr_set_create(&cset->head, cset->zdev->s_op);
	if (err)
		goto out;
	/* Register cset */
	err = device_register(&cset->head.dev);
	if (err)
		goto out_dev_reg;
	/* Check attribute hierarchy */
	err = __check_dev_zattr(&cset->zdev->zattr_set, &cset->zattr_set);
	if (err)
		goto out_remove_sys;
	pr_debug("%s:%d\n", __func__, __LINE__);
	cset->chan = cset_alloc_chan(cset);
	if (IS_ERR(cset->chan)) {
		err = PTR_ERR(cset->chan);
		goto out_remove_sys;
	}
	spin_lock_init(&cset->lock);
	/*
	 * The cset must have a buffer type. If none is associated
	 * to the cset, ZIO selects the preferred or default one.
	 */
	if (!cset->zbuf) {
		name = cset->zdev->preferred_buffer;
		zbuf = zio_buffer_get(name);
		if (name && IS_ERR(zbuf))
			pr_warning("%s: no buffer \"%s\" (error %li), using "
				   "default\n", __func__, name, PTR_ERR(zbuf));
		if (IS_ERR(zbuf))
			zbuf = zio_buffer_get(ZIO_DEFAULT_BUFFER);
		if (IS_ERR(zbuf)) {
			err = PTR_ERR(zbuf);
			goto out_buf;
		}
		cset->zbuf = zbuf;
	}

	/*
	 * If no name was assigned, ZIO assigns it.  cset name is
	 * set to the kobject name. kobject name has no length limit,
	 * so the cset name is the first ZIO_NAME_LEN characters of
	 * kobject name. A duplicate cset name is not a problem
	 * anyways.
	 */
	if (!strlen(cset->head.name))
		strncpy(cset->head.name, dev_name(&cset->head.dev),
			ZIO_NAME_LEN);

	/*
	 * The cset must have a trigger type. If none  is associated
	 * to the cset, ZIO selects the default or preferred one.
	 * This is done late because each channel must be ready when
	 * the trigger fires.
	 */
	if (!cset->trig) {
		name = cset->zdev->preferred_trigger;
		trig = zio_trigger_get(name);
		if (name && IS_ERR(trig))
			pr_warning("%s: no trigger \"%s\" (error %li), using "
				   "default\n", __func__, name, PTR_ERR(trig));
		if (IS_ERR(trig))
			trig = zio_trigger_get(ZIO_DEFAULT_TRIGGER);
		if (IS_ERR(trig)) {
			err = PTR_ERR(trig);
			goto out_trig;
		}

		ti = __ti_create_and_init(trig, cset);
		if (IS_ERR(ti)) {
			err = PTR_ERR(ti);
			goto out_trig;
		}
		err = __ti_register(trig, cset, ti, "trigger");
		if (err)
			goto out_tr;
		cset->trig = trig;
		cset->ti = ti;
	}

	/* Register all child channels */
	for (i = 0; i < cset->n_chan; i++) {
		cset->chan[i].index = i;
		cset->chan[i].cset = cset;
		cset->chan[i].flags |= cset->flags & ZIO_DIR;
		err = chan_register(&cset->chan[i]);
		if (err)
			goto out_reg;
	}
	/* Private initialization function */
	if (cset->init) {
		err = cset->init(cset);
		if (err)
			goto out_reg;
	}

	spin_lock(&zstat->lock);
	list_add(&cset->list_cset, &zstat->list_cset);
	spin_unlock(&zstat->lock);

	return 0;

out_reg:
	for (j = i-1; j >= 0; j--)
		chan_unregister(&cset->chan[j]);
	__ti_unregister(trig, ti);
out_tr:
	__ti_destroy(trig, ti);
out_trig:
	zio_trigger_put(cset->trig);
	cset->trig = NULL;
	zio_buffer_put(cset->zbuf);
	cset->zbuf = NULL;
out_buf:
	cset_free_chan(cset);
out_remove_sys:
	device_unregister(&cset->head.dev);
out_dev_reg:
	zattr_set_remove(&cset->head);
out:
	__zio_minorbase_put(cset);
	return err;
}

static void cset_unregister(struct zio_cset *cset)
{
	int i;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!cset)
		return;
	/* Private exit function */
	if (cset->exit)
		cset->exit(cset);

	/* Remove from csets list*/
	spin_lock(&zstat->lock);
	list_del(&cset->list_cset);
	spin_unlock(&zstat->lock);
	/* destroy instance and decrement trigger usage */
	__ti_unregister(cset->trig, cset->ti);
	__ti_destroy(cset->trig,  cset->ti);
	zio_trigger_put(cset->trig);
	cset->trig = NULL;
	/* Unregister all child channels */
	for (i = 0; i < cset->n_chan; i++)
		chan_unregister(&cset->chan[i]);
	/* decrement buffer usage */
	zio_buffer_put(cset->zbuf);
	cset->zbuf = NULL;
	cset_free_chan(cset);
	device_unregister(&cset->head.dev);
	zattr_set_remove(&cset->head);
	/* Release a group of minors */
	__zio_minorbase_put(cset);
}

/*
 * Register a generic zio object. It can be a device, a buffer type or
 * a trigger type.
 */
static int zobj_register(struct zio_object_list *zlist,
			 struct zio_obj_head *head,
			 struct module *owner,
			 const char *name)
{
	int err;
	struct zio_object_list_item *item;

	if (strlen(name) > ZIO_OBJ_NAME_LEN)
		pr_warning("ZIO: name too long, cut to %d characters\n",
			   ZIO_OBJ_NAME_LEN);
	strncpy(head->name, name, ZIO_OBJ_NAME_LEN);

	/* Name must be unique */
	err = zobj_unique_name(zlist, head->name);
	if (err)
		goto out;

	/* Add to object list */
	item = kmalloc(sizeof(struct zio_object_list_item), GFP_KERNEL);
	if (!item) {
		err = -ENOMEM;
		goto out;
	}
	item->obj_head = head;
	item->owner = owner;
	strncpy(item->name, head->name, ZIO_OBJ_NAME_LEN);
	/* add to the object list*/
	spin_lock(&zstat->lock);
	list_add(&item->list, &zlist->list);
	spin_unlock(&zstat->lock);
	return 0;

out:
	return err;
}
static void zobj_unregister(struct zio_object_list *zlist,
		struct zio_obj_head *head)
{
	struct zio_object_list_item *item;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!head)
		return;
	list_for_each_entry(item, &zlist->list, list) {
		if (item->obj_head == head) {
			/* Remove from object list */
			spin_lock(&zstat->lock);
			list_del(&item->list);
			spin_unlock(&zstat->lock);
			kfree(item);
			break;
		}
	}
}

/* Register a zio device */
int zio_register_dev(struct zio_device *zdev, const char *name)
{
	int err = 0, i, j;

	if (!zdev->owner) {
		pr_err("%s: new device has no owner\n", __func__);
		return -EINVAL;
	}

	zdev->head.zobj_type = ZDEV;
	if (zdev->zattr_set.std_zattr)
		zdev->zattr_set.n_std_attr = ZATTR_STD_NUM_ZDEV;
	spin_lock_init(&zdev->lock);
	/* Create standard and extended sysfs attribute for device */
	err = zattr_set_create(&zdev->head, zdev->s_op);
	if (err)
		goto out;
	/* Register the device */
	err = zobj_register(&zstat->all_devices, &zdev->head, zdev->owner, name);
	if (err)
		goto out_reg;

	dev_set_name(&zdev->head.dev, zdev->head.name);
	zdev->head.dev.type = &zobj_device_type;
	zdev->head.dev.bus = &zio_bus_type;
	err = device_register(&zdev->head.dev);
	if (err)
		goto out_dev;
	/* Register all child channel sets */
	for (i = 0; i < zdev->n_cset; i++) {
		zdev->cset[i].index = i;
		zdev->cset[i].zdev = zdev;
		err = cset_register(&zdev->cset[i]);
		if (err)
			goto out_cset;
	}
	pr_debug("%s:%d\n", __func__, __LINE__);
	/* Fix extended attribute index */
	err = __zattr_dev_init_ctrl(zdev);
	if (err)
		goto out_cset;
	return 0;

out_cset:
	for (j = i-1; j >= 0; j--)
		cset_unregister(zdev->cset + j);
out_dev:
	zobj_unregister(&zstat->all_devices, &zdev->head);
out_reg:
	zattr_set_remove(&zdev->head);
out:
	return err;
}
EXPORT_SYMBOL(zio_register_dev);

void zio_unregister_dev(struct zio_device *zdev)
{
	int i;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!zdev)
		return;

	/* Unregister all child channel sets */
	for (i = 0; i < zdev->n_cset; i++)
		cset_unregister(&zdev->cset[i]);
	device_unregister(&zdev->head.dev);
	zobj_unregister(&zstat->all_devices, &zdev->head);
	zattr_set_remove(&zdev->head);
}
EXPORT_SYMBOL(zio_unregister_dev);

/* Register a buffer into the available buffer list */
int zio_register_buf(struct zio_buffer_type *zbuf, const char *name)
{
	int err;
	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!zbuf || !name)
		return -EINVAL;

	err = zio_init_buffer_fops(zbuf);
	if (err < 0)
		return err;

	zbuf->head.zobj_type = ZBUF;
	err = zobj_register(&zstat->all_buffer_types, &zbuf->head,
			    zbuf->owner, name);
	if (err) {
		zio_fini_buffer_fops(zbuf);
		return err;
	}
	if (zbuf->zattr_set.std_zattr)
		zbuf->zattr_set.n_std_attr = ZATTR_STD_NUM_ZBUF;
	INIT_LIST_HEAD(&zbuf->list);
	spin_lock_init(&zbuf->lock);

	return 0;
}
EXPORT_SYMBOL(zio_register_buf);

void zio_unregister_buf(struct zio_buffer_type *zbuf)
{
	if (!zbuf)
		return;
	zio_fini_buffer_fops(zbuf);
	zobj_unregister(&zstat->all_buffer_types, &zbuf->head);
}
EXPORT_SYMBOL(zio_unregister_buf);

/* Register a trigger into the available trigger list */
int zio_register_trig(struct zio_trigger_type *trig, const char *name)
{
	int err;

	if (!trig || !name)
		return -EINVAL;

	if (!trig->zattr_set.std_zattr)
		goto err_nsamp;
	if (!trig->zattr_set.std_zattr[ZATTR_TRIG_NSAMPLES].attr.attr.mode)
		goto err_nsamp;
	trig->head.zobj_type = ZTRIG;
	err = zobj_register(&zstat->all_trigger_types, &trig->head,
			    trig->owner, name);
	if (err)
		return err;
	if (trig->zattr_set.std_zattr)
		trig->zattr_set.n_std_attr = ZATTR_STD_NUM_TRIG;
	INIT_LIST_HEAD(&trig->list);
	spin_lock_init(&trig->lock);

	return 0;

err_nsamp:
	pr_err("%s: trigger \"%s\" lacks mandatory \"nsamples\" attribute",
	       __func__, name);
	return -EINVAL;
}
EXPORT_SYMBOL(zio_register_trig);

void zio_unregister_trig(struct zio_trigger_type *trig)
{
	if (!trig)
		return;
	zobj_unregister(&zstat->all_trigger_types, &trig->head);
}
EXPORT_SYMBOL(zio_unregister_trig);

static int __init zio_init(void)
{
	int err;

	/* Some compile-time checks, so developers are free to hack around */
	BUILD_BUG_ON_NOT_POWER_OF_2(ZIO_CHAN_MAXNUM);
	BUILD_BUG_ON_NOT_POWER_OF_2(ZIO_CSET_MAXNUM);
	BUILD_BUG_ON(ZIO_CSET_MAXNUM * ZIO_CHAN_MAXNUM * 2 > MINORMASK);
	BUILD_BUG_ON(ZATTR_STD_NUM_ZDEV != ARRAY_SIZE(zio_zdev_attr_names));
	BUILD_BUG_ON(ZATTR_STD_NUM_ZBUF != ARRAY_SIZE(zio_zbuf_attr_names));
	BUILD_BUG_ON(ZATTR_STD_NUM_TRIG != ARRAY_SIZE(zio_trig_attr_names));

	err = zio_slab_init();
	if (err)
		return err;
	/* Register ZIO bus */
	err = bus_register(&zio_bus_type);
	if (err)
		goto out;
	/* Initialize char device */
	err = __zio_register_cdev();
	if (err)
		goto out_cdev;

	INIT_LIST_HEAD(&zstat->all_devices.list);
	zstat->all_devices.zobj_type = ZDEV;
	INIT_LIST_HEAD(&zstat->all_trigger_types.list);
	zstat->all_trigger_types.zobj_type = ZTRIG;
	INIT_LIST_HEAD(&zstat->all_buffer_types.list);
	zstat->all_buffer_types.zobj_type = ZBUF;

	err = zio_default_buffer_init();
	if (err)
		pr_warn("%s: cannot register default buffer\n", __func__);
	err = zio_default_trigger_init();
	if (err)
		pr_warn("%s: cannot register default trigger\n", __func__);
	pr_info("zio-core had been loaded\n");
	return 0;

out_cdev:
	bus_unregister(&zio_bus_type);
out:
	zio_slab_exit();
	return err;
}

static void __exit zio_exit(void)
{
	zio_default_trigger_exit();
	zio_default_buffer_exit();

	/* Remove char device */
	__zio_unregister_cdev();
	/* Remove ZIO bus */
	bus_unregister(&zio_bus_type);
	zio_slab_exit();
	pr_info("zio-core had been unloaded\n");
	return;
}

subsys_initcall(zio_init);
module_exit(zio_exit);

MODULE_AUTHOR("Federico Vaga and Alessandro Rubini");
/* Federico wrote the core, Alessandro wrote default trigger and buffer */
MODULE_DESCRIPTION("ZIO - ZIO Input Output");
MODULE_LICENSE("GPL");
