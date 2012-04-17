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
 * Maximum number of standard and extended attributes. These two values
 * _cannot_ be changed
 */
#define ZIO_MAX_STD_ATTR 16
#define ZIO_MAX_EXT_ATTR 32

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
	uint32_t std_val[ZIO_MAX_STD_ATTR];
	uint32_t ext_val[ZIO_MAX_EXT_ATTR];
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
	uint8_t more_ctrl;	/* number of further ctrl, for interleaved */
	uint8_t alarms;		/* set by channel, persistent, write 1 to clr */

	/* byte 4*/
	uint32_t seq_num;	/* block sequence number */
	uint32_t flags;		/* endianness etc, see below */
	uint32_t nsamples;	/* number of samples in this data block */

	/* byte 16 */
	uint16_t ssize;		/* sample-size for each of them, in bytes */
	uint16_t nbits;		/* sample-bits: number of valid bits */

	/* byte 20 */
	/* This block of information uniquely identifies the channel */
	uint8_t hostid[8];	/* Macaddress or whatever unique */
	char devname[ZIO_OBJ_NAME_LEN];
	uint32_t dev_id;	/* Driver-specific id */
	uint16_t cset_i;	/* index of channel-set within device */
	uint16_t chan_i;	/* index of channel within cset */

	/* byte 48 */
	struct zio_timestamp tstamp;

	/* byte 72 */
	uint32_t mem_offset;	/* position in mmap buffer of this block */
	uint32_t reserved;	/* possibly another offset, or space for 64b */

	/* byte 80 */
	/* Each data block is associated with a trigger and its features */
	char triggername[ZIO_OBJ_NAME_LEN];

	/* byte 92 */
	struct zio_ctrl_attr attr_channel;
	struct zio_ctrl_attr attr_trigger;

	/* byte 492 */
	uint8_t __fill_end[ZIO_CONTROL_SIZE - 492];
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
