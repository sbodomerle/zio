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
#include <linux/zio-utils.h>


ZIO_PARAM_TRIGGER(smb380_trigger);
ZIO_PARAM_BUFFER(smb380_buffer);

enum smb380_devices {
	ID_SMB380,
};

static const struct spi_device_id smb380_id[] = {
	{"smb380", ID_SMB380},
	{}
};

enum smb380_reg_enum {
	/* INFO */
	SMB380_CHIPID,
	SMB380_AL_VERSION,
	SMB380_ML_VERSION,
	/* DATA */
	SMB380_TEMP,
	SMB380_AXIS_X_HI,
	SMB380_AXIS_X_LO,
	SMB380_AXIS_X_NEW,
	SMB380_AXIS_Y_HI,
	SMB380_AXIS_Y_LO,
	SMB380_AXIS_Y_NEW,
	SMB380_AXIS_Z_HI,
	SMB380_AXIS_Z_LO,
	SMB380_AXIS_Z_NEW,
	/* PRIVATE */
	SMB380_RESERVED_1,
	SMB380_RESERVED_2,
	/* STATUS */
	SMB380_ALERT_PHASE,
	SMB380_HG_LATCHED,
	SMB380_LG_LATCHED,
	SMB380_HG_STATUS,
	SMB380_LG_STATUS,
	/* CONTROL */
	SMB380_SELFTEST_0,
	SMB380_SELFTEST_1,
	SMB380_ST_RESULT,
	SMB380_SLEEP,
	SMB380_SOFT_RESET,
	SMB380_EE_W,
	SMB380_UPDATE_IMG,
	SMB380_RESET_INT,
	SMB380_NEW_DATA,
	SMB380_COUNTER_HG,
	SMB380_COUNTER_LG,
	SMB380_LATCH,
	SMB380_ALERT,
	SMB380_ANY_MOTION,
	SMB380_ENABLE_HG,
	SMB380_ENABLE_LG,
	SMB380_ENABLE_ADV,
	SMB380_SHADOW_DIS,
	SMB380_WAKE_UP_PAUSE,
	SMB380_WAKE_UP,
	SMB380_SPI4,
	/* SETTINGS */
	SMB380_LG_THRES,
	SMB380_LG_DUR,
	SMB380_HG_THRES,
	SMB380_HG_DUR,
	SMB380_ANY_MOTION_THRES,
	SMB380_ANY_MOTION_DUR,
	SMB380_HG_HYST,
	SMB380_LG_HYST,
	SMB380_RANGE,
	SMB380_BANDWIDTH,
	/* TRIMMING */
	SMB380_OFFSET_T,
	SMB380_OFFSET_X,
	SMB380_OFFSET_Y,
	SMB380_OFFSET_Z,
	SMB380_OFFSET_T_LO,
	SMB380_OFFSET_X_LO,
	SMB380_OFFSET_Y_LO,
	SMB380_OFFSET_Z_LO,
	SMB380_GAIN_T,
	SMB380_GAIN_X,
	SMB380_GAIN_Y,
	SMB380_GAIN_Z,
};
static const struct zio_field_desc smb380_regs[] = {
	[SMB380_CHIPID] =		{0x00, 0x03, 0},
	[SMB380_ML_VERSION] =		{0x01, 0x0F, 0},
	[SMB380_AL_VERSION] =		{0x01, 0x0F, 4},
	[SMB380_AXIS_X_NEW] =		{0x02, 0x01, 0},
	[SMB380_AXIS_X_LO] =		{0x02, 0x03, 6},
	[SMB380_AXIS_X_HI] =		{0x03, 0xFF, 0},
	[SMB380_AXIS_Y_NEW] =		{0x04, 0x01, 0},
	[SMB380_AXIS_Y_LO] =		{0x04, 0x03, 6},
	[SMB380_AXIS_Y_HI] =		{0x05, 0xFF, 0},
	[SMB380_AXIS_Z_NEW] =		{0x06, 0x01, 0},
	[SMB380_AXIS_Z_LO] =		{0x06, 0x03, 6},
	[SMB380_AXIS_Z_HI] =		{0x07, 0xFF, 0},
	[SMB380_TEMP] =			{0x08, 0xFF, 0},
	[SMB380_HG_STATUS] =		{0x09, 0x01, 0},
	[SMB380_LG_STATUS] =		{0x09, 0x01, 1},
	[SMB380_HG_LATCHED] =		{0x09, 0x01, 2},
	[SMB380_LG_LATCHED] =		{0x09, 0x01, 3},
	[SMB380_ALERT_PHASE] =		{0x09, 0x01, 4},
	[SMB380_ST_RESULT] =		{0x09, 0x01, 7},
	[SMB380_SLEEP] =		{0x0A, 0x01, 0},
	[SMB380_SOFT_RESET] =		{0x0A, 0x01, 1},
	[SMB380_SELFTEST_0] =		{0x0A, 0x01, 2},
	[SMB380_SELFTEST_1] =		{0x0A, 0x01, 3},
	[SMB380_EE_W] =			{0x0A, 0x01, 4},
	[SMB380_UPDATE_IMG] =		{0x0A, 0x01, 5},
	[SMB380_RESET_INT] =		{0x0A, 0x01, 6},
	[SMB380_ENABLE_LG] =		{0x0B, 0x01, 0},
	[SMB380_ENABLE_HG] =		{0x0B, 0x02, 1},
	[SMB380_COUNTER_LG] =		{0x0B, 0x03, 2},
	[SMB380_COUNTER_HG] =		{0x0B, 0x03, 4},
	[SMB380_ANY_MOTION] =		{0x0B, 0x01, 6},
	[SMB380_ALERT] =		{0x0B, 0x01, 7},
	[SMB380_LG_THRES] =		{0x0C, 0xFF, 0},
	[SMB380_LG_DUR] =		{0x0D, 0xFF, 0},
	[SMB380_HG_THRES] =		{0x0E, 0xFF, 0},
	[SMB380_HG_DUR] =		{0x0F, 0xFF, 0},
	[SMB380_ANY_MOTION_THRES] =	{0x10, 0xFF, 0},
	[SMB380_LG_HYST] =		{0x11, 0x07, 0},
	[SMB380_HG_HYST] =		{0x11, 0x07, 3},
	[SMB380_ANY_MOTION_DUR] =	{0x11, 0x03, 6},
	[SMB380_RESERVED_1] =		{0x12, 0xFF, 0},
	[SMB380_RESERVED_2] =		{0x13, 0xFF, 0},
	[SMB380_BANDWIDTH] =		{0x14, 0x07, 0},
	[SMB380_RANGE] =		{0x14, 0x03, 3},
	[SMB380_WAKE_UP] =		{0x15, 0x01, 0},
	[SMB380_WAKE_UP_PAUSE] =	{0x15, 0x03, 1},
	[SMB380_SHADOW_DIS] =		{0x15, 0x01, 3},
	[SMB380_LATCH] =		{0x15, 0x01, 4},
	[SMB380_NEW_DATA] =		{0x15, 0x01, 5},
	[SMB380_ENABLE_ADV] =		{0x15, 0x01, 6},
	[SMB380_SPI4] =			{0x15, 0x03, 7},
	[SMB380_GAIN_Z] =		{0x16, 0x3F, 0},
	[SMB380_OFFSET_Z_LO] =		{0x16, 0x03, 6},
	[SMB380_GAIN_Y] =		{0x17, 0x3F, 0},
	[SMB380_OFFSET_Y_LO] =		{0x17, 0x03, 6},
	[SMB380_GAIN_X] =		{0x18, 0x3F, 0},
	[SMB380_OFFSET_X_LO] =		{0x18, 0x03, 6},
	[SMB380_GAIN_T] =		{0x19, 0x3F, 0},
	[SMB380_OFFSET_T_LO] =		{0x19, 0x03, 6},
	[SMB380_OFFSET_Z] =		{0x1A, 0xFF, 0},
	[SMB380_OFFSET_Y] =		{0x1B, 0xFF, 0},
	[SMB380_OFFSET_X] =		{0x1C, 0xFF, 0},
	[SMB380_OFFSET_T] =		{0x1D, 0xFF, 0},
};

