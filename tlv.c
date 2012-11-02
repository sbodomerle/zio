/*
 * Copyright 2013 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"


struct zio_tlv_builder {
	struct zio_obj_head *head;
	struct zio_tlv tlv;
};

static inline void zio_tlv_set_payload(struct zio_tlv *tlv, void *pay)
{

}
static inline void *zio_tlv_get_payload(struct zio_tlv *tlv)
{
	void *ptr = NULL;

	return ptr;
}


struct zio_ctrl_attr *zio_tlv_get_ctrl_attr(struct zio_tlv *tlv,
					    unsigned int n_chan)
{
	struct zio_ctrl_attr *ctrl_attr;

	ctrl_attr = zio_tlv_get_payload(tlv)
		    + sizeof(struct zio_ctrl_attr) * (n_chan - 1);

	return ctrl_attr;
}


/**
 * The function invalidates a set of TLV. A TLV is invalid when its type
 * is ZTLV_NONE. Anyway, the function also clear length and payload.
 *
 * @param tlv is a vector of TLV
 * @param from is the index where start invalidation
 * @param to is the index where stop invalidation
 */
static void zio_tlv_invalidate(struct zio_tlv_builder *tlv_bld,
			       unsigned int from,
			       unsigned int to)
{
	int i = 0;

	for (i = from; i < ZIO_TLV_N_MAX && i < to; ++i) {
		tlv_bld[i].tlv.type = 0;
		tlv_bld[i].tlv.length = 0;
		zio_tlv_set_payload(&tlv_bld[i].tlv, 0);
	}
}


/**
 * The function verifies if a given channel-set requires a TLV extension
 * because of interleaving.
 *
 * @param cset is the channel set to verify
 * @return 1 if the channel set require tlv because of interleaving. 0
 * otherwise
 */
static int zio_interleave_need_tlv(struct zio_cset *cset)
{
	struct zio_channel *chan;
	int i, useless = 0;

	if (!(cset->flags & ZIO_CSET_CHAN_INTERLEAVE))
		return 0;

	/* The loop counts how many channels have not attribute. */
	for (i = 0; i < cset->n_chan; ++i) {
		chan = &cset->chan[i];
		/* FIXME I'm not sure that n_std_attr is 0 when there is no
		 * attribute. Verify it */
		if (!chan->zattr_set.n_ext_attr && !chan->zattr_set.n_std_attr)
			useless++;
	}

	/* If all channels have not attribute, then TLV is not required */
	return !(cset->n_chan == useless);
}


static void *zio_tlv_build_info_single(struct zio_tlv_builder *tlv_bld,
					struct zio_obj_head *head,
					enum zio_tlv_type type,
					uint32_t length, void *start_offset)
{
	dev_dbg(&head->dev, "Configure %s TLV\n",
		dev_name(&head->dev));
	tlv_bld->head = head;
	tlv_bld->tlv.type = type;
	zio_tlv_set_payload(&tlv_bld->tlv, start_offset);

	/* +1 because one 16byte-lump is for zio_tlv */
	tlv_bld->tlv.length = length + 1;

	dev_vdbg(&head->dev, "  Configure TLV: T %d, L %d, V %p\n",
		tlv_bld->tlv.type, tlv_bld->tlv.length, start_offset);

	return start_offset + (length * 16);
}

/**
 * The function calculate the total length of the TLV extensions and fill
 * a given vector of TLVs with the necessary information to describe each
 * one. The function cannot set a real pointer to the payload, so it sets
 * the offset to the payload.
 *
 * @param zdev
 * @param tlv is a vector of tlv to add
 * @return the total length of the TLV extensions
 */
static size_t zio_tlv_build_info(struct zio_device *zdev,
				 struct zio_tlv_builder tlv_bld[ZIO_TLV_N_MAX])
{
	struct zio_cset *cset;
	unsigned int i, ztlv_n = 0;
	void *offset = 0;
	uint32_t len;

