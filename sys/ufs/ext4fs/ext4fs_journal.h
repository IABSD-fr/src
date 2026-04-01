/*
 * Copyright (c) 2025 kmx.io.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _EXT4FS_JOURNAL_H_
#define _EXT4FS_JOURNAL_H_

/*
 * JBD2 journal on-disk structures.
 * All JBD2 fields are big-endian.
 */

#define JBD2_MAGIC		0xC03B3998

/* Block types */
#define JBD2_DESCRIPTOR_BLOCK	1
#define JBD2_COMMIT_BLOCK	2
#define JBD2_SUPERBLOCK_V1	3
#define JBD2_SUPERBLOCK_V2	4
#define JBD2_REVOKE_BLOCK	5

/* Descriptor tag flags */
#define JBD2_FLAG_ESCAPE	0x01
#define JBD2_FLAG_SAME_UUID	0x02
#define JBD2_FLAG_DELETED	0x04
#define JBD2_FLAG_LAST_TAG	0x08

/* Journal feature flags (in journal superblock) */
#define JBD2_FEATURE_COMPAT_CHECKSUM	0x01

#define JBD2_FEATURE_INCOMPAT_REVOKE		0x01
#define JBD2_FEATURE_INCOMPAT_64BIT		0x02
#define JBD2_FEATURE_INCOMPAT_ASYNC_COMMIT	0x04
#define JBD2_FEATURE_INCOMPAT_CSUM_V2		0x08
#define JBD2_FEATURE_INCOMPAT_CSUM_V3		0x10

/* Common block header (12 bytes) */
struct jbd2_header {
	u_int32_t	h_magic;
	u_int32_t	h_blocktype;
	u_int32_t	h_sequence;
} __attribute__((packed));

/* Journal superblock */
struct jbd2_superblock {
	struct jbd2_header s_header;
	/* 0x0C */
	u_int32_t	s_blocksize;
	u_int32_t	s_maxlen;
	u_int32_t	s_first;
	/* 0x18 */
	u_int32_t	s_sequence;
	u_int32_t	s_start;
	/* 0x20 */
	u_int32_t	s_errno;
	/* V2+ fields */
	u_int32_t	s_feature_compat;
	u_int32_t	s_feature_incompat;
	u_int32_t	s_feature_ro_compat;
	/* 0x30 */
	u_int8_t	s_uuid[16];
	/* 0x40 */
	u_int32_t	s_nr_users;
	u_int32_t	s_dynsuper;
	/* 0x48 */
	u_int32_t	s_max_transaction;
	u_int32_t	s_max_trans_data;
	/* 0x50 */
	u_int8_t	s_checksum_type;
	u_int8_t	s_padding2[3];
	/* 0x54 */
	u_int8_t	s_padding[168];
	/* 0xFC */
	u_int32_t	s_checksum;
	/* 0x100 */
	u_int8_t	s_users[16 * 48];
} __attribute__((packed));

/* Descriptor block tag v3 (CSUM_V3, 16 bytes without UUID) */
struct jbd2_block_tag3 {
	u_int32_t	t_blocknr;
	u_int32_t	t_flags;
	u_int32_t	t_blocknr_high;
	u_int32_t	t_checksum;
} __attribute__((packed));

/* Descriptor block tag v2 (no CSUM_V3) */
struct jbd2_block_tag {
	u_int32_t	t_blocknr;
	u_int16_t	t_checksum;
	u_int16_t	t_flags;
	u_int32_t	t_blocknr_high;	/* only if 64BIT */
} __attribute__((packed));

/* Revoke block header */
struct jbd2_revoke_header {
	struct jbd2_header r_header;
	u_int32_t	r_count;	/* bytes used in this block */
} __attribute__((packed));

/* Revocation table entry */
struct jbd2_revoke_entry {
	u_int64_t	re_block;
	u_int32_t	re_sequence;
};

/* Block map entry: journal block -> filesystem block */
struct jbd2_blockmap_entry {
	u_int64_t	jb_fsblock;	/* filesystem block number */
};

struct jbd2_replay_ctx {
	struct vnode		*rc_devvp;
	struct m_ext4fs		*rc_fs;
	struct ext4fs_extent_header *rc_journal_eh;

	/* Journal geometry (from journal superblock, host order) */
	u_int32_t		rc_blocksize;
	u_int32_t		rc_maxlen;
	u_int32_t		rc_first;
	u_int32_t		rc_sequence;	/* starting sequence */
	u_int32_t		rc_start;	/* starting block */

	/* Journal feature flags */
	u_int32_t		rc_features_incompat;

	/* Block map: journal block number -> filesystem block */
	struct jbd2_blockmap_entry *rc_blockmap;
	u_int32_t		rc_blockmap_count;

	/* Revocation table */
	struct jbd2_revoke_entry *rc_revoke;
	u_int32_t		rc_revoke_count;
	u_int32_t		rc_revoke_alloc;

	/* Scan result */
	u_int32_t		rc_end_sequence;
	u_int32_t		rc_replay_count;
};

int ext4fs_journal_replay(struct vnode *, struct m_ext4fs *);
int ext4fs_orphan_cleanup(struct mount *);

#endif /* _EXT4FS_JOURNAL_H_ */
