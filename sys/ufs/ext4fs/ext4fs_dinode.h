/*
 * Copyright (c) 2025 kmx.io.
 * Copyright (c) 1997 Manuel Bouyer.
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Modified for ext4fs by kmx.io.
 */

#include <sys/stat.h>

#define EXT4FS_EXTENT_HEADER_MAGIC  0xF30A

struct ext4fs_extent_header {
  u_int16_t eh_magic;
  u_int16_t eh_entries;
  u_int16_t eh_max;
  u_int16_t eh_depth;
  u_int32_t eh_generation;
} __attribute__((packed));

struct ext4fs_extent {
  u_int32_t e_block;
  u_int16_t e_len;
  u_int16_t e_start_hi;
  u_int32_t e_start_lo;
} __attribute__((packed));

struct ext4fs_extent_idx {
  u_int32_t ei_block;
  u_int32_t ei_leaf_lo;
  u_int16_t ei_leaf_hi;
  u_int16_t ei_unused;
} __attribute__((packed));

struct ext4fs_dinode {
  u_int16_t i_mode;
  u_int16_t i_uid_lo;
  u_int32_t i_size_lo;
  u_int32_t i_atime;
  u_int32_t i_ctime;
  /* 0x10 */
  u_int32_t i_mtime;
  u_int32_t i_dtime;
  u_int16_t i_gid_lo;
  u_int16_t i_links_count;
  u_int32_t i_blocks_lo;
  /* 0x20 */
  u_int32_t i_flags;
  u_int32_t i_version;
  union {
    u_int32_t i_block[15];
    struct {
      struct ext4fs_extent_header i_extent_header;
      union {
        struct ext4fs_extent i_extent[4];
        struct ext4fs_extent_idx i_extent_idx[4];
      };
    };
  };
  u_int32_t i_nfs_generation;
  u_int32_t i_extended_attributes_lo;
  u_int32_t i_size_hi;
  /* 0x70 */
  u_int32_t i_fragment_address;
  u_int16_t i_blocks_hi;
  u_int16_t i_extended_attributes_hi;
  u_int16_t i_uid_hi;
  u_int16_t i_gid_hi;
  u_int16_t i_checksum_lo;
  u_int16_t i_reserved_7e;
  /* 0x80 */
  u_int16_t i_extra_isize;
  u_int16_t i_checksum_hi;
  u_int32_t i_ctime_extra;
  u_int32_t i_mtime_extra;
  u_int32_t i_atime_extra;
  /* 0x90 */
  u_int32_t i_crtime;
  u_int32_t i_crtime_extra;
  u_int32_t i_version_hi;
  u_int32_t i_project_id;
  /* 0xA0 */
} __attribute__((packed));

struct ext4fs_dinode_256 {
  struct ext4fs_dinode dinode;
  u_int8_t extended_attributes[256 - sizeof(struct ext4fs_dinode)];
};
