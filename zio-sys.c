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
	[ZATTR_NBIT]		= "resolution-bits",
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
	ti->flags &= (~ZTI_STATUS);
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
		memcpy(ctrl, ti->current_ctrl, ZIO_CONTROL_SIZE);
		ctrl->chan_i = chan->index;

		block = zbuf->b_op->alloc_block(chan->bi, ctrl,
						ctrl->ssize * ctrl->nsamples,
						GFP_ATOMIC);
		if (IS_ERR(block)) {
			if (!errdone++)
				pr_err("%s: can't alloc block\n", __func__);
			zio_free_control(ctrl);
			continue;
		}
		chan->active_block = block;
	}
	if (!zdev->d_op->input_cset(cset)) {
		/* It succeeded immediately */
		ti->t_op->data_done(cset);
	}
}

static void __zio_fire_output_trigger(struct zio_ti *ti)
{
	struct zio_cset *cset = ti->cset;
	struct zio_device *zdev = cset->zdev;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* We are expected to already have a block in active channels */
	if (!zdev->d_op->output_cset(cset)) {
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
	/* chek if trigger is disabled */
	if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED))
		return;
	/* check if previouvs fire is still running*/
	if ((ti->flags & ZTI_STATUS) == ZTI_STATUS_ON)
		return;
	ti->flags |= ZTI_STATUS_ON;
	/* Copy the stamp (we are software driven anyways) */
	ti->current_ctrl->tstamp.secs = ti->tstamp.tv_sec;
	ti->current_ctrl->tstamp.ticks = ti->tstamp.tv_nsec;
	ti->current_ctrl->tstamp.bins = ti->tstamp_extra;
	/*
	 * And the sequence number too (first returned seq is 1).
	 * Sequence number is always increased to identify un-stored
	 * blocks or other errors in trigger activation.
	 */
	ti->current_ctrl->seq_num++;

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
		lock = &to_zio_dev(&head->kobj)->lock;
		break;
	case ZCSET:
		lock = &to_zio_cset(&head->kobj)->zdev->lock;
		break;
	case ZCHAN:
		lock = &to_zio_chan(&head->kobj)->cset->zdev->lock;
		break;
	case ZTI: /* we might not want to take a lock but... */
		lock = &to_zio_ti(&head->kobj)->lock;
		break;
	case ZBI:
		lock = &to_zio_bi(&head->kobj)->lock;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return lock;
}

