// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Implementation of libfsverity_compute_digest().
 *
 * Copyright 2018 Google LLC
 * Copyright (C) 2020 Facebook
 */

#include "lib_private.h"

#include <stdlib.h>
#include <string.h>

#define FS_VERITY_MAX_LEVELS	64

/*
 * Merkle tree properties.  The file measurement is the hash of this structure
 * excluding the signature and with the sig_size field set to 0.
 */
struct fsverity_descriptor {
	__u8 version;		/* must be 1 */
	__u8 hash_algorithm;	/* Merkle tree hash algorithm */
	__u8 log_blocksize;	/* log2 of size of data and tree blocks */
	__u8 salt_size;		/* size of salt in bytes; 0 if none */
	__le32 sig_size;	/* size of signature in bytes; 0 if none */
	__le64 data_size;	/* size of file the Merkle tree is built over */
	__u8 root_hash[64];	/* Merkle tree root hash */
	__u8 salt[32];		/* salt prepended to each hashed block */
	__u8 __reserved[144];	/* must be 0's */
	__u8 signature[];	/* optional PKCS#7 signature */
};

struct block_buffer {
	u32 filled;
	u8 *data;
};

/*
 * Hash a block, writing the result to the next level's pending block buffer.
 * Returns true if the next level's block became full, else false.
 */
static bool hash_one_block(struct hash_ctx *hash, struct block_buffer *cur,
			   u32 block_size, const u8 *salt, u32 salt_size)
{
	struct block_buffer *next = cur + 1;

	/* Zero-pad the block if it's shorter than block_size. */
	memset(&cur->data[cur->filled], 0, block_size - cur->filled);

	libfsverity_hash_init(hash);
	libfsverity_hash_update(hash, salt, salt_size);
	libfsverity_hash_update(hash, cur->data, block_size);
	libfsverity_hash_final(hash, &next->data[next->filled]);

	next->filled += hash->alg->digest_size;
	cur->filled = 0;

	return next->filled + hash->alg->digest_size > block_size;
}

/*
 * Compute the file's Merkle tree root hash using the given hash algorithm,
 * block size, and salt.
 */
static int compute_root_hash(void *fd, libfsverity_read_fn_t read_fn,
			     u64 file_size, struct hash_ctx *hash,
			     u32 block_size, const u8 *salt, u32 salt_size,
			     u8 *root_hash)
{
	const u32 hashes_per_block = block_size / hash->alg->digest_size;
	const u32 padded_salt_size = roundup(salt_size, hash->alg->block_size);
	u8 *padded_salt = NULL;
	u64 blocks;
	int num_levels = 0;
	int level;
	struct block_buffer _buffers[1 + FS_VERITY_MAX_LEVELS + 1] = {};
	struct block_buffer *buffers = &_buffers[1];
	u64 offset;
	int err = 0;

	/* Root hash of empty file is all 0's */
	if (file_size == 0) {
		memset(root_hash, 0, hash->alg->digest_size);
		return 0;
	}

	if (salt_size != 0) {
		padded_salt = libfsverity_zalloc(padded_salt_size);
		if (!padded_salt)
			return -ENOMEM;
		memcpy(padded_salt, salt, salt_size);
	}

	/* Compute number of levels */
	for (blocks = DIV_ROUND_UP(file_size, block_size); blocks > 1;
	     blocks = DIV_ROUND_UP(blocks, hashes_per_block)) {
		if (WARN_ON(num_levels >= FS_VERITY_MAX_LEVELS)) {
			err = -EINVAL;
			goto out;
		}
		num_levels++;
	}

	/*
	 * Allocate the block buffers.  Buffer "-1" is for data blocks.
	 * Buffers 0 <= level < num_levels are for the actual tree levels.
	 * Buffer 'num_levels' is for the root hash.
	 */
	for (level = -1; level < num_levels; level++) {
		buffers[level].data = libfsverity_zalloc(block_size);
		if (!buffers[level].data) {
			err = -ENOMEM;
			goto out;
		}
	}
	buffers[num_levels].data = root_hash;

	/* Hash each data block, also hashing the tree blocks as they fill up */
	for (offset = 0; offset < file_size; offset += block_size) {
		buffers[-1].filled = min(block_size, file_size - offset);

		err = read_fn(fd, buffers[-1].data, buffers[-1].filled);
		if (err) {
			libfsverity_error_msg("error reading file");
			goto out;
		}

		level = -1;
		while (hash_one_block(hash, &buffers[level], block_size,
				      padded_salt, padded_salt_size)) {
			level++;
			if (WARN_ON(level >= num_levels)) {
				err = -EINVAL;
				goto out;
			}
		}
	}
	/* Finish all nonempty pending tree blocks */
	for (level = 0; level < num_levels; level++) {
		if (buffers[level].filled != 0)
			hash_one_block(hash, &buffers[level], block_size,
				       padded_salt, padded_salt_size);
	}

	/* Root hash was filled by the last call to hash_one_block() */
	if (WARN_ON(buffers[num_levels].filled != hash->alg->digest_size)) {
		err = -EINVAL;
		goto out;
	}
	err = 0;
out:
	for (level = -1; level < num_levels; level++)
		free(buffers[level].data);
	free(padded_salt);
	return err;
}

