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

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

const char zio_attr_names[ZATTR_STD_ATTR_NUM][ZIO_NAME_LEN] = {
	[ZATTR_GAIN]		= "gain_factor",
	[ZATTR_OFFSET]		= "offset",
	[ZATTR_NBIT]		= "resolution_bits",
	[ZATTR_MAXRATE]		= "max_sample_rate",
	[ZATTR_VREFTYPE]	= "vref_src",
};
EXPORT_SYMBOL(zio_attr_names);

/*
 * @zattrs_to_attrs: extract a 'struct attribute **' from a
 * 'struct zio_attribute *' and return it
 */
static struct attribute **zattrs_to_attrs(struct zio_attribute *zattr,
						unsigned int n_attr)
{
	int i;
	struct attribute **attrs;

	pr_debug("%s\n", __func__);
	if (!zattr || !n_attr)
		return NULL;

	attrs = kzalloc(sizeof(struct attribute) * n_attr, GFP_KERNEL);
	if (!attrs)
		return NULL;

	for (i = 0; i < n_attr; i++)
		attrs[i] = &zattr[i].attr;

	return attrs;
}

/*
 * Top-level ZIO objects has a unique name.
 * You can find a particular object by searching its name.
 */
static inline struct zio_obj_head *__find_by_name(
	struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *cur;

	list_for_each_entry(cur, &zobj_list->list, list) {
		if (strcmp(cur->obj_head->name, name) == 0)
			return cur->obj_head; /* object found */
	}
	pr_debug("%s:%d %s\n", __func__, __LINE__, name);
	return NULL;
}
static struct zio_buffer_type *zbuf_find_by_name(char *name)
{
	struct zio_obj_head *zbuf_head;

	zbuf_head = __find_by_name(&zstat.all_buffer_types, name);
	pr_debug("%s:%d %s\n", __func__, __LINE__, name);
	if (!zbuf_head)
		return NULL;
	pr_debug("%s:%d %s\n", __func__, __LINE__, zbuf_head->name);
	return container_of(zbuf_head, struct zio_buffer_type, head);
}
static struct zio_trigger_type *trig_find_by_name(char *name)
{
	struct zio_obj_head *trig_head;

	trig_head = __find_by_name(&zstat.all_trigger_types, name);
	pr_debug("%s:%d %s\n", __func__, __LINE__, name);
	if (!trig_head)
		return NULL;
	pr_debug("%s:%d %s\n", __func__, __LINE__, trig_head->name);
	return container_of(trig_head, struct zio_trigger_type, head);
}

/*
 * when a trigger fire, this function must be call
 */
int zio_fire_trigger(struct zio_ti *ti)
{
	struct zio_buffer_type *zbuf;
	struct zio_block *block;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_channel *chan;
	struct zio_control *ctrl;
	int err = 0;

	pr_debug("%s:%d\n", __func__, __LINE__);

	cset = ti->cset;
	/* If the trigger runs too early, ti->cset is still NULL */
	if (!cset)
		return -EAGAIN;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	/* copy the stamp (we are software driven anyways) */
	ti->current_ctrl->tstamp.secs = ti->tstamp.tv_sec;
	ti->current_ctrl->tstamp.ticks = ti->tstamp.tv_nsec;
	ti->current_ctrl->tstamp.bins = ti->tstamp_extra;
	/* and the sequence number too (first returned seq is 1) */
	ti->current_ctrl->seq_num++;

	cset_for_each(cset, chan) {
		/* alloc the buffer for the incoming sample */
		/* trigger know the size */
		ctrl = NULL;
		ctrl = zio_alloc_control(GFP_ATOMIC);
		if (!ctrl){
			/* FIXME: what do I do? */
			return -ENOMEM;
		}
		memcpy(ctrl, ti->current_ctrl, ZIO_CONTROL_SIZE);
		ctrl->chan_i = chan->index;

		block = zbuf->b_op->alloc_block(chan->bi, ctrl,
						ctrl->ssize * ctrl->nsamples,
						GFP_ATOMIC);
		if (IS_ERR(block)) {
			err = PTR_ERR(block);
			goto out_alloc;
		}

		/* TODO: condition to identify in or out */
		/* get samples, and control block */
		err = zdev->d_op->input_block(chan, block);
		if (err) {
			pr_err("%s: input_block(%s:%i:%i) error %d\n", __func__,
			       chan->cset->zdev->head.name,
			       chan->cset->index,
			       chan->index,
			       err);
			zbuf->b_op->free_block(chan->bi, block);
		}
		err = zbuf->b_op->store_block(chan->bi, block);
		if (err) {
			/* no error message for common error */
			zbuf->b_op->free_block(chan->bi, block);		      
		}

	}
	return 0;
out_alloc:
	return err;
}
EXPORT_SYMBOL(zio_fire_trigger);

