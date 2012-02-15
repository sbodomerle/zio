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


ZIO_PARAM_TRIGGER(ad788x_trigger);
ZIO_PARAM_BUFFER(ad788x_buffer);

enum ad788x_devices {
	ID_AD7887,
	ID_AD7888,
};

static const struct spi_device_id ad788x_id[] = {
	{"ad7887", ID_AD7887},
	{"ad7888", ID_AD7888},
	{}
};
#define AD788x_PM_NAME		"power-management"
#define AD7887_DUAL_NAME	"dual-channel"
#define AD788x_ADDR_SHIFT	11
#define AD788x_PM_ADDR	0x0300
#define AD788x_PM_SHIFT	8
#define AD7888_VREF_ADDR	0x0400
#define AD7888_VREF_SHIFT	10
#define AD7887_VREF_ADDR	0x2000
#define AD7887_VREF_SHIFT	13
#define AD7887_SINDUAL_ADDR	0x1000
#define AD7887_SINDUAL_SHIFT	12

struct ad788x_context {
	struct spi_message	message;
	struct spi_transfer	transfer;
	struct zio_cset		*cset;
	unsigned int		chan_enable; /* number of enabled channel */
	uint32_t		nsamples; /* number of samples */
};

struct ad788x {
	struct zio_device	zdev;
	enum ad788x_devices	type;
	struct spi_device	*spi;
	uint16_t		cmd;
};
#define to_ad788x(_zdev) container_of(_zdev, struct ad788x, zdev)
/*
 * AD788x don't have register to store data configuration; configuration
 * option are sent every time when we want acquire. So, there is no address
 * register to set but only a value. Address is used as mask in the tx_buf.
 */
/* Standard attributes for AD7887*/
static DEFINE_ZATTR_STD(ZDEV, zattr_dev_ad7887) = {
	ZATTR_REG(zdev, ZATTR_NBITS, S_IRUGO, 0, 12),
	/* vref_src can be internal (0) or external (1)*/
	ZATTR_REG(zdev, ZATTR_VREFTYPE, S_IRUGO | S_IWUGO, AD7887_VREF_ADDR, 1),
};
/* Standard attributes for AD7888*/
static DEFINE_ZATTR_STD(ZDEV, zattr_dev_ad7888) = {
	ZATTR_REG(zdev, ZATTR_NBITS, S_IRUGO, 0, 12),
	/* vref_src can be internal (0) or external (1)*/
	ZATTR_REG(zdev, ZATTR_VREFTYPE, S_IRUGO | S_IWUGO, AD7888_VREF_ADDR, 0),
};
/* Extended attributes for AD7887 */
static struct zio_attribute zattr_dev_ext_ad7887[] = {
		ZATTR_EXT_REG(AD788x_PM_NAME, S_IRUGO | S_IWUGO,
			      AD788x_PM_ADDR, 0x0),
		/* 0 single channel, 1 dual channel*/
		ZATTR_EXT_REG(AD7887_DUAL_NAME, S_IRUGO | S_IWUGO,
			      AD7887_SINDUAL_ADDR, 1),
};
/* Extended attributes for AD7888 */
static struct zio_attribute zattr_dev_ext_ad7888[] = {
		ZATTR_EXT_REG(AD788x_PM_NAME, S_IRUGO | S_IWUGO,
			      AD788x_PM_ADDR, 0x0),
};

static int ad788x_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	unsigned long mask = zattr->priv.addr;
	struct ad788x *ad788x_tmp;

	ad788x_tmp = to_ad788x(to_zio_dev(dev));
	switch (mask) {
	case AD788x_PM_ADDR:		/* power management */
		if (usr_val < 4)
			return -EINVAL;
		ad788x_tmp->cmd = (ad788x_tmp->cmd & ~mask) | usr_val;
		break;
	case AD7887_VREF_ADDR:		/* v_ref source ad7887 */
	case AD7888_VREF_ADDR:		/* v_ref source ad7888 */
	case AD7887_SINDUAL_ADDR:	/* ad7887 single or dual */
		if (usr_val > 1) /* single bit: 0 or 1 */
			return -EINVAL;
		ad788x_tmp->cmd = (ad788x_tmp->cmd & ~mask) |
				  (usr_val ? mask : 0);
		break;
	}
	pr_debug("%s:%d 0x%x\n", __func__, __LINE__, ad788x_tmp->cmd);
	return 0;
}

/* read from AD788x and return the pointer to the data */
static void ad788x_complete(void *cont)
{
	struct ad788x_context *context = (struct ad788x_context *) cont;
	struct ad788x *ad788x;
	struct zio_channel *chan;
	struct zio_cset *cset ;
	uint16_t *data, *buf;
	int i, j = 0;

	cset = context->cset;
	ad788x = to_ad788x(cset->zdev);
	data = (uint16_t *) context->transfer.rx_buf;
	data = &data[1];
	/* demux data */
	cset_for_each(cset, chan) {
			buf = (uint16_t *)chan->active_block->data;
			for (i = 0; i < context->nsamples; ++i)
				buf[i] = data[i * context->chan_enable + j];
		++j;
	}
	cset->ti->t_op->data_done(cset);
	/* free context */
	kfree(context->transfer.tx_buf);
	kfree(context->transfer.rx_buf);
	kfree(context);
}

