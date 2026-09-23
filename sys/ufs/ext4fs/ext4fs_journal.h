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
#define JBD2_FEATURE_INCOMPAT_FAST_COMMIT	0x20

/*
 * Recovery currently supports metadata checksums v2/v3, but not the older
 * transaction-wide checksum format, asynchronous commits, or fast commits.
 * Unknown incompatible features must never be silently ignored during replay.
 */
#define JBD2_FEATURE_COMPAT_SUPPORTED		0
#define JBD2_FEATURE_RO_COMPAT_SUPPORTED	0
#define JBD2_FEATURE_INCOMPAT_SUPPORTED		\
	(JBD2_FEATURE_INCOMPAT_REVOKE | \
	 JBD2_FEATURE_INCOMPAT_64BIT | \
	 JBD2_FEATURE_INCOMPAT_CSUM_V2 | \
	 JBD2_FEATURE_INCOMPAT_CSUM_V3)

/* Checksum algorithms used by commit and journal superblocks. */
#define JBD2_CHECKSUM_CRC32	1
#define JBD2_CHECKSUM_MD5	2
#define JBD2_CHECKSUM_SHA1	3
#define JBD2_CHECKSUM_CRC32C	4
#define JBD2_CHECKSUM_SIZE	4

/* Keep a malicious superblock from forcing an unbounded wired allocation. */
#define JBD2_MAX_BLOCKMAP_ENTRIES	(1U << 20)
#define JBD2_MAX_REVOKE_ENTRIES		(1U << 18)

/* Extra trailing bytes in the e2fsprogs checksum-v2 tag encoding. */
#define JBD2_CSUM_V2_TAG_EXTRA		2

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

/* Commit block header.  The rest of the journal block is zero-filled. */
struct jbd2_commit_header {
	struct jbd2_header h_header;
	u_int8_t	h_checksum_type;
	u_int8_t	h_checksum_size;
	u_int8_t	h_padding[2];
	u_int32_t	h_checksum[8];
	u_int64_t	h_commit_sec;
	u_int32_t	h_commit_nsec;
} __attribute__((packed));

/* Checksum tail at the end of descriptor and revoke blocks. */
struct jbd2_block_tail {
	u_int32_t	t_checksum;
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
	u_int64_t	re_block;	/* filesystem block + 1; zero means unused */
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
	u_int32_t		rc_features_compat;
	u_int32_t		rc_features_incompat;
	u_int32_t		rc_features_ro_compat;
	u_int32_t		rc_checksum_seed;
	u_int8_t		rc_uuid[16];
	u_int32_t		rc_journal_ino;
	u_int32_t		rc_journal_gen;	/* little-endian on-disk value */

	/* Block map: journal block number -> filesystem block */
	struct jbd2_blockmap_entry *rc_blockmap;
	u_int32_t		rc_blockmap_count;
	u_int64_t		*rc_blockset;
	u_int32_t		rc_blockset_mask;

	/* Revocation table */
	struct jbd2_revoke_entry *rc_revoke;
	u_int32_t		rc_revoke_count;
	u_int32_t		rc_revoke_alloc;

	/* Scan result */
	u_int32_t		rc_end_sequence;
	u_int32_t		rc_replay_count;
};

struct ext4fs_journal_handle;
struct proc;

int	ext4fs_journal_replay (struct vnode *, struct m_ext4fs *,
    struct proc *);

void	ext4fs_journal_abort (struct mount *, int);
int	ext4fs_journal_begin (struct mount *, unsigned int,
    struct ext4fs_journal_handle **);
int	ext4fs_journal_dirty_metadata (struct ext4fs_journal_handle *,
    struct buf *);
int	ext4fs_journal_end (struct ext4fs_journal_handle *);
int	ext4fs_journal_force_commit (struct mount *);
int	ext4fs_journal_get_write_access (struct ext4fs_journal_handle *,
    struct buf *, u_int64_t);
int	ext4fs_journal_revoke (struct ext4fs_journal_handle *, u_int64_t);