/* Retrieve an attribute set from an object head */
static struct zio_attribute_set *__get_zattr_set(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;

	switch (head->zobj_type) {
	case ZDEV:
		zattr_set = &to_zio_dev(&head->kobj)->zattr_set;
		break;
	case ZCSET:
		zattr_set = &to_zio_cset(&head->kobj)->zattr_set;
		break;
	case ZCHAN:
		zattr_set = &to_zio_chan(&head->kobj)->zattr_set;
		break;
	case ZTRIG:
		zattr_set = &to_zio_trig(&head->kobj)->zattr_set;
		break;
	case ZBUF:
		zattr_set = &to_zio_buf(&head->kobj)->zattr_set;
		break;
	case ZTI:
		zattr_set = &to_zio_ti(&head->kobj)->zattr_set;
		break;
	case ZBI:
		zattr_set = &to_zio_bi(&head->kobj)->zattr_set;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return zattr_set;
}
/* Retrieve flag from an object head */
static unsigned long *__get_flag(struct zio_obj_head *head)
{
	unsigned long *flags;

	switch (head->zobj_type) {
	case ZDEV:
		flags = &to_zio_dev(&head->kobj)->flags;
		break;
	case ZCSET:
		flags = &to_zio_cset(&head->kobj)->flags;
		break;
	case ZCHAN:
		flags = &to_zio_chan(&head->kobj)->flags;
		break;
	case ZTRIG:
		flags = &to_zio_chan(&head->kobj)->flags;
		break;
	case ZBUF:
		flags = &to_zio_chan(&head->kobj)->flags;
		break;
	case ZTI:
		flags = &to_zio_ti(&head->kobj)->flags;
		break;
	case ZBI:
		flags = &to_zio_bi(&head->kobj)->flags;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return flags;
}

static int zio_change_current_trigger(struct zio_cset *cset, char *name)
{
	struct zio_trigger_type *trig, *trig_old = cset->trig;
	struct zio_ti *ti, *ti_old = cset->ti;
	int err;

	/* FIXME get all necessary spinlock */
	pr_debug("%s\n", __func__);
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
	/* Rename trigger-tmp to trigger */
	err = kobject_rename(&ti->head.kobj, "trigger");
	if (err)
		goto out_rename;
	return 0;

out_rename:
	__ti_unregister(trig, ti);
out_reg:
	__ti_destroy(trig, ti);
out:
	zio_trigger_put(trig);
	return err;
}

/* FIXME test this function when a new kind of buffer is available */
static int zio_change_current_buffer(struct zio_cset *cset, char *name)
{
	struct zio_buffer_type *zbuf;
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

	for (i = 0; i < cset->n_chan; ++i) {
		bi_vector[i] = __bi_create_and_init(zbuf, &cset->chan[i]);
		if (IS_ERR(bi_vector[i])) {
			pr_err("%s can't create buffer instance\n", __func__);
			err = PTR_ERR(bi_vector[i]);
			goto out_create;
		}
	}

	for (i = 0; i < cset->n_chan; ++i) {
		/* delete old buffer instance */
		__bi_unregister(cset->zbuf, cset->chan[i].bi);
		__bi_destroy(cset->zbuf, cset->chan[i].bi);
		/* register new buffer instance */
		__bi_register(zbuf, &cset->chan[i], bi_vector[i], "buffer-tmp");
		cset->chan[i].bi = bi_vector[i];
	}
	kfree(bi_vector);
	cset->zbuf = zbuf;

	return 0;
out_create:
	for (j = i-1; j >= 0; --j)
		__bi_destroy(zbuf, bi_vector[j]);
	kfree(bi_vector);
out:
	zio_buffer_put(zbuf);
	return err;
}

static inline void __zattr_valcpy(struct zio_ctrl_attr *ctrl,
				  enum zattr_flags flags,
				  int index, uint32_t value)
{
	pr_debug("%s\n", __func__);
	if ((flags & ZATTR_TYPE) == ZATTR_TYPE_EXT)
		ctrl->ext_val[index] = value;
	else
		ctrl->std_val[index] = value;
}
static void __zattr_propagate_value(struct zio_obj_head *head,
			       struct zio_attribute *zattr)
{
	int i, j, index, value, flags;
	struct zio_ti *ti;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_channel *chan;

	pr_debug("%s\n", __func__);
	index = zattr->index;
	value = zattr->value;
	flags = zattr->flags;
	switch (head->zobj_type) {
	case ZDEV:
		zdev = to_zio_dev(&head->kobj);
		for (i = 0; i < zdev->n_cset; ++i) {
			cset = &zdev->cset[i];
			for (j = 0; j < cset->n_chan; ++j)
				__zattr_valcpy(&cset->chan[j].zattr_val,
						   flags, index, value);
		}
		break;
	case ZCSET:
		cset = to_zio_cset(&head->kobj);
		for (i = 0; i < cset->n_chan; ++i)
			__zattr_valcpy(&cset->chan[i].zattr_val,
					   flags, index, value);
		break;
	case ZCHAN:
		chan = to_zio_chan(&head->kobj);
		__zattr_valcpy(&chan->zattr_val, flags, index, value);
		break;
	case ZTI:
		ti = to_zio_ti(&head->kobj);
		__zattr_valcpy(&ti->zattr_val, flags, index, value);
		break;
	default:
		return;
	}
}

static void __zattr_init_ctrl(struct zio_obj_head *head,
			      struct zio_attribute_set *zattr_set)
{
	int i;

	pr_debug("%s\n", __func__);
	/* copy standard attribute default value */
	for (i = 0; i < zattr_set->n_std_attr; ++i)
		__zattr_propagate_value(head, &zattr_set->std_zattr[i]);
	/* copy extended attribute default value */
	for (i = 0; i < zattr_set->n_ext_attr; ++i)
		__zattr_propagate_value(head, &zattr_set->ext_zattr[i]);
}

 /*
 * Zio objects all handle uint32_t values. So the show and store
 * are centralized here, and each device has its own get_info and set_conf
 * which handle binary 32-bit numbers. Both the function are locked to prevent
 * concurrency issue when editing device register.
 */
static ssize_t zattr_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	int err = 0;
	ssize_t len = 0;
	spinlock_t *lock;
	struct zio_attribute *zattr = to_zio_zattr(attr);

	pr_debug("%s\n", __func__);
	/* print device name */
	if (unlikely(strcmp(attr->name, ZOBJ_SYSFS_NAME) == 0))
		return sprintf(buf, "%s\n", to_zio_head(kobj)->name);
	/* print current trigger name */
	if (unlikely(strcmp(attr->name, CSET_SYSFS_TRIGGER) == 0))
		return sprintf(buf, "%s\n", to_zio_cset(kobj)->trig->head.name);
	/* print current buffer name */
	if (unlikely(strcmp(attr->name, CSET_SYSFS_BUFFER) == 0))
		return sprintf(buf, "%s\n", to_zio_cset(kobj)->zbuf->head.name);
	/* print current enable status */
	if (unlikely(strcmp(attr->name, ZOBJ_SYSFS_ENABLE) == 0))
		return sprintf(buf, "%d\n",
			  !((*__get_flag(to_zio_head(kobj))) & ZIO_DISABLED));

	if (zattr->s_op->info_get) {
		lock = __get_spinlock(to_zio_head(kobj));
		spin_lock(lock);
		err = zattr->s_op->info_get(kobj, zattr, &zattr->value);
		spin_unlock(lock);
		if (err)
			return err;
	}
	len = sprintf(buf, "%i\n", zattr->value);
	return len;
}

/* enable/disable a zio object*/
static void __zobj_enable(struct kobject *kobj, unsigned int enable,
			  unsigned int need_lock)
{
	unsigned long *flags;
	int i, status;
	struct zio_obj_head *head;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_ti *ti;
	spinlock_t *lock;

	pr_debug("%s\n", __func__);
	head = to_zio_head(kobj);

	/* lock object if needed */
	lock = __get_spinlock(head);
	if (need_lock)
		spin_lock(lock);

	flags = __get_flag(to_zio_head(kobj));
	status = !((*flags) & ZIO_STATUS);
	/* if the status is not changing */
	if (!(enable ^ status)) {
		goto out;
	}
	/* change status */
	*flags = (*flags | ZIO_STATUS) & status;
	switch (head->zobj_type) {
	case ZDEV:
		pr_debug("%s: zdev\n", __func__);

		zdev = to_zio_dev(kobj);
		/* enable/disable all cset */
		for (i = 0; i < zdev->n_cset; ++i) {
			__zobj_enable(&zdev->cset[i].head.kobj, enable, 0);
		}
		/* device callback */
		break;
	case ZCSET:
		pr_debug("%s: zcset\n", __func__);

		cset = to_zio_cset(kobj);
		/* enable/disable trigger instance */
		__zobj_enable(&cset->ti->head.kobj, enable, 1);
		/* enable/disable all channel*/
		for (i = 0; i < cset->n_chan; ++i) {
			__zobj_enable(&cset->chan[i].head.kobj, enable, 0);
		}
		/* cset callback */
		break;
	case ZCHAN:
		pr_debug("%s: zchan\n", __func__);
		/* channel callback */
		break;
	case ZTI:
		pr_debug("%s: zti\n", __func__);

		ti = to_zio_ti(kobj);
		/* if trigger is running, abort it*/
		if ((*flags & ZTI_STATUS) == ZTI_STATUS_ON)
			if(ti->t_op->abort)
				ti->t_op->abort(ti->cset);
		/* trigger instance callback */
		if (ti->t_op->change_status) {
			pr_debug("%s:%d\n", __func__, __LINE__);
			ti->t_op->change_status(ti, status);
		}
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
out:
	if (need_lock)
		spin_unlock(lock);
}

static ssize_t zattr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t size)
{
	long val;
	int err = 0;
	char buf_tmp[ZIO_OBJ_NAME_LEN];
	struct zio_attribute *zattr = to_zio_zattr(attr);
	spinlock_t *lock;

	pr_debug("%s\n", __func__);
	if (unlikely(strcmp(attr->name, CSET_SYSFS_TRIGGER) == 0)) {
		if (strlen(buf) > ZIO_OBJ_NAME_LEN + 1)
			return -EINVAL; /* name too long */
		sscanf(buf, "%s\n", buf_tmp);
		err = zio_change_current_trigger(to_zio_cset(kobj), buf_tmp);
		return err == 0 ? size : err;
	}
	/* change current buffer */
	if (unlikely(strcmp(attr->name, CSET_SYSFS_BUFFER) == 0)) {
		if (strlen(buf) > ZIO_OBJ_NAME_LEN + 1)
			return -EINVAL; /* name too long */
		sscanf(buf, "%s\n", buf_tmp);
		err = zio_change_current_buffer(to_zio_cset(kobj), buf_tmp);
		return err == 0 ? size : err;
	}

	err = strict_strtol(buf, 0, &val);
	if (err)
		return -EINVAL;

	/* change enable status */
	if (unlikely(strcmp(attr->name, ZOBJ_SYSFS_ENABLE) == 0 &&
	    (val == 0 || val == 1))) {
		__zobj_enable(kobj, val, 1);
		return size;
	}

	/* device attributes */
	if (zattr->s_op->conf_set) {
		lock = __get_spinlock(to_zio_head(kobj));
		spin_lock(lock);
		err = zattr->s_op->conf_set(kobj, zattr, (uint32_t)val);
		if (err) {
			spin_unlock(lock);
			return err;
		}
		zattr->value = (uint32_t)val;
		__zattr_propagate_value(to_zio_head(kobj), zattr);
		spin_unlock(lock);
	}
	return size;
}

static const struct sysfs_ops zio_attribute_ktype_ops = {
	.show  = zattr_show,
	.store = zattr_store,
};
static struct attribute default_cset_attrs[] = {
		{	/* show the name */
			.name = ZOBJ_SYSFS_NAME,
			.mode = 0444, /* read only */
		},
		{
			/* enable/disable object */
			.name = ZOBJ_SYSFS_ENABLE,
			.mode = 0666, /* read write */
		},
		{	/* get/set trigger */
			.name = CSET_SYSFS_TRIGGER,
			.mode = 0666, /* read write */
		},
		{	/* get/set buffer */
			.name = CSET_SYSFS_BUFFER,
			.mode = 0666, /* read write */
		},
};
static struct attribute *def_cset_attr_ptr[] = {
	&default_cset_attrs[0],
	&default_cset_attrs[1],
	&default_cset_attrs[2],
	&default_cset_attrs[3],
	NULL,
};
static struct kobj_type zdkctype = { /* only for cset */
	.release   = NULL,
	.sysfs_ops = &zio_attribute_ktype_ops,
	.default_attrs = def_cset_attr_ptr,
};
static struct attribute default_attrs[] = {
		{	/* show the name */
			.name = ZOBJ_SYSFS_NAME,
			.mode = 0444, /* read only */
		},
		{	/* enable/disable object */
			.name = ZOBJ_SYSFS_ENABLE,
			.mode = 0666, /* read write */
		},
};
static struct attribute *def_attr_ptr[] = {
	&default_attrs[0],
	&default_attrs[1],
	NULL,
};

static struct kobj_type zdktype = { /* for all the other object */
	.release   = NULL,
	.sysfs_ops = &zio_attribute_ktype_ops,
	.default_attrs = def_attr_ptr,
};


static int __check_dev_zattr(struct zio_attribute_set *parent,
			     struct zio_attribute_set *this)
{
	int i, j;

	pr_debug("%s\n", __func__);
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
				       this->std_zattr[i].attr.name);
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

/* create a set of zio attributes: the standard one and the extended one */
static int zattr_set_create(struct zio_obj_head *head,
			    const struct zio_sysfs_operations *s_op)
{
	int n_attr, i, j, attr_count = 0, err;
	struct zio_attribute_set *zattr_set;
	struct attribute_group *group;

	pr_debug("%s\n", __func__);
	zattr_set = __get_zattr_set(head);
	if (!zattr_set)
		return -EINVAL; /* message already printed */

	group = &zattr_set->group;
	n_attr = (zattr_set->n_std_attr + zattr_set->n_ext_attr);
	if (!n_attr || (!zattr_set->std_zattr && !zattr_set->ext_zattr)) {
		zattr_set->n_std_attr = 0;
		zattr_set->n_ext_attr = 0;
		return 0;
	}
	group->attrs = kzalloc(sizeof(struct attribute) * n_attr, GFP_KERNEL);
	if (!group->attrs)
		return -ENOMEM;

	if (!zattr_set->std_zattr)
		goto ext;
	/* standard attribute */
	for (i = 0; i < zattr_set->n_std_attr; ++i) {
		err = __check_attr(&zattr_set->std_zattr[i].attr, s_op);
		switch (err) {
		case 0:
			/* valid attribute */
			group->attrs[attr_count++] =
						&zattr_set->std_zattr[i].attr;
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
ext:
	if (!zattr_set->ext_zattr)
		goto out;
	/* extended attribute */
	for (j = 0; j < zattr_set->n_ext_attr; ++j) {
		err = __check_attr(&zattr_set->ext_zattr[j].attr, s_op);
		if (err)
			return err;

		/* valid attribute */
		group->attrs[attr_count++] = &zattr_set->ext_zattr[j].attr;
		zattr_set->ext_zattr[j].s_op = s_op;
		zattr_set->ext_zattr[j].index = j;
		zattr_set->ext_zattr[j].flags |= ZATTR_TYPE_EXT;
	}
out:
	return sysfs_create_group(&head->kobj, group);
}
/* Remove an existent set of attributes */
static void zattr_set_remove(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;

	zattr_set = __get_zattr_set(head);
	if (!zattr_set)
		return;
	if (!zattr_set->group.attrs)
		return;
	/* remove all standard and extended attributes */
	sysfs_remove_group(&head->kobj, &zattr_set->group);
	kfree(zattr_set->group.attrs);
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
	bi->flags |= (chan->flags & ZIO_DIR);
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
}
static int __bi_register(struct zio_buffer_type *zbuf,
			 struct zio_channel *chan,
			 struct zio_bi *bi, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	/* register whitin sysfs */
	err = kobject_init_and_add(&bi->head.kobj, &zdktype, &chan->head.kobj,
				   name);
	if (err)
		goto out_kobj;
	err = zattr_set_create(&bi->head, zbuf->s_op);
	if (err)
		goto out_sysfs;
	/* Add to buffer instance list */
	spin_lock(&zbuf->lock);
	list_add(&bi->list, &zbuf->list);
	spin_unlock(&zbuf->lock);
	bi->cset = chan->cset;
	chan->bi = bi;
	/* Done. This chan->bi marks everything is running (FIXME: audit) */
	mb();
	bi->chan = chan;

	return 0;

out_sysfs:
	__zattr_set_free(&bi->zattr_set);
	kobject_del(&bi->head.kobj);
out_kobj:
	kobject_put(&bi->head.kobj);
	return err;
}
static void __bi_unregister(struct zio_buffer_type *zbuf, struct zio_bi *bi)
{
	pr_debug("%s\n", __func__);
	/* Remove from buffer instance list */
	spin_lock(&zbuf->lock);
	list_del(&bi->list);
	spin_unlock(&zbuf->lock);
	/* Remove from sysfs */
	zattr_set_remove(&bi->head);
	__zattr_set_free(&bi->zattr_set);
	kobject_del(&bi->head.kobj);
	kobject_put(&bi->head.kobj);
}

/* create and initialize a new trigger instance */
static struct zio_ti *__ti_create_and_init(struct zio_trigger_type *trig,
					   struct zio_cset *cset)
{
	int err;
	struct zio_control *ctrl;
	struct zio_ti *ti;

	pr_debug("%s\n", __func__);
	/* Allocate and fill current control as much as possible*/
	ctrl = zio_alloc_control(GFP_KERNEL);
	if (!ctrl)
		return ERR_PTR(-ENOMEM);
	ctrl->cset_i = cset->index;
	strncpy(ctrl->devname, cset->zdev->head.name, ZIO_NAME_LEN);
	strncpy(ctrl->triggername, trig->head.name, ZIO_NAME_LEN);
	ctrl->sbits = 8; /* FIXME: retrieve from attribute */
	ctrl->ssize = cset->ssize;
	/* Create trigger */
	ti = trig->t_op->create(trig, cset, ctrl, 0/*FIXME*/);
	if (IS_ERR(ti)) {
		zio_free_control(ctrl);
		pr_err("ZIO %s: can't create trigger, error %ld\n",
		       __func__, PTR_ERR(ti));
		goto out;
	}
	/* Initialize trigger */
	ti->t_op = trig->t_op;
	ti->f_op = trig->f_op;
	ti->flags |= cset->flags & ZIO_DIR;
	ti->head.zobj_type = ZTI;
	snprintf(ti->head.name, ZIO_NAME_LEN, "%s-%s-%d",
		 trig->head.name, cset->zdev->head.name, cset->index);
	/* Copy sysfs attribute from trigger type */
	err = __zattr_set_copy(&ti->zattr_set, &trig->zattr_set);
	if (err) {
		zio_free_control(ctrl);
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
	zio_free_control(ti->current_ctrl);
}
static int __ti_register(struct zio_trigger_type *trig, struct zio_cset *cset,
			 struct zio_ti *ti, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	/* register whitin sysfs */
	err = kobject_init_and_add(&ti->head.kobj, &zdktype, &cset->head.kobj,
				   name);
	if (err)
		goto out_kobj;
	err = zattr_set_create(&ti->head, trig->s_op);
	if (err)
		goto out_sysfs;
	/* Add to trigger instance list */
	spin_lock(&trig->lock);
	list_add(&ti->list, &trig->list);
	spin_unlock(&trig->lock);
	cset->ti = ti;
	/* Done. This cset->ti marks everything is running (FIXME: audit) */
	mb();
	ti->cset = cset;

	return 0;

out_sysfs:
	__zattr_set_free(&ti->zattr_set);
	kobject_del(&ti->head.kobj);
out_kobj:
	kobject_put(&ti->head.kobj);
	return err;
}
static void __ti_unregister(struct zio_trigger_type *trig, struct zio_ti *ti)
{
	pr_debug("%s\n", __func__);
	/* Remove from trigger instance list */
	spin_lock(&trig->lock);
	list_del(&ti->list);
	spin_unlock(&trig->lock);
	/* Remove from sysfs */
	zattr_set_remove(&ti->head);
	__zattr_set_free(&ti->zattr_set);
	kobject_del(&ti->head.kobj);
	kobject_put(&ti->head.kobj);
}

/*
 * chan_register registers one channel.  It is important to register
 * or unregister all the channels of a cset at the same time to prevent
 * overlaps in the minors.
 */
static int chan_register(struct zio_channel *chan)
{
	struct zio_bi *bi;
	int err;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!chan)
		return -EINVAL;

	chan->head.zobj_type = ZCHAN;
	err = kobject_init_and_add(&chan->head.kobj, &zdktype,
			&chan->cset->head.kobj, "chan%i", chan->index);
	if (err)
		goto out_add;

	/* Create sysfs channel attributes */
	chan->zattr_set.n_std_attr = ZATTR_STD_NUM_ZDEV;
	err = zattr_set_create(&chan->head, chan->cset->zdev->s_op);
	if (err)
		goto out_sysfs;
	/* Check attribute hierarchy */
	err = __check_dev_zattr(&chan->cset->zattr_set, &chan->zattr_set);
	if (err)
		goto out_remove_sys;
	err = __check_dev_zattr(&chan->cset->zdev->zattr_set, &chan->zattr_set);
	if (err)
		goto out_remove_sys;
	/* copy default attribute value to ctrl */
	__zattr_init_ctrl(&chan->head, &chan->zattr_set);
	/* Create buffer */
	bi = __bi_create_and_init(chan->cset->zbuf, chan);
	if (IS_ERR(bi)) {
		err = PTR_ERR(bi);
		goto out_remove_sys;
	}
	err = __bi_register(chan->cset->zbuf, chan, bi, "buffer");
	if (err)
		goto out_bi_destroy;

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
		strncpy(chan->head.name, chan->head.kobj.name, ZIO_NAME_LEN);
	return 0;

out_create:
	__bi_unregister(chan->cset->zbuf, bi);
out_bi_destroy:
	__bi_destroy(chan->cset->zbuf, bi);
out_remove_sys:
	zattr_set_remove(&chan->head);
out_sysfs:
	kobject_del(&chan->head.kobj);
out_add:
	/* we must _put even if it returned error */
	kobject_put(&chan->head.kobj);
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
	/* remove sysfs cset attributes */
	zattr_set_remove(&chan->head);
	kobject_del(&chan->head.kobj);
	kobject_put(&chan->head.kobj);
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
		__zattr_set_copy(&cset->chan->zattr_set,
				 &cset->chan_template->zattr_set);
	}

	return cset->chan;
}
static inline void cset_free_chan(struct zio_cset *cset)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	/* Only allocated channels need to be freed */
	if (cset->flags & ZCSET_CHAN_ALLOC)
		kfree(cset->chan);
}

static int cset_register(struct zio_cset *cset)
{
	int i, j, err = 0;
	struct zio_buffer_type *zbuf = NULL;
	struct zio_trigger_type *trig = NULL;
	struct zio_ti *ti = NULL;
	char *name;

	pr_debug("%s:%d\n", __func__, __LINE__);
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

	cset->head.zobj_type = ZCSET;
	err = kobject_init_and_add(&cset->head.kobj, &zdkctype,
			&cset->zdev->head.kobj, "cset%i", cset->index);
	if (err)
		goto out_add;
	/* Create sysfs cset attributes */
	cset->zattr_set.n_std_attr = ZATTR_STD_NUM_ZDEV;
	err = zattr_set_create(&cset->head, cset->zdev->s_op);
	if (err)
		goto out_sysfs;
	/* Check attribute hierarchy */
	err = __check_dev_zattr(&cset->zdev->zattr_set, &cset->zattr_set);
	if (err)
		goto out_remove_sys;

	cset->chan = cset_alloc_chan(cset);
	if (IS_ERR(cset->chan)) {
		err = PTR_ERR(cset->chan);
		goto out_remove_sys;
	}

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

	/* Register all child channels */
	for (i = 0; i < cset->n_chan; i++) {
		cset->chan[i].index = i;
		cset->chan[i].cset = cset;
		cset->chan[i].flags |= cset->flags & ZIO_DIR;
		err = chan_register(&cset->chan[i]);
		if (err)
			goto out_reg;
	}
	/* copy default attribute value to ctrl */
	__zattr_init_ctrl(&cset->head, &cset->zattr_set);
	/*
	 * If no name was assigned, ZIO assigns it.  cset name is
	 * set to the kobject name. kobject name has no length limit,
	 * so the cset name is the first ZIO_NAME_LEN characters of
	 * kobject name. A duplicate cset name is not a problem
	 * anyways.
	 */
	if (!strlen(cset->head.name))
		strncpy(cset->head.name, cset->head.kobj.name, ZIO_NAME_LEN);

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
	}

	list_add(&cset->list_cset, &zstat->list_cset);

	/* Private initialization function */
	if (cset->init) {
		err = cset->init(cset);
		if (err)
			goto out_init;
	}
	return 0;

out_init:
	cset->trig = NULL;
	__ti_unregister(trig, ti);
out_tr:
	__ti_destroy(trig, ti);
out_trig:
	zio_trigger_put(cset->trig);
out_reg:
	for (j = i-1; j >= 0; j--)
		chan_unregister(&cset->chan[j]);
	zio_buffer_put(cset->zbuf);
out_buf:
	cset_free_chan(cset);
out_remove_sys:
	zattr_set_remove(&cset->head);
out_sysfs:
	kobject_del(&cset->head.kobj);
out_add:
	/* we must _put even if it returned error */
	kobject_put(&cset->head.kobj);
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
	list_del(&cset->list_cset);
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
	/* Remove from sysfs */
	zattr_set_remove(&cset->head);
	kobject_del(&cset->head.kobj);
	kobject_put(&cset->head.kobj);
	/* Release a group of minors */
	__zio_minorbase_put(cset);
}

