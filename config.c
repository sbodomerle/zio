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
#include <linux/version.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"


/**
 * The function validates the a given zio_control
 *
 * @param chan is the channel where apply the control
 * @param ctrl is the given zio_control to apply
 * @return
 */
static int zio_validate_control(struct zio_channel *chan,
				struct zio_control *ctrl)
{
	struct zio_control *cur_ctrl = chan->current_ctrl;
	unsigned int nsample;

	/* Verify ZIO compatibility */
	if (cur_ctrl->major_version != ctrl->major_version) {
		dev_err(&chan->head.dev,
			"incompatible ZIO version (%d), control rejected\n",
			cur_ctrl->major_version);
		return -EINVAL;
	}
	if (cur_ctrl->minor_version != ctrl->minor_version)
		dev_warn(&chan->head.dev,
			 "WARNING, different ZIO minor version (%d)\n",
			 cur_ctrl->minor_version);

	/* Sample size is fixed */
	if (cur_ctrl->ssize != ctrl->ssize) {
		dev_err(&chan->head.dev,
			"different sample size, control rejected\n");
		return -EINVAL;
	}

	/* The zio address cannot be changed */
	if (memcmp(&cur_ctrl->addr, &ctrl->addr, sizeof(struct zio_addr))) {
		dev_err(&chan->head.dev,
			"different zio address, control rejected\n");
		return -EINVAL;
	}


	/*
	 * The nsamples and nbits must be coherent with the value in the
	 * attributes array
	 */
	nsample = ctrl->attr_trigger.std_val[ZIO_ATTR_TRIG_POST_SAMP] +
		  ctrl->attr_trigger.std_val[ZIO_ATTR_TRIG_PRE_SAMP];
	if (ctrl->nsamples != nsample) {
		dev_err(&chan->head.dev,
			"nsamples differs from PRE + POST samples (%i %i)",
			ctrl->nsamples, nsample);
		return -EINVAL;
	}
	if (ctrl->nbits != ctrl->attr_channel.std_val[ZIO_ATTR_DEV_NBITS]) {
		dev_err(&chan->head.dev,
			"nbits differs from nbit attributes (%i %i)",
			ctrl->nbits, ctrl->attr_channel.std_val[ZIO_ATTR_DEV_NBITS]);
		return -EINVAL;
	}

	return 0;
}


/**
 * The function find an attribute in a given zio_attribute_set
 *
 * @param zattr_set is the attribute set where look for the attribute
 * @param is_ext is the attribute type that we are looking for. It is
 * 1 if extended, 0 otherwise
 * @param index is the index assigned to the attribute in the control
 * @return a pointer to zio_attribute. NULL on error
 */
static struct zio_attribute *___zattr_find_in_set(
		struct zio_attribute_set *zattr_set, int is_ext, int index)
{
	int i;

	if (is_ext) {	/* extended attribute */
		for (i = 0; i < zattr_set->n_ext_attr; ++i)
			if(zattr_set->ext_zattr[i].index == index)
				return &zattr_set->ext_zattr[i];
	} else {	/* standard attribute */
		/*
		 * Standard attribute position is fixed. So, check only if the
		 * standard attribute is enable (i.e. zattr.index == index)
		 */
		if(zattr_set->std_zattr &&
				zattr_set->std_zattr[index].index == index)
			return &zattr_set->std_zattr[index];
	}

	return NULL;
}


/**
 * The function find an attribute in the trigger attribute set
 *
 * @param ti is the trigger look for the attribute
 * @param is_ext is the attribute type that we are looking for. It is
 * 1 if extended, 0 otherwise
 * @param index is the index assigned to the attribute in the control
 * @return a pointer to zio_attribute. NULL on error
 */
static struct zio_attribute *__zattr_find_in_ti(struct zio_ti *ti,
					     int is_ext, int index)
{
	struct zio_attribute *zattr;

	zattr = ___zattr_find_in_set(&ti->zattr_set, is_ext, index);
	if(zattr)
		return zattr;
	/* no zattr found, ZIO has a problem */
	WARN(1, "ZIO: No triggers's attribute found with index %i", index);
	return NULL;
}