static int __as_auto_index(char *s)
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
 * @zobj_unique_name: the zio device name be unique. If is not unique a busy
 * error is returned. Developer must choose a unique name.
 */
static int zobj_unique_name(struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *cur;
	struct zio_obj_head *tmp;
	unsigned int counter = 0, again, len;
	char name_to_check[ZIO_NAME_LEN];
	int auto_index = __as_auto_index(name);

	pr_debug("%s\n", __func__);

	if (!name)
		return -EINVAL;

	len = strlen(name);
	if (!len)
		return -EINVAL;

	strncpy(name_to_check, name, ZIO_NAME_LEN);
	do {
		again = 0;
		if (auto_index) {
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
		return ERR_PTR(-ENOMEM);

	dest = memcpy(dest, src, size);

	return dest;
}
static int zattr_chan_pre_set(struct zio_channel *chan)
{
	struct zio_cset *cset = chan->cset;

	if (!(cset->flags & ZCSET_CHAN_ALLOC))
		return 0; /* nothing to do */

	/*
	 * if the channel has been allocated by ZIO, then attributes
	 * are cloned the template channel description within parent cset
	 */
	chan->zattr_set.std_zattr =
		__zattr_clone(
			cset->zattr_set_chan.std_zattr,
			ZATTR_STD_ATTR_NUM);
	if (IS_ERR(chan->zattr_set.std_zattr))
		return PTR_ERR(chan->zattr_set.std_zattr);
	chan->zattr_set.ext_zattr =
		__zattr_clone(
			cset->zattr_set.ext_zattr,
			cset->zattr_set.n_ext_attr);
	if (IS_ERR(chan->zattr_set.ext_zattr)) {
		kfree(chan->zattr_set.std_zattr);
		return PTR_ERR(chan->zattr_set.ext_zattr);
	}
	return 0;
}

static void zattr_chan_post_remove(struct zio_channel *chan)
{
	if (chan->cset->flags & ZCSET_CHAN_ALLOC) {
		kfree(chan->zattr_set.std_zattr);
		kfree(chan->zattr_set.ext_zattr);
	}
}

/* When touching attributes, we always get the spinlock for the hosting dev */
static spinlock_t *zdev_get_spinlock(struct zio_obj_head *head)
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
		lock = &to_zio_ti(&head->kobj)->cset->zdev->lock;
		break;
	case ZBI:
		lock = &to_zio_bi(&head->kobj)->cset->zdev->lock;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return lock;
}

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

 /*
 * Zio objects all handle uint32_t values. So the show and store
 * are centralized here, and each device has its own get_info and set_conf
 * which use binary 32-bit numbers. The zattr structure has a field
 * those functions can use to identify the actual attribute involved
 */
static ssize_t zio_attr_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	int err = 0;
	ssize_t len = 0;
	spinlock_t *lock;
	struct zio_attribute *zattr = to_zio_zattr(attr);

	pr_debug("%s\n", __func__);
	if (unlikely(strcmp(attr->name, "name") == 0)) {
		/* print device name*/
		return sprintf(buf, "%s\n", to_zio_head(kobj)->name);
	}

	if (zattr->d_op->info_get) {
		lock = zdev_get_spinlock(to_zio_head(kobj));
		spin_lock(lock);
		err = zattr->d_op->info_get(kobj, zattr, &zattr->value);
		spin_unlock(lock);
		if (err)
			return err;
	}
	len = sprintf(buf, "%i\n", zattr->value);
	return len;
}
static ssize_t zio_attr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t size)
{
	long val;
	int err = 0;
	struct zio_attribute *zattr = to_zio_zattr(attr);
	spinlock_t *lock;

	pr_debug("%s\n", __func__);
	err = strict_strtol(buf, 10, &val);
	if (err)
		return -EINVAL;
	if (zattr->d_op->conf_set) {
		lock = zdev_get_spinlock(to_zio_head(kobj));
		spin_lock(lock);
		err = zattr->d_op->conf_set(kobj, zattr, val);
		spin_unlock(lock);
	}
	return err == 0 ? size : err;
}