/* Standard attributes for axis x */
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_chan_x) = {
	ZIO_ATTR(zdev, ZIO_ATTR_OFFSET, S_IRUGO, SMB380_OFFSET_X, 0),
	ZIO_ATTR(zdev, ZIO_ATTR_GAIN, S_IRUGO | S_IWUGO, SMB380_GAIN_X, 0),
};
/* Standard attributes for axis y */
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_chan_y) = {
	ZIO_ATTR(zdev, ZIO_ATTR_OFFSET, S_IRUGO, SMB380_OFFSET_Y, 0),
	ZIO_ATTR(zdev, ZIO_ATTR_GAIN, S_IRUGO | S_IWUGO, SMB380_GAIN_Y, 0),
};
/* Standard attributes for axis z */
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_chan_z) = {
	ZIO_ATTR(zdev, ZIO_ATTR_OFFSET, S_IRUGO, SMB380_OFFSET_Z, 0),
	ZIO_ATTR(zdev, ZIO_ATTR_GAIN, S_IRUGO | S_IWUGO, SMB380_GAIN_Z, 0),
};


static ZIO_ATTR_DEFINE_STD(ZIO_DEV, smb380_zattr_zdev) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, S_IRUGO | S_IWUGO, 0, 8),
};
/* Extended attributes for AD7888 */
static struct zio_attribute smb380_zattr_zdev_ext[] = {
	ZIO_ATTR_EXT("any_motion_dur", S_IRUGO | S_IWUGO, SMB380_ANY_MOTION_DUR, 0),
	ZIO_ATTR_EXT("any_motion_thres", S_IRUGO | S_IWUGO, SMB380_ANY_MOTION_THRES, 0),