/**
 * The function find an attribute in the device hierarchy. It implements
 * a bottom-up approach, so it starts the inspection from a channel
 *
 * @param chan is the channel where start the inspection
 * @param is_ext is the attribute type that we are looking for. It is
 * 1 if extended, 0 otherwise
 * @param index is the index assigned to the attribute in the control
 * @return a pointer to zio_attribute. NULL on error
 */
static struct zio_attribute *__zattr_find_in_dev(struct zio_channel *chan,
						 int is_ext, int index)
{
	struct zio_attribute *zattr;
	struct zio_device *zdev;
	struct zio_cset *cset;

	/* Look in channel */
	zattr = ___zattr_find_in_set(&chan->zattr_set, is_ext, index);
	if (zattr)
		goto out;
	/* Look in cset */
	cset = chan->cset;
	zattr = ___zattr_find_in_set(&cset->zattr_set, is_ext, index);
	if (zattr)
		goto out;
	/* Look in device */
	zdev = cset->zdev;
	zattr = ___zattr_find_in_set(&zdev->zattr_set, is_ext, index);
	if (zattr)
		goto out;

	/* No zattr found, ZIO has a probelm */
	WARN(1, "ZIO: No device's attribute found with index %i", index);
	return NULL;
out:
	if (!(zattr->attr.attr.mode & S_IWUGO)) {
		dev_err(&zattr->parent->dev, " you cannot write %s attribute\n",
			zattr->attr.attr.name);
		return NULL;
	}
	return zattr;
}


/**
 * The function find an attribute in a ZIO object.
 *
 * @param head is the ZIO header where inspect
 * @param is_ext is the attribute type that we are looking for. It is
 * 1 if extended, 0 otherwise
 * @param index is the index assigned to the attribute in the control
 * @return a pointer to zio_attribute. NULL on error
 */
static struct zio_attribute *__zattr_find(struct zio_obj_head *head,
					  int is_ext, int index)
{
	switch (head->zobj_type) {
		case ZIO_TI:
			return __zattr_find_in_ti(to_zio_ti(&head->dev),
						  is_ext, index);
			break;
		case ZIO_CHAN:
			return __zattr_find_in_dev(to_zio_chan(&head->dev),
						   is_ext, index);
			break;
		default:
			WARN(1, "ZIO: invalid zio object %i to config\n",
				head->zobj_type);
	}
	return NULL;
}


/**
 * This function analyze the difference between the old zio_ctrl_attr and the
 * new one (it work both for trigger and device) and return a pointer
 * to a zattr_config structure.
 *
 * NOTE: The returned structure must be freed once it is used.
 *
 * @param head is the header of the zio object where look for attributes
 * @param old is the current set of attributes in use by the object
 * @param new is the new set of attributes to apply on the object
 * @return 0 on success; negative error code on error
 */
static struct zio_attr_config *__zattr_find_modified(struct zio_obj_head *head,
						     struct zio_ctrl_attr *old,
						     struct zio_ctrl_attr *new)
{
	int i, std_index[ZIO_MAX_STD_ATTR], ext_index[ZIO_MAX_EXT_ATTR];
	int snd = 0; /* Standard Number Difference */
	int end = 0; /* Extended Number Difference */
	uint32_t std_mask, ext_mask;
	struct zio_attr_config *zattr_cfg;
	unsigned int size;

	/* Set only common attributes, ignore others */
	std_mask = old->std_mask & new->std_mask;
	ext_mask = old->ext_mask & new->ext_mask;


	/* Initialize an attribute configuration structure */
	zattr_cfg = kmalloc(sizeof(struct zio_attr_config), GFP_KERNEL);
	if (!zattr_cfg)
		return NULL;

	/* Prepare the configuration structure: attribute index and value */
	for (i = 0; i < ZIO_MAX_STD_ATTR; ++i) {
		if ((std_mask & (1<<i)) && (old->std_val[i] != new->std_val[i])) {
			zattr_cfg->value[snd] = new->std_val[i];
			std_index[snd++] = i;
		}
	}
	for (i = 0; i < ZIO_MAX_EXT_ATTR; ++i) {
		if ((ext_mask & (1<<i)) && (old->ext_val[i] != new->ext_val[i])) {
			zattr_cfg->value[end + snd] = new->ext_val[i];
			ext_index[end++] = i;
		}
	}

