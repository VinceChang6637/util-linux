/*
 * Copyright (C) 2013 Rolf Fokkens <rolf@fokkens.nl>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 *
 * Based on code fragments from bcache-tools by Kent Overstreet:
 * http://evilpiepirate.org/git/bcache-tools.git
 */

#include <stddef.h>
#include <stdio.h>

#include "superblocks.h"
#include "crc32c.h"
#include "crc64.h"
#include "xxhash.h"

#define SB_LABEL_SIZE      32

/*
 * The bcache_super_block is heavily simplified version of struct cache_sb in kernel.
 * https://github.com/torvalds/linux/blob/master/include/uapi/linux/bcache.h
 */
struct bcache_super_block {
	uint64_t		csum;
	uint64_t		offset;		/* where this super block was written */
	uint64_t		version;
	uint8_t			magic[16];	/* bcache file system identifier */
	uint8_t			uuid[16];	/* device identifier */
} __attribute__((packed));

struct bcachefs_sb_field {
	uint32_t	u64s;
	uint32_t	type;
}  __attribute__((packed));

struct bcachefs_sb_member {
	uint8_t		uuid[16];
	uint64_t	nbuckets;
	uint16_t	first_bucket;
	uint16_t	bucket_size;
	uint32_t	pad;
	uint64_t	last_mount;
	uint64_t	flags[2];
} __attribute__((packed));

struct bcachefs_sb_field_members {
	struct bcachefs_sb_field	field;
	struct bcachefs_sb_member	members[];
}  __attribute__((packed));

enum bcachefs_sb_csum_type {
	BCACHEFS_SB_CSUM_TYPE_NONE = 0,
	BCACHEFS_SB_CSUM_TYPE_CRC32C = 1,
	BCACHEFS_SB_CSUM_TYPE_CRC64 = 2,
	BCACHEFS_SB_CSUM_TYPE_XXHASH = 7,
};

union bcachefs_sb_csum {
	uint32_t crc32c;
	uint64_t crc64;
	XXH64_hash_t xxh64;
	uint8_t raw[16];
} __attribute__((packed));

struct bcachefs_super_block {
	union bcachefs_sb_csum	csum;
	uint16_t	version;
	uint16_t	version_min;
	uint16_t	pad[2];
	uint8_t		magic[16];
	uint8_t		uuid[16];
	uint8_t		user_uuid[16];
	uint8_t		label[SB_LABEL_SIZE];
	uint64_t	offset;
	uint64_t	seq;
	uint16_t	block_size;
	uint8_t		dev_idx;
	uint8_t		nr_devices;
	uint32_t	u64s;
	uint64_t	time_base_lo;
	uint32_t	time_base_hi;
	uint32_t	time_precision;
	uint64_t	flags[8];
	uint64_t	features[2];
	uint64_t	compat[2];
	uint8_t		layout[512];
	struct bcachefs_sb_field _start[];
}  __attribute__((packed));

/* magic string */
#define BCACHE_SB_MAGIC     "\xc6\x85\x73\xf6\x4e\x1a\x45\xca\x82\x65\xf5\x7f\x48\xba\x6d\x81"
#define BCACHEFS_SB_MAGIC   "\x18\x00\x18\x00\x00\x00\x00\x00\xc6\x85\x73\xf6\x4e\x1a\x45\xca"
/* magic string len */
#define BCACHE_SB_MAGIC_LEN (sizeof(BCACHE_SB_MAGIC) - 1)
/* super block offset */
#define BCACHE_SB_OFF       0x1000
/* supper block offset in kB */
#define BCACHE_SB_KBOFF     (BCACHE_SB_OFF >> 10)
/* magic string offset within super block */
#define BCACHE_SB_MAGIC_OFF offsetof(struct bcache_super_block, magic)
/* start of checksummed data within superblock */
#define BCACHE_SB_CSUMMED_START 8
/* end of checksummed data within superblock */
#define BCACHE_SB_CSUMMED_END 208
/* granularity of offset and length fields within superblock */
#define BCACHEFS_SECTOR_SIZE   512
/* maximum superblock size */
#define BCACHEFS_SB_MAX_SIZE   4096
/* fields offset within super block */
#define BCACHEFS_SB_FIELDS_OFF offsetof(struct bcachefs_super_block, _start)
/* tag value for members field */
#define BCACHEFS_SB_FIELD_TYPE_MEMBERS 1