static const struct sysfs_ops zio_attribute_ktype_ops = {
	.show  = zio_attr_show,
	.store = zio_attr_store,
};

struct attribute default_attrs[] = {
		{
			.name = "name",
			.mode = 0444, /* read only */
		},
};
struct attribute *def_attr_ptr[] = {
	&default_attrs[0],
	NULL,	/* must be null ended */
};

struct kobj_type zdktype = { /* for standard and extended attribute */
	.release   = NULL,
	.sysfs_ops = &zio_attribute_ktype_ops,
	.default_attrs = def_attr_ptr,
};

static mode_t zattr_is_visible(struct kobject *kobj, struct attribute *attr,
				int n)
{
	unsigned int flag1, flag2, flag3;
	mode_t mode = attr->mode;

	/*
	 * TODO: for the future. If it's decided that activation
	 * is always the first bit then is faster doing:
	 * flag1 & flag2 & flag3 & 0x1
	 * to verify content
	 */
	switch (__zio_get_object_type(kobj)) {
	case ZDEV:
		flag1 = to_zio_dev(kobj)->flags;
		if (flag1 & ZIO_DISABLED)
			mode = 0;
		break;
	case ZCSET:
		flag1 = to_zio_cset(kobj)->flags;
		flag2 = to_zio_cset(kobj)->zdev->flags;
		if ((flag1 | flag2) & ZIO_DISABLED)
			mode = 0;
		break;
	case ZCHAN:
		flag1 = to_zio_chan(kobj)->flags;
		flag2 = to_zio_chan(kobj)->cset->flags;
		flag3 = to_zio_chan(kobj)->cset->zdev->flags;
		if ((flag1 | flag2 | flag3) & ZIO_DISABLED)
			mode = 0;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n",
		     __zio_get_object_type(kobj));
	}

	return mode;
}
static int zattr_create_group(struct kobject *kobj,
	struct attribute_group *grp, unsigned int n_attr,
	const struct zio_device_operations *d_op, int is_ext)
{
	int i;

	if (!grp->attrs)
		return 0;
	grp->is_visible = zattr_is_visible;
	for (i = 0; i < n_attr; i++) {
		/* assign show and store function */
		to_zio_zattr(grp->attrs[i])->d_op = d_op;
		if (!grp->attrs[i]->name) {
			if (is_ext) {
				pr_warning("%s: can't create ext attributes. "
				"%ith attribute has not a name", __func__, i);
				return 0;
			}
			/*
			 * only standard attributes need these lines to fill
			 * the empty hole in the array of attributes
			 */
			grp->attrs[i]->name = zio_attr_names[i];
			grp->attrs[i]->mode = 0;
		}
	}
	return sysfs_create_group(kobj, grp);
}