	/* Allocate temporary list of attributes to be modified */
	zattr_cfg->n = snd + end;
	size = sizeof(struct zio_attribute *) * zattr_cfg->n;
	zattr_cfg->zattr = kmalloc(size, GFP_KERNEL);
	if (!zattr_cfg->zattr)
		goto out_alloc;
	/* Fill the zio attributes vector with the correct zio attributes */
	for (i = 0; i < snd; ++i) {
		zattr_cfg->zattr[i] = __zattr_find(head, 0, std_index[i]);
		if (!zattr_cfg->zattr[i])
			goto out_find;
	}
	for (i = 0; i < end; ++i) {
		zattr_cfg->zattr[snd + i] = __zattr_find(head, 1, ext_index[i]);
		if (!zattr_cfg->zattr[snd + i])
			goto out_find;
	}
	return zattr_cfg;
out_find:
	kfree(zattr_cfg->zattr);
out_alloc:
	kfree(zattr_cfg);
	return NULL;
}


/**
 * The function applies the attribute configuration in 'zattr_cfg'. The
 * function performs the conf_set() operation on every attributes
 *
 * @param zattr_cfg: configuration to apply.
 * @return 0 on success; negative error code on error
 */
int zio_generic_config(struct zio_attr_config *zattr_cfg)
{
	struct zio_attribute *zattr;
	int i, err, last_err = 0;

	for (i = 0; i < zattr_cfg->n; ++i) {
		zattr = zattr_cfg->zattr[i];
		err = __zio_conf_set(zattr->parent, zattr, zattr_cfg->value[i]);
		if (err) {
			dev_err(&zattr->parent->dev,
				"cannot configure \"%s\" attribute",
				zattr->attr.attr.name);
			last_err = err;
		}
	}
	return last_err;
}

static void __zio_config_propagate_ctrl(struct zio_attr_config *zattr_cfg)
{
	int i;

	for (i = 0; i < zattr_cfg->n; ++i) {
		zattr_cfg->zattr[i]->value = zattr_cfg->value[i];
		__zio_attr_propagate_value(zattr_cfg->zattr[i]->parent,
					   zattr_cfg->zattr[i]);
	}
}

/**
 * The function update ZIO attributes that was changed during configuration.
 * It is duty of the configuration operation to write values on the peripheral
 * register and eventually to restore the previous value on error.
 *
 * This function must be called while holding the zio_device spinlock
 *
 * @param head is the header of the zio object to change
 * @param old is the current set of attributes in use by the object
 * @param new is the new set of attributes to apply on the object
 * @return 0 on success; negative error code on error
 */
static int __zio_config_object(struct zio_obj_head *head,
				   struct zio_ctrl_attr *old,
				   struct zio_ctrl_attr *new)
{
	struct zio_attr_config *zattr_cfg;
	struct zio_channel *chan;
	struct zio_device *zdev;
	struct zio_ti *ti;
	int err = 0;


	if (!memcmp(old, new, sizeof(struct zio_ctrl_attr)))
		return 0; /* nothing is changed, done */

	/* Look for every modified attribute */
	zattr_cfg = __zattr_find_modified(head, old, new);
	if (!zattr_cfg)
		return -EINVAL;

