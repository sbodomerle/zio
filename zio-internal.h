#ifndef ZIO_INTERNAL_H_
#define ZIO_INTERNAL_H_

/* Defined in zio-sys.c */
extern struct device_type zdev_generic_type;
extern struct device_type zobj_device_type;
extern struct device_type cset_device_type;
extern struct device_type bi_device_type;
extern struct bin_attribute zio_attr_cur_ctrl;

/* This list is used in the core to keep track of registered objects */
struct zio_object_list {
	enum zio_object_type	zobj_type;
	struct list_head	list;
};
struct zio_object_list_item {
	struct list_head	list;
	char			name[ZIO_OBJ_NAME_LEN]; /* object name copy*/
	struct module		*owner;
	struct zio_obj_head	*obj_head;
};

/* Global framework status (i.e., globals in zio-core) */
struct zio_status {
	/* a pointer to set up standard ktype with create */
	struct kobject		*kobj;
	/* The minor numbers are allocated with the first-fit allocator. */
	struct zio_ffa		*minors;
	struct cdev		chrdev;
	dev_t			basedev;
	spinlock_t		lock;

	/* List of cset, used to retrieve a cset from a minor base*/
	struct list_head	list_cset;

	/* The three lists of registered devices, with owner module */
	struct zio_object_list	all_devices;
	struct zio_object_list	all_trigger_types;
	struct zio_object_list	all_buffer_types;
};

extern struct zio_status zio_global_status;

/* Defined in zio-cdev.c */
extern int zio_minorbase_get(struct zio_cset *zcset);
extern void zio_minorbase_put(struct zio_cset *zcset);

extern int zio_register_cdev(void);
extern void zio_unregister_cdev(void);

extern int zio_create_chan_devices(struct zio_channel *zchan);
extern void zio_destroy_chan_devices(struct zio_channel *zchan);

extern int zio_init_buffer_fops(struct zio_buffer_type *zbuf);
extern int zio_fini_buffer_fops(struct zio_buffer_type *zbuf);

/* Exported but those that know to be the default */
extern int zio_default_buffer_init(void);
extern void zio_default_buffer_exit(void);
extern int zio_default_trigger_init(void);
extern void zio_default_trigger_exit(void);
extern struct zio_device *zio_find_device(char *name, uint32_t dev_id);


/* Defined in zio-sys.c */
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
extern int __check_dev_zattr(struct zio_attribute_set *parent,
		      struct zio_attribute_set *this);
extern int __zattr_dev_init_ctrl(struct zio_device *zdev);

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
