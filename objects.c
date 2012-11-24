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

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"

static struct zio_status *zstat = &zio_global_status; /* Always use ptr */

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
	struct zio_cset *cset, struct zio_object_list *zobj_list, char *name)
{
	struct zio_object_list_item *list_item;

	pr_debug("%s:%d\n", __func__, __LINE__);
	/* search for default trigger */
	list_item = __find_by_name(zobj_list, name);
	if (!list_item)
		return NULL;
	/* If different owner, try to increment its use count */
	if (cset->zdev->owner != list_item->owner
	    && !try_module_get(list_item->owner))
		return NULL;

	return list_item;
}
static struct zio_buffer_type *zio_buffer_get(struct zio_cset *cset,
					      char *name)
{
	struct zio_object_list_item *list_item;

	if (!name)
		return ERR_PTR(-EINVAL);

	list_item = __zio_object_get(cset, &zstat->all_buffer_types, name);
	if (!list_item)
		return ERR_PTR(-ENODEV);
	return container_of(list_item->obj_head, struct zio_buffer_type, head);
}
void zio_buffer_put(struct zio_buffer_type *zbuf, struct module *dev_owner)
{
	if (zbuf->owner != dev_owner)
		module_put(zbuf->owner);
}
static struct zio_trigger_type *zio_trigger_get(struct zio_cset *cset,
						char *name)
{
	struct zio_object_list_item *list_item;

	if (!name)
		return ERR_PTR(-EINVAL);

	list_item = __zio_object_get(cset, &zstat->all_trigger_types, name);
	if (!list_item)
		return ERR_PTR(-ENODEV);
	return container_of(list_item->obj_head, struct zio_trigger_type, head);
}
void zio_trigger_put(struct zio_trigger_type *trig, struct module *dev_owner)
{
	if (trig->owner != dev_owner)
		module_put(trig->owner);
}

/* create and initialize a new buffer instance */
struct zio_bi *__bi_create_and_init(struct zio_buffer_type *zbuf,
				    struct zio_channel *chan)
{
	struct zio_bi *bi;
	int err;

