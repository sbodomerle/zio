/*
 * Copyright 2012 Federico Vaga <federico.vaga@gmail.com>
 *
 * Licensed under the GPL or later.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/delay.h>

#include <linux/platform_device.h>

#include <linux/platform_data/at91_adc.h>
#include <mach/at91_adc.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include <linux/zio-utils.h>

/*
 * allow_sw_trigger is a flag used to enable software solicitation to start an
 * acquisition. The software trigger is enabled by default.
 *
 * If you allow the use of software trigger, you can use any software trigger
 * like "timer", "user" or "gpio" instead of the ZAT91 ADC hardware trigger.
 * More over, you can use the software trigger with the ZAT91 ADC trigger by
 * writing in the sysfs attribute "fire"
 */
static int allow_sw_trigger = 1;
module_param(allow_sw_trigger, int, 0444);

/* Choose prefered trigger and buffer from module parameter interface */
ZIO_PARAM_TRIGGER(zat91_adc_trigger);
ZIO_PARAM_BUFFER(zat91_adc_buffer);

/* zat91_adc This structure represent the AT91 ADC */
struct zat91_adc {
	struct platform_device	*pdev;
	struct zio_device	*zdev;	/* real zio device */
	void __iomem		*base;	/* Register base address */
	int			irq;	/* IRQ number */
	uint32_t		flags;
	struct clk		*clk;	/* clk interface for the master clock */
	struct clk		*adc_clk; /* clk interface for the ADC clock */
};

enum zat91_adc_flags {
	ZAT91_ADC_SW_TRIGGER = 0x1,	/* Use software trigger fire */
};

/* Represent the status of the cset during an acquisition of N samples */
struct zat91_cset_status {
	/* number of acquisition for the current trigger run */
	unsigned int n_acq;
	/* bit mask of channel to acquire */
	unsigned int chan_mask;
	/* bit mask of channel to acquire currently */
	unsigned int chan_mask_cur;
};

#define ZAT91_TRG_NAME "at91_trg"

/* enumberate the AT91 ADC register's values */
enum zat91_adc_r_enum {
	ZAT91_ADC_SWRST = 0,
	ZAT91_ADC_START,
	ZAT91_ADC_TRGEN,
	ZAT91_ADC_TRGSEL,
	ZAT91_ADC_LOWRES,
	ZAT91_ADC_SLEEP,
	ZAT91_ADC_PRESCAL,
	ZAT91_ADC_STARTUP,
	ZAT91_ADC_SHTIM,
	ZAT91_ADC_CHER,
	ZAT91_ADC_CHDR,
	ZAT91_ADC_CHSR,
	ZAT91_ADC_SR,
	ZAT91_ADC_LCDR,
	ZAT91_ADC_IER,
	ZAT91_ADC_IDR,
	ZAT91_ADC_IMR,
	ZAT91_ADC_CDR0,
	ZAT91_ADC_CDR1,
	ZAT91_ADC_CDR2,
	ZAT91_ADC_CDR3,
	ZAT91_NOADDR,
};

#define ZAT91_ADC_INT_MASK 0x00000F0F
/* Enumerate the AT91 ADC interrupts */
enum zat91_interrupt {
	/* End of Conversion */
	ZAT91_EOC0 = 0x000001,
	ZAT91_EOC1 = 0x000002,
	ZAT91_EOC2 = 0x000004,
	ZAT91_EOC3 = 0x000008,
	/* Overrun */
	ZAT91_OVR0 = 0x000100,
	ZAT91_OVR1 = 0x000200,
	ZAT91_OVR2 = 0x000400,
	ZAT91_OVR3 = 0x000800,
	/* Data Ready */
	ZAT91_DRDY = 0x010000,
	/* General Over Run */
	ZAT91_GOVR = 0x020000,
	/* End of Recive buffer */
	ZAT91_ENDR = 0x040000,
	/* Recive Buffer Full */
	ZAT91_RFUL = 0x080000,
};