	ZIO_ATTR_EXT("HG_dur", S_IRUGO | S_IWUGO, SMB380_HG_DUR, 150),
	ZIO_ATTR_EXT("HG_thres", S_IRUGO | S_IWUGO, SMB380_HG_THRES, 160),
	ZIO_ATTR_EXT("HG_hyst", S_IRUGO | S_IWUGO, SMB380_HG_HYST, 0),

	ZIO_ATTR_EXT("LG_dur", S_IRUGO | S_IWUGO, SMB380_LG_DUR, 150),
	ZIO_ATTR_EXT("LG_thres", S_IRUGO | S_IWUGO, SMB380_LG_THRES, 20),
	ZIO_ATTR_EXT("LG_hyst", S_IRUGO | S_IWUGO, SMB380_LG_HYST, 0),

	ZIO_ATTR_EXT("range", S_IRUGO | S_IWUGO, SMB380_RANGE, 0x8),
	ZIO_ATTR_EXT("bandwidth", S_IRUGO | S_IWUGO, SMB380_BANDWIDTH, 0x6),

	/* Parameters, not in control structure*/
	ZIO_PARAM_EXT("chip-id", S_IRUGO, SMB380_CHIPID, 0x0),
	ZIO_PARAM_EXT("spi4", S_IRUGO | S_IWUGO, SMB380_SPI4, 0x0),
	ZIO_PARAM_EXT("datax", S_IRUGO, SMB380_AXIS_X_LO, 0x0),
	ZIO_PARAM_EXT("reset", S_IWUGO, SMB380_SOFT_RESET, 0x0),
};

struct smb380_context {
	struct spi_message	msg;
	struct spi_transfer	*trans;
	uint8_t			*tx;
	uint8_t			*rx;
	struct zio_cset		*cset;
	unsigned int		chan_enable; /* number of enabled channel */
	uint32_t		nsamples; /* number of samples */
};

struct smb380 {
	struct zio_device	*zdev_empty;
	struct spi_device	*spi;
};

static struct smb380 *get_smb380(struct device *dev)
{
	switch (to_zio_head(dev)->zobj_type) {
		case ZIO_DEV:
			return to_zio_dev(dev)->priv_d;
		case ZIO_CSET:
			return to_zio_cset(dev)->zdev->priv_d;
		case ZIO_CHAN:
			return to_zio_chan(dev)->cset->zdev->priv_d;
		default:
			return NULL;
	}
}
static int smb380_conf_set(struct device *dev, struct zio_attribute *zattr,
			   uint32_t  usr_val)
{
	const struct zio_field_desc *reg;
	struct smb380 *smb380;
	int tx;

	pr_info("%s:%d 0x%x\n", __func__, __LINE__, usr_val);
	/* get the register descriptor for the attribute */
	reg = &smb380_regs[zattr->id];

	if (!(usr_val & reg->mask)) {
		dev_err(dev, "value must fill this mask 0x%x\n", reg->mask);
		return -EINVAL;
	}
	pr_info("%s:%d\n", __func__, __LINE__);
	if (!(zattr->flags & ZIO_ATTR_TYPE_EXT) && zattr->index == ZIO_ATTR_NBITS) {
		if (usr_val != 8 && usr_val != 10) {
			dev_err(dev, "Invalid number of bits, 8 or 10 allowed");
			return -EINVAL;
		} else {
			/* FIXME */
		}
	}
	pr_info("%s:%d\n", __func__, __LINE__);
	smb380 = get_smb380(dev);
	if (!smb380)
		return -ENODEV;
	pr_info("%s:%d\n", __func__, __LINE__);
	tx = ((reg->addr << 8) | usr_val << reg->shift);

	return spi_write(smb380->spi, &tx, 2);
}
static int smb380_info_get(struct device *dev, struct zio_attribute *zattr,
			   uint32_t *usr_val)
{
	const struct zio_field_desc *reg;
	struct smb380 *smb380;
	uint16_t tx = 0, rx = 0;
	struct spi_transfer t;
	struct spi_message m;
	int err;

	pr_info("%s:%d 0x%x/0x%x", __func__, __LINE__, tx, rx);
	smb380 = get_smb380(dev);
	if (!smb380)
		return -ENODEV;

	spi_message_init(&m);
	memset(&t, 0, sizeof(struct spi_transfer));
	t.tx_buf = &tx;
	t.rx_buf = &rx;
	t.len = 2;
	spi_message_add_tail(&t, &m);
	pr_info("%s:%d 0x%x/0x%x", __func__, __LINE__, tx, rx);
	/* get the register descriptor for the attribute */
	reg = &smb380_regs[zattr->id];
	tx = (reg->addr | 0x80) << 8; /* first bit high to perform read */
	err = spi_sync(smb380->spi, &m);
	if (err)
		return err;

	pr_info("%s:%d 0x%x/0x%x", __func__, __LINE__, tx, rx);
	*usr_val = (rx >> reg->shift) & reg->mask;
	return 0;
}