static int zattr_create_set(struct zio_obj_head *head,
		const struct zio_device_operations *d_op)
{
	int err = 0;
	struct zio_attribute_set *zattr_set;

	zattr_set = __get_zattr_set(head);
	if (!zattr_set)
		return -EINVAL; /* message already printed */


	zattr_set->std_attr.attrs = zattrs_to_attrs(zattr_set->std_zattr,
			ZATTR_STD_ATTR_NUM);
	err = zattr_create_group(&head->kobj, &zattr_set->std_attr,
			ZATTR_STD_ATTR_NUM, d_op, 0);
	if (err)
		goto out;

	zattr_set->ext_attr.attrs = zattrs_to_attrs(zattr_set->ext_zattr,
				zattr_set->n_ext_attr);
	err = zattr_create_group(&head->kobj, &zattr_set->ext_attr,
				zattr_set->n_ext_attr, d_op, 1);
	if (err && zattr_set->std_attr.attrs)
		sysfs_remove_group(&head->kobj, &zattr_set->std_attr);
out:
	return err;
}
static void zattr_remove_set(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;

	zattr_set = __get_zattr_set(head);
	if (!zattr_set)
		return;
	if (zattr_set->std_attr.attrs) /*TODO is condition needed? */
		sysfs_remove_group(&head->kobj, &zattr_set->std_attr);
	if (zattr_set->ext_attr.attrs)
		sysfs_remove_group(&head->kobj, &zattr_set->ext_attr);
}

static int __buffer_create_instance(struct zio_channel *chan)
{
	struct zio_buffer_type *zbuf = chan->cset->zbuf;
	struct zio_bi *bi;
	int err;

	/* create buffer */
	bi = zbuf->b_op->create(zbuf, chan, FMODE_READ);
	if (IS_ERR(bi))
		return PTR_ERR(bi);
	/* Now fill the trigger instance, ops, head, then the rest */
	bi->b_op = zbuf->b_op;
	bi->f_op = zbuf->f_op;
	bi->head.zobj_type = ZBI;
	err = kobject_init_and_add(&bi->head.kobj, &zdktype,
			&chan->head.kobj, "buffer");
	if (err)
		goto out_kobj;
	snprintf(bi->head.name, ZIO_NAME_LEN,"%s-%s-%d-%d",
			zbuf->head.name,
			chan->cset->zdev->head.name,
			chan->cset->index,
			chan->index);

	err = zattr_create_set(&bi->head, chan->cset->zdev->d_op);
	if (err)
		goto out_sysfs;
	init_waitqueue_head(&bi->q);

	/* add to buffer instance list */
	spin_lock(&zbuf->lock);
	list_add(&bi->list, &zbuf->list);
	spin_unlock(&zbuf->lock);
	bi->cset = chan->cset;
	chan->bi = bi;

	/* done. This cset->ti marks everything is running (FIXME?) */
	mb();
	bi->chan = chan;

	return 0;

out_sysfs:
	kobject_del(&bi->head.kobj);
out_kobj:
	kobject_put(&bi->head.kobj);
	zbuf->b_op->destroy(bi);
	return err;
}
static void __buffer_destroy_instance(struct zio_channel *chan)
{
	struct zio_buffer_type *zbuf = chan->cset->zbuf;
	struct zio_bi *bi = chan->bi;

	chan->bi = NULL;

	/* remove from buffer instance list */
	spin_lock(&zbuf->lock);
	list_del(&bi->list);
	spin_unlock(&zbuf->lock);

	zattr_remove_set(&bi->head);
	kobject_del(&bi->head.kobj);
	kobject_put(&bi->head.kobj);
	zbuf->b_op->destroy(bi);
}