/* Declaration of the AT91 ADC registers */
static struct zio_field_desc zat91_adc_r[] = {
	/* ADC_CR */
	[ZAT91_ADC_SWRST] =	{ 0x00, 0x000000001, 0},
	[ZAT91_ADC_START] =	{ 0x00, 0x000000001, 1},
	/* ADC_MR */
	[ZAT91_ADC_TRGEN] =	{ 0x04, 0x000000001, 0},
	[ZAT91_ADC_TRGSEL] =	{ 0x04, 0x00000000E, 1},
	[ZAT91_ADC_LOWRES] =	{ 0x04, 0x000000010, 4},
	[ZAT91_ADC_SLEEP] =	{ 0x04, 0x000000020, 5},
	[ZAT91_ADC_PRESCAL] =	{ 0x04, 0x000003F00, 8},
	[ZAT91_ADC_STARTUP] =	{ 0x04, 0x0001F0000, 16},
	[ZAT91_ADC_SHTIM] =	{ 0x04, 0x00F000000, 24},
	/* ADC_CHER */
	[ZAT91_ADC_CHER] =	{ 0x10, 0x00000000F, 0},
	/* ADC_CHDR */
	[ZAT91_ADC_CHDR] =	{ 0x14, 0x00000000F, 0},
	/* ADC_CHSR */
	[ZAT91_ADC_CHSR] =	{ 0x18, 0x00000000F, 0},
	/* ADC_SR */
	[ZAT91_ADC_SR] =	{ 0x1C, 0x0F0F0F, 0},

	/* ADC_LCDR - don't export this as ZIO attribute */
	[ZAT91_ADC_LCDR] =	{ 0x20, 0x0000003FF, 0},
	/* ADC_IER */
	[ZAT91_ADC_IER] =	{ 0x24, 0x0000F0F0F, 0},
	/* ADC_IDR */
	[ZAT91_ADC_IDR] =	{ 0x28, 0x0000F0F0F, 0},
	/* ADC_IMR */
	[ZAT91_ADC_IMR] =	{ 0x2C, 0x0000F0F0F, 0},

	/* ADC_CDR0 */
	[ZAT91_ADC_CDR0] =	{ 0x30, 0x0000003FF, 0},
	/* ADC_CDR1 */
	[ZAT91_ADC_CDR1] =	{ 0x34, 0x0000003FF, 0},
	/* ADC_CDR2 */
	[ZAT91_ADC_CDR2] =	{ 0x38, 0x0000003FF, 0},
	/* ADC_CDR3 */
	[ZAT91_ADC_CDR3] =	{ 0x3C, 0x0000003FF, 0},
};

static struct zio_driver zat91_adc_zdrv;

/* To read an ADC register */
static uint32_t zat91_adc_read_mask(struct zat91_adc *zat91, int index)
{
	uint32_t tmp;
	tmp =  readl(zat91->base + zat91_adc_r[index].addr);
	return zio_get_field(&zat91_adc_r[index], tmp);
}
/* To write and ADC register */
static void zat91_adc_write_mask(struct zat91_adc *zat91, uint32_t val,
				 int index)
{
	uint32_t tmp;

	tmp = readl(zat91->base + zat91_adc_r[index].addr);
	tmp = zio_set_field(&zat91_adc_r[index], tmp, val);
	writel(val, zat91->base + zat91_adc_r[index].addr);
}

static void zat91_adc_writel(struct zat91_adc *zat91, uint32_t val, int index)
{
	writel(val, zat91->base + zat91_adc_r[index].addr);
}

/*
 * The function start the acquisition:
 * - if trigger is HW, device does not need SW solicitation;
 * - if trigger is SW, we must force the acquisition.
 */
static void zg20_acquire(struct zat91_adc *zat91)
{
	if (!allow_sw_trigger || !(zat91->flags & ZAT91_ADC_SW_TRIGGER))
		return; /* We are using hw trigger, it will fire by hw*/

	/* Force start acquisition because we are using software trigger */
	zat91_adc_writel(zat91, 0x2, ZAT91_ADC_START);
}

/* The function returns the private data zat91_adc from a generic device */
static struct zat91_adc *__get_zat91(struct device *dev)
{
	struct zio_obj_head *head = to_zio_head(dev);
	struct zat91_adc *zat91;

