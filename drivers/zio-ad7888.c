/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

enum ad7888_driver_supported_device {
	ID_AD7888
};

ZIO_PARAM_TRIGGER(ad7888_trigger);
ZIO_PARAM_BUFFER(ad7888_buffer);

struct ad7888_context {
	struct spi_message	message;
	struct spi_transfer	transfer;
	struct zio_cset		*cset;
};

struct ad7888_private {
	struct spi_device	*spi;
	uint16_t		cmd;
};
struct ad7888_private *ad7888_prv;
/*
 * AD7888 doesn't have register to store data configuration; configuration
 * option are sent every time when we want acquire. So, there is no address
 * register to set but only a value. Address is used as mask in the tx_buf.
 */
DEFINE_ZATTR_STD(ZDEV, zattr_dev) = {
	ZATTR_REG(zdev, ZATTR_NBIT, S_IRUGO, 0, 12),
	/* vref_src can be internal (0) or external (1)*/
	ZATTR_REG(zdev, ZATTR_VREFTYPE, S_IRUGO | S_IWUGO, 0x40, 0),
};

struct zio_attribute zattr_dev_ext[] = {
	ZATTR_EXT_REG("power_managment", S_IRUGO | S_IWUGO, 0x30, 0x0),
};

int ad7888_conf_set(struct kobject *kobj, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	unsigned long mask = zattr->priv.addr;

	pr_debug("%s:%d", __func__, __LINE__);
	switch (mask) {
	case 0x30: /* power management */
		if (usr_val & ~mask)
			return -EINVAL;
		ad7888_prv->cmd = (ad7888_prv->cmd & ~mask) | usr_val;
		break;
	case 0x40: /* v_ref source */
		if (usr_val > 1) /* single bit: 0 or 1*/
			return -EINVAL;
		ad7888_prv->cmd = (ad7888_prv->cmd & ~mask) |
				  (usr_val << (mask - 2));
	}
	zattr->value = usr_val;
	return 0;
}
/* read from AD7888 and return the pointer to the data */
static void ad7888_complete(void *cont)
{
	struct ad7888_context *context = (struct ad7888_context *) cont;
	struct zio_cset *cset ;
	uint16_t *data, *buf;
	int i, j;

	cset = context->cset;
	data = (uint16_t *) context->transfer.rx_buf;
	data = &data[1];
	/* demux data */
	/*FIXME this loop is bugged, sometimes it doesn't work */
	for (j = 0; j < cset->n_chan; ++j) {
		buf = (uint16_t *)cset->chan[j].active_block->data;
		for (i = 0; i < cset->ti->current_ctrl->nsamples; ++i)
			buf[i] = data[i * cset->n_chan + j];
	}
	cset->ti->t_op->data_done(cset);
	/* free context */
	kfree(context->transfer.tx_buf);
	kfree(context->transfer.rx_buf);
	kfree(context);
}

int ad7888_input_cset(struct zio_cset *cset)
{
	int i, j, k, err = -EBUSY;
	struct ad7888_context *context;
	uint16_t *command;
	uint32_t size;

	/* alloc context */
	context = kzalloc(sizeof(struct ad7888_context), GFP_ATOMIC);
	if (!context)
		return -ENOMEM;

	/* prepare SPI message and transfer */
	size = (cset->n_chan * cset->ti->current_ctrl->nsamples * 2) + 2;
	context->transfer.tx_buf = kmalloc(size, GFP_ATOMIC);
	context->transfer.rx_buf = kmalloc(size, GFP_ATOMIC);
	if (!context->transfer.tx_buf || !context->transfer.rx_buf) {
		kfree(context->transfer.tx_buf);
		kfree(context->transfer.rx_buf);
		return -ENOMEM;
	}
	context->transfer.len = size;
	/* configure transfer buffer*/
	command = (uint16_t *)context->transfer.tx_buf;
	for (i = 0,  k = 0; i < cset->ti->current_ctrl->nsamples; ++i)
		for (j = 0; j < cset->n_chan; ++j, ++k)
			command[k] = (j << 11) | ad7888_prv->cmd;
	command[k] = ad7888_prv->cmd;

	spi_message_init(&context->message);
	context->message.complete = ad7888_complete;
	context->message.context = context;
	context->cset = cset;
	spi_message_add_tail(&context->transfer, &context->message);

	/* start acquisition */
	err = spi_async_locked(ad7888_prv->spi, &context->message);
	if (!err)
		return -EAGAIN;

	kfree(context->transfer.tx_buf);
	kfree(context->transfer.rx_buf);
	kfree(context);
	return err;
}

struct zio_sysfs_operations ad7888_s_op = {
	.conf_set = ad7888_conf_set,
};

/* channel sets available */
static struct zio_cset ad7888_ain_cset[] = {
	{
		.raw_io = ad7888_input_cset,
		.ssize = 2,
		.n_chan = 8,
		.flags = ZCSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};
struct zio_device zdev_ad7888 = {
	.owner = THIS_MODULE,
	.s_op = &ad7888_s_op,
	.flags = 0,
	.cset = ad7888_ain_cset,
	.n_cset = ARRAY_SIZE(ad7888_ain_cset),
	.zattr_set = {
		.std_zattr = zattr_dev,
		.ext_zattr = zattr_dev_ext,
		.n_ext_attr = ARRAY_SIZE(zattr_dev_ext),
	},
};

static int __devinit ad7888_probe(struct spi_device *spi)
{
	int err;

	ad7888_prv = kmalloc(sizeof(struct ad7888_private), GFP_KERNEL);
	ad7888_prv->spi = spi;
	spi_set_drvdata(spi, &zdev_ad7888);
	spi->bits_per_word = 16;
	err = spi_setup(spi);
	if (err)
		return err;

	ad7888_prv->cmd = zattr_dev[ZATTR_VREFTYPE].value << 6 |
			  zattr_dev_ext[0].value << 0;

	if (ad7888_trigger)
		zdev_ad7888.preferred_trigger = ad7888_trigger;
	if (ad7888_buffer)
		zdev_ad7888.preferred_buffer = ad7888_buffer;
	err = zio_register_dev(&zdev_ad7888, "ad7888");
	if (err)
		kfree(ad7888_prv);

	return err;
}

static int __devexit ad7888_remove(struct spi_device *spi)
{
	zio_unregister_dev(&zdev_ad7888);
	kfree(ad7888_prv);
	return 0;
}

static const struct spi_device_id ad7888_id[] = {
	{"ad7888", ID_AD7888},
	{}
};

static struct spi_driver ad7888_driver = {
	.driver = {
		.name	= "ad7888",
		.bus	= &spi_bus_type,
		.owner	= THIS_MODULE,
	},
	.id_table	= ad7888_id,
	.probe		= ad7888_probe,
	.remove		= __devexit_p(ad7888_remove),
};

static int __init ad7888_init(void)
{
	return spi_register_driver(&ad7888_driver);
}
static void __exit ad7888_exit(void)
{
	driver_unregister(&ad7888_driver.driver);
}

module_init(ad7888_init);
module_exit(ad7888_exit);

MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("AD7888 driver for ZIO framework");
MODULE_LICENSE("GPL");