static int __trigger_create_instance(struct zio_cset *cset)
{
	int err;
	struct zio_control *ctrl;
	struct zio_ti *ti;

	pr_debug("%s:%d\n", __func__, __LINE__);
	/* Allocate and fill current control as much as possible*/
	ctrl = zio_alloc_control(GFP_KERNEL);
	if (!ctrl)
	  return -ENOMEM;
	ctrl->cset_i = cset->index;
	strncpy(ctrl->devname, cset->zdev->head.name, ZIO_NAME_LEN);
	strncpy(ctrl->triggername, cset->trig->head.name, ZIO_NAME_LEN);
	ctrl->sbits = cset->ssize * 8; /* FIXME: retrieve from attribute */
	ctrl->ssize = cset->ssize;

	ti = cset->trig->t_op->create(cset->trig, cset, ctrl, 0/*FIXME*/);
	if (IS_ERR(ti)) {
		err = PTR_ERR(ti);
		pr_err("%s: can't create trigger error %i\n", __func__, err);
		goto out;
	}
	/* Now fill the trigger instance, ops, head, then the rest */
	ti->t_op = cset->trig->t_op;
	ti->f_op = cset->trig->f_op;
	ti->head.zobj_type = ZTI;
	err = kobject_init_and_add(&ti->head.kobj, &zdktype,
		&cset->head.kobj, "trigger");
	if (err)
		goto out_kobj;
	snprintf(ti->head.name, ZIO_NAME_LEN, "%s-%s-%d",
			cset->trig->head.name,
			cset->zdev->head.name,
			cset->index);

	err = zattr_create_set(&ti->head, cset->zdev->d_op);
	if (err)
		goto out_sysfs;

	/* add to trigger instance list */
	spin_lock(&cset->trig->lock);
	list_add(&ti->list, &cset->trig->list);
	spin_unlock(&cset->trig->lock);
	cset->ti = ti;

	/* done. This cset->ti marks everything is running (FIXME?) */
	mb();
	ti->cset = cset;

	return 0;

out_sysfs:
	kobject_del(&ti->head.kobj);
out_kobj:
	kobject_put(&ti->head.kobj);
	ti->t_op->destroy(ti);
out:
	zio_free_control(ctrl);
	return err;
}
static void __trigger_destroy_instance(struct zio_cset *cset)
{
	struct zio_ti *ti = cset->ti;
	struct zio_control *ctrl = ti->current_ctrl;

	cset->ti = NULL;

	/* remove from trigger instance list */
	spin_lock(&cset->trig->lock);
	list_del(&ti->list);
	spin_unlock(&cset->trig->lock);

	zattr_remove_set(&ti->head);
	kobject_del(&ti->head.kobj);
	kobject_put(&ti->head.kobj);
	cset->trig->t_op->destroy(ti);
	zio_free_control(ctrl);
}

/*
 * chan_register register one channel, but it is important to register
 * or unregister all the channels of a cset at the same time to prevent
 * minors overlapping
 */
static int chan_register(struct zio_channel *chan)
{
	int err;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!chan)
		return -EINVAL;

	chan->head.zobj_type = ZCHAN;
	err = kobject_init_and_add(&chan->head.kobj, &zdktype,
			&chan->cset->head.kobj, "chan%i", chan->index);
	if (err)
		goto out_add;

	err = zattr_chan_pre_set(chan);
	if (err)
		goto out_pre;

	/* create sysfs channel attributes */
	err = zattr_create_set(&chan->head, chan->cset->zdev->d_op);
	if (err)
		goto out_sysfs;

	/* create buffer */
	err = __buffer_create_instance(chan);
	if (err)
		goto out_buf;

	/* create channel char devices*/
	err = chan_create_device(chan);
	if (err)
		goto out_create;
	/*
	 * if no name was assigned, ZIO assign it.
	 * cset name is set to the kobject name. kobject name has no
	 * length limit, so the cset name is set at maximum at the
	 * first ZIO_NAME_LEN character of kobject name. Is extremely rare
	 * and is not destructive a duplicated name for cset
	 */
	if (!strlen(chan->head.name))
		strncpy(chan->head.name, chan->head.kobj.name, ZIO_NAME_LEN);
	return 0;

/* ERRORS */
out_create:
	__buffer_destroy_instance(chan);
out_buf:
	zattr_remove_set(&chan->head);
out_sysfs:
	zattr_chan_post_remove(chan);
out_pre:
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
	chan_destroy_device(chan);
	__buffer_destroy_instance(chan);
	zattr_remove_set(&chan->head);
	zattr_chan_post_remove(chan);
	kobject_del(&chan->head.kobj);
	kobject_put(&chan->head.kobj);
}

/*
 * @cset_alloc_chan: when low-level drivers do not allocate their channels,
 * but set only how many exist, ZIO allocate them
 * @cset_free_chan: if ZIO allocated channels, then it free them; otherwise
 * it does nothing
 */