LIBEXPORT int
libfsverity_compute_digest(void *fd, libfsverity_read_fn_t read_fn,
			   const struct libfsverity_merkle_tree_params *params,
			   struct libfsverity_digest **digest_ret)
{
	const struct fsverity_hash_alg *hash_alg;
	struct hash_ctx *hash = NULL;
	struct libfsverity_digest *digest;
	struct fsverity_descriptor desc;
	int i;
	int err;

	if (!read_fn || !params || !digest_ret) {
		libfsverity_error_msg("missing required parameters for compute_digest");
		return -EINVAL;
	}
	if (params->version != 1) {
		libfsverity_error_msg("unsupported version (%u)",
				      params->version);
		return -EINVAL;
	}
	if (!is_power_of_2(params->block_size)) {
		libfsverity_error_msg("unsupported block size (%u)",
				      params->block_size);
		return -EINVAL;
	}
	if (params->salt_size > sizeof(desc.salt)) {
		libfsverity_error_msg("unsupported salt size (%u)",
				      params->salt_size);
		return -EINVAL;
	}
	if (params->salt_size && !params->salt)  {
		libfsverity_error_msg("salt_size specified, but salt is NULL");
		return -EINVAL;
	}
	for (i = 0; i < ARRAY_SIZE(params->reserved); i++) {
		if (params->reserved[i]) {
			libfsverity_error_msg("reserved bits set in merkle_tree_params");
			return -EINVAL;
		}
	}

	hash_alg = libfsverity_find_hash_alg_by_num(params->hash_algorithm);
	if (!hash_alg) {
		libfsverity_error_msg("unknown hash algorithm: %u",
				      params->hash_algorithm);
		return -EINVAL;
	}

	hash = hash_alg->create_ctx(hash_alg);
	if (!hash)
		return -ENOMEM;

	memset(&desc, 0, sizeof(desc));
	desc.version = 1;
	desc.hash_algorithm = params->hash_algorithm;
	desc.log_blocksize = ilog2(params->block_size);
	desc.data_size = cpu_to_le64(params->file_size);
	if (params->salt_size != 0) {
		memcpy(desc.salt, params->salt, params->salt_size);
		desc.salt_size = params->salt_size;
	}

	err = compute_root_hash(fd, read_fn, params->file_size, hash,
				params->block_size, params->salt,
				params->salt_size, desc.root_hash);
	if (err)
		goto out;

	digest = libfsverity_zalloc(sizeof(*digest) + hash_alg->digest_size);
	if (!digest) {
		err = -ENOMEM;
		goto out;
	}
	digest->digest_algorithm = params->hash_algorithm;
	digest->digest_size = hash_alg->digest_size;
	libfsverity_hash_full(hash, &desc, sizeof(desc), digest->digest);
	*digest_ret = digest;
	err = 0;
out:
	libfsverity_free_hash_ctx(hash);
	return err;
}
