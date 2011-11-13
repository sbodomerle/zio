#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mutex.h>

#define __ZIO_INTERNAL__
#include <linux/zio.h>
#include <linux/zio-buffer.h>

static DEFINE_MUTEX(zmutex);
struct zio_status zstat;

static ssize_t zio_show_version(struct class *class,
			struct class_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d.%d\n", ZIO_MAJOR_VERSION, ZIO_MINOR_VERSION);
}

static struct class_attribute zclass_attrs[] = {
	__ATTR(version, S_IRUGO, zio_show_version, NULL),
	__ATTR_NULL,
};

static char *zio_devnode(struct device *dev, mode_t *mode)
{
	return kasprintf(GFP_KERNEL, "zio/%s", dev_name(dev));
}

/*
 * zio_class: don't use class_create to create class because it doesn't permit
 * to insert a set of class attributes. This structure is the exact
 * reproduction of what class_create does but with some additional settings.
 */
static struct class zio_class = {
	.name		= "zio",
	.owner		= THIS_MODULE,
	.class_attrs	= zclass_attrs,
	.devnode	= zio_devnode,
};

/* retrieve a channel from one on its minors */
static struct zio_channel *__zio_minor_to_chan(dev_t mm)
{
	struct zio_cset *zcset;
	dev_t cset_base, chan_minor;
	int found = 0;

	chan_minor = mm & (ZIO_NMAX_CSET_MINORS-1);
	cset_base = mm & (~(ZIO_NMAX_CSET_MINORS-1));
	list_for_each_entry(zcset, &zstat.list_cset, list_cset) {
		if (cset_base == zcset->basedev) {
			found = 1;
			break;
		}
	}
	if (!found)
		return NULL;
	return &zcset->chan[chan_minor/2];
}

static int zio_f_open(struct inode *ino, struct file *f)
{
	struct zio_f_priv *priv = NULL;
	struct zio_channel *chan;
	struct zio_buffer_type *zbuf;
	const struct file_operations *old_fops, *new_fops;
	int ret = -EINVAL, minor;

	pr_debug("%s:%i\n", __func__, __LINE__);
	if (f->f_flags & FMODE_WRITE)
		goto out;

	minor = iminor(ino);
	chan = __zio_minor_to_chan(ino->i_rdev);
	if (!chan) {
		pr_err("ZIO: can't retrieve channel for minor %i\n", minor);
		return -EBUSY;
	}
	zbuf = chan->cset->zbuf;
	f->private_data = NULL;
	priv = kzalloc(sizeof(struct zio_f_priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* if there is no instance, then create a new one */
	if (!chan->bi)
		chan->bi = zbuf->b_op->create(zbuf, chan, FMODE_READ);
	priv->chan = chan;

	/* even number is control, odd number is data */
	if (minor & 0x1)
		priv->type = ZIO_CDEV_DATA;
	else
		priv->type = ZIO_CDEV_CTRL;
	f->private_data = priv;

	/* replace zio fops with buffer fops (FIXME: make it a lib function */
	mutex_lock(&zmutex);
	old_fops = f->f_op;
	new_fops = fops_get(zbuf->f_op);
	ret = 0;
	if (new_fops->open)
		ret = new_fops->open(ino, f);
	if (ret) {
		fops_put(zbuf->f_op);
		mutex_unlock(&zmutex);
		goto out;
	}
	fops_put(old_fops);
	f->f_op = new_fops;
	mutex_unlock(&zmutex);
	return 0;

out:
	kfree(priv);
	return ret;
}

static const struct file_operations zfops = {
	.owner = THIS_MODULE,
	.open = zio_f_open,
};

int __zio_minorbase_get(struct zio_cset *zcset)
{
	int i;

	i = find_first_zero_bit(zstat.cset_minors_mask, ZIO_CSET_MAXNUM);
	if (i >= ZIO_CSET_MAXNUM)
		return 1;
	set_bit(i, zstat.cset_minors_mask);
	/* set the base minor for a cset*/
	zcset->basedev = zstat.basedev + (i * ZIO_NMAX_CSET_MINORS);
	pr_debug("%s:%i BASEMINOR 0x%x\n", __func__, __LINE__, zcset->basedev);
	return 0;
}
void __zio_minorbase_put(struct zio_cset *zcset)
{
	int i;

	i = (zcset->basedev - zstat.basedev) / ZIO_NMAX_CSET_MINORS;
	clear_bit(i, zstat.cset_minors_mask);
}

/*
 * create control and data char devices for a channel. The even minor
 * is for control, the odd one for data.
 */
int zio_create_chan_devices(struct zio_channel *chan)
{
	int err;
	dev_t devt_c, devt_d;


	devt_c = chan->cset->basedev + chan->index * 2;
	pr_debug("%s:%d dev_t=0x%x\n", __func__, __LINE__, devt_c);
	chan->ctrl_dev = device_create(&zio_class, NULL, devt_c, NULL,
			"%s-%i-%i-ctrl",
			chan->cset->zdev->head.name,
			chan->cset->index,
			chan->index);
	if (IS_ERR(&chan->ctrl_dev)) {
		err = PTR_ERR(&chan->ctrl_dev);
		goto out;
	}

	devt_d = devt_c + 1;
	pr_debug("%s:%d dev_t=0x%x\n", __func__, __LINE__, devt_d);
	chan->data_dev = device_create(&zio_class, NULL, devt_d, NULL,
			"%s-%i-%i-data",
			chan->cset->zdev->head.name,
			chan->cset->index,
			chan->index);
	if (IS_ERR(&chan->data_dev)) {
		err = PTR_ERR(&chan->data_dev);
		goto out_data;
	}

	return 0;

out_data:
	device_destroy(&zio_class, chan->ctrl_dev->devt);
out:
	return err;
}

void zio_destroy_chan_devices(struct zio_channel *chan)
{
	pr_debug("%s\n", __func__);
	device_destroy(&zio_class, chan->data_dev->devt);
	device_destroy(&zio_class, chan->ctrl_dev->devt);
}

int __zio_register_cdev()
{
	int err;

	err = class_register(&zio_class);
	if (err) {
		pr_err("%s: unable to register class\n", __func__);
		goto out;
	}
	/* alloc to zio the maximum number of minors usable in ZIO */
	err = alloc_chrdev_region(&zstat.basedev, 0,
			ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS, "zio");
	if (err) {
		pr_err("%s: unable to allocate region for %i minors\n",
			__func__, ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
		goto out;
	}
	/* all ZIO's devices, buffers and triggers has zfops as f_op */
	cdev_init(&zstat.chrdev, &zfops);
	zstat.chrdev.owner = THIS_MODULE;
	err = cdev_add(&zstat.chrdev, zstat.basedev,
			ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
	if (err)
		goto out_cdev;
	INIT_LIST_HEAD(&zstat.list_cset);
	return 0;
out_cdev:
	unregister_chrdev_region(zstat.basedev,
			ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
out:
	class_unregister(&zio_class);
	return err;
}
void __zio_unregister_cdev()
{
	cdev_del(&zstat.chrdev);
	unregister_chrdev_region(zstat.basedev,
				ZIO_CSET_MAXNUM * ZIO_NMAX_CSET_MINORS);
	class_unregister(&zio_class);
}
