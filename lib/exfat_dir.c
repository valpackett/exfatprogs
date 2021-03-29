// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2020 Hyunchul Lee <hyc.lee@gmail.com>
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "exfat_fs.h"
#include "exfat_dir.h"

static struct path_resolve_ctx path_resolve_ctx;

#define fsck_err(parent, inode, fmt, ...)		\
({							\
		exfat_resolve_path_parent(&path_resolve_ctx,	\
			parent, inode);			\
		exfat_err("ERROR: %s: " fmt,		\
			path_resolve_ctx.local_path,	\
			##__VA_ARGS__);			\
})

static ssize_t write_block(struct exfat_de_iter *iter, unsigned int block)
{
	off_t device_offset;
	struct exfat *exfat = iter->exfat;
	struct buffer_desc *desc;
	unsigned int i;

	desc = &iter->buffer_desc[block & 0x01];
	device_offset = exfat_c2o(exfat, desc->p_clus) + desc->offset;

	for (i = 0; i < iter->read_size / iter->write_size; i++) {
		if (desc->dirty[i]) {
			if (exfat_write(exfat->blk_dev->dev_fd,
					desc->buffer + i * iter->write_size,
					iter->write_size,
					device_offset + i * iter->write_size)
					!= (ssize_t)iter->write_size)
				return -EIO;
			desc->dirty[i] = 0;
		}
	}
	return 0;
}

static int read_ahead_first_blocks(struct exfat_de_iter *iter)
{
#ifdef POSIX_FADV_WILLNEED
	struct exfat *exfat = iter->exfat;
	clus_t clus_count;
	unsigned int size;

	clus_count = iter->parent->size / exfat->clus_size;

	if (clus_count > 1) {
		iter->ra_begin_offset = 0;
		iter->ra_next_clus = 1;
		size = exfat->clus_size;
	} else {
		iter->ra_begin_offset = 0;
		iter->ra_next_clus = 0;
		size = iter->ra_partial_size;
	}
	return posix_fadvise(exfat->blk_dev->dev_fd,
			exfat_c2o(exfat, iter->parent->first_clus), size,
			POSIX_FADV_WILLNEED);
#else
	return -ENOTSUP;
#endif
}

/**
 * read the next fragment in advance, and assume the fragment
 * which covers @clus is already read.
 */
static int read_ahead_next_blocks(struct exfat_de_iter *iter,
		clus_t clus, unsigned int offset, clus_t p_clus)
{
#ifdef POSIX_FADV_WILLNEED
	struct exfat *exfat = iter->exfat;
	off_t device_offset;
	clus_t clus_count, ra_clus, ra_p_clus;
	unsigned int size;
	int ret = 0;

	clus_count = iter->parent->size / exfat->clus_size;
	if (clus + 1 < clus_count) {
		ra_clus = clus + 1;
		if (ra_clus == iter->ra_next_clus &&
				offset >= iter->ra_begin_offset) {
			ret = exfat_get_next_clus(exfat, iter->parent,
						  p_clus, &ra_p_clus);
			if (ret)
				return ret;

			if (ra_p_clus == EXFAT_EOF_CLUSTER)
				return -EIO;

			device_offset = exfat_c2o(exfat, ra_p_clus);
			size = ra_clus + 1 < clus_count ?
				exfat->clus_size : iter->ra_partial_size;
			ret = posix_fadvise(exfat->blk_dev->dev_fd,
					device_offset, size,
					POSIX_FADV_WILLNEED);
			iter->ra_next_clus = ra_clus + 1;
			iter->ra_begin_offset = 0;
		}
	} else {
		if (offset >= iter->ra_begin_offset &&
				offset + iter->ra_partial_size <=
				exfat->clus_size) {
			device_offset = exfat_c2o(exfat, p_clus) +
				offset + iter->ra_partial_size;
			ret = posix_fadvise(exfat->blk_dev->dev_fd,
					device_offset, iter->ra_partial_size,
					POSIX_FADV_WILLNEED);
			iter->ra_begin_offset =
				offset + iter->ra_partial_size;
		}
	}

	return ret;
#else
	return -ENOTSUP;
#endif
}

static int read_ahead_next_dir_blocks(struct exfat_de_iter *iter)
{
#ifdef POSIX_FADV_WILLNEED
	struct exfat *exfat = iter->exfat;
	struct list_head *current;
	struct exfat_inode *next_inode;
	off_t offset;

	if (list_empty(&exfat->dir_list))
		return -EINVAL;

	current = exfat->dir_list.next;
	if (iter->parent == list_entry(current, struct exfat_inode, list) &&
			current->next != &exfat->dir_list) {
		next_inode = list_entry(current->next, struct exfat_inode,
				list);
		offset = exfat_c2o(exfat, next_inode->first_clus);
		return posix_fadvise(exfat->blk_dev->dev_fd, offset,
				iter->ra_partial_size,
				POSIX_FADV_WILLNEED);
	}

	return 0;
#else
	return -ENOTSUP;
#endif
}

static ssize_t read_block(struct exfat_de_iter *iter, unsigned int block)
{
	struct exfat *exfat = iter->exfat;
	struct buffer_desc *desc, *prev_desc;
	off_t device_offset;
	ssize_t ret;

	desc = &iter->buffer_desc[block & 0x01];
	if (block == 0) {
		desc->p_clus = iter->parent->first_clus;
		desc->offset = 0;
	}

	/* if the buffer already contains dirty dentries, write it */
	if (write_block(iter, block))
		return -EIO;

	if (block > 0) {
		if (block > iter->parent->size / iter->read_size)
			return EOF;

		prev_desc = &iter->buffer_desc[(block-1) & 0x01];
		if (prev_desc->offset + 2 * iter->read_size <=
				exfat->clus_size) {
			desc->p_clus = prev_desc->p_clus;
			desc->offset = prev_desc->offset + iter->read_size;
		} else {
			ret = exfat_get_next_clus(exfat, iter->parent,
						  prev_desc->p_clus, &desc->p_clus);
			desc->offset = 0;
			if (ret)
				return ret;
			else if (desc->p_clus == EXFAT_EOF_CLUSTER)
				return EOF;
		}
	}

	device_offset = exfat_c2o(exfat, desc->p_clus) + desc->offset;
	ret = exfat_read(exfat->blk_dev->dev_fd, desc->buffer,
			iter->read_size, device_offset);
	if (ret <= 0)
		return ret;

	/*
	 * if a buffer is filled with dentries, read blocks ahead of time,
	 * otherwise read blocks of the next directory in advance.
	 */
	if (desc->buffer[iter->read_size - 32] != EXFAT_LAST)
		read_ahead_next_blocks(iter,
				(block * iter->read_size) / exfat->clus_size,
				(block * iter->read_size) % exfat->clus_size,
				desc->p_clus);
	else
		read_ahead_next_dir_blocks(iter);
	return ret;
}

int exfat_de_iter_init(struct exfat_de_iter *iter, struct exfat *exfat,
		       struct exfat_inode *dir, struct buffer_desc *bd)
{
	iter->exfat = exfat;
	iter->parent = dir;
	iter->write_size = exfat->sect_size;
	iter->read_size = exfat->clus_size <= 4*KB ? exfat->clus_size : 4*KB;
	if (exfat->clus_size <= 32 * KB)
		iter->ra_partial_size = MAX(4 * KB, exfat->clus_size / 2);
	else
		iter->ra_partial_size = exfat->clus_size / 4;
	iter->ra_partial_size = MIN(iter->ra_partial_size, 8 * KB);

	iter->buffer_desc = bd;

	if (iter->parent->size == 0)
		return EOF;

	read_ahead_first_blocks(iter);
	if (read_block(iter, 0) != (ssize_t)iter->read_size) {
		exfat_err("failed to read directory entries.\n");
		return -EIO;
	}

	iter->de_file_offset = 0;
	iter->next_read_offset = iter->read_size;
	iter->max_skip_dentries = 0;
	return 0;
}

int exfat_de_iter_get(struct exfat_de_iter *iter,
			int ith, struct exfat_dentry **dentry)
{
	off_t next_de_file_offset;
	ssize_t ret;
	unsigned int block;

	next_de_file_offset = iter->de_file_offset +
			ith * sizeof(struct exfat_dentry);
	block = (unsigned int)(next_de_file_offset / iter->read_size);

	if (next_de_file_offset + sizeof(struct exfat_dentry) >
		iter->parent->size)
		return EOF;
	/* the dentry must be in current, or next block which will be read */
	if (block > iter->de_file_offset / iter->read_size + 1)
		return -ERANGE;

	/* read next cluster if needed */
	if (next_de_file_offset >= iter->next_read_offset) {
		ret = read_block(iter, block);
		if (ret != (ssize_t)iter->read_size)
			return ret;
		iter->next_read_offset += iter->read_size;
	}

	if (ith + 1 > iter->max_skip_dentries)
		iter->max_skip_dentries = ith + 1;

	*dentry = (struct exfat_dentry *)
			(iter->buffer_desc[block & 0x01].buffer +
			next_de_file_offset % iter->read_size);
	return 0;
}

int exfat_de_iter_get_dirty(struct exfat_de_iter *iter,
			int ith, struct exfat_dentry **dentry)
{
	off_t next_file_offset;
	unsigned int block;
	int ret, sect_idx;

	ret = exfat_de_iter_get(iter, ith, dentry);
	if (!ret) {
		next_file_offset = iter->de_file_offset +
				ith * sizeof(struct exfat_dentry);
		block = (unsigned int)(next_file_offset / iter->read_size);
		sect_idx = (int)((next_file_offset % iter->read_size) /
				iter->write_size);
		iter->buffer_desc[block & 0x01].dirty[sect_idx] = 1;
	}

	return ret;
}

int exfat_de_iter_flush(struct exfat_de_iter *iter)
{
	if (write_block(iter, 0) || write_block(iter, 1))
		return -EIO;
	return 0;
}

int exfat_de_iter_advance(struct exfat_de_iter *iter, int skip_dentries)
{
	if (skip_dentries > iter->max_skip_dentries)
		return -EINVAL;

	iter->max_skip_dentries = 0;
	iter->de_file_offset = iter->de_file_offset +
				skip_dentries * sizeof(struct exfat_dentry);
	return 0;
}

off_t exfat_de_iter_device_offset(struct exfat_de_iter *iter)
{
	struct buffer_desc *bd;
	unsigned int block;

	if ((uint64_t)iter->de_file_offset >= iter->parent->size)
		return EOF;

	block = iter->de_file_offset / iter->read_size;
	bd = &iter->buffer_desc[block & 0x01];
	return exfat_c2o(iter->exfat, bd->p_clus) + bd->offset +
		iter->de_file_offset % iter->read_size;
}

/*
 * try to find the dentry set matched with @filter. this function
 * doesn't verify the dentry set.
 *
 * if found, return 0. if not found, return EOF. otherwise return errno.
 */
int exfat_lookup_dentry_set(struct exfat *exfat, struct exfat_inode *parent,
			    struct exfat_lookup_filter *filter)
{
	struct buffer_desc *bd = NULL;
	struct exfat_dentry *dentry = NULL;
	off_t free_offset = 0;
	struct exfat_de_iter de_iter;
	int dentry_count;
	int retval;
	bool last_is_free = false;

	bd = exfat_alloc_buffer(2, exfat->clus_size, exfat->sect_size);
	if (!bd)
		return -ENOMEM;

	retval = exfat_de_iter_init(&de_iter, exfat, parent, bd);
	if (retval == EOF || retval)
		goto out;

	filter->out.dentry_set = NULL;
	while (1) {
		retval = exfat_de_iter_get(&de_iter, 0, &dentry);
		if (retval == EOF) {
			break;
		} else if (retval) {
			fsck_err(parent->parent, parent,
				 "failed to get a dentry. %d\n", retval);
			goto out;
		}

		dentry_count = 1;
		if (dentry->type == filter->in.type) {
			retval = 0;
			if (filter->in.filter)
				retval = filter->in.filter(&de_iter,
							filter->in.param,
							&dentry_count);

			if (retval == 0) {
				struct exfat_dentry *d;
				int i;

				filter->out.dentry_set = calloc(dentry_count,
								sizeof(struct exfat_dentry));
				if (!filter->out.dentry_set) {
					retval = -ENOMEM;
					goto out;
				}
				for (i = 0; i < dentry_count; i++) {
					exfat_de_iter_get(&de_iter, i, &d);
					memcpy(filter->out.dentry_set + i, d,
					       sizeof(struct exfat_dentry));
				}
				filter->out.dentry_count = dentry_count;
				goto out;
			} else if (retval < 0) {
				goto out;
			}
			last_is_free = false;
		} else if ((dentry->type == EXFAT_LAST ||
			    IS_EXFAT_DELETED(dentry->type))) {
			if (!last_is_free) {
				free_offset = exfat_de_iter_device_offset(&de_iter);
				last_is_free = true;
			}
		} else {
			last_is_free = false;
		}

		exfat_de_iter_advance(&de_iter, dentry_count);
	}

out:
	if (retval == 0)
		filter->out.dentry_d_offset =
			exfat_de_iter_device_offset(&de_iter);
	else if (retval == EOF && last_is_free)
		filter->out.dentry_d_offset = free_offset;
	else
		filter->out.dentry_d_offset = EOF;
	if (bd)
		exfat_free_buffer(bd, 2);
	return retval;
}