/* read from smb380 and return the pointer to the data */
static void smb380_complete(void *cont)
{
	struct smb380_context *context = (struct smb380_context *) cont;
	struct zio_channel *chan;
	struct zio_cset *cset;
	uint16_t *buf;
	uint8_t *rx;
	int i, j = 0, rx_i;

	cset = context->cset;
	rx = context->rx;
	/* demux data */
	chan_for_each(chan, cset) {
		buf = (uint16_t *)chan->active_block->data;
		for (i = 0; i < context->nsamples; ++i) {
			rx_i = i * cset->n_chan + j;
			/* axis-LO >> 6 | axis-HI << 2 */
			buf[i] = (rx[rx_i] >> 6) | (rx[rx_i + 1] << 2);
			/* FIXME a define for these offset? */
		}
		j++;
	}
	zio_trigger_data_done(cset);
	/* free context */
	kfree(context->tx);
	kfree(context->rx);
	kfree(context->trans);
	kfree(context);
}
static int smb380_input_cset(struct zio_cset *cset)
{
	struct smb380 *smb380 = cset->zdev->priv_d;
	struct smb380_context *context;
	uint32_t nsamples;
	uint8_t *tx, *rx;
	int err, i, n;

	/* alloc context */
	context = kzalloc(sizeof(struct smb380_context), GFP_ATOMIC);
	if (!context)
		return -ENOMEM;

	context->chan_enable = zio_get_n_chan_enabled(cset);

	/* prepare SPI message and transfer */
	nsamples = cset->chan->current_ctrl->nsamples;
	context->nsamples = nsamples;
	n = context->chan_enable * (nsamples + 1);
	context->trans = kzalloc(sizeof(struct spi_transfer) * nsamples,
				 GFP_ATOMIC);
	context->tx = kzalloc(sizeof(uint8_t) * n, GFP_ATOMIC);
	context->rx = kzalloc(sizeof(uint8_t) * n, GFP_ATOMIC);
	if (!context->trans || !context->tx || !context->rx) {
		err = -ENOMEM;
		goto out;
	}
	dev_info(&cset->head.dev, "%s:%d", __func__, __LINE__);
	spi_message_init(&context->msg);
	context->msg.complete = smb380_complete;
	context->msg.context = context;
	context->cset = cset;
	/*
	 * SMB380 need different transfer for different samples because if
	 * CS stay active SMB380 retrieve data automatically from the next
	 * mememory address.
	 *
	 * FIXME active this option
	 *
	 * For each samples we read the three axies. We set the first address
	 * to read (LSB of axis x) and then we read 6byte (to read all the axis)
	 */
	tx = context->tx;
	rx = context->rx;
	for (i = 0; i < nsamples; ++i) {
		context->trans[i].cs_change = 1; /* turn CS off */
		context->trans[i].delay_usecs = 1; /* FIXME calculate */
		*tx = smb380_regs[SMB380_AXIS_X_LO].addr & 0x80;
		context->trans[i].tx_buf = tx;
		context->trans[i].rx_buf = rx;
		context->trans[i].len = 6;
		spi_message_add_tail(&context->trans[i], &context->msg);
		tx+=6;
		rx+=6;
	}
	/* start acquisition */
	err = spi_async_locked(smb380->spi, &context->msg);
	if (!err)
		return -EAGAIN;
	return 0;
out:
	kfree(context->tx);
	kfree(context->rx);
	kfree(context->trans);
	kfree(context);
	return err;
}