/*
 * Register a generic zio object. It can be a device, a buffer type or
 * a trigger type.
 */
static int zobj_register(struct zio_object_list *zlist,
			 struct zio_obj_head *head,
			 enum zio_object_type type,
			 struct module *owner,
			 const char *name)
{
	int err;
	struct zio_object_list_item *item;

	head->zobj_type = type;
	if (strlen(name) > ZIO_OBJ_NAME_LEN)
		pr_warning("ZIO: name too long, cut to %d characters\n",
			   ZIO_OBJ_NAME_LEN);
	strncpy(head->name, name, ZIO_OBJ_NAME_LEN);

	/* Name must be unique */
	err = zobj_unique_name(zlist, head->name);
	if (err)
		goto out;
	err = kobject_init_and_add(&head->kobj, &zdktype, zlist->kobj,
					head->name);
	if (err)
		goto out_kobj;

	/* Add to object list */
	item = kmalloc(sizeof(struct zio_object_list_item), GFP_KERNEL);
	if (!item) {
		err = -ENOMEM;
		goto out_km;
	}
	item->obj_head = head;
	item->owner = owner;
	strncpy(item->name, head->name, ZIO_OBJ_NAME_LEN);
	/* add to the object list*/
	spin_lock(&zstat->lock);
	list_add(&item->list, &zlist->list);
	spin_unlock(&zstat->lock);
	return 0;

out_km:
	kobject_del(&head->kobj);
out_kobj:
	kobject_put(&head->kobj); /* we must _put even if it returned error */
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

	kobject_del(&head->kobj);
	kobject_put(&head->kobj);
}