	for (i = 0; i < zdev->n_cset; ++i) {
		cset = &zdev->cset[i];

		/* Look for custom TLV */
		if (cset->tlv.length && cset->tlv.type) {
			offset = zio_tlv_build_info_single(&tlv_bld[ztlv_n],
							   &cset->head,
							   cset->tlv.type,
							   cset->tlv.length,
							   offset);

			ztlv_n++;
			if (ztlv_n >= ZIO_TLV_N_MAX)
				goto out;
		}


		/* Look for interleaving TLV */
		if (!zio_interleave_need_tlv(cset)) {
			cset->tlv_interleave = NULL;	/* FIXME useless */
			continue;
		}

		len = sizeof(struct zio_ctrl_attr);
		if ((cset->n_chan - 1) & 0x1)
			len +=8;	/* Align to 16byte */
		len = (len * (cset->n_chan - 1)) / 16; /* in 16byte-lump */

		offset = zio_tlv_build_info_single(&tlv_bld[ztlv_n],
						   &cset->head,
						   ZTLV_INTERLEAVE,
						   len,
						   offset);
		dev_dbg(&zdev->head.dev, "Configure %s TLV interleaving\n",
			dev_name(&cset->head.dev));
		ztlv_n++;
		if (ztlv_n >= ZIO_TLV_N_MAX)
			goto out;
	}

	/* Invalidate the others */
	zio_tlv_invalidate(tlv_bld, ztlv_n, ZIO_TLV_N_MAX);

	return (size_t)offset;

out:
	return 0;
}

static struct zio_tlv *zio_tlv_copy(void *mem, struct zio_tlv_builder *tlv_bld)
{
	struct zio_tlv *tmp_tlv;
	void *offset;

	/* Configure TLV in the TLV memory region */
	offset = zio_tlv_get_payload(&tlv_bld->tlv);
	tmp_tlv = mem + (long)offset;
	tmp_tlv->type = tlv_bld->tlv.type;
	tmp_tlv->length = tlv_bld->tlv.length;
	zio_tlv_set_payload(tmp_tlv, offset + sizeof(struct zio_tlv));

	return tmp_tlv;
}

/**
 * The function allocates the necessary memory to contains all TLV extensions.
 * The function also fill a given vector of TLV with the correspondent
 * information
 *
 * @param zdev is the zio_device to inspect for TLVs
 * @param tlv is an empty vector of TLV to fill with TLV information
 * @return a pointer to the allocated memory
 */
static void *zio_tlv_allocate(struct zio_device *zdev,
				struct zio_tlv_builder tlv_bld[ZIO_TLV_N_MAX])
{
	struct zio_cset *cset;
	struct zio_tlv *tmp_tlv;
	size_t length;
	void *payload;
	int i, j;

	/* Calculate the length of the necessary memory */
	length = zio_tlv_build_info(zdev, tlv_bld);
	if (!length)
	  return NULL;

	/* Allocate the entire memory for tlv */
	payload = kmalloc(length, GFP_KERNEL);
	if (!payload)
		return ERR_PTR(-ENOMEM);

	/* Update payload pointer and convert length in 16byte-lump */
	for (i = 0; i < ZIO_TLV_N_MAX; ++i) {
		if (tlv_bld[i].tlv.type == ZTLV_NONE) {
			zdev->priv_tlv[i] = NULL;
			continue;
		}

		/* Configure TLV in the TLV memory region */
		tmp_tlv = zio_tlv_copy(payload, &tlv_bld[i]);
		dev_dbg(&tlv_bld[i].head->dev, "Assign TLV %d at %p\n",
			i, tmp_tlv);
		/* Assign back tlv information to a ZIO object */
		switch (tlv_bld[i].head->zobj_type) {
		case ZIO_CSET:
			cset = to_zio_cset(&tlv_bld[i].head->dev);
			if (tmp_tlv->type == ZTLV_INTERLEAVE) {
				cset->tlv_interleave = tmp_tlv;
				cset->interleave->current_ctrl->tlv[0].type = ZTLV_MORE;
			} else {
				/* Copy the pay load */
				memcpy(cset->tlv.payload, tmp_tlv->payload, 8);
				for (j = 0; j < cset->n_chan; ++j)
					cset->chan[j].current_ctrl->tlv[0].type = ZTLV_MORE;
			}
			break;
		default:
			/* FIXME others ?*/
			break;
		}
		zdev->priv_tlv[i] = tmp_tlv;
	}

	return payload;
}

/**
 *
 * @param zdev
 */
int zio_tlv_create(struct zio_device *zdev)
{
	struct zio_tlv_builder tlv_builder[ZIO_TLV_N_MAX];
	void *payload;

	payload = zio_tlv_allocate(zdev, tlv_builder);
	if (IS_ERR(payload))
		return PTR_ERR(payload);

	if (!payload)
		return 0;

	dev_dbg(&zdev->head.dev, "Allocated TLV region %p\n", payload);
	/* TODO something to do? */

	return 0;
}

/**
 *
 * @param zdev
 */
void zio_tlv_destroy(struct zio_device *zdev)
{
	void *ptr;

	ptr = zio_tlv_get_payload(zdev->priv_tlv[0]);
	kfree(ptr);
}
