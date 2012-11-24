/*
 * Copyright 2011 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * GNU GPLv2 or later
 */
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

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"


#define ZOBJ_SYSFS_NAME "name"
#define ZOBJ_SYSFS_ENABLE "enable"
#define CSET_SYSFS_BUFFER "current_buffer"
#define CSET_SYSFS_TRIGGER "current_trigger"

const char zio_zdev_attr_names[_ZIO_DEV_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_GAIN]		= "gain_factor",
	[ZIO_ATTR_OFFSET]		= "offset",
	[ZIO_ATTR_NBITS]		= "resolution-bits",
	[ZIO_ATTR_MAXRATE]		= "max-sample-rate",
	[ZIO_ATTR_VREFTYPE]	= "vref-src",
};
EXPORT_SYMBOL(zio_zdev_attr_names);
const char zio_trig_attr_names[_ZIO_TRG_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_TRIG_REENABLE]	= "re-enable",
	[ZIO_ATTR_TRIG_PRE_SAMP]	= "pre-samples",
	[ZIO_ATTR_TRIG_POST_SAMP]	= "post-samples",
};
EXPORT_SYMBOL(zio_trig_attr_names);
const char zio_zbuf_attr_names[_ZIO_BUF_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_ZBUF_MAXLEN]	= "max-buffer-len",
	[ZIO_ATTR_ZBUF_MAXKB]	= "max-buffer-kb",
};
EXPORT_SYMBOL(zio_zbuf_attr_names);