	switch (head->zobj_type) {
	case ZIO_DEV:
		zat91 = to_zio_dev(dev)->priv_d;
		break;
	case ZIO_TI:
		zat91 = to_zio_ti(dev)->cset->zdev->priv_d;
		break;
	case ZIO_CSET:
		zat91 = to_zio_cset(dev)->zdev->priv_d;
		break;
	case ZIO_CHAN:
		zat91 = to_zio_chan(dev)->cset->zdev->priv_d;
		break;
	default:
		zat91 = NULL;
	}

	return zat91;
}


/* * * * * * * * * * * * * * * * * Interrupt * * * * * * * * * * * * * * * * */
static void zat91_adc_irq_enable(struct zat91_adc *zat91, unsigned int e)
{
	unsigned int interrupt;

	/*
	 * When enable interrupts we handle the following signals:
	 * - OVRx (0x000F00): over run on on channel x
	 * - EOCx (0x00000F): end of conversion on channel x
	 */
	interrupt = e ? ZAT91_ADC_INT_MASK : 0x0;
	/* Enable interrupts */
	zat91_adc_writel(zat91, interrupt, ZAT91_ADC_IER);
	/* Disable receive buffer interrupts */
	zat91_adc_writel(zat91, ~interrupt, ZAT91_ADC_IDR);
	/* Mask interrupts */
	interrupt = zat91_adc_read_mask(zat91, ZAT91_ADC_IMR);
	dev_info(&zat91->zdev->head.dev,"interrupts 0x%x\n", interrupt);
}

/*
 * lost one or more sample, but it is stored anyway. Alternative
 * way to handle this interrupt is to discard all channels data
 * for the current sample acquisition and move to the next sample.
 * this because if overrun occur, something is going wrong during
 * this sample acquisition
 */
static void zat91_adc_irq_hdl_ovr(struct zat91_adc *zat91)
{
	dev_info(&zat91->zdev->head.dev,"lost one or more samples\n");
}

static inline void zat91_adc_mark_acquired(struct zat91_cset_status *cset_st,
					   struct zio_channel *chan)
{
	/* Mark the channel as acquired */
	cset_st->chan_mask_cur &= (~(1 << chan->index));
}
/*
 * On AT91 there is a single ADC shared between 4 channel. Conversions
 * start from channel 0 to channel 3; when DRDY interrupt occurs a conversion
 * is just ended and the result is available in LCD. By reading EOCx bit we know
 * which conversion end.
 */
static void zat91_adc_irq_hdl_dtdy(struct zat91_adc *zat91, uint32_t adc_sr)
{
	struct zio_cset *cset = zat91->zdev->cset;
	struct zat91_cset_status *cset_st = cset->priv_d;
	struct zio_channel *chan;
	unsigned int i;
	uint16_t *data, val;

	pr_info("Fetch data ...\n");

	/* Acquire all available data */
	chan_for_each(chan, cset) {
		i = chan->index;
		if (!(adc_sr & (ZAT91_EOC0 << i))) {
			dev_info(&chan->head.dev, "Data not ready\n");
			continue; /* Sample is not ready yet for this channel */
		}

		/*
		 * If we are expecting a sample from this channel, then
		 * store it
		 */
		if (cset_st->chan_mask_cur & (1 << i)) {
			/* Store data and reset EOCx interrupt */
			val = zat91_adc_read_mask(zat91, ZAT91_ADC_CDR0 + i);
			/* Filter if is 8bit (1)*/
			if (cset->zattr_set.std_zattr[ZIO_ATTR_NBITS].value)
				val &= 0xFF;

			zat91_adc_mark_acquired(cset_st, chan);
			dev_info(&chan->head.dev, "sample 0x%x (block %p)\n",
				 val, chan->active_block);

			if (!chan->active_block)
				continue; /* We cannot store data. Next */

			/* Retrieve pointer to store data */
			data = (uint16_t *)(chan->active_block->data +
					    (cset_st->n_acq * cset->ssize));
			*data = val;
		} else {
			dev_info(&chan->head.dev, "Unexpected sample\n");
		}
	}

	/* Prepare cset_st for the next channel's acquisitions */
	if (cset_st->chan_mask_cur != 0x0)
		return; /* Missing channel */

	/* All channel acquired */
	cset_st->chan_mask_cur = cset_st->chan_mask;
	cset_st->n_acq++;
	/*
	 * If we acquire all the sample, invoke data_done and disable
	 * the interrupt, otherwise acquire again
	 */
	dev_info(&zat91->zdev->head.dev, "sample %d/%d\n",
		  cset_st->n_acq, cset->chan->current_ctrl->nsamples);
	if (cset_st->n_acq == cset->chan->current_ctrl->nsamples) {
		zat91_adc_irq_enable(zat91, 0);
		zio_trigger_data_done(cset);
	} else {
		zg20_acquire(zat91);
	}
}