static struct zio_channel *cset_alloc_chan(struct zio_cset *cset)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	/*if no static channels, then ZIO must alloc them */
	if (cset->chan)
		return cset->chan;
	/* initialize memory to zero to have correct flags and attrs */
	cset->chan = kzalloc(sizeof(struct zio_channel) *
					cset->n_chan, GFP_KERNEL);
	if (!cset->chan)
		return ERR_PTR(-ENOMEM);
	cset->flags |= ZCSET_CHAN_ALLOC;

	return cset->chan;
}
static inline void cset_free_chan(struct zio_cset *cset)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	/* only allocated channels need to be freed*/
	if (cset->flags & ZCSET_CHAN_ALLOC) {
		/* free allocated channel*/
		kfree(cset->chan);
	}
}

static int cset_register(struct zio_cset *cset)
{
	int i, j, err = 0;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!cset)
		return -EINVAL;

	if (!cset->n_chan) {
		pr_err("ZIO: no channels in cset%i\n", cset->index);
		return -EINVAL;
	}

	err = __zio_minorbase_get(cset);
	if (err) {
		pr_err("ZIO: no minors available\n");
		return -EBUSY;
	}

	cset->head.zobj_type = ZCSET;
	err = kobject_init_and_add(&cset->head.kobj, &zdktype,
			&cset->zdev->head.kobj, "cset%i", cset->index);
	if (err)
		goto out_add;

	err = zattr_create_set(&cset->head, cset->zdev->d_op);
	if (err)
		goto out_sysfs;

	cset->chan = cset_alloc_chan(cset);
	if (IS_ERR(cset->chan)) {
		err = PTR_ERR(cset->chan);
		goto out_alloc;
	}

	/*
	 * cset must have a buffer. If no buffer is associated to the
	 * cset, ZIO set the default one
	 */
	if (!cset->zbuf) {
		cset->zbuf = zbuf_find_by_name(ZIO_DEFAULT_BUFFER);
		if (!cset->zbuf) {
			pr_err("ZIO: can't find buffer \"%s\"\n",
				ZIO_DEFAULT_BUFFER);
			err = -EBUSY;
			goto out_buf;
		}
	}

	/* register all child channels */
	for (i = 0; i < cset->n_chan; i++) {
		cset->chan[i].index = i;
		cset->chan[i].cset = cset;
		err = chan_register(&cset->chan[i]);
		if (err)
			goto out_reg;
	}

	/*
	 * if no name was assigned, ZIO assigns it.
	 * cset name is set to the kobject name. kobject name has no
	 * length limit, so the cset name is set as the
	 * first ZIO_NAME_LEN characters of kobject name. it is extremely rare
	 * and is not destructive a duplicated name for cset
	 */
	if (!strlen(cset->head.name))
		strncpy(cset->head.name, cset->head.kobj.name, ZIO_NAME_LEN);

	/*
	 * cset must have a trigger. If no trigger is provided, then
	 * ZIO apply the default one. Trigger assignment must be the last,
	 * because each channel and the cset itself must be ready for trigger
	 * fire.
	 */
	if (!cset->trig) {
		cset->trig = trig_find_by_name(ZIO_DEFAULT_TRIGGER);
		if (!cset->trig) {
			pr_err("ZIO: can't find trigger \"%s\"\n",
				ZIO_DEFAULT_TRIGGER);
			err = -EBUSY;
			goto out_reg;
		}
		err = __trigger_create_instance(cset);
		if (err)
			goto out_trig;
	}

	list_add(&cset->list_cset, &zstat.list_cset);

	/* private initialization function */
	if (cset->init) {
		err = cset->init(cset);
		if (err)
			goto out_init;
	}
	return 0;

out_init:
	cset->trig->t_op->destroy(cset->ti);
out_trig:
	cset->trig = NULL;
out_reg:
	for (j = i-1; j >= 0; j--)
		chan_unregister(&cset->chan[i]);
out_buf:
	cset_free_chan(cset);