	pr_debug("%s\n", __func__);
	/* Create buffer, ensuring it's not reentrant */
	spin_lock(&zbuf->lock);
	bi = zbuf->b_op->create(zbuf, chan);
	spin_unlock(&zbuf->lock);
	if (IS_ERR(bi)) {
		pr_err("ZIO %s: can't create buffer, error %ld\n",
		       __func__, PTR_ERR(bi));
		goto out;
	}
	/* Initialize buffer */
	spin_lock_init(&bi->lock);
	bi->b_op = zbuf->b_op;
	bi->f_op = zbuf->f_op;
	bi->v_op = zbuf->v_op;
	bi->flags |= (chan->flags & ZIO_DIR);
	/* Initialize head */
	bi->head.dev.type = &bi_device_type;
	bi->head.dev.parent = &chan->head.dev;
	bi->head.zobj_type = ZIO_BI;
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
void __bi_destroy(struct zio_buffer_type *zbuf, struct zio_bi *bi)
{
	pr_debug("%s\n", __func__);
	zbuf->b_op->destroy(bi);
	__zattr_set_free(&bi->zattr_set);
}
int __bi_register(struct zio_buffer_type *zbuf, struct zio_channel *chan,
		  struct zio_bi *bi, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	dev_set_name(&bi->head.dev, name);
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
void __bi_unregister(struct zio_buffer_type *zbuf, struct zio_bi *bi)
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
struct zio_ti *__ti_create_and_init(struct zio_trigger_type *trig,
				    struct zio_cset *cset)
{
	int err;
	struct zio_ti *ti;

	pr_debug("%s\n", __func__);
	/* Create trigger, ensuring it's not reentrant */
	spin_lock(&trig->lock);
	ti = trig->t_op->create(trig, cset, NULL, 0/*FIXME*/);
	spin_unlock(&trig->lock);
	if (IS_ERR(ti)) {
		pr_err("ZIO %s: can't create trigger, error %ld\n",
		       __func__, PTR_ERR(ti));
		goto out;
	}
	/* Initialize trigger */
	spin_lock_init(&ti->lock);
	ti->t_op = trig->t_op;
	ti->flags |= cset->flags & ZIO_DIR;
	/* Initialize head */
	ti->head.dev.type = &zobj_device_type;
	ti->head.dev.parent = &cset->head.dev;
	ti->head.zobj_type = ZIO_TI;
	snprintf(ti->head.name, ZIO_NAME_LEN, "%s-%s-%d",
		 trig->head.name, cset->zdev->head.name, cset->index);
	/* Copy sysfs attribute from trigger type */
	err = __zattr_set_copy(&ti->zattr_set, &trig->zattr_set);
	if (err) {
		trig->t_op->destroy(ti);
		return ERR_PTR(err);
	}
	/* Special case: nsamples */
	__ctrl_update_nsamples(ti);

out:
	return ti;

}
void __ti_destroy(struct zio_trigger_type *trig, struct zio_ti *ti)
{
	pr_debug("%s\n", __func__);
	trig->t_op->destroy(ti);
	__zattr_set_free(&ti->zattr_set);
}
int __ti_register(struct zio_trigger_type *trig, struct zio_cset *cset,
		  struct zio_ti *ti, const char *name)
{
	int err;

	pr_debug("%s\n", __func__);
	dev_set_name(&ti->head.dev, name);
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
void __ti_unregister(struct zio_trigger_type *trig, struct zio_ti *ti)
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


int zio_change_current_trigger(struct zio_cset *cset, char *name)
{
	struct zio_trigger_type *trig, *trig_old = cset->trig;
	struct zio_channel *chan;
	struct zio_ti *ti, *ti_old = cset->ti;
	int err, i;

	pr_debug("%s\n", __func__);
	spin_lock(&cset->lock);
	if (ti_old->flags & ZIO_TI_BUSY) {
		spin_unlock(&cset->lock);
		return -EBUSY;
	}
	/* Set ti BUSY, so it cannot fire */
	ti_old->flags |= ZIO_TI_BUSY;
	spin_unlock(&cset->lock);

	if (strlen(name) > ZIO_OBJ_NAME_LEN)
		return -EINVAL; /* name too long */
	if (unlikely(strcmp(name, trig_old->head.name) == 0))
		return 0; /* is the current trigger */

	/* get the new trigger */
	trig = zio_trigger_get(cset, name);
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
	zio_trigger_put(trig_old, cset->zdev->owner);
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
	zio_trigger_put(trig, cset->zdev->owner);
	return err;
}

int zio_change_current_buffer(struct zio_cset *cset, char *name)
{
	struct zio_buffer_type *zbuf, *zbuf_old = cset->zbuf;
	struct zio_bi **bi_vector;
	int i, j, err;

	pr_debug("%s\n", __func__);
	if (strlen(name) > ZIO_OBJ_NAME_LEN)
		return -EINVAL; /* name too long */
	if (unlikely(strcmp(name, cset->zbuf->head.name) == 0))
		return 0; /* is the current buffer */

	zbuf = zio_buffer_get(cset, name);
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
	zio_buffer_put(zbuf_old, cset->zdev->owner);

	return 0;

out_create:
	for (j = i-1; j >= 0; --j) {
		__bi_unregister(zbuf, bi_vector[j]);
		__bi_destroy(zbuf, bi_vector[j]);
	}
	kfree(bi_vector);
out:
	zio_buffer_put(zbuf, cset->zdev->owner);
	return err;
}

int cset_set_trigger(struct zio_cset *cset)
{
	struct zio_trigger_type *trig;
	char *name = NULL;

	if (cset->trig)
		return -EINVAL;

	if (cset->default_trig) /* cset default */
		name = cset->default_trig;
	if (cset->zdev->preferred_trigger) /* preferred device trigger */
		name = cset->zdev->preferred_trigger;

	trig = zio_trigger_get(cset, name);
	if (IS_ERR(trig)) {
		dev_dbg(&cset->head.dev, "no trigger \"%s\" (error %li), using "
			 "default\n", name, PTR_ERR(trig));
		trig = zio_trigger_get(cset, ZIO_DEFAULT_TRIGGER);
	}
	if (IS_ERR(trig))
		return PTR_ERR(trig);

	cset->trig = trig;
	return 0;
}
int cset_set_buffer(struct zio_cset *cset)
{
	struct zio_buffer_type *zbuf;
	char *name = NULL;

	if (cset->zbuf)
		return -EINVAL;

	if (cset->default_zbuf) /* cset default */
		name = cset->default_zbuf;
	if (cset->zdev->preferred_buffer) /* preferred device buffer */
		name = cset->zdev->preferred_buffer;

	zbuf = zio_buffer_get(cset, name);
	if (IS_ERR(zbuf)) {
		dev_dbg(&cset->head.dev, "no buffer \"%s\" (error %li), using "
			 "default\n", name, PTR_ERR(zbuf));
		zbuf = zio_buffer_get(cset, ZIO_DEFAULT_BUFFER);
	}
	if (IS_ERR(zbuf))
		return PTR_ERR(zbuf);

	cset->zbuf = zbuf;
	return 0;
}