/*
 * This is the main function of the interrupt handler. It reads the status of
 * the interrupt and it invokes particular function to handle different
 * situation
 */
static irqreturn_t zat91_adc_irq_hdl(int irq, void *dev_id) {

	struct zat91_adc *zat91 = dev_id;
	uint32_t adc_sr;

	/* Reading ADC_SR. It clears interrupt GOV e OVRx */
	adc_sr = zat91_adc_read_mask(zat91, ZAT91_ADC_SR);

	dev_info(&zat91->zdev->head.dev, "Interrupt status= 0x%x\n", adc_sr);
	if (adc_sr & ZAT91_GOVR)
		zat91_adc_irq_hdl_ovr(zat91);

	/* At least one conversion is completed */
	if (adc_sr & (ZAT91_EOC0 | ZAT91_EOC1 | ZAT91_EOC2 | ZAT91_EOC3))
		zat91_adc_irq_hdl_dtdy(zat91, adc_sr);

	/* FIXME don't handle ENDRX BUFFRX. Don't understand how they work */

	return IRQ_HANDLED;
}


/* * * * * * * * * * * * * * * * * * Trigger * * * * * * * * * * * * * * * * */
enum zat91_trg_sel {
	ZAT91_TRG_TIOA0	= 0x0,
	ZAT91_TRG_TIOA1	= 0x1,
	ZAT91_TRG_TIOA2	= 0x2,
	ZAT91_TRG_EXT	= 0x6,
};

/* Trigger attributes */
static ZIO_ATTR_DEFINE_STD(ZIO_TRG, zat91_adc_attr_trg_std) = {
	ZIO_ATTR(trig, ZIO_ATTR_TRIG_POST_SAMP, S_IRUGO | S_IWUGO,
		 ZAT91_NOADDR, 1),
};
static struct zio_attribute zat91_adc_attr_trg_ext[] = {
	/*
	 * Select the source of the hardware trigger
	 * 0x0 TIOA0
	 * 0x1 TIOA1
	 * 0x2 TIOA2
	 * 0x6 External
	 */
	ZIO_ATTR_EXT("select", S_IRUGO | S_IWUGO, ZAT91_ADC_TRGSEL, 0x0),
	/* HW ignore trigger if it fired during a conversion sequence */
	ZIO_PARAM_EXT("fire", S_IWUGO, ZAT91_ADC_START, 0x0),
};

static int zat91_adc_trg_conf_set(struct device *dev,
				  struct zio_attribute *zattr,
				  uint32_t usr_val)
{
	struct zat91_adc *zat91 = __get_zat91(dev);
	struct zio_ti *ti = to_zio_ti(dev);
	uint32_t val = usr_val;

	dev_info(dev, "%s:%d\n", __func__, __LINE__);
	switch (zattr->id) {
	case ZAT91_ADC_TRGSEL:
		if (usr_val > 0x2 && usr_val != 0x6) {
			dev_err(dev, "invalid trigger selection\n");
			return -EINVAL;
		}
		break;
	case ZAT91_ADC_START:
		/* don't fire during conversion */
		dev_info(dev, "Software Fire\n");
		zio_arm_trigger(ti);
		zat91_adc_writel(zat91, 0x2, ZAT91_ADC_START);
		return 0;
	default:
		return 0;
	}

	zat91_adc_write_mask(zat91, val, zattr->id);
	return 0;
}
static int zat91_adc_trg_info_get(struct device *dev,
				  struct zio_attribute *zattr,
				  uint32_t *usr_val)
{
	struct zat91_adc *zat91 = __get_zat91(dev);

	if (zattr->id == ZAT91_NOADDR)
		return 0;

	dev_info(dev, "%s:%d\n", __func__, __LINE__);
	*usr_val = zat91_adc_read_mask(zat91, zattr->id);

	return 0;
}

