/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
/*
 * zio-zero is a simple zio driver, with both input and output.  The
 *  channels are completely software driven. The input channels fill
 *  the data block with zeroes, random data and sequential numbers,
 *  respectively. The output channel just discards data it receives.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/random.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>

ZIO_PARAM_TRIGGER(zzero_trigger);
ZIO_PARAM_BUFFER(zzero_buffer);

static int zzero_input(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;
	static uint8_t datum;
	uint8_t *data;
	int i;

	/* Return immediately: just fill the blocks */
	cset_for_each(cset, chan) {
		block = chan->active_block;
		if (!block)
			continue;
		switch (chan->index) {
		case 0: /* zero */
			memset(block->data, 0x0, block->datalen);
			break;
		case 1: /* random */
			get_random_bytes(block->data, block->datalen);
			break;
		case 2: /* sequence */
			data = block->data;
			for (i = 0; i < block->datalen; i++)
				data[i] = datum++;
		}
	}
	return 0; /* Already done */
}

static int zzero_output(struct zio_cset *cset)
{
	/* We just eat data, like /dev/zero and /dev/null */
	return 0; /* Already done */
}

static const struct zio_device_operations zzero_d_op = {
	.input_cset =		zzero_input,
	.output_cset =		zzero_output,
};

static struct zio_cset zzero_cset[] = {
	{
		SET_OBJECT_NAME("zero-input"),
		.n_chan =	3,
		.ssize =	1,
		.flags =	ZIO_DIR_INPUT | ZCSET_TYPE_ANALOG,
	},
	{
		SET_OBJECT_NAME("zero-output"),
		.n_chan =	1,
		.ssize =	1,
		.flags =	ZIO_DIR_OUTPUT | ZCSET_TYPE_ANALOG,
	},
};

static struct zio_device zzero_dev = {
	.owner =		THIS_MODULE,
	.d_op =			&zzero_d_op,
	.cset =			zzero_cset,
	.n_cset =		ARRAY_SIZE(zzero_cset),
};

static int __init zzero_init(void)
{
	if (zzero_trigger)
		zzero_dev.preferred_trigger = zzero_trigger;
	if (zzero_buffer)
		zzero_dev.preferred_buffer = zzero_buffer;
	return zio_register_dev(&zzero_dev, "zzero");
}

static void __exit zzero_exit(void)
{
	zio_unregister_dev(&zzero_dev);
}

module_init(zzero_init);
module_exit(zzero_exit);

MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("A zio driver which fakes zero, random and sawtooth input");
MODULE_LICENSE("GPL");