#define BYTES(f) ((((uint64_t) le32_to_cpu((f)->u64s)) * 8))

static int bcache_verify_checksum(blkid_probe pr, const struct blkid_idmag *mag,
		const struct bcache_super_block *bcs)
{
	unsigned char *csummed = blkid_probe_get_sb_buffer(pr, mag, BCACHE_SB_CSUMMED_END);
	uint64_t csum = ul_crc64_we(csummed + BCACHE_SB_CSUMMED_START,
			BCACHE_SB_CSUMMED_END - BCACHE_SB_CSUMMED_START);
	return blkid_probe_verify_csum(pr, csum, le64_to_cpu(bcs->csum));
}

static int probe_bcache (blkid_probe pr, const struct blkid_idmag *mag)
{
	struct bcache_super_block *bcs;

	bcs = blkid_probe_get_sb(pr, mag, struct bcache_super_block);
	if (!bcs)
		return errno ? -errno : BLKID_PROBE_NONE;

	if (!bcache_verify_checksum(pr, mag, bcs))
		return BLKID_PROBE_NONE;

	if (le64_to_cpu(bcs->offset) != BCACHE_SB_OFF / 512)
		return BLKID_PROBE_NONE;

	if (blkid_probe_set_uuid(pr, bcs->uuid) < 0)
		return BLKID_PROBE_NONE;

	blkid_probe_set_wiper(pr, 0, BCACHE_SB_OFF);

	return BLKID_PROBE_OK;
}

static void probe_bcachefs_sb_members(blkid_probe pr,
				     const struct bcachefs_super_block *bcs,
				     const struct bcachefs_sb_field *field,
				     uint8_t dev_idx)
{
	struct bcachefs_sb_field_members *members =
			(struct bcachefs_sb_field_members *) field;
	uint64_t sectors = 0;
	uint8_t i;

	if (BYTES(field) != offsetof(typeof(*members), members[bcs->nr_devices]))
		return;

	blkid_probe_set_uuid_as(pr, members->members[dev_idx].uuid, "UUID_SUB");

	for (i = 0; i < bcs->nr_devices; i++) {
		struct bcachefs_sb_member *member = &members->members[i];
		sectors += le64_to_cpu(member->nbuckets) * le16_to_cpu(member->bucket_size);
	}
	blkid_probe_set_fssize(pr, sectors * BCACHEFS_SECTOR_SIZE);
}

static void probe_bcachefs_sb_fields(blkid_probe pr, const struct bcachefs_super_block *bcs,
				     unsigned char *sb_start, unsigned char *sb_end)
{
	unsigned char *field_addr = sb_start + BCACHEFS_SB_FIELDS_OFF;

	while (1) {
		struct bcachefs_sb_field *field = (struct bcachefs_sb_field *) field_addr;
		uint64_t field_size;
		uint32_t type;

		if ((unsigned char *) field + sizeof(*field) > sb_end)
			break;

		field_size = BYTES(field);

		if (field_size < sizeof(*field))
			break;

		if ((unsigned char *) field + field_size > sb_end)
			break;

		type = le32_to_cpu(field->type);
		if (!type)
			break;

		if (type == BCACHEFS_SB_FIELD_TYPE_MEMBERS)
			probe_bcachefs_sb_members(pr, bcs, field, bcs->dev_idx);

		field_addr += BYTES(field);
	}
}