/* Register a zio device */
int zio_register_dev(struct zio_device *zdev, const char *name)
{
	int err = 0, i, j;

	if (!zdev->d_op) {
		pr_err("%s: new devices has no operations\n", __func__);
		return -EINVAL;
	}
	if (!zdev->owner) {
		pr_err("%s: new device has no owner\n", __func__);
		return -EINVAL;
	}
	/* Register the device */
	err = zobj_register(&zstat->all_devices, &zdev->head,
			    ZDEV, zdev->owner, name);
	if (err)
		goto out;
	zdev->zattr_set.n_std_attr = ZATTR_STD_NUM_ZDEV;
	spin_lock_init(&zdev->lock);
	/* Create standard and extended sysfs attribute for device */
	err = zattr_set_create(&zdev->head, zdev->s_op);
	if (err)
		goto out_sysfs;

	/* Register all child channel sets */
	for (i = 0; i < zdev->n_cset; i++) {
		zdev->cset[i].index = i;
		zdev->cset[i].zdev = zdev;
		err = cset_register(&zdev->cset[i]);
		if (err)
			goto out_cset;
	}
	/* copy default attribute value to ctrl */
	__zattr_init_ctrl(&zdev->head, &zdev->zattr_set);
	return 0;

out_cset:
	for (j = i-1; j >= 0; j--)
		cset_unregister(zdev->cset + j);
out_sysfs:
	zobj_unregister(&zstat->all_devices, &zdev->head);
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
	/* Remove from sysfs */
	zattr_set_remove(&zdev->head);
	/* Unregister the device */
	zobj_unregister(&zstat->all_devices, &zdev->head);
}
EXPORT_SYMBOL(zio_unregister_dev);