out_alloc:
	zattr_remove_set(&cset->head);
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
	/* private erase function */
	if (cset->exit)
		cset->exit(cset);

	__trigger_destroy_instance(cset);
	cset->trig = NULL;
	/* unregister all child channels */
	for (i = 0; i < cset->n_chan; i++)
		chan_unregister(&cset->chan[i]);

	cset->zbuf = NULL;
	cset_free_chan(cset);
	/* remove sysfs */
	zattr_remove_set(&cset->head);
	kobject_del(&cset->head.kobj);
	kobject_put(&cset->head.kobj);
	__zio_minorbase_put(cset);
}

static int zobj_register(struct zio_object_list *zlist,
			 struct zio_obj_head *head,
			 enum zio_object_type type,
			 struct module *owner,
			 const char *name)
{
	int err;
	struct zio_object_list_item *item;

	pr_debug("%s:%d\n", __func__, __LINE__);
	head->zobj_type = type;
	if (strlen(name) > ZIO_NAME_OBJ)
		pr_warning("ZIO: name too long, cut to %d characters\n",
			   ZIO_NAME_OBJ);
	strncpy(head->name, name, ZIO_NAME_OBJ);
	/* name must be unique */
	err = zobj_unique_name(zlist, head->name);
	if (err)
		goto out;
	err = kobject_init_and_add(&head->kobj, &zdktype, zlist->kobj,
					head->name);
	if (err)
		goto out_kobj;

	/* add to object list */
	item = kmalloc(sizeof(struct zio_object_list_item), GFP_KERNEL);
	if (!item) {
		err = -ENOMEM;
		goto out_km;
	}
	item->obj_head = head;
	item->owner = owner;
	spin_lock(&zstat.lock);
	list_add(&item->list, &zlist->list);
	spin_unlock(&zstat.lock);
	return 0;
out_km:
	kobject_del(&head->kobj);
out_kobj:
	kobject_put(&head->kobj); /* we must _put even if it returned error */
out:
	return err;
}
static void zobj_unregister(struct zio_object_list *zlist,
		struct zio_obj_head *zobj)
{
	struct zio_object_list_item *item;

	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!zobj)
		return;
	list_for_each_entry(item, &zlist->list, list) {
		if (item->obj_head == zobj) {
			spin_lock(&zstat.lock);
			list_del(&item->list);
			spin_unlock(&zstat.lock);
			break;
		}
	}

	kobject_del(&zobj->kobj);
	kobject_put(&zobj->kobj);
}

int zio_register_dev(struct zio_device *zdev, const char *name)
{
	int err = 0, i, j;

	if (!zdev || !name)
		return -EINVAL;

	if (!zdev->d_op->input_block && !zdev->d_op->output_block) {
		pr_err("%s: no input_block nor output_block methods\n",
			__func__);
		return -EINVAL;
	}

	err = zobj_register(&zstat.all_devices, &zdev->head,
			    ZDEV, zdev->owner, name);
	if (err)
		goto out;

	spin_lock_init(&zdev->lock);

	err = zattr_create_set(&zdev->head, zdev->d_op);
	if (err)
		goto out_sysfs;

	/* register all child channel sets */
	for (i = 0; i < zdev->n_cset; i++) {
		zdev->cset[i].index = i;
		zdev->cset[i].zdev = zdev;
		err = cset_register(&zdev->cset[i]);
		if (err)
			goto out_cset;
	}

	return 0;

out_cset:
	for (j = i-1; j >= 0; j--)
		cset_unregister(zdev->cset + j);
out_sysfs:
	zobj_unregister(&zstat.all_devices, &zdev->head);
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

	/* unregister all child channel sets */
	for (i = 0; i < zdev->n_cset; i++)
		cset_unregister(&zdev->cset[i]);
	zattr_remove_set(&zdev->head);
	zobj_unregister(&zstat.all_devices, &zdev->head);
}
EXPORT_SYMBOL(zio_unregister_dev);