static int ad788x_input_cset(struct zio_cset *cset)
{
	int i, k, err = -EBUSY;
	struct ad788x *ad788x;
	struct ad788x_context *context;
	struct zio_channel *chan;
	uint16_t *command;
	uint32_t size, nsamples;

	/* alloc context */
	context = kzalloc(sizeof(struct ad788x_context), GFP_ATOMIC);
	if (!context)
		return -ENOMEM;

	ad788x = to_ad788x(cset->zdev);
	context->chan_enable = __get_n_chan_enabled(cset);

	/* prepare SPI message and transfer */
	nsamples = cset->chan->current_ctrl->nsamples;
	size = (context->chan_enable * nsamples * 2) + 2; /* +2 for SPI */

	context->nsamples = nsamples;
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
	/* configure transfer buffer*/
	for (i = 0,  k = 0; i < nsamples; ++i)
		cset_for_each(cset, chan)
			command[k++] = (chan->index << AD788x_ADDR_SHIFT) |
							ad788x->cmd;
	command[k] = ad788x->cmd;

	spi_message_init(&context->message);
	context->message.complete = ad788x_complete;
	context->message.context = context;
	context->cset = cset;
	spi_message_add_tail(&context->transfer, &context->message);

	/* start acquisition */
	err = spi_async_locked(ad788x->spi, &context->message);
	if (!err)
		return -EAGAIN;

	kfree(context->transfer.tx_buf);
	kfree(context->transfer.rx_buf);
	kfree(context);
	return err;
}

struct zio_sysfs_operations ad788x_s_op = {
	.conf_set = ad788x_conf_set,
};

/* channel sets available */
static struct zio_cset ad7887_ain_cset[] = { /* ad7887 cset */
	{
		.raw_io = ad788x_input_cset,
		.ssize = 2,
		.n_chan = 2,
		.flags = ZCSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};
static struct zio_cset ad7888_ain_cset[] = { /* ad7888 cset */
	{
		.raw_io = ad788x_input_cset,
		.ssize = 2,
		.n_chan = 8,
		.flags = ZCSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};
static struct ad788x ad788x_devices[] = {
	[ID_AD7887] = {
		.type = ID_AD7887,
		.zdev = { /* ad7887 template */
			.owner = THIS_MODULE,
			.s_op = &ad788x_s_op,
			.flags = 0,
			.cset = ad7887_ain_cset,
			.n_cset = ARRAY_SIZE(ad7887_ain_cset),
			.zattr_set = {
				.std_zattr = zattr_dev_ad7887,
				.ext_zattr = zattr_dev_ext_ad7887,
				.n_ext_attr = ARRAY_SIZE(zattr_dev_ext_ad7887),
			},
		},
	},
	[ID_AD7888] = {
		.type = ID_AD7888,
		.zdev = { /* ad7888 template */
			.owner = THIS_MODULE,
			.s_op = &ad788x_s_op,
			.flags = 0,
			.cset = ad7888_ain_cset,
			.n_cset = ARRAY_SIZE(ad7888_ain_cset),
			.zattr_set = {
				.std_zattr = zattr_dev_ad7888,
				.ext_zattr = zattr_dev_ext_ad7888,
				.n_ext_attr = ARRAY_SIZE(zattr_dev_ext_ad7888),
			},
		},
	},
};

static int __devinit ad788x_probe(struct spi_device *spi)
{
	const struct spi_device_id *spi_id;
	struct ad788x *ad788x_tmp;
	int err;

	spi_id = spi_get_device_id(spi);
	if (!spi_id)
		return -ENODEV;

	switch (spi_id->driver_data) {
		case ID_AD7887:
			ad788x_tmp = &ad788x_devices[ID_AD7887];
			ad788x_tmp->cmd =
			   zattr_dev_ad7887[ZATTR_VREFTYPE].value <<
							    AD7887_VREF_SHIFT |
			   zattr_dev_ext_ad7887[1].value <<
							   AD7887_SINDUAL_SHIFT;
			break;
		case ID_AD7888:
			ad788x_tmp = &ad788x_devices[ID_AD7888];
			ad788x_tmp->cmd =
			   zattr_dev_ad7888[ZATTR_VREFTYPE].value <<
							      AD7888_VREF_SHIFT;
			break;
		default:
			WARN(1, "%s cannot identify device\n", __func__);
			break;
	}
	/* default value for PM is the same for all the ad788x */
	ad788x_tmp->cmd |= zattr_dev_ext_ad7888[0].value << AD788x_PM_SHIFT;

	if (ad788x_trigger)
		ad788x_devices[ad788x_tmp->type].zdev.preferred_trigger =
								ad788x_trigger;
	if (ad788x_buffer)
		ad788x_devices[ad788x_tmp->type].zdev.preferred_buffer =
								ad788x_buffer;
	ad788x_tmp->spi = spi;
	spi_set_drvdata(spi, &ad788x_tmp);
	spi->bits_per_word = 16;
	err = spi_setup(spi);
	if (err)
		return err;


	return zio_register_dev(&ad788x_devices[ad788x_tmp->type].zdev,
				spi_id->name);
}

static int __devexit ad788x_remove(struct spi_device *spi)
{
	struct ad788x *ad788x_tmp;

	ad788x_tmp = spi_get_drvdata(spi);
	zio_unregister_dev(&ad788x_devices[ad788x_tmp->type].zdev);
	return 0;
}

static struct spi_driver ad788x_driver = {
	.driver = {
		.name	= "ad788x",
		.bus	= &spi_bus_type,
		.owner	= THIS_MODULE,
	},
	.id_table	= ad788x_id,
	.probe		= ad788x_probe,
	.remove		= __devexit_p(ad788x_remove),
};

static int __init ad788x_init(void)
{
	return spi_register_driver(&ad788x_driver);
}
static void __exit ad788x_exit(void)
{
	driver_unregister(&ad788x_driver.driver);
}

module_init(ad788x_init);
module_exit(ad788x_exit);

MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("AD788x driver for ZIO framework");
MODULE_LICENSE("GPL");