struct zio_sysfs_operations smb380_s_op = {
	.conf_set = smb380_conf_set,
	.info_get = smb380_info_get,
};

/* channels available */
static struct zio_channel smb380_ain_chan[] = {
	{
		ZIO_SET_OBJ_NAME("axis-x"),
		.zattr_set = {
			.std_zattr = zattr_chan_x,
		},
	},
	{
		ZIO_SET_OBJ_NAME("axis-y"),
		.zattr_set = {
			.std_zattr = zattr_chan_y,
		},
	},
	{
		ZIO_SET_OBJ_NAME("axis-z"),
		.zattr_set = {
			.std_zattr = zattr_chan_z,
		},
	}
};

/* channel sets available */
static struct zio_cset smb380_ain_cset[] = { /* SMB380 cset */
	{
		.raw_io = smb380_input_cset,
		.ssize = 2,	/* 10 or 8 bit*/
		.chan = smb380_ain_chan,
		.n_chan = ARRAY_SIZE(smb380_ain_chan),
		.flags = ZIO_CSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};

static struct zio_device smb380_tmpl = {
	.owner = THIS_MODULE,
	.s_op = &smb380_s_op,
	.flags = 0,
	.cset = smb380_ain_cset,
	.n_cset = ARRAY_SIZE(smb380_ain_cset),
	.zattr_set = {
		.std_zattr = smb380_zattr_zdev,
		.ext_zattr = smb380_zattr_zdev_ext,
		.n_ext_attr = ARRAY_SIZE(smb380_zattr_zdev_ext),
	},
};

static int smb380_zio_probe(struct zio_device *zdev)
{
	/* Set the private data on every hierachy level for fast access */


	return 0;
}

static const struct zio_device_id smb380_table[] = {
	{"smb380", &smb380_tmpl},
	{},
};
static struct zio_driver smb380_zdrv = {
	.driver = {
		.name = "zio-smb380",
		.owner = THIS_MODULE,
	},
	.id_table = smb380_table,
	.probe = smb380_zio_probe,
};

static int __devinit smb380_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *spi_id;
	struct smb380 *smb380;
	int err = 0;
	uint32_t dev_id;

	pr_info("%s:%d\n", __func__, __LINE__);
	smb380 = kzalloc(sizeof(struct smb380), GFP_KERNEL);
	if (!smb380)
		return -ENOMEM;
	/* Configure SPI */
	spi_set_drvdata(spi, smb380);
	spi->bits_per_word = 16;
	err = spi_setup(spi);
	if (err)
		return err;
	spi_id = spi_get_device_id(spi);
	if (!spi_id)
		return -ENODEV;
	smb380->spi = spi;
	smb380->zdev_empty = zio_allocate_device(); /* The empty device */
	smb380->zdev_empty->priv_d = smb380;
	smb380->zdev_empty->owner = THIS_MODULE;

	dev_id = spi->chip_select | (spi->master->bus_num << 8);

	/* Register a ZIO device */
	err= zio_register_device(smb380->zdev_empty, spi_id->name, dev_id);
	if (err)
		kfree(smb380);
	return err;
}

static int __devexit smb380_spi_remove(struct spi_device *spi)
{
	struct smb380 *smb380;

	pr_info("%s:%d\n", __func__, __LINE__);
	smb380 = spi_get_drvdata(spi);
	if (smb380->zdev_empty)
		zio_unregister_device(smb380->zdev_empty);
	kfree(smb380);
	return 0;
}

static struct spi_driver smb380_driver = {
	.driver = {
		.name	= "smb380",
		.bus	= &spi_bus_type,
		.owner	= THIS_MODULE,
	},
	.id_table	= smb380_id,
	.probe		= smb380_spi_probe,
	.remove		= __devexit_p(smb380_spi_remove),
};

static int __init smb380_init(void)
{
	int err;

	if (smb380_trigger)
		smb380_tmpl.preferred_trigger = smb380_trigger;
	if (smb380_buffer)
		smb380_tmpl.preferred_buffer = smb380_buffer;
	err = zio_register_driver(&smb380_zdrv);
	if (err)
		return err;
	return spi_register_driver(&smb380_driver);
}
static void __exit smb380_exit(void)
{
	driver_unregister(&smb380_driver.driver);
	zio_unregister_driver(&smb380_zdrv);
}

module_init(smb380_init);
module_exit(smb380_exit);

MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("SMB380 driver for ZIO framework");
MODULE_LICENSE("GPL");