static const struct zio_sysfs_operations zat91_adc_trg_s_op = {
	.conf_set = zat91_adc_trg_conf_set,
	.info_get = zat91_adc_trg_info_get,
};

/*
 * Create the trigger fot AT91 ADC. This trigger works only with the AT91 ADC
 * so it refuses to register with any other devices. By default it enables the
 * hardware trigger with external source.
 */
static struct zio_ti *zat91_trg_create(struct zio_trigger_type *trig,
				       struct zio_cset *cset,
				       struct zio_control *ctrl, fmode_t flags)
{
	struct zat91_adc *zat91 = cset->zdev->priv_d;
	struct zio_ti *ti;

	/* Verify if it is an AT91 ADC */
	if (cset->zdev->head.dev.driver != &zat91_adc_zdrv.driver) {
		dev_err(&cset->zdev->head.dev,
		       "%s is incompatibile with this device", trig->head.name);
		return ERR_PTR(-EINVAL);
	}

	ti = kzalloc(sizeof(*ti), GFP_KERNEL);
	if (!ti)
		return ERR_PTR(-ENOMEM);

	/* Enable hw trigger */
	zat91_adc_write_mask(zat91, 1, ZAT91_ADC_TRGEN);
	/* Select external trigger */
	zat91_adc_write_mask(zat91, ZAT91_TRG_EXT, ZAT91_ADC_TRGSEL);

	ti->cset = cset;
	return ti;
}
static void zat91_trg_destroy(struct zio_ti *ti)
{
	struct zat91_adc *zat91 = ti->cset->zdev->priv_d;

	/* Disable hw trigger */
	zat91_adc_write_mask(zat91, 0, ZAT91_ADC_TRGEN);
	zat91->flags |= ZAT91_ADC_SW_TRIGGER;
	kfree(ti);
}
/* Enable or disable hardware trigger. The hardware trigger is the prefered
 * trigger, so it correspond to the ZIO enable of the trigger. Status is active
 * low on ZIO but active high on the AT91 ADC, then use '!' on status
 */
static void zat91_adc_trg_cng_sts(struct zio_ti *ti, unsigned int status)
{
	struct zat91_adc *zat91 = ti->cset->zdev->priv_d;

	zat91_adc_write_mask(zat91, !status, ZAT91_ADC_TRGEN);
}

static const struct zio_trigger_operations zat91_adc_trigger_ops = {
	.create = zat91_trg_create,
	.destroy = zat91_trg_destroy,
	.change_status = zat91_adc_trg_cng_sts,
	.config = zio_internal_trig_config,
};

static struct zio_trigger_type zat91_adc_trg_type = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = zat91_adc_attr_trg_std,
		.ext_zattr = zat91_adc_attr_trg_ext,
		.n_ext_attr = ARRAY_SIZE(zat91_adc_attr_trg_ext),
	},
	.s_op = &zat91_adc_trg_s_op,
	.t_op = &zat91_adc_trigger_ops,
};


/* * * * * * * * * * * * * * * * * Device * * * * * * * * * * * * * * * * */
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zat91_adc_attr_cset_std) = {
	/* valid value: 10 and 8 (0 and 1 on the register) */
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, S_IRUGO | S_IWUGO, ZAT91_ADC_LOWRES, 10),
};

static struct zio_attribute zat91_adc_attr_cset_ext[] = {
	ZIO_ATTR_EXT("prescale",S_IRUGO | S_IWUGO, ZAT91_ADC_PRESCAL, 0),
	ZIO_ATTR_EXT("startup", S_IRUGO | S_IWUGO, ZAT91_ADC_STARTUP, 0),
	ZIO_ATTR_EXT("sample-and-hold",S_IRUGO | S_IWUGO, ZAT91_ADC_SHTIM, 0),
	ZIO_PARAM_EXT("reset", S_IRUGO | S_IWUGO, ZAT91_ADC_SWRST, 0),
	ZIO_PARAM_EXT("sleep", S_IRUGO | S_IWUGO, ZAT91_ADC_SLEEP, 0),
	ZIO_PARAM_EXT("last-sample", S_IRUGO, ZAT91_ADC_LCDR, 0),
};

