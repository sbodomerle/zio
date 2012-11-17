#ifndef ZIO_INTERNAL_H_
#define ZIO_INTERNAL_H_

/* Defined in zio-sys.c */
extern struct device_type zdev_generic_type;
extern struct device_type zobj_device_type;

extern int __zdev_register(struct zio_device *parent,
			   const struct zio_device_id *id);

#endif /* ZIO_INTERNAL_H_ */
