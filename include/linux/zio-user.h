/*
 * Alessandro Rubini, Federico Vaga for CERN, 2011, GNU GPLv2 or later
 *
 * This header must be included by user space to make sense of the
 * control structure. Currently no other ZIO headers are meant to
 * be accessed from user space.
 */
#ifndef __ZIO_USER_H__
#define __ZIO_USER_H__

/*
 * Data transfers on the control channel only happen by half a kB.
 * This is fixed for forward compatibility; zio_control may have more
 * fields in the future, and current apps should handle it.
 */
#define ZIO_CONTROL_SIZE	512

/*
 * The timestamp is mostly app-specific. It cam be timspec-alike but
 * indidual devices may do whatever they want to match hardware.
 */
struct zio_timestamp {
	uint64_t secs;
	uint64_t ticks;
	uint64_t bins;
};

/*
 * Attributes are the same number for channels and triggers: 16 standard
 * and 32 extended attrs, with two associated masks and a filler to align.
 * The size, used later to calculate ctrl size, is  4 * (2 + 16 + 32) = 200
 */
struct zio_ctrl_attr {
	uint16_t std_mask;
	uint16_t unused;
	uint32_t ext_mask;
	uint32_t std_val[16];
	uint32_t ext_val[32];
};

/*
 * The following data item is the control structure that is being exchanged
 * on the control device associated to each data device. The size of each
 * fields is fixed to ease portability of binary dumps (esp i386/x86-64).
 * The endianness, however, is native for speed reasons.
 */

struct zio_control {
	/* byte 0 */
	uint8_t major_version;
	uint8_t minor_version;
	uint8_t unused[2];
	/* byte 4*/
	uint32_t seq_num;	/* block sequence number */
	uint32_t flags;		/* endianness etc, see below */
	uint32_t nsamples;	/* number of samples in this data block */
	/* byte 16 */
	uint16_t ssize;		/* sample-size for each of them, in bytes */
	uint16_t sbits;		/* sample-bits: number of valid bits */
	uint16_t cset_i;	/* index of channel-set within device */
	uint16_t chan_i;	/* index of channel within cset */

	/* byte 24 */
	/* The control block includes what device the data belong to */
	char devname[ZIO_NAME_LEN];

	/* byte 56 */
	/* Each data block is associated with a trigger and its features */
	char triggername[ZIO_NAME_LEN];

	/* byte 88 */
	struct zio_timestamp tstamp;

	/* byte 112 */
	uint32_t ext_attr_mask;	/* mask of active extended attributes */
	uint32_t std_attr_mask;	/* mask of active standard attributes */
	/* byte 120 */
	uint32_t std_attrs[32];	/* value of each standard attribute */
	uint32_t ext_attrs[32];	/* value of each extended attribute */

	/* This filler must be updated if you change fields above */
	uint8_t __fill_end[ZIO_CONTROL_SIZE - 120 - 4 * (32 + 32)];
};

/* The following flags are used in the control structure */
#define ZIO_CONTROL_LITTLE_ENDIAN	0x01000001
#define ZIO_CONTROL_BIG_ENDIAN		0x02000002

#define ZIO_CONTROL_MSB_ALIGN		0x00000004 /* for analog data */
#define ZIO_CONTROL_LSB_ALIGN		0x00000008 /* for analog data */

#ifdef __KERNEL__
/*
 * Compile-time check that the control structure is the right size.
 * This should leave in the same header as the structure it checks,
 * even if BUILD_BUG_ON is not available for user applications.
 */
static inline void __unused_check_size(void)
{
	/* if zio_control is smaller then ZIO_CONTROL_SIZE, compile error */
	static int __used v1[sizeof(struct zio_control) - ZIO_CONTROL_SIZE];
	/* if zio_control is greater then ZIO_CONTROL_SIZE, compile error */
	static int __used v2[ZIO_CONTROL_SIZE - sizeof(struct zio_control)];

	BUILD_BUG_ON(sizeof(struct zio_control) != ZIO_CONTROL_SIZE);
}

#endif /* __KERNEL__ */

#endif /* __ZIO_USER_H__ */