/* Device's register configuration */
static int zat91_adc_conf_set(struct device *dev, struct zio_attribute *zattr,
			 uint32_t usr_val)
{
	struct zat91_adc *zat91 = __get_zat91(dev);
	uint32_t val = usr_val;

	dev_info(dev, "%s:%d\n", __func__, __LINE__);
	switch (zattr->id) {
	case ZAT91_ADC_LOWRES:
		if (val != 8 || val != 10) {
			dev_err(dev, "invalid resolution bits\n");
			return -EINVAL;
		}
		val = (val == 10 ? 0 : 1);
		break;
	case ZAT91_ADC_SWRST:
		/* don't reset if acquisition is running */
		return 0;
		break;
	}

	zat91_adc_write_mask(zat91, val, zattr->id);
	return 0;
}
/* Device's register current value */
static int zat91_adc_info_get(struct device *dev, struct zio_attribute *zattr,
			 uint32_t *usr_val)
{
	struct zat91_adc *zat91 = __get_zat91(dev);

	dev_info(dev, "%s:%d\n", __func__, __LINE__);
	*usr_val = zat91_adc_read_mask(zat91, zattr->id);

	/* Show resolution bit for human */
	if (zattr->id == ZAT91_ADC_LOWRES)
		*usr_val = (*usr_val == 0 ? 10 : 8);

	return 0;
}

static const struct zio_sysfs_operations zat91_adc_s_op = {
	.conf_set = zat91_adc_conf_set,
	.info_get = zat91_adc_info_get,
};

/* single 10bit rzat91_adc_rister, only 1 sample at time */
static int zat91_input_cset(struct zio_cset *cset)
{

	struct zat91_cset_status *cset_st = cset->priv_d;
	struct zat91_adc *zat91 = cset->zdev->priv_d;
	struct zio_channel *chan;


	/* Initialize acquisition */
	cset_st->n_acq = 0;
	cset_st->chan_mask = 0x0;
	chan_for_each(chan, cset) {
		cset_st->chan_mask |= 1 << chan->index;
	}

	/* Reset the current mask acquisition */
	cset_st->chan_mask_cur = cset_st->chan_mask;
	dev_info(&cset->head.dev, "Acquisition mask 0x%x \n",
		 cset_st->chan_mask_cur);

	/* Enable the interrupt to handle acquisition */
	zat91_adc_irq_enable(zat91, 1);

	/*
	 * Start acquisition of the first sample (next samples will be acquired
	 * by interrupt)
	 */
	zg20_acquire(zat91);

	return -EAGAIN; /* data_done on interrupt DTDY*/
}

/* To use the ADC we must enable clock */
static int zat91_adc_configure_clk(struct zat91_adc *zat91)
{
	struct at91_adc_data *pdata = zat91->pdev->dev.platform_data;
	unsigned long master_clk, adc_clk, prescale, startup;
	int err;

	zat91->clk = devm_clk_get(&zat91->pdev->dev, "adc_clk");
	if (IS_ERR(zat91->clk)) {
		dev_err(&zat91->pdev->dev, "Failed to get the clock.\n");
		return PTR_ERR(zat91->clk);
	}

	err = clk_prepare_enable(zat91->clk);
	if (err) {
		dev_err(&zat91->pdev->dev,
			"Could not prepare or enable the clock.\n");
		return err;
	}

	zat91->adc_clk = devm_clk_get(&zat91->pdev->dev, "adc_op_clk");
	if (IS_ERR(zat91->adc_clk)) {
		dev_err(&zat91->pdev->dev, "Failed to get the ADC clock.\n");
		err = PTR_ERR(zat91->adc_clk);
		goto out_clk;
	}

	err = clk_prepare_enable(zat91->adc_clk);
	if (err) {
		dev_err(&zat91->pdev->dev,
			"Could not prepare or enable the ADC clock.\n");
		goto out_clk;
	}


	master_clk = clk_get_rate(zat91->clk);
	adc_clk = clk_get_rate(zat91->adc_clk);
	/* Formula from the AT91 ADC manual FIXME */
	prescale = (master_clk / (2 * adc_clk)) - 1;
	zat91_adc_write_mask(zat91, prescale, ZAT91_ADC_PRESCAL);

	if (!pdata->startup_time) {
		dev_err(&zat91->pdev->dev, "No startup time available.\n");
		err = -EINVAL;
		goto out_adc_clk;
	}

	/* Formula from the AT91 ADC manual FIXME */
	startup = round_up((pdata->startup_time * adc_clk / 1000000) - 1, 8) / 8;
	zat91_adc_write_mask(zat91, startup, ZAT91_ADC_STARTUP);

	return 0;
out_adc_clk:
	clk_disable_unprepare(zat91->adc_clk);
out_clk:
	clk_disable_unprepare(zat91->clk);
	return err;
}
static void zat91_adc_unconfigure_clk(struct zat91_adc *zat91)
{
	clk_disable_unprepare(zat91->adc_clk);
	clk_disable_unprepare(zat91->clk);
}

