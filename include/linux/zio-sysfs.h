/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef ZIO_SYSFS_H_
#define ZIO_SYSFS_H_

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include <linux/zio.h>
#include <linux/zio-user.h>

/*
 * zio_attribute: the attribute to access device parameters.
 *
 * Many devices store configuration in hardware registers, and thus
 * many configuration steps can be reduced to a read/write from/to a
 * particular device register, located at a specific address.
 *
 * A zio_attribute provides a generic way to access those registers
 *
 * @attribute: standard attribute structure used to create a sysfs access
 * @flags: to set attribute capabilities
 * @index [INTERNAL]: index within a group of attribute (standard or extended)
 * @priv.reg: register address to use as is
 * @priv.reg_descriptor: a generic pointer used to specify how access to a
 *	particular register on device. This is defined by driver developer
 * @value: is the value stored on device
 * @show: is equivalent to info_get from zio_operations
 * @store: is equivalent to  conf_set from zio_operations
 */
struct zio_attribute {
	struct attribute			attr;
	uint32_t				flags;
	int					index;
	union { /* priv is sometimes a pointer and sometimes an hw addr */
		void				*ptr;
		unsigned long			addr;
	} priv;
	uint32_t				value;
	const struct zio_sysfs_operations	*s_op;
};
#define ZATTR_INDEX_NONE -1
enum zattr_flags {
	ZATTR_TYPE	= 0x10,
	ZATTR_TYPE_STD	= 0x00,
	ZATTR_TYPE_EXT	= 0x10,
};

struct zio_sysfs_operations {
	int (*info_get)(struct kobject *kobj, struct zio_attribute *zattr,
			uint32_t *usr_val);
	int (*conf_set)(struct kobject *kobj, struct zio_attribute *zattr,
			uint32_t  usr_val);
};

/* attribute -> zio_attribute */
#define to_zio_zattr(aptr) container_of(aptr, struct zio_attribute, attr)

/*
 * Every object has both std attributes (whole length is known)
 * and extended attributes (as we need to be told how many).
 * Then, the sysfs attribute_groups are what we build to actually register
 */
struct zio_attribute_set {
	struct zio_attribute	*std_zattr;
	unsigned int		n_std_attr;
	struct zio_attribute	*ext_zattr;
	unsigned int		n_ext_attr;
	struct attribute_group	group;
};

enum zattr_standard_zdev {
	ZATTR_NBIT,	/* number of bits per sample */
	ZATTR_GAIN,	/* gain for signal, integer in 0.001 steps */
	ZATTR_OFFSET,	/* microvolts */
	ZATTR_MAXRATE,	/* hertz */
	ZATTR_VREFTYPE,	/* source of Vref (0 = default) */
	ZATTR_STD_NUM_ZDEV,		/* used to size arrays */
};
enum zattr_standard_trig {
	ZATTR_TRIG_REENABLE = 0,/* re-arm trigger */
	ZATTR_TRIG_NSAMPLES,	/* samples for each transfer */
	ZATTR_STD_NUM_TRIG,	/* used to size arrays */
};
enum zattr_standard_zbuf {
	ZATTR_ZBUF_MAXLEN = 0,	/* max number of element in buffer */
	ZATTR_ZBUF_MAXKB,	/* max number of kB in buffer */
	ZATTR_STD_NUM_ZBUF,	/* used to size arrays */
};

extern const char zio_zdev_attr_names[ZATTR_STD_NUM_ZDEV][ZIO_NAME_LEN];
extern const char zio_trig_attr_names[ZATTR_STD_NUM_TRIG][ZIO_NAME_LEN];
extern const char zio_zbuf_attr_names[ZATTR_STD_NUM_ZBUF][ZIO_NAME_LEN];

#define DEFINE_ZATTR_STD(_type, _name) struct zio_attribute \
	_name[ZATTR_STD_NUM_##_type]

/*
 * @ZATTR_REG: define a zio attribute with address register
 * @ZATTR_PRV: define a zio attribute with private register
 * @ZATTR_EXT_REG: define a zio extended attribute with address register
 * @ZATTR_EXT_PRV: define a zio extended attribute with private register
 */
#define ZATTR_REG(zobj, _type, _mode, _add, _val)[_type] = {		\
		.attr = {						\
			.name = zio_##zobj##_attr_names[_type],		\
			.mode = _mode					\
		},							\
		.priv.addr = _add,					\
		.value = _val,						\
}
#define ZATTR_PRV(zobj, _type, _mode, _priv, _val)[_type] = {		\
		.attr = {						\
			.name = zio_##zobj##_attr_names[_type],		\
			.mode = _mode					\
		},							\
		.priv.ptr = _priv,					\
		.value = _val,						\
}
#define ZATTR_EXT_REG(_name, _mode, _add, _val) {			\
		.attr = {.name = _name, .mode = _mode},			\
		.priv.addr = _add,					\
		.value = _val,						\
}
#define ZATTR_EXT_PRV(_name, _mode, _priv, _val) {			\
		.attr = {.name = _name, .mode = _mode},			\
		.priv.ptr = _priv,					\
		.value = _val,						\
}

#endif /* ZIO_SYSFS_H_ */
