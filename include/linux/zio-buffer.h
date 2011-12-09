/* Alessandro Rubini, Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_BUFFER_H__
#define __ZIO_BUFFER_H__

/*
 * Data transfers on the control channel only happen by half a kB.
 * This is fixed for forward compatibility; zio_control may have more
 * fields in the future, and current apps should handle it.
 */
#define ZIO_CONTROL_SIZE	512

/*
 * The timestamp is mostly app-specific. It cam be timspec-alike but
 * indidual devices may do whatever they want to match hardware.
 */
struct zio_timestamp {
	uint64_t secs;
	uint64_t ticks;
	uint64_t bins;
};

/*
 * The following data item is the control structure that is being exchanged
 * on the control device associated to each data device. The size of each
 * fields is fixed to ease portability of binary dumps (esp i386/x86-64).
 * The endianness, however, is native for speed reasons.
 */

struct zio_control {
	/* byte 0 */
	uint8_t major_version;
	uint8_t minor_version;
	uint8_t unused[2];
	/* byte 4*/
	uint32_t seq_num;	/* block sequence number */
	uint32_t flags;		/* endianness etc, see below */
	uint32_t nsamples;	/* number of samples in this data block */
	/* byte 16 */
	uint16_t ssize;		/* sample-size for each of them, in bytes */
	uint16_t sbits;		/* sample-bits: number of valid bits */
	uint16_t cset_i;	/* index of channel-set within device */
	uint16_t chan_i;	/* index of channel within cset */

	/* byte 24 */
	/* The control block includes what device the data belong to */
	char devname[ZIO_NAME_LEN];

	/* byte 56 */
	/* Each data block is associated with a trigger and its features */
	char triggername[ZIO_NAME_LEN];

	/* byte 88 */
	struct zio_timestamp tstamp;

	/* byte 112 */
	uint32_t ext_attr_mask;	/* mask of active extended attributes */
	uint32_t std_attr_mask;	/* mask of active standard attributes */
	/* byte 120 */
	uint32_t std_attrs[32];	/* value of each standard attribute */
	uint32_t ext_attrs[32];	/* value of each extended attribute */

	/* This filler must be updated if you change fields above */
	uint8_t __fill_end[ZIO_CONTROL_SIZE - 120 - 4 * (32 + 32)];
};

/* The following flags are used in the control structure */
#define ZIO_CONTROL_LITTLE_ENDIAN	0x01000001
#define ZIO_CONTROL_BIG_ENDIAN		0x02000002

#define ZIO_CONTROL_MSB_ALIGN		0x00000004 /* for analog data */
#define ZIO_CONTROL_LSB_ALIGN		0x00000008 /* for analog data */

#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/zio.h>
#include <linux/gfp.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#define ZIO_DEFAULT_BUFFER "kmalloc" /* For devices with no own buffer type */

/* Compile-time check that the control structure is the right size */
static inline void __unused_check_size(void)
{
	/* if zio_control is smaller then ZIO_CONTROL_SIZE, compile error */
	static int __used v1[sizeof(struct zio_control) - ZIO_CONTROL_SIZE];
	/* if zio_control is greater then ZIO_CONTROL_SIZE, compile error */
	static int __used v2[ZIO_CONTROL_SIZE - sizeof(struct zio_control)];

	BUILD_BUG_ON(sizeof(struct zio_control) != ZIO_CONTROL_SIZE);
}

/* FIXME: use a kmem_cache and real functions for control alloc/free */
static inline struct zio_control *zio_alloc_control(gfp_t gfp)
{
	struct zio_control *ctrl;

	ctrl = kzalloc(sizeof(*ctrl), gfp);
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
static inline void zio_free_control(struct zio_control *ctrl)
{
	kfree(ctrl);
}

/*
 * The following structure defines a buffer type, with methods.
 * An instance is created for each channel using it
 */
struct zio_buffer_operations;
struct zio_buffer_type {
	struct zio_obj_head	head;
	struct module		*owner;
	struct list_head	list; /* instances, and list lock */
	struct spinlock		lock;
	unsigned long		flags; /* to be defined */

	/* file operations (read/write etc) are buffer-specific too */
	const struct zio_sysfs_operations	*s_op;
	const struct zio_buffer_operations	*b_op;
	const struct file_operations		*f_op;

	/* default attributes for instance */
	struct zio_attribute_set		zattr_set;
	/* FIXME: how "own" devices are listed (here or elsewhere?) */
	struct zio_device	*zdev_owner;
	unsigned int		n_zdev_owner;
};
#define to_zio_buf(obj) container_of(obj, struct zio_buffer, head.kobj)

/* read and write may often be the generic ones */
ssize_t zio_generic_read(struct file *, char __user *,
			 size_t, loff_t *);
ssize_t zio_generic_write(struct file *, const char __user *,
			  size_t, loff_t *);
unsigned int zio_generic_poll(struct file *, struct poll_table_struct *);
int zio_generic_release(struct inode *inode, struct file *f);


int __must_check zio_register_buf(struct zio_buffer_type *zbuf,
				  const char *name);
void zio_unregister_buf(struct zio_buffer_type *zbuf);

struct zio_bi {
	struct zio_obj_head	head;
	struct list_head	list;		/* instance list */
	struct zio_channel	*chan;
	struct zio_cset		*cset;		/* short for chan->cset */

	/* Those using generic_read need this information */
	unsigned long flags;			/* input or output, etc */
	wait_queue_head_t q;			/* for reading or writing */

	/* Standard and extended attributes for this object */
	struct zio_attribute_set		zattr_set;

	const struct zio_buffer_operations	*b_op;
	const struct file_operations		*f_op;
};
#define to_zio_bi(_kobj) container_of(_kobj, struct zio_bi, head.kobj)

/* The block is the basic data item being transferred */
struct zio_block {
	unsigned long		ctrl_flags;
	void			*data;
	size_t			datalen;
	size_t			uoff;
};

/*
 * We must know whether the ctrl block has been filled/read or not: "cdone"
 * No "set_ctrl" or "clr_cdone" are needed, as cdone starts 0 and is only set
 */
#define zio_get_ctrl(block) ((struct zio_control *)((block)->ctrl_flags & ~1))
#define zio_set_ctrl(block, ctrl) ((block)->ctrl_flags = (unsigned long)(ctrl))
#define zio_is_cdone(block)  ((block)->ctrl_flags & 1)
#define zio_set_cdone(block)  ((block)->ctrl_flags |= 1)


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
 */
struct zio_buffer_operations {
	struct zio_block *	(*alloc_block)(struct zio_bi *bi,
					       struct zio_control *ctrl,
					       size_t datalen, gfp_t gfp);
	void			(*free_block)(struct zio_bi *bi,
					      struct zio_block *block);

	int			(*store_block)(struct zio_bi *bi,
					       struct zio_block *block);
	struct zio_block *	(*retr_block) (struct zio_bi *bi);

	struct zio_bi *		(*create)(struct zio_buffer_type *zbuf,
					  struct zio_channel *chan,
					  fmode_t f_flags);
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

ssize_t zio_generic_read(struct file *f, char __user *ubuf,
			 size_t count, loff_t *offp);
ssize_t zio_generic_write(struct file *f, const char __user *ubuf,
			  size_t count, loff_t *offp);
unsigned int zio_generic_poll(struct file *f, struct poll_table_struct *w);

#endif /* __KERNEL__ */
#endif /* __ZIO_BUFFER_H__ */
