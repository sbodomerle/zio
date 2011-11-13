/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
/*
 * zero-zio is a simple zio driver which fill buffer with 0. From the ZIO
 * point of view is an input device
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/random.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>

int zzero_input(struct zio_channel *chan, struct zio_block *block)
{
	static uint8_t datum;
	uint8_t *data;
	int i;

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
	return 0;
}

static struct zio_device_operations zzero_d_op = {
	.input_block =		zzero_input,
};
static struct zio_cset zzero_cset[] = {
	{
		.n_chan =	3,
		.ssize =	1,
		.flags =	ZCSET_TYPE_ANALOG | ZCSET_DIR_INPUT,
	},
};
static struct zio_device zzero_dev = {
	.d_op =			&zzero_d_op,
	.cset =			zzero_cset,
	.n_cset =		ARRAY_SIZE(zzero_cset),

};

static int __init zzero_init(void)
{
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