/* Register a buffer into the available buffer list */
int zio_register_buf(struct zio_buffer_type *zbuf, const char *name)
{
	int err;
	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!zbuf || !name)
		return -EINVAL;

	err = zobj_register(&zstat->all_buffer_types, &zbuf->head,
			    ZBUF, zbuf->owner, name);
	if (err)
		return err;
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
	if (!trig->zattr_set.std_zattr[ZATTR_TRIG_NSAMPLES].attr.mode)
		goto err_nsamp;

	err = zobj_register(&zstat->all_trigger_types, &trig->head,
			    ZTRIG, trig->owner, name);
	if (err)
		return err;
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

/* Initialize a list of objects */
static int zlist_register(struct zio_object_list *zlist,
			  struct kobject *parent,
			  enum zio_object_type type,
			  const char *name)
{
	int err = 0;

	/* Create a defaul kobject for the list and add it to sysfs */
	zlist->kobj = kobject_create_and_add(name, parent);
	if (!zlist->kobj)
		goto out_kobj;
	pr_debug("%s:%d\n", __func__, __LINE__);

	/* Initialize the specific list */
	INIT_LIST_HEAD(&zlist->list);
	zlist->zobj_type = type;
	return 0;

out_kobj:
	kobject_put(zlist->kobj); /* we must _put even if it returned error */
	return err;
}
/* Remove a list of objects */
static void zlist_unregister(struct zio_object_list *zlist)
{
	kobject_del(zlist->kobj);
	kobject_put(zlist->kobj);
}

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
	/* Initialize char device */
	err = __zio_register_cdev();
	if (err)
		goto out_cdev;

	/* Create the zio container */
	zstat->kobj = kobject_create_and_add("zio", NULL);
	if (!zstat->kobj)
		goto out_kobj;

	/* Register the three object lists (device, buffer and trigger) */
	zlist_register(&zstat->all_devices, zstat->kobj, ZDEV,
			"devices");
	zlist_register(&zstat->all_trigger_types, zstat->kobj, ZTRIG,
			"triggers");
	zlist_register(&zstat->all_buffer_types, zstat->kobj, ZBUF,
			"buffers");
	err = zio_default_buffer_init();
	if (err)
		pr_warn("%s: cannot register default buffer\n", __func__);
	err = zio_default_trigger_init();
	if (err)
		pr_warn("%s: cannot register default trigger\n", __func__);
	pr_info("zio-core had been loaded\n");
	return 0;

out_kobj:
	__zio_unregister_cdev();
out_cdev:
	zio_slab_exit();
	return err;
}

static void __exit zio_exit(void)
{
	zio_default_trigger_exit();
	zio_default_buffer_exit();
	/* Remove the three object lists*/
	zlist_unregister(&zstat->all_devices);
	zlist_unregister(&zstat->all_buffer_types);
	zlist_unregister(&zstat->all_trigger_types);

	/* Remove from sysfs */
	kobject_del(zstat->kobj);
	kobject_put(zstat->kobj);

	/* Remove char device */
	__zio_unregister_cdev();
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