static int bcachefs_validate_checksum(blkid_probe pr, const struct bcachefs_super_block *bcs,
				      unsigned char *sb, unsigned char *sb_end)
{
	uint8_t checksum_type = be64_to_cpu(bcs->flags[0]) >> 58;
	unsigned char *checksummed_data_start = sb + sizeof(bcs->csum);
	size_t checksummed_data_size = sb_end - checksummed_data_start;

	switch (checksum_type) {
		case BCACHEFS_SB_CSUM_TYPE_NONE:
			return 1;
		case BCACHEFS_SB_CSUM_TYPE_CRC32C: {
			uint32_t crc = crc32c(~0LL, checksummed_data_start, checksummed_data_size) ^ ~0LL;
			return blkid_probe_verify_csum(pr, crc, le32_to_cpu(bcs->csum.crc32c));
		}
		case BCACHEFS_SB_CSUM_TYPE_CRC64: {
			uint64_t crc = ul_crc64_we(checksummed_data_start, checksummed_data_size);
			return blkid_probe_verify_csum(pr, crc, le64_to_cpu(bcs->csum.crc64));
		}
		case BCACHEFS_SB_CSUM_TYPE_XXHASH: {
			XXH64_hash_t xxh64 = XXH64(checksummed_data_start, checksummed_data_size, 0);
			return blkid_probe_verify_csum(pr, xxh64, le64_to_cpu(bcs->csum.xxh64));
		}
		default:
			DBG(LOWPROBE, ul_debug("bcachefs: unknown checksum type %d, ignoring.", checksum_type));
			return 1;
	}
}

static int probe_bcachefs(blkid_probe pr, const struct blkid_idmag *mag)
{
	struct bcachefs_super_block *bcs;
	unsigned char *sb, *sb_end;
	uint64_t sb_size, blocksize;

	bcs = blkid_probe_get_sb(pr, mag, struct bcachefs_super_block);
	if (!bcs)
		return errno ? -errno : BLKID_PROBE_NONE;

	if (le64_to_cpu(bcs->offset) != BCACHE_SB_OFF / BCACHEFS_SECTOR_SIZE)
		return BLKID_PROBE_NONE;

	if (bcs->nr_devices == 0 || bcs->dev_idx >= bcs->nr_devices)
		return BLKID_PROBE_NONE;

	sb_size = BCACHEFS_SB_FIELDS_OFF + BYTES(bcs);
	if (sb_size > BCACHEFS_SB_MAX_SIZE)
		return BLKID_PROBE_NONE;

	sb = blkid_probe_get_sb_buffer(pr, mag, sb_size);
	if (!sb)
		return BLKID_PROBE_NONE;
	sb_end = sb + sb_size;

	if (!bcachefs_validate_checksum(pr, bcs, sb, sb_end))
		return BLKID_PROBE_NONE;

	blkid_probe_set_uuid(pr, bcs->user_uuid);
	blkid_probe_set_label(pr, bcs->label, sizeof(bcs->label));
	blkid_probe_sprintf_version(pr, "%d", le16_to_cpu(bcs->version));
	blocksize = le16_to_cpu(bcs->block_size);
	blkid_probe_set_block_size(pr, blocksize * BCACHEFS_SECTOR_SIZE);
	blkid_probe_set_fsblocksize(pr, blocksize * BCACHEFS_SECTOR_SIZE);
	blkid_probe_set_wiper(pr, 0, BCACHE_SB_OFF);

	probe_bcachefs_sb_fields(pr, bcs, sb, sb_end);

	return BLKID_PROBE_OK;
}

const struct blkid_idinfo bcache_idinfo =
{
	.name		= "bcache",
	.usage		= BLKID_USAGE_OTHER,
	.probefunc	= probe_bcache,
	.minsz		= 8192,
	.magics		=
	{
		{
			.magic = BCACHE_SB_MAGIC,
			.len   = BCACHE_SB_MAGIC_LEN,
			.kboff = BCACHE_SB_KBOFF,
			.sboff = BCACHE_SB_MAGIC_OFF
		},
		{ NULL }
	}
};

const struct blkid_idinfo bcachefs_idinfo =
{
	.name		= "bcachefs",
	.usage		= BLKID_USAGE_FILESYSTEM,
	.probefunc	= probe_bcachefs,
	.minsz		= 256 * BCACHEFS_SECTOR_SIZE,
	.magics		= {
		{
			.magic = BCACHE_SB_MAGIC,
			.len   = BCACHE_SB_MAGIC_LEN,
			.kboff = BCACHE_SB_KBOFF,
			.sboff = BCACHE_SB_MAGIC_OFF,
		},
		{
			.magic = BCACHEFS_SB_MAGIC,
			.len   = BCACHE_SB_MAGIC_LEN,
			.kboff = BCACHE_SB_KBOFF,
			.sboff = BCACHE_SB_MAGIC_OFF,
		},
		{ NULL }
	}
};
