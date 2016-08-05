/* Alessandro Rubini, Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_BUFFER_H__
#define __ZIO_BUFFER_H__

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/zio.h>
#include <linux/gfp.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <linux/zio.h>
#include <linux/zio-user.h>

#define ZIO_DEFAULT_BUFFER "kmalloc" /* For devices with no own buffer type */

/*
 * The following structure defines a buffer type, with methods.
 * An instance is created for each channel using it
 */
struct zio_buffer_operations;
struct zio_buffer_type {
	struct zio_obj_head	head;
	struct module		*owner;
	struct list_head	list; /* instances, and list lock */
	spinlock_t		lock;
	unsigned long		flags;

	const struct zio_sysfs_operations	*s_op;
	const struct zio_buffer_operations	*b_op;
	/*
	 * file operations (read/write etc) are buffer-specific too, but
	 * you are strongly suggested to use zio_generic_file_operations.
	 * If the field is NULL, you'll get ENODEV when opening the cdev.
	 */
	const struct file_operations		*f_op;
	const struct vm_operations_struct	*v_op;

	/* default attributes for instance */
	struct zio_attribute_set		zattr_set;
};
#define to_zio_buf(ptr) container_of(ptr, struct zio_buffer_type, head.dev)

/* buffer_type->flags */
#define ZIO_BUF_FLAG_ALLOC_FOPS	0x00000001 /* set by zio-core */

extern const struct file_operations zio_generic_file_operations;

int __must_check zio_register_buf(struct zio_buffer_type *zbuf,
				  const char *name);
void zio_unregister_buf(struct zio_buffer_type *zbuf);

/* We have our own kmem_cache (a.k.a. slab) for control structures */
int zio_slab_init(void);
void zio_slab_exit(void);
struct zio_control *zio_alloc_control(gfp_t gfp);
void zio_free_control(struct zio_control *ctrl);


struct zio_bi {
	struct zio_obj_head	head;
	struct list_head	list;		/* instance list */
	struct zio_channel	*chan;
	struct zio_cset		*cset;		/* short for chan->cset */

	/* Those using generic_read need this information */
	unsigned long flags;			/* input or output, etc */
	wait_queue_head_t q;			/* for reading or writing */
	spinlock_t		lock;
	atomic_t		use_count;

	/* Standard and extended attributes for this object */
	struct zio_attribute_set		zattr_set;

	const struct zio_buffer_operations	*b_op;
	const struct file_operations		*f_op;
	const struct vm_operations_struct	*v_op;
};
#define to_zio_bi(obj) container_of(obj, struct zio_bi, head.dev)

/* first 4bit are reserved for zio object universal flags */
enum zio_bi_flag_mask {
	/* Status */
	ZIO_BI_PUSHING = 0x10,	/* a push is being performed */
	ZIO_BI_NOSPACE = 0x20, /**< No space left in the buffer
				  (e.g. buffer is full ) */
	/* Configuration */
	ZIO_BI_PREF_NEW = 0x100, /**< prefer new blocks instead old ones */
	ZIO_BI_SOCKET   = 0x200, /* network capable */
};

/**
 * This helper returns the value of a sysfs attribute of a buffer instance
 */
static inline uint32_t zio_bi_std_val(struct zio_bi *bi,
				      enum zio_buf_std_attr attr)
{
	return bi->zattr_set.std_zattr[attr].value;
}

/*
 * Each buffer implementation must provide the following methods, because
 * internal management of individual data instances is left to each of them.
 *
 * "store" is for input and "retr" for output (called by low-level driver).
 * After store, the block is ready for user space and freed internally;
 * after retr, it's the low level driver that must cal the free method.
 * The "alloc" method is called on trigger setup (activate), because the
 * data storage must be available when data transfer really happens (thus,
 * a DMA-only device will have its own buffer as the preferred one).
 * The buffer may use its own alloc for blocks created at write(2) time.
 *
 * Note that each buffer type will need more information, so the block
 * is usually inside a custom structure, reached by container_of().
 * Thus, all blocks for a buffer type must be allocated and freed using
 * the methods of that specific buffer type.
 *
 */
struct zio_buffer_operations {
	/* Alloc returns NULL on error */
	struct zio_block *	(*alloc_block)(struct zio_bi *bi,
					       size_t datalen, gfp_t gfp);
	void			(*free_block)(struct zio_bi *bi,
					      struct zio_block *block);

	/* Retr return NULL when empty */
	struct zio_block *	(*retr_block) (struct zio_bi *bi);
	int			(*store_block)(struct zio_bi *bi,
					       struct zio_block *block);
  
	/* Create returns ERR_PTR on error */
	struct zio_bi *		(*create)(struct zio_buffer_type *zbuf,
					  struct zio_channel *chan);
	void			(*destroy)(struct zio_bi *bi);
};

/*
 * This is the structure we place in f->private_data at open time.
 * Note that the buffer_create function is called by zio-core.
 */
enum zio_cdev_type {
	ZIO_CDEV_CTRL,
	ZIO_CDEV_DATA,
};
struct zio_f_priv {
	struct zio_channel *chan; /* where current block and buffer live */
	enum zio_cdev_type type;
};

/* Buffer helpers */
static inline struct zio_block *zio_buffer_retr_block(struct zio_bi *bi)
{
	if (unlikely(bi->flags & ZIO_DISABLED)) {
		dev_err(&bi->head.dev, "Buffer disabled, cannot retrieve\n");
		return NULL;
	}

	return bi->b_op->retr_block(bi);
}

/**
 * The helper store a given block into a buffer. If the store procedure fails,
 * this helper automatically free the given block.
 */
static inline void zio_buffer_store_block(struct zio_bi *bi, struct zio_block *block)
{
	int ret;

	if (unlikely(bi->flags & ZIO_DISABLED)) {
		dev_err(&bi->head.dev, "Buffer disabled, cannot store\n");
		bi->b_op->free_block(bi, block);
		return;
	}

	ret = bi->b_op->store_block(bi, block);
	if (unlikely(ret)) {
		bi->chan->current_ctrl->zio_alarms |= ZIO_ALARM_LOST_BLOCK;
		bi->b_op->free_block(bi, block);
	}
}

static inline int zio_buffer_free_block(struct zio_bi *bi,
					struct zio_block *block)
{
	if (unlikely(!block))
		return -1;
	bi->b_op->free_block(bi, block);

	return 0;
}

static inline struct zio_block *zio_buffer_alloc_block(struct zio_bi *bi,
					size_t datalen, gfp_t gfp)
{
	struct  zio_block *block;

	block = bi->b_op->alloc_block(bi, datalen, gfp);
	if (!block && (bi->flags & ZIO_BI_NOSPACE)) {
		/* We cannot allocate because the buffer is full */
		if (bi->flags & ZIO_BI_PREF_NEW) {
			/* try by removing the oldest block */
			block = bi->b_op->retr_block(bi);
			bi->b_op->free_block(bi, block);
			block = bi->b_op->alloc_block(bi, datalen, gfp);
		}
		/*
		 * NOTE: with this kind of management we'll have problem with
		 * SELF_TIMED device because they are going to arm as soon as
		 * they store data. This mean that you should always add extra
		 * space to your buffer to get the real buffer depth you want.
		 * The extreme example is kmalloc with buffer length 1: it will
		 * not work because the arm will always overwrite the data, so
		 * if you really one a buffer with 1 block, you have to declare
		 * a buffer length of 2, actually one will be like a temporary
		 * block
		 */
	}
	return block;
}

#endif /* __ZIO_BUFFER_H__ */