	switch (head->zobj_type) {
	case ZIO_TI:	/* apply configuration on trigger instance */
		ti = to_zio_ti(&head->dev);

		/*
		 * trigger operations are declared constant so we cannot set a
		 * a default operation when the driver operation is missing.
		 * If the operation correspond to the generic configuration
		 * function, then there is no need to propagate the values to
		 * the control
		 */
		if (ti->t_op->config)
			err = ti->t_op->config(ti, zattr_cfg);
		else
			err = zio_generic_config_trigger(ti, zattr_cfg);

		if (err)
			goto out;
		if (ti->t_op->config != zio_generic_config_trigger)
			__zio_config_propagate_ctrl(zattr_cfg);
		break;
	case ZIO_CHAN:	/* apply configuration on channel */
		chan = to_zio_chan(&head->dev);
		zdev = chan->cset->zdev;

		/*
		 * zdev->config is always valid because checked on device
		 * loading. If it is the generic configuration function, then
		 * there is no need to propagate the values to the control
		 */
		err = zdev->config(zdev, zattr_cfg);
		if (err)
			goto out;
		if (zdev->config != zio_generic_config_device)
			__zio_config_propagate_ctrl(zattr_cfg);

		break;
	default:
		err = -ENODEV;
		goto out;
	}

out:
	kfree(zattr_cfg);
	return err;
}


/**
 * This function configures the device hierarchy with a bottom-up approach, from
 * a channel to the device. It also configure the trigger that drives the
 * channel.
 *
 * @param chan modified channel
 * @param ctrl control to use for configuration
 * @return 0 on success; negative error code on error
 */
int zio_configure(struct zio_channel *chan, struct zio_control *ctrl)
{
	struct zio_cset *cset = chan->cset;
	struct zio_ti *ti = cset->ti;
	int trigger_changed, err = 0;
	unsigned int tflags;
	spinlock_t *lock;

	/* Verify that is a valid control structure */
	err = zio_validate_control(chan, ctrl);
	if (err)
		return err;


	/* Configure trigger */
	trigger_changed = strcmp(chan->current_ctrl->triggername,
				 ctrl->triggername);
	if (trigger_changed) {
		/* Change the trigger */
		err = zio_change_current_trigger(cset, ctrl->triggername);
		if (err)
			return err;
		/*
		 * Trigger is changed, update ti pointer, and take the new
		 * trigger's attribute from the current control of a channel
		 */
		ti = cset->ti;
		dev_dbg(&ti->head.dev, "trigger type is changed to \"%s\"\n",
			ctrl->triggername);
	}

	/* Configure output timestamp (overwritten by the input flow)*/
	chan->current_ctrl->tstamp = ctrl->tstamp;

	lock = __zio_get_dev_spinlock(&chan->cset->head);
	spin_lock(lock);

	/* Disable the trigger for a clean configuration */
	tflags = zio_trigger_abort_disable(chan->cset, ZIO_DISABLED);
	/* Configure the trigger*/
	dev_dbg(&ti->head.dev, "Changing attributes (std 0x%x, ext 0x%x)\n",
			ctrl->attr_trigger.std_mask,
			ctrl->attr_trigger.ext_mask);
	/* Preliminary check to verify if there is any attribute to change */
	if (ctrl->attr_trigger.std_mask || ctrl->attr_trigger.ext_mask) {
		err = __zio_config_object(&ti->head,
					  &chan->current_ctrl->attr_trigger,
					  &ctrl->attr_trigger);
		if (err)
			goto out;
	}


	/* Configure the device hierarchy */
	dev_dbg(&chan->head.dev, "Changing attributes (std 0x%x, ext 0x%x)\n",
			ctrl->attr_channel.std_mask,
			ctrl->attr_channel.ext_mask);
	/* Preliminary check to verify if there is any attribute to change */
	if (ctrl->attr_trigger.std_mask || ctrl->attr_trigger.ext_mask) {
		err = __zio_config_object(&chan->head,
					  &chan->current_ctrl->attr_channel,
					  &ctrl->attr_channel);
		if (err)
			goto out;
	}

	/* Update sequence number */
	if (ctrl->seq_num)
		chan->current_ctrl->seq_num = ctrl->seq_num;

out:
	err = __zio_object_enable(&ti->head, tflags & ZIO_STATUS);
	if (err)
		dev_err(&ti->head.dev,
			"Cannot re-enable trigger after configuration\n");
	spin_unlock(lock);

	/* Finally, if the trigger was armed, re-arm */
	if (tflags & ZIO_TI_ARMED)
		zio_arm_trigger(ti);

	return err;
}
EXPORT_SYMBOL(zio_configure);
