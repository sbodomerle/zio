#ifndef ZIO_INTERNAL_H_
#define ZIO_INTERNAL_H_

/* Defined in zio-sys.c */
extern struct device_type zdev_generic_type;
extern struct device_type zobj_device_type;
extern struct device_type bi_device_type;

extern int __zdev_register(struct zio_device *parent,
			   const struct zio_device_id *id);
extern int __zattr_set_copy(struct zio_attribute_set *dest,
			    struct zio_attribute_set *src);
extern void __zattr_set_free(struct zio_attribute_set *zattr_set);
extern int zattr_set_create(struct zio_obj_head *head,
			    const struct zio_sysfs_operations *s_op);
extern void zattr_set_remove(struct zio_obj_head *head);
extern void __ctrl_update_nsamples(struct zio_ti *ti);
extern void __zattr_trig_init_ctrl(struct zio_ti *ti, struct zio_control *ctrl);

/* Defined in objects.c */
extern int zio_change_current_trigger(struct zio_cset *cset, char *name);
extern int zio_change_current_buffer(struct zio_cset *cset, char *name);
extern int cset_set_trigger(struct zio_cset *cset);
extern int cset_set_buffer(struct zio_cset *cset);
extern struct zio_bi *__bi_create_and_init(struct zio_buffer_type *zbuf,
					   struct zio_channel *chan);
extern void __bi_destroy(struct zio_buffer_type *zbuf, struct zio_bi *bi);
extern int __bi_register(struct zio_buffer_type *zbuf, struct zio_channel *chan,
			 struct zio_bi *bi, const char *name);
extern void __bi_unregister(struct zio_buffer_type *zbuf, struct zio_bi *bi);
extern struct zio_ti *__ti_create_and_init(struct zio_trigger_type *trig,
					   struct zio_cset *cset);
extern void __ti_destroy(struct zio_trigger_type *trig, struct zio_ti *ti);
extern int __ti_register(struct zio_trigger_type *trig, struct zio_cset *cset,
			 struct zio_ti *ti, const char *name);
extern void __ti_unregister(struct zio_trigger_type *trig, struct zio_ti *ti);
extern void zio_trigger_put(struct zio_trigger_type *trig,
			    struct module *dev_owner);
extern void zio_buffer_put(struct zio_buffer_type *zbuf,
			   struct module *dev_owner);

#endif /* ZIO_INTERNAL_H_ */