static struct zio_attribute *__zattr_clone(const struct zio_attribute *src,
		unsigned int n)
{
	struct zio_attribute *dest = NULL;
	unsigned int size;

	if (!src || !n)
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

int __zattr_set_copy(struct zio_attribute_set *dest,
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
void __zattr_set_free(struct zio_attribute_set *zattr_set)
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
	case ZIO_DEV:
		lock = &to_zio_dev(&head->dev)->lock;
		break;
	case ZIO_CSET:
		lock = &to_zio_cset(&head->dev)->zdev->lock;
		break;
	case ZIO_CHAN:
		lock = &to_zio_chan(&head->dev)->cset->zdev->lock;
		break;
	case ZIO_TI: /* we might not want to take a lock but... */
		lock = &to_zio_ti(&head->dev)->cset->zdev->lock;
		break;
	case ZIO_BI:
		lock = &to_zio_bi(&head->dev)->cset->zdev->lock;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return lock;
}


/*
 * used to init and update sysfs attribute value into a control.
 * The bit mask is set also during update to make the code simple, but
 * this does not decrease performance
 */
static inline void __zattr_valcpy(struct zio_ctrl_attr *ctrl,
				  struct zio_attribute *zattr)
{
	if ((zattr->flags & ZIO_ATTR_TYPE) == ZIO_ATTR_TYPE_EXT) {
		ctrl->ext_mask |= (1 << zattr->index);
		ctrl->ext_val[zattr->index] = zattr->value;
	} else {
		if (zattr->index == ZIO_ATTR_INDEX_NONE)
			return;
		ctrl->std_mask |= (1 << zattr->index);
		ctrl->std_val[zattr->index] = zattr->value;
	}
}

void __ctrl_update_nsamples(struct zio_ti *ti)
{
	ti->nsamples = ti->zattr_set.std_zattr[ZIO_ATTR_TRIG_PRE_SAMP].value +
		       ti->zattr_set.std_zattr[ZIO_ATTR_TRIG_POST_SAMP].value;
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

	if (!(zattr->flags & ZIO_ATTR_CONTROL))
		return; /* the attribute is not in the control */

	pr_debug("%s\n", __func__);
	switch (head->zobj_type) {
	case ZIO_DEV:
		zdev = to_zio_dev(&head->dev);
		for (i = 0; i < zdev->n_cset; ++i) {
			cset = &zdev->cset[i];
			for (j = 0; j < cset->n_chan; ++j) {
				ctrl = cset->chan[j].current_ctrl;
				__zattr_valcpy(&ctrl->attr_channel, zattr);
			}
		}
		break;
	case ZIO_CSET:
		cset = to_zio_cset(&head->dev);
		for (i = 0; i < cset->n_chan; ++i) {
			ctrl = cset->chan[i].current_ctrl;
			__zattr_valcpy(&ctrl->attr_channel, zattr);
		}
		break;
	case ZIO_CHAN:
		ctrl = to_zio_chan(&head->dev)->current_ctrl;
		__zattr_valcpy(&ctrl->attr_channel, zattr);
		break;
	case ZIO_TI:
		ti = to_zio_ti(&head->dev);
		__ctrl_update_nsamples(ti);
		/* Update attributes in all "current_ctrl" struct */
		for (i = 0; i < ti->cset->n_chan; ++i) {
			chan = &ti->cset->chan[i];
			ctrl = chan->current_ctrl;
			__zattr_valcpy(&ctrl->attr_trigger, zattr);
			if ((zattr->flags & ZIO_ATTR_TYPE) == ZIO_ATTR_TYPE_EXT)
				continue; /* continue to the next channel */
		}
		break;
	default:
		return;
	}
}

void __zattr_trig_init_ctrl(struct zio_ti *ti, struct zio_control *ctrl)
{
	int i;
	struct zio_ctrl_attr *ctrl_attr_trig = &ctrl->attr_trigger;

	strncpy(ctrl->triggername, ti->cset->trig->head.name, ZIO_OBJ_NAME_LEN);
	ctrl_attr_trig->std_mask = 0;
	ctrl_attr_trig->ext_mask = 0;
	/* Copy trigger value */
	for (i = 0; i < ti->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_trig, &ti->zattr_set.std_zattr[i]);
	for (i = 0; i < ti->zattr_set.n_ext_attr; ++i)
		if (ti->zattr_set.ext_zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_trig,
				       &ti->zattr_set.ext_zattr[i]);
}
static int __zattr_chan_init_ctrl(struct zio_channel *chan, unsigned int start)
{
	struct zio_ctrl_attr *ctrl_attr_chan;
	struct zio_attribute *zattr;
	struct zio_control *ctrl;
	struct zio_device *zdev;
	struct zio_cset *cset;
	int i;

	cset = chan->cset;
	zdev = cset->zdev;
	ctrl = chan->current_ctrl;
	ctrl->addr.dev_id = chan->cset->zdev->dev_id;
	ctrl_attr_chan = &chan->current_ctrl->attr_channel;
	if (!(start + chan->zattr_set.n_ext_attr < 32)) {
		dev_err(&zdev->head.dev, "too many extended attribute");
		return -EINVAL;
	}

	__zattr_trig_init_ctrl(cset->ti, chan->current_ctrl);
	/* Copy standard attributes into the control */
	for (i = 0; i < chan->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &chan->zattr_set.std_zattr[i]);
	for (i = 0; i < cset->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &cset->zattr_set.std_zattr[i]);
	for (i = 0; i < zdev->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &zdev->zattr_set.std_zattr[i]);

	/* Fix and copy attributes within channel */
	zattr = chan->zattr_set.ext_zattr;
	for (i = 0; i < chan->zattr_set.n_ext_attr; ++i) {
		if (zattr[i].flags & ZIO_ATTR_CONTROL) {
			/* Fix channel extended attribute index */
			zattr[i].index = start + i;
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
		} else {
			zattr[i].index = ZIO_ATTR_INDEX_NONE;
		}
	}
	/* Copying attributes from cset */
	zattr = cset->zattr_set.ext_zattr;
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
	/* Copying attributes from zdev */
	zattr = zdev->zattr_set.ext_zattr;
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
	return 0;
}
static int __zattr_cset_init_ctrl(struct zio_cset *cset, unsigned int start)
{
	struct zio_attribute *zattr;
	int i, err, start_c = start;

	/* Fix cset extended attribute index */
	zattr = cset->zattr_set.ext_zattr;
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			zattr[i].index = start_c++;
		else
			zattr[i].index = ZIO_ATTR_INDEX_NONE;

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
int __zattr_dev_init_ctrl(struct zio_device *zdev)
{
	struct zio_attribute *zattr;
	int i, err, start = 0;

	pr_debug("%s\n", __func__);
	/* Device level */
	/* Fix device extended attribute index */
	zattr = zdev->zattr_set.ext_zattr;
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			zattr[i].index = start++;
		else
			zattr[i].index = ZIO_ATTR_INDEX_NONE;

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

	flags = zio_get_from_obj(to_zio_head(dev), flags);
	status = !((*flags) & ZIO_STATUS);
	/* if the status is not changing */
	if (!(enable ^ status))
		return;
	/* change status */
	*flags = (*flags & (~ZIO_STATUS)) | status;
	switch (head->zobj_type) {
	case ZIO_DEV:
		pr_debug("%s: zdev\n", __func__);

		zdev = to_zio_dev(dev);
		/* enable/disable all cset */
		for (i = 0; i < zdev->n_cset; ++i)
			__zobj_enable(&zdev->cset[i].head.dev, enable);
		/* device callback */
		break;
	case ZIO_CSET:
		pr_debug("%s: zcset\n", __func__);

		cset = to_zio_cset(dev);
		/* enable/disable trigger instance */
		__zobj_enable(&cset->ti->head.dev, enable);
		/* enable/disable all channel*/
		for (i = 0; i < cset->n_chan; ++i)
			__zobj_enable(&cset->chan[i].head.dev, enable);
		/* cset callback */
		break;
	case ZIO_CHAN:
		pr_debug("%s: zchan\n", __func__);
		/* channel callback */
		break;
	case ZIO_TI:
		pr_debug("%s: zti\n", __func__);

		ti = to_zio_ti(dev);
		zio_trigger_abort(ti->cset);
		/* trigger instance callback */
		if (ti->t_op->change_status)
			ti->t_op->change_status(ti, status);
		break;
	/* following objects can't be enabled/disabled */
	case ZIO_BUF:
	case ZIO_TRG:
	case ZIO_BI:
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

	flags = zio_get_from_obj(to_zio_head(dev), flags);
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

/*
 * zobj_read_cur_ctrl
 * it returns the current control to userspace through binary sysfs file
 */
ssize_t zobj_read_cur_ctrl(struct file *file,struct kobject *kobj,
			   struct bin_attribute *bin_attr,
			   char *buf, loff_t off, size_t count)
{
	struct zio_channel *chan;

	if (off >= bin_attr->size)
		return 0;

	/* This file must be read entirely */
	if (off != 0)
		return -ESPIPE; /* Illegal seek */
	if (count < bin_attr->size)
		return -EINVAL;
	if (count > bin_attr->size)
		count = bin_attr->size;

	chan = to_zio_chan(container_of(kobj, struct device, kobj));
	memcpy(buf, chan->current_ctrl, count); /* FIXME: lock */

	return count;
}

struct bin_attribute zio_attr_cur_ctrl = {
	.attr = { .name = "current-control", .mode = 0444, },
	.size = ZIO_CONTROL_SIZE, /* Will be modified for TLV support */
	.read = zobj_read_cur_ctrl,
};

/* default zio attributes */
static struct device_attribute zio_default_attributes[] = {
	__ATTR(name, 0444, zobj_show_name, NULL),
	__ATTR(enable, 0666, zobj_show_enable, zobj_store_enable),
	__ATTR(current_trigger, 0666, zobj_show_cur_trig, zobj_store_cur_trig),
	__ATTR(current_buffer, 0666, zobj_show_cur_zbuf, zobj_store_cur_zbuf),
	__ATTR_NULL,
};
/* default attributes for most of the zio object */
static struct attribute *def_device_attrs_ptr[] = {
	&zio_default_attributes[0].attr,	/* name */
	&zio_default_attributes[1].attr,	/* enable */
	NULL,
};
/* default attributes for channel set */
static struct attribute *def_cset_attrs_ptr[] = {
	&zio_default_attributes[2].attr,	/* current_trigger */
	&zio_default_attributes[3].attr,	/* current_buffer */
	NULL,
};
/* default attributes for buffer instance*/
static struct attribute *def_bi_attrs_ptr[] = {
	&zio_default_attributes[0].attr,	/* name */
	NULL,
};
/* default zio groups */
static const struct attribute_group zio_groups[] = {
	{	/* group for all zio object*/
		.attrs = def_device_attrs_ptr,
	},
	{	/* cset only group */
		.attrs = def_cset_attrs_ptr,
	},
	{	/* bi only group */
		.attrs = def_bi_attrs_ptr,
	},
};
/* default groups for most of the zio object */
static const struct attribute_group *def_device_groups_ptr[] = {
	&zio_groups[0],	/* group for all zio object*/
	NULL,
};
/* default groups for channel set */
static const struct attribute_group *def_cset_groups_ptr[] = {
	&zio_groups[0],	/* group for all zio object*/
	&zio_groups[1],	/* cset only group */
	NULL,
};
static const struct attribute_group *def_bi_groups_ptr[] = {
	&zio_groups[2],	/* bi only group */
	NULL,
};

/* Device types */
void zio_device_release(struct device *dev)
{
	pr_debug("RELEASE %s\n", dev_name(dev));
	return;
}
void __zio_generic_device_release(struct device *dev)
{
	pr_debug("RELEASE %s\n", dev_name(dev));
	return;
}
struct device_type zdev_generic_type = {
	.name = "zio generic device type",
	.release = __zio_generic_device_release,
};
struct device_type zobj_device_type = {
	.name = "zio_obj_type",
	.release = zio_device_release,
	.groups = def_device_groups_ptr,
};
struct device_type cset_device_type = {
	.name = "zio_cset_type",
	.release = zio_device_release,
	.groups = def_cset_groups_ptr,
};
struct device_type bi_device_type = {
	.name = "zio_bi_type",
	.release = zio_device_release,
	.groups = def_bi_groups_ptr,
};

int __check_dev_zattr(struct zio_attribute_set *parent,
		      struct zio_attribute_set *this)
{
	int i, j;

	pr_debug("%s %d\n", __func__, this->n_std_attr);
	/* verify standard attribute */
	for (i = 0; i < this->n_std_attr; ++i) {
		if (this->std_zattr[i].index == ZIO_ATTR_INDEX_NONE)
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
int zattr_set_create(struct zio_obj_head *head,
		     const struct zio_sysfs_operations *s_op)
{
	int i, err, a_count, g_count = 0, g = 0;
	const struct attribute_group **groups;
	struct zio_attribute_set *zattr_set;
	struct attribute *attr;

	pr_debug("%s\n", __func__);
	zattr_set = zio_get_from_obj(head, zattr_set);
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

	if (!g_count)
		goto out;

	/* Allocate needed groups. dev->groups is null ended */
	groups = kzalloc(sizeof(struct attribute_group *) * (g_count + 1),
			 GFP_KERNEL);
	if (!groups)
		return -ENOMEM;

	/* Allocate standard attribute group */
	if (!zattr_set->std_zattr || !zattr_set->n_std_attr)
		goto ext;
	groups[g] = __allocate_group(zattr_set->n_std_attr);
	if (IS_ERR(groups[g]))
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
			zattr_set->std_zattr[i].index = ZIO_ATTR_INDEX_NONE;
			break;
		default:
			return err;
		}
	}
	++g;
ext:
	/* Allocate extended attribute group */
	if (!zattr_set->ext_zattr || !zattr_set->n_ext_attr)
		goto out_assign;
	groups[g] = __allocate_group(zattr_set->n_ext_attr);
	if (IS_ERR(groups[g]))
		return PTR_ERR(groups[g]);
	for (i = 0, a_count = 0; i < zattr_set->n_ext_attr; ++i) {
		attr = &zattr_set->ext_zattr[i].attr.attr;
		err = __check_attr(attr, s_op);
		if (err)
			return err;
		/* valid attribute */
		groups[g]->attrs[a_count++] = attr;
		zattr_set->ext_zattr[i].attr.show = zattr_show;
		zattr_set->ext_zattr[i].attr.store = zattr_store;
		zattr_set->ext_zattr[i].s_op = s_op;
		zattr_set->ext_zattr[i].index = i;
		zattr_set->ext_zattr[i].flags |= ZIO_ATTR_TYPE_EXT;
	}
	++g;

out_assign:
	groups[g] = NULL;
	head->dev.groups = groups;
out:
	return 0;
}
/* Remove an existent set of attributes */
void zattr_set_remove(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;
	int i;

	zattr_set = zio_get_from_obj(head, zattr_set);
	if (!zattr_set)
		return;
	if (!head->dev.groups)
		return;
	for (i = 0; head->dev.groups[i]; ++i) {
		kfree(head->dev.groups[i]->attrs);
		kfree(head->dev.groups[i]);
	}
}
