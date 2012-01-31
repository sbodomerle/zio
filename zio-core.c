/* Alessandro Rubini, Federico Vaga for CERN, 2011, GNU GPLv2 or later */

/*
 * This file includes functions that build up the zio core, but are not
 * strictly related to sysfs and configuration. Those are in zio-sysfs.c
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/zio.h>
#include <linux/zio-buffer.h>

/*
 * We use a local slab for control structures.
 */
static struct kmem_cache *zio_ctrl_slab;

struct zio_control *zio_alloc_control(gfp_t gfp)
{
	struct zio_control *ctrl;

	ctrl = kmem_cache_alloc(zio_ctrl_slab, gfp);
	if (!ctrl)
		return NULL;
	ctrl->major_version = ZIO_MAJOR_VERSION;
	ctrl->minor_version = ZIO_MINOR_VERSION;
	if (ntohl(1) == 1)
		ctrl->flags |= ZIO_CONTROL_BIG_ENDIAN;
	else
		ctrl->flags |= ZIO_CONTROL_LITTLE_ENDIAN;
	return ctrl;
}
EXPORT_SYMBOL(zio_alloc_control); /* used by buffers */

void zio_free_control(struct zio_control *ctrl)
{
	kmem_cache_free(zio_ctrl_slab, ctrl);
}
EXPORT_SYMBOL(zio_free_control); /* used by buffers */

int __init zio_slab_init(void)
{
	zio_ctrl_slab = KMEM_CACHE(zio_control, 0);
	if (!zio_ctrl_slab)
		return -ENOMEM;
	return 0;
}

void zio_slab_exit(void) /* not __exit: called from zio_init on failures */
{
	if (zio_ctrl_slab)
		kmem_cache_destroy(zio_ctrl_slab);
	return;
}