/* register the buffer into the available buffer list */
int zio_register_buf(struct zio_buffer_type *zbuf, const char *name)
{
	int err = 0;
	pr_debug("%s:%d\n", __func__, __LINE__);
	if (!zbuf || !name)
		return -EINVAL;

	err = zobj_register(&zstat.all_buffer_types, &zbuf->head,
			    ZBUF, zbuf->owner, name);
	if (err)
		goto out;

	INIT_LIST_HEAD(&zbuf->list);
	spin_lock_init(&zbuf->lock);
out:
	return err;
}
EXPORT_SYMBOL(zio_register_buf);

void zio_unregister_buf(struct zio_buffer_type *zbuf)
{
	if (!zbuf)
		return;
	zobj_unregister(&zstat.all_buffer_types, &zbuf->head);
}
EXPORT_SYMBOL(zio_unregister_buf);

/* register the buffer into the available buffer list */
int zio_register_trig(struct zio_trigger_type *trig, const char *name)
{
	int err;

	if (!trig || !name)
		return -EINVAL;
	err = zobj_register(&zstat.all_trigger_types, &trig->head,
			    ZTRIG, trig->owner, name);
	if (err)
		goto out;

	INIT_LIST_HEAD(&trig->list);
	spin_lock_init(&trig->lock);
out:
	return err;
}
EXPORT_SYMBOL(zio_register_trig);

void zio_unregister_trig(struct zio_trigger_type *trig)
{
	if (!trig)
		return;
	zobj_unregister(&zstat.all_trigger_types, &trig->head);
}
EXPORT_SYMBOL(zio_unregister_trig);


/* initializer for zio_object_list */
static int zlist_register(struct zio_object_list *zlist,
			  struct kobject *parent,
			  enum zio_object_type type,
			  const char *name)
{
	int err = 0;

	zlist->kobj = kobject_create_and_add(name, parent);
	if (!zlist->kobj)
		goto out_kobj;
	pr_debug("%s:%d\n", __func__, __LINE__);
	/* initialize device, buffer and trigger lists */
	INIT_LIST_HEAD(&zlist->list);
	zlist->zobj_type = type;
	return 0;
out_kobj:
	kobject_put(zlist->kobj); /* we must _put even if it returned error */
	return err;
}
static void zlist_unregister(struct zio_object_list *zlist)
{
	kobject_del(zlist->kobj);
	kobject_put(zlist->kobj);
}
static int __init zio_init(void)
{
	int err;

	BUILD_BUG_ON_NOT_POWER_OF_2(ZIO_CHAN_MAXNUM);
	BUILD_BUG_ON_NOT_POWER_OF_2(ZIO_CSET_MAXNUM);
	BUILD_BUG_ON(ZIO_CSET_MAXNUM * ZIO_CHAN_MAXNUM * 2 > MINORMASK);
	BUILD_BUG_ON(ZATTR_STD_ATTR_NUM != ARRAY_SIZE(zio_attr_names));

	err = __zio_register_cdev();
	if (err)
		goto out_cdev;
	/* create the zio container */
	zstat.kobj = kobject_create_and_add("zio", NULL);
	if (!zstat.kobj)
		goto out_kobj;

	/* register the object lists (device, buffer and trigger) */
	zlist_register(&zstat.all_devices, zstat.kobj, ZDEV,
			"devices");
	zlist_register(&zstat.all_trigger_types, zstat.kobj, ZTRIG,
			"triggers");
	zlist_register(&zstat.all_buffer_types, zstat.kobj, ZBUF,
			"buffers");
	pr_info("zio-core had been loaded\n");
	return 0;

/* ERROR */
out_kobj:
	__zio_unregister_cdev();
out_cdev:
	return err;
}

static void __exit zio_exit(void)
{
	zlist_unregister(&zstat.all_devices);
	zlist_unregister(&zstat.all_buffer_types);
	zlist_unregister(&zstat.all_trigger_types);

	kobject_del(zstat.kobj);
	kobject_put(zstat.kobj);

	__zio_unregister_cdev();

	pr_info("zio-core had been unloaded\n");
	return;
}

subsys_initcall(zio_init);
module_exit(zio_exit);

MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("ZIO - ZIO Input Output");
MODULE_LICENSE("GPL");