/*
 * The function enable/disable a channel
 * @chan: channel to enable/disable
 * @mask: changed mask
 */
static void zat91_adc_chan_enable(struct zio_channel *chan, unsigned long mask)
{
	struct zat91_adc *zat91 =  chan->cset->zdev->priv_d;
	uint32_t cur_val, usr_val;

	if (mask != ZIO_STATUS)
		return; /* we handle only status flag */

	/* Get the current stuatus of the enabled channels */
	cur_val = zat91_adc_read_mask(zat91, ZAT91_ADC_CHSR);

	if (chan->flags & ZIO_STATUS) /* Disable */
		usr_val = (cur_val) & (~(0x1 << chan->index));
	else		/* Enable */
		usr_val = (cur_val) | (0x1 << chan->index);

	/* Write enable configuration to the board */
	zat91_adc_write_mask(zat91, usr_val, ZAT91_ADC_CHER);
	zat91_adc_write_mask(zat91, (~usr_val), ZAT91_ADC_CHDR);
}
static struct zio_channel zat91_chan_tmpl = {
	.change_flags = zat91_adc_chan_enable,
};

/* Initialize cset */
static int zat91_adc_cset_init(struct zio_cset *cset)
{
	struct zat91_adc *zat91 = cset->zdev->priv_d;

	cset->priv_d = kzalloc(sizeof(struct zat91_cset_status), GFP_KERNEL);
	if (!cset->priv_d)
		return -ENOMEM;

	/* Enable all channels */
	zat91_adc_write_mask(zat91, 0xF, ZAT91_ADC_CHER);
	zat91_adc_write_mask(zat91, 0x0, ZAT91_ADC_CHDR);

	/* Disable HW trigger if the trigger is not the AT91 ADC trigger */
	if (cset->trig != &zat91_adc_trg_type) {
		zat91_adc_write_mask(zat91, 0, ZAT91_ADC_TRGEN);
		/* Trigger fire by software */
		zat91->flags |= ZAT91_ADC_SW_TRIGGER;
	}

	return 0;
}
static void zat91_adc_cset_exit(struct zio_cset *cset)
{
	struct zat91_adc *zat91 = cset->zdev->priv_d;

	free_irq(zat91->irq, zat91);
	kfree(cset->priv_d);
}

static struct zio_cset zat91_adc_cset[] = {
	{
		.raw_io = zat91_input_cset,
		.ssize = 2,			/* 8 or 10 bits */
		.chan_template = &zat91_chan_tmpl,
		.n_chan = 4,
		.flags = ZIO_CSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
		.init = zat91_adc_cset_init,
		.exit = zat91_adc_cset_exit,
		.zattr_set = {
			.std_zattr = zat91_adc_attr_cset_std,
			.ext_zattr = zat91_adc_attr_cset_ext,
			.n_ext_attr = ARRAY_SIZE(zat91_adc_attr_cset_ext),
		},
	}
};

static struct zio_device zat91_adc_tmpl = {
	.owner = THIS_MODULE,
	.s_op = &zat91_adc_s_op,
	.config = zio_internal_zdev_config,
	.cset = zat91_adc_cset,
	.n_cset = ARRAY_SIZE(zat91_adc_cset),
	.preferred_trigger = ZAT91_TRG_NAME,
};

