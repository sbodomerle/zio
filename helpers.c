/*
 * Copyright 2011 CERN
 * Author: Alessandro Rubini <rubini@gnudd.com>
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

static void __zio_internal_data_done(struct zio_cset *cset)
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
		chan_for_each(chan, cset) {
			bi = chan->bi;
			block = chan->active_block;
			if (block)
				zbuf->b_op->free_block(chan->bi, block);
			/* We may have a new block ready, or not */
			chan->active_block = zbuf->b_op->retr_block(chan->bi);
		}
		return;
	}
	/* DIR_INPUT */
	chan_for_each(chan, cset) {
		bi = chan->bi;
		block = chan->active_block;
		if (!block)
			continue;
		/* Copy the stamp: it is cset-wide so it lives in the trigger */
		chan->current_ctrl->tstamp.secs = ti->tstamp.tv_sec;
		chan->current_ctrl->tstamp.ticks = ti->tstamp.tv_nsec;
		chan->current_ctrl->tstamp.bins = ti->tstamp_extra;
		memcpy(zio_get_ctrl(block), chan->current_ctrl,
		       ZIO_CONTROL_SIZE);

		if (zbuf->b_op->store_block(bi, block)) /* may fail, no prob */
			zbuf->b_op->free_block(bi, block);
	}
}

/*
 * zio_trigger_data_done
 * This is a ZIO helper to invoke the data_done trigger operation when a data
 * transfer is over and we need to complete the operation.
 * It is useless check for pending ZIO_TI_COMPLETING because only one fire at
 * time is allowed so they cannot exist concurrent completation.
 */
void zio_trigger_data_done(struct zio_cset *cset)
{
	spin_lock(&cset->lock);
	cset->ti->flags |= ZIO_TI_COMPLETING; /* transfer is completing*/
	spin_unlock(&cset->lock);

	/* Call the data_done function */
	if (cset->ti->t_op->data_done)
		cset->ti->t_op->data_done(cset);
	else
		__zio_internal_data_done(cset);

	/* transfer is over, resetting completing and busy flags */
	spin_lock(&cset->lock);
	cset->ti->flags &= (~(ZIO_TI_COMPLETING | ZIO_TI_BUSY));
	spin_unlock(&cset->lock);
}
EXPORT_SYMBOL(zio_trigger_data_done);

static void __zio_internal_abort_free(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;

	chan_for_each(chan, cset) {
		block = chan->active_block;
		if (block)
			cset->zbuf->b_op->free_block(chan->bi, block);
		chan->active_block = NULL;
	}
}

/*
 * zio_trigger_abort
 * This is a ZIO helper to invoke the abort function. This must be used when
 * something is going wrong during the acquisition.
 */
void zio_trigger_abort(struct zio_cset *cset)
{
	struct zio_ti *ti = cset->ti;

	/*
	 * If trigger is running (ZIO_TI_BUSY) but it is not
	 * completing the transfer (ZIO_TI_COMPLETING), then abort it.
	 * If the trigger is completing its run, don't abort it because
	 * it finished and the blocks are full of data.
	 */
	spin_lock(&cset->lock);
	if ((ti->flags & ZIO_TI_BUSY) && !(ti->flags & ZIO_TI_COMPLETING)) {
		if (ti->t_op->abort)
			ti->t_op->abort(cset);
		else
			__zio_internal_abort_free(cset);
		ti->flags &= (~ZIO_TI_BUSY); /* when disabled is not busy */
	}
	spin_unlock(&cset->lock);
}
EXPORT_SYMBOL(zio_trigger_abort);

static void __zio_fire_input_trigger(struct zio_ti *ti)
{
	struct zio_buffer_type *zbuf;
	struct zio_block *block;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_channel *chan;
	struct zio_control *ctrl;
	int datalen;

	cset = ti->cset;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* Allocate the buffer for the incoming sample, in active channels */
	chan_for_each(chan, cset) {
		ctrl = chan->current_ctrl;
		ctrl->seq_num++;

		ctrl->nsamples = ti->nsamples;
		datalen = ctrl->ssize * ti->nsamples;
		block = zbuf->b_op->alloc_block(chan->bi, datalen,
						GFP_ATOMIC);
		/* on error it returns NULL so we are all happy */
		chan->active_block = block;
	}
	if (!cset->raw_io(cset)) {
		/* It succeeded immediately */
		zio_trigger_data_done(cset);
	}
}

static void __zio_fire_output_trigger(struct zio_ti *ti)
{
	struct zio_cset *cset = ti->cset;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* We are expected to already have a block in active channels */
	if (!cset->raw_io(cset)) {
		/* It succeeded immediately */
		zio_trigger_data_done(cset);
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
			(ti->flags & ZIO_TI_BUSY)))
		return;
	spin_lock(&ti->cset->lock);
	ti->flags |= ZIO_TI_BUSY;
	spin_unlock(&ti->cset->lock);

	if (likely((ti->flags & ZIO_DIR) == ZIO_DIR_INPUT))
		__zio_fire_input_trigger(ti);
	else
		__zio_fire_output_trigger(ti);
}
EXPORT_SYMBOL(zio_fire_trigger);