static int zat91_adc_probe(struct zio_device *zdev)
{
	struct zat91_adc *zat91 = zdev->priv_d;
	int err;

	pr_info("%s:%d\n", __func__, __LINE__);
	zat91->zdev = zdev;
	/* Reset ADC -- hw trigger is disabled */
	zat91_adc_write_mask(zat91, 0x1, ZAT91_ADC_IER);
	/* Sleep for 1ms to be sure that HW is resetted */
	msleep(1);


	err = zat91_adc_configure_clk(zat91);
	if (err)
		return err;
	/* Disable interrupt and request irq */
	err = request_irq(zat91->irq, zat91_adc_irq_hdl, IRQF_SHARED,
			  KBUILD_MODNAME, (void *)zat91);
	if (err)
		goto out;
	zat91_adc_irq_enable(zat91, 0);

	return 0;
out:
	zat91_adc_unconfigure_clk(zat91);
	return err;


}

static const struct zio_device_id zat91_adc_table[] = {
	{"zat91_adc", &zat91_adc_tmpl},
	{},
};

static struct zio_driver zat91_adc_zdrv = {
	.driver = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
	},
	.id_table = zat91_adc_table,
	.probe = zat91_adc_probe,
};


/* * * * * * * * * * * * * * * * * Platform * * * * * * * * * * * * * * * * */
static int __devinit at91_adc_probe(struct platform_device *pdev)
{
	struct zat91_adc *zat91;
	struct zio_device *zdev;
	struct resource *res;
	int err;

	/* FIXME look ad Timer Counter section to program the G20 trigger */
	err = zio_register_trig(&zat91_adc_trg_type, ZAT91_TRG_NAME);
	if (err)
		goto out;

	/* Register the ZIO driver */
	err = zio_register_driver(&zat91_adc_zdrv);
	if (err)
		goto out_drv;
	/* Allocate ZIO fake device */
	zdev = zio_allocate_device();
	if (IS_ERR(zdev)) {
		err = PTR_ERR(zdev);
		goto out_all;
	}
	/* Allocate private data for the AT91 ADC*/
	zat91 = kzalloc(sizeof(struct zat91_adc), GFP_KERNEL);
	if (!zat91) {
		err = -ENOMEM;
		goto out_all2;
	}
	platform_set_drvdata(pdev, zdev);
	zdev->owner = THIS_MODULE;
	zdev->priv_d = zat91;
	zat91->pdev = pdev;
	zat91->irq = platform_get_irq(pdev, 0);
	if (zat91->irq < 0) {
		dev_err(&pdev->dev, "No IRQ ID is designated\n");
		err = -ENODEV;
		goto out_reg;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	zat91->base = devm_request_and_ioremap(&pdev->dev, res);
	if (!zat91->base) {
		dev_err(&pdev->dev, "cannot request and remap\n");
		err = -ENOMEM;
		goto out_reg;
	}

	if (zat91_adc_trigger)
		zat91_adc_tmpl.preferred_trigger = zat91_adc_trigger;
	if (zat91_adc_buffer)
		zat91_adc_tmpl.preferred_buffer = zat91_adc_buffer;
	/* Register the ZIO fake device */
	err = zio_register_device(zdev, "zat91_adc", 0);
	if (err)
		goto out_reg;
	return 0;

out_reg:
	kfree(zat91);
out_all2:
	zio_free_device(zdev);
out_all:
	zio_unregister_driver(&zat91_adc_zdrv);
out_drv:
	zio_unregister_trig(&zat91_adc_trg_type);
out:
	return err;
}

static int __devexit at91_adc_remove(struct platform_device *pdev)
{
	struct zio_device *zdev =  platform_get_drvdata(pdev);

	zio_unregister_device(zdev);
	kfree(zdev->priv_d); /* AT91 ADC private data */
	zio_free_device(zdev);
	zio_unregister_driver(&zat91_adc_zdrv);
	zio_unregister_trig(&zat91_adc_trg_type);
	return 0;
}

static const struct platform_device_id at91_adc_table[] = {
	{.name = "at91_adc",},
	{},
};
static struct platform_driver zat91_adc_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
	},
	.probe = at91_adc_probe,
	.remove = __devexit_p(at91_adc_remove),
	.id_table = at91_adc_table,

};

module_platform_driver(zat91_adc_driver);

MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("Atmel G20 ADC ZIO driver");
MODULE_LICENSE("GPL");
