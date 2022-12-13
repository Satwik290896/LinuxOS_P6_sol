#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>

#include "ezfs.h"
#include "ezfs_ops.h"

#define DEBUG

#ifdef DEBUG
#define debug(x...)	pr_info(x)
#else
#define debug(x...)
#endif

/* ezfs helper */
static inline struct buffer_head *get_ezfs_sb_bh(struct super_block *sb)
{
	return ((struct ezfs_sb_buffer_heads *)sb->s_fs_info)->sb_bh;
}

static inline struct ezfs_super_block *get_ezfs_sb(struct super_block *sb)
{
	return (struct ezfs_super_block *) get_ezfs_sb_bh(sb)->b_data;
}

static inline struct buffer_head *get_ezfs_i_bh(struct super_block *sb)
{
	return ((struct ezfs_sb_buffer_heads *)sb->s_fs_info)->i_store_bh;
}

static inline struct ezfs_inode *get_ezfs_inode(struct inode *inode)
{
	return inode->i_private;
}

static struct inode *ezfs_iget(struct super_block *sb, int ino)
{
	struct inode *inode = iget_locked(sb, ino);

	if (inode && inode->i_state & I_NEW) {
		struct ezfs_inode *ezfs_inode = (struct ezfs_inode *)
		get_ezfs_i_bh(sb)->b_data + ino - EZFS_ROOT_INODE_NUMBER;

		inode->i_private = ezfs_inode;
		inode->i_mode = ezfs_inode->mode;
		inode->i_op = &ezfs_inode_ops;
		inode->i_sb = sb;
		if (inode->i_mode & S_IFDIR)
			inode->i_fop = &ezfs_dir_ops;
		else
			inode->i_fop = &ezfs_file_ops;
		inode->i_mapping->a_ops = &ezfs_aops;
		inode->i_size = ezfs_inode->file_size;
		inode->i_blocks = ezfs_inode->nblocks * 8;
		set_nlink(inode, ezfs_inode->nlink);
		inode->i_atime = ezfs_inode->i_atime;
		inode->i_mtime = ezfs_inode->i_mtime;
		inode->i_ctime = ezfs_inode->i_ctime;
		i_uid_write(inode, ezfs_inode->uid);
		i_gid_write(inode, ezfs_inode->gid);
		unlock_new_inode(inode);
	}

	return inode;
}
static int ezfs_move_block(unsigned long base, unsigned long from, unsigned long to,
					struct super_block *sb, struct address_space *mapping)
{
	struct buffer_head *old, *new;
	struct page *page = find_get_page(mapping, from - base);

	to += EZFS_ROOT_DATABLOCK_NUMBER;
	new = sb_getblk(sb, to);
	if (page) {
		memcpy(new->b_data, page_address(page), new->b_size);
	} else {
		from += EZFS_ROOT_DATABLOCK_NUMBER;
		old = sb_bread(sb, from);
		if (!old)
			return -EIO;
		memcpy(new->b_data, old->b_data, old->b_size);
		bforget(old);
	}

	debug("[%s] mark_buffer_dirty(%lu)\n", __func__, to);
	mark_buffer_dirty(new);
	brelse(new);
	return 0;
}

/* iof returns whether i is out of range[s, s+e-1] */
static inline int iof(uint64_t s, uint64_t e, int i)
{
	return s > i || s + e - 1 < i;
}

static int ezfs_get_block(struct inode *inode, sector_t block,
			struct buffer_head *bh_result, int create)
{
	int ret, phys, i, ez_blk_n, ez_n_blk, ez_blk_sidx;
	int new_n_blk, blk_num_i, sfb, new_blk_sidx;
	struct super_block *sb = inode->i_sb;
	struct buffer_head *ezfs_sb_bh = get_ezfs_sb_bh(sb);
	struct ezfs_super_block *ezfs_sb = get_ezfs_sb(sb);
	struct ezfs_inode *ezfs_inode = get_ezfs_inode(inode);

	ret = phys = 0;
	ez_blk_n = ezfs_inode->data_block_number;
	ez_n_blk = inode->i_blocks / 8;
	if (ez_n_blk)
		phys = ez_blk_n + block;

	debug("[%s] ino=%ld, block=%llu, phys=%d, create=%d page=%p\n", __func__,
			inode->i_ino, block, phys, create, bh_result->b_page);

	if (ez_n_blk && block < ez_n_blk) {
		map_bh(bh_result, sb, phys);
		return 0;
	}

	if (!create)
		return 0;

	/* The file will be extended, so let's see if there is enough space. */
	if (phys >= EZFS_ROOT_DATABLOCK_NUMBER + EZFS_MAX_DATA_BLKS)
		return -ENOSPC;

	/* The rest has to be protected against itself. */
	mutex_lock(ezfs_sb->ezfs_lock);

	/* The file is empty, try to find a block for it */
	if (!ez_n_blk) {
		for (i = 0; i < EZFS_MAX_DATA_BLKS && IS_SET(ezfs_sb->free_data_blocks, i); ++i);
		if (i == EZFS_MAX_DATA_BLKS)
			return -ENOSPC;

		phys = i + EZFS_ROOT_DATABLOCK_NUMBER;
		ezfs_inode->data_block_number = phys;
		goto success;
	}

	/*
	 * If the file is not empty and the requested block right next
	 * to the last existing data block is empty, we can grant it.
	 */
	if (!IS_SET(ezfs_sb->free_data_blocks, phys - EZFS_ROOT_DATABLOCK_NUMBER))
		goto success;

	/* Reallocate! */
	new_n_blk = ez_n_blk + 1;
	for (i = 0, sfb = 0; sfb < new_n_blk && i < EZFS_MAX_DATA_BLKS; i++, sfb++) {
		blk_num_i = i + EZFS_ROOT_DATABLOCK_NUMBER;
		if (IS_SET(ezfs_sb->free_data_blocks, i) &&
			iof(ez_blk_n, ez_n_blk, blk_num_i))
			sfb = -1;
	}

	if (sfb < new_n_blk) {
		ret = -ENOSPC;
		goto out;
	}

	/* Move existing contents to new place data_idx[i - new_n_blk, i - 1] */
	new_blk_sidx = i - new_n_blk;
	phys = i - 1 + EZFS_ROOT_DATABLOCK_NUMBER;
	ez_blk_sidx = ez_blk_n - EZFS_ROOT_DATABLOCK_NUMBER;
	for (i = 0; i < ez_n_blk; i++) {
		ezfs_move_block(ez_blk_sidx, ez_blk_sidx + i, new_blk_sidx + i, sb, inode->i_mapping);
		CLEARBIT(ezfs_sb->free_data_blocks, ez_blk_sidx + i);
	}
	for (i = 0; i < ez_n_blk; i++)
		SETBIT(ezfs_sb->free_data_blocks, new_blk_sidx + i);
	ezfs_inode->data_block_number = new_blk_sidx + EZFS_ROOT_DATABLOCK_NUMBER;

success:
	debug("[%s] Reallocate from [%d-%d] to [%llu-%d]\n", __func__,
		ez_blk_n, ez_blk_n + ez_n_blk - 1, ezfs_inode->data_block_number, phys);
	map_bh(bh_result, sb, phys);

	SETBIT(ezfs_sb->free_data_blocks, phys - EZFS_ROOT_DATABLOCK_NUMBER);
	mark_buffer_dirty(ezfs_sb_bh);

out:
	mutex_unlock(ezfs_sb->ezfs_lock);
	return ret;
}

/* ezfs_dir_ops */
int ezfs_iterate(struct file *filp, struct dir_context *ctx)
{
	int i, pos;
	struct inode *inode = file_inode(filp);
	uint64_t filp_blk_num = get_ezfs_inode(file_inode(filp))->data_block_number;
	struct buffer_head *bh = sb_bread(inode->i_sb, filp_blk_num);
	struct ezfs_dir_entry *ezfs_dentry;

	debug("[%s] dir_ino=%ld, pos=%lld\n", __func__, inode->i_ino, ctx->pos);

	if (!dir_emit_dots(filp, ctx))
		return 0;
	pos = ctx->pos - 2;

	if (!bh)
		return -EIO;

	ezfs_dentry = (struct ezfs_dir_entry *) bh->b_data + pos;
	for (i = pos; i < EZFS_MAX_CHILDREN; ++i, ++ezfs_dentry, ++ctx->pos) {
		if (ezfs_dentry->active) {
			if (!dir_emit(ctx, ezfs_dentry->filename,
					strlen(ezfs_dentry->filename),
					ezfs_dentry->inode_no, DT_UNKNOWN))
				break;
		}
	}
	brelse(bh);

	return 0;
}

/* ezfs_aops */
int ezfs_readpage(struct file *file, struct page *page)
{
	debug("[%s] file=%s\n", __func__, file->f_path.dentry->d_name.name);
	return block_read_full_page(page, ezfs_get_block);
}

int ezfs_writepage(struct page *page, struct writeback_control *wbc)
{
	debug("[%s]\n", __func__);
	return block_write_full_page(page, ezfs_get_block, wbc);
}

static void ezfs_write_failed(struct address_space *mapping, loff_t to)
{
	if (to > mapping->host->i_size)
		truncate_pagecache(mapping->host, mapping->host->i_size);
}

int ezfs_write_begin(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned int len, unsigned int flags,
		struct page **pagep, void **fsdata)
{
	int ret;

	debug("[%s]\n", __func__);
	ret = block_write_begin(mapping, pos, len, flags, pagep,
				ezfs_get_block);
	if (unlikely(ret))
		ezfs_write_failed(mapping, pos + len);

	return ret;
}

int ezfs_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{
	int ret;
	struct inode *inode = mapping->host;
	loff_t old_size = inode->i_size;

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);

	if (old_size != inode->i_size) {
		int old_blocks = (old_size + EZFS_BLOCK_SIZE - 1) / EZFS_BLOCK_SIZE;
		int new_blocks = (inode->i_size + EZFS_BLOCK_SIZE - 1) / EZFS_BLOCK_SIZE;

		inode->i_blocks = new_blocks * 8;
		mark_inode_dirty(inode);

		if (old_blocks > new_blocks) {
			struct ezfs_super_block *ezfs_sb = get_ezfs_sb(inode->i_sb);
			struct ezfs_inode *ezfs_inode = get_ezfs_inode(inode);
			int i, data_blk_n = ezfs_inode->data_block_number;

			mutex_lock(ezfs_sb->ezfs_lock);
			for (i = new_blocks; i < old_blocks; ++i)
				CLEARBIT(ezfs_sb->free_data_blocks, data_blk_n + i - EZFS_ROOT_DATABLOCK_NUMBER);
			mutex_unlock(ezfs_sb->ezfs_lock);
		}
	}
	return ret;
}

sector_t ezfs_bmap(struct address_space *mapping, sector_t block)
{
	debug("[%s] block=%llu\n", __func__, block);
	return generic_block_bmap(mapping, block, ezfs_get_block);
}

/* ezfs_inode_ops */
struct dentry *ezfs_lookup(struct inode *dir, struct dentry *child_dentry,
		unsigned int flags)
{
	int i;
	struct ezfs_dir_entry *ezfs_dentry;
	struct inode *inode = NULL;
	uint64_t dir_blk_num = get_ezfs_inode(dir)->data_block_number;
	struct buffer_head *dir_bh = sb_bread(dir->i_sb, dir_blk_num);

	debug("[%s] dir_ino=%ld, dentry=%s\n", __func__,
			dir->i_ino, child_dentry->d_name.name);

	if (!dir_bh)
		return ERR_PTR(-EIO);

	ezfs_dentry = (struct ezfs_dir_entry *) dir_bh->b_data;
	for (i = 0; i < EZFS_MAX_CHILDREN; ++i, ++ezfs_dentry) {
		if (ezfs_dentry->active &&
				child_dentry->d_name.len ==
				strlen(ezfs_dentry->filename) &&
				!memcmp(ezfs_dentry->filename,
					child_dentry->d_name.name,
					child_dentry->d_name.len)) {
			inode = ezfs_iget(dir->i_sb, ezfs_dentry->inode_no);
			break;
		}
	}
	brelse(dir_bh);

	return d_splice_alias(inode, child_dentry);
}

static void write_inode_helper(struct inode *inode,
							  struct ezfs_inode *ezfs_inode)
{
	ezfs_inode->mode = inode->i_mode;
	ezfs_inode->file_size = inode->i_size;
	ezfs_inode->nlink = inode->i_nlink;
	ezfs_inode->i_atime = inode->i_atime;
	ezfs_inode->i_mtime = inode->i_mtime;
	ezfs_inode->i_ctime = inode->i_ctime;
	ezfs_inode->uid = inode->i_uid.val;
	ezfs_inode->gid = inode->i_gid.val;
	ezfs_inode->nblocks = inode->i_blocks / 8;
}

static struct inode *create_helper(struct inode *dir,
		struct dentry *dentry, umode_t mode, bool isdir)
{
	int i, i_idx, d_idx, i_num, d_num;
	struct ezfs_super_block *ezfs_sb = get_ezfs_sb(dir->i_sb);
	struct buffer_head *dir_bh, *i_bh;
	struct ezfs_dir_entry *ezfs_dentry;
	struct inode *new_inode, *ret = NULL;
	struct ezfs_inode *new_ezfs_inode;
	uint64_t dir_blk_num = get_ezfs_inode(dir)->data_block_number;

	if (strnlen(dentry->d_name.name, EZFS_MAX_FILENAME_LENGTH + 1) >
			EZFS_MAX_FILENAME_LENGTH) {
		pr_err("Filename too long\n");
		return ERR_PTR(-ENAMETOOLONG);
	}

	dir_bh = sb_bread(dir->i_sb, dir_blk_num);
	if (!dir_bh)
		return ERR_PTR(-EIO);

	ezfs_dentry = (struct ezfs_dir_entry *) dir_bh->b_data;
	for (i = 0; i < EZFS_MAX_CHILDREN && ezfs_dentry->active; ++i, ++ezfs_dentry);
	if (i == EZFS_MAX_CHILDREN) {
		brelse(dir_bh);
		return ERR_PTR(-ENOSPC);
	}

	mutex_lock(ezfs_sb->ezfs_lock);
	/* find an empty inode */
	for (i_idx = 0; i_idx < EZFS_MAX_INODES && IS_SET(ezfs_sb->free_inodes, i_idx); i_idx++);
	if (i_idx == EZFS_MAX_INODES) {
		ret = ERR_PTR(-ENOSPC);
		goto out;
	}
	i_num = i_idx + EZFS_ROOT_INODE_NUMBER;

	/*
	 * empty regular files don't need data block
	 * find an empty data block for the created folder
	 */
	if (isdir)
		mode |= S_IFDIR;

	if (mode & S_IFDIR) {
		struct buffer_head *new_dir_bh;

		for (d_idx = 0; d_idx < EZFS_MAX_DATA_BLKS && IS_SET(ezfs_sb->free_data_blocks, d_idx); d_idx++);
		if (d_idx == EZFS_MAX_DATA_BLKS) {
			ret = ERR_PTR(-ENOSPC);
			goto out;
		}
		d_num = d_idx + EZFS_ROOT_DATABLOCK_NUMBER;
		/* folder data block should be zeroed out */
		new_dir_bh = sb_bread(dir->i_sb, d_num);
		if (!new_dir_bh) {
			ret = ERR_PTR(-EIO);
			goto out;
		}
		memset(new_dir_bh->b_data, 0, EZFS_BLOCK_SIZE);
		mark_buffer_dirty(new_dir_bh);
		brelse(new_dir_bh);
	}

	new_inode = iget_locked(dir->i_sb, i_num);
	if (IS_ERR(new_inode)) {
		ret = new_inode;
		goto out;
	}

	/* initialize new inode & ezfs_inode */
	i_bh = get_ezfs_i_bh(dir->i_sb);
	new_ezfs_inode = ((struct ezfs_inode *) i_bh->b_data) + i_idx;
	new_inode->i_mode = mode;
	new_inode->i_op = &ezfs_inode_ops;
	new_inode->i_sb = dir->i_sb;
	if (new_inode->i_mode & S_IFDIR) {
		new_inode->i_fop = &ezfs_dir_ops;
		new_inode->i_size = EZFS_BLOCK_SIZE;
		new_inode->i_blocks = 8;
		new_ezfs_inode->data_block_number = d_num;
		set_nlink(new_inode, 2);
	} else {
		new_inode->i_fop = &ezfs_file_ops;
		new_inode->i_size = 0;
		new_inode->i_blocks = 0;
		new_ezfs_inode->data_block_number = -1;
		set_nlink(new_inode, 1);
	}
	new_inode->i_mapping->a_ops = &ezfs_aops;
	new_inode->i_atime = new_inode->i_mtime = new_inode->i_ctime = current_time(new_inode);
	inode_init_owner(new_inode, dir, mode);

	write_inode_helper(new_inode, new_ezfs_inode);
	mark_buffer_dirty(i_bh);
	new_inode->i_private = (void *) new_ezfs_inode;

	d_instantiate_new(dentry, new_inode);
	mark_inode_dirty(new_inode);

	/* write new dentry to dir data block */
	strncpy(ezfs_dentry->filename, dentry->d_name.name,
					strlen(dentry->d_name.name));
	ezfs_dentry->active = 1;
	ezfs_dentry->inode_no = i_num;
	mark_buffer_dirty(dir_bh);

	/* update dir inode attributes */
	dir->i_mtime = dir->i_ctime = current_time(dir);
	if (new_inode->i_mode & S_IFDIR)
		inc_nlink(dir);
	mark_inode_dirty(dir);

	SETBIT(ezfs_sb->free_inodes, i_idx);
	if (mode & S_IFDIR)
		SETBIT(ezfs_sb->free_data_blocks, d_idx);
	mark_buffer_dirty(get_ezfs_sb_bh(dir->i_sb));
out:
	brelse(dir_bh);
	mutex_unlock(ezfs_sb->ezfs_lock);
	return ret;
}

int ezfs_create(struct inode *dir,
	struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode;

	debug("[%s] dir_ino=%ld, dentry=%s\n", __func__,
			dir->i_ino, dentry->d_name.name);
	inode = create_helper(dir, dentry, mode, false);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	return 0;
}

static int ezfs_unlink_dentry(struct buffer_head *bh, struct dentry *dentry)
{
	int i, ret = 0;
	struct ezfs_dir_entry *ezfs_dentry = (struct ezfs_dir_entry *) bh->b_data;

	for (i = 0; i < EZFS_MAX_CHILDREN; ++i, ++ezfs_dentry) {
		if (ezfs_dentry->active && strlen(ezfs_dentry->filename) &&
			!memcmp(ezfs_dentry->filename,
			dentry->d_name.name, dentry->d_name.len)) {
			memset(ezfs_dentry, 0, sizeof(struct ezfs_dir_entry));
			mark_buffer_dirty(bh);
			ret = 1;
			goto out;
		}
	}

out:
	brelse(bh);
	return ret;
}

int ezfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	uint64_t dir_blk_num = get_ezfs_inode(dir)->data_block_number;
	struct buffer_head *bh = sb_bread(dir->i_sb, dir_blk_num);

	if (!bh)
		return -EIO;

	if (ezfs_unlink_dentry(bh, dentry)) {
		debug("[%s] dir_ino=%ld, dentry=%s\n", __func__,
			dir->i_ino, dentry->d_name.name);
		inode->i_ctime = dir->i_ctime = dir->i_mtime = current_time(inode);
		drop_nlink(inode);
		mark_inode_dirty(dir);
	}

	return 0;
}

int ezfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;

	debug("[%s] dir_ino=%ld, dentry=%s\n", __func__,
			dir->i_ino, dentry->d_name.name);
	inode = create_helper(dir, dentry, mode, true);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	return 0;
}

static int ezfs_dir_empty(struct buffer_head *bh)
{
	int i, ret = 1;
	struct ezfs_dir_entry *dentry = (struct ezfs_dir_entry *)bh->b_data;

	for (i = 0; i < EZFS_MAX_CHILDREN; ++i, ++dentry) {
		if (dentry->active) {
			ret = 0;
			break;
		}
	}
	brelse(bh);
	return ret;
}

int ezfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct buffer_head *dir_bh = sb_bread(dir->i_sb,
			get_ezfs_inode(d_inode(dentry))->data_block_number);

	debug("[%s] dir_ino=%ld, dentry=%s\n", __func__,
			d_inode(dentry)->i_ino, dentry->d_name.name);

	if (!dir_bh)
		return -EIO;

	if (!ezfs_dir_empty(dir_bh))
		return -ENOTEMPTY;

	/* the directory is empty, rmdir */
	ezfs_unlink(dir, dentry);
	/* drop nlink for . */
	drop_nlink(d_inode(dentry));
	/* drop nlink for .. */
	drop_nlink(dir);
	return 0;
}

int ezfs_rename(struct inode *old_dir, struct dentry *old_dentry,
	struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
	int i, ret;
	struct ezfs_dir_entry *ezfs_dentry;
	struct buffer_head *new_bh, *old_bh;

	debug("[%s] old_dentry=%s new_dentry=%s\n", __func__,
			old_dentry->d_name.name, new_dentry->d_name.name);

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	if (strnlen(new_dentry->d_name.name, EZFS_MAX_FILENAME_LENGTH + 1) >
			EZFS_MAX_FILENAME_LENGTH)
		return -ENAMETOOLONG;

	/* Find an empty ezfs dentry */
	new_bh = sb_bread(new_dir->i_sb, get_ezfs_inode(new_dir)->data_block_number);
	if (!new_bh)
		return -EIO;

	ezfs_dentry = (struct ezfs_dir_entry *) new_bh->b_data;
	for (i = 0; i < EZFS_MAX_CHILDREN && ezfs_dentry->active; ++i, ++ezfs_dentry);
	if (i >= EZFS_MAX_CHILDREN) {
		brelse(new_bh);
		return -ENOSPC;
	}

	old_bh = sb_bread(new_dir->i_sb, get_ezfs_inode(old_dir)->data_block_number);
	if (!old_bh) {
		brelse(new_bh);
		return -EIO;
	}

	if (d_really_is_positive(new_dentry)) {
		if (d_is_dir(old_dentry)) {
			if ((ret = ezfs_rmdir(new_dir, new_dentry))) {
				brelse(new_bh);
				brelse(old_bh);
				return ret;
			}
		}
		else
			ezfs_unlink(new_dir, new_dentry);
	}

	if (d_is_dir(old_dentry)) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}

	/* Populate the new ezfs_dentry found above */
	ezfs_dentry->inode_no = d_inode(old_dentry)->i_ino;
	ezfs_dentry->active = 1;
	strncpy(ezfs_dentry->filename, new_dentry->d_name.name, EZFS_MAX_FILENAME_LENGTH);
	mark_buffer_dirty(new_bh);
	brelse(new_bh);

	/* Deactivate the old ezfs_dentry */
	ezfs_unlink_dentry(old_bh, old_dentry);

	old_dir->i_ctime = old_dir->i_mtime = new_dir->i_ctime =
	new_dir->i_mtime = d_inode(old_dentry)->i_ctime = current_time(old_dir);

	mark_inode_dirty(old_dir);
	mark_inode_dirty(new_dir);
	mark_inode_dirty(d_inode(old_dentry));

	return 0;
}

/* ezfs_sb_ops */
void ezfs_evict_inode(struct inode *inode)
{
	int i;
	struct buffer_head *sb_bh = get_ezfs_sb_bh(inode->i_sb);
	struct ezfs_super_block *ezfs_sb = get_ezfs_sb(inode->i_sb);
	struct ezfs_inode *ezfs_inode = get_ezfs_inode(inode);

	debug("[%s] ino=%ld\n", __func__, inode->i_ino);

	mutex_lock(ezfs_sb->ezfs_lock);
	if (!inode->i_nlink) {
		int data_blk_num = ezfs_inode->data_block_number;

		debug("[%s] CLEARBIT i_ino=%ld, d_num=[%d-%d]\n", __func__, inode->i_ino,
			data_blk_num, data_blk_num + (int) inode->i_blocks / 8 - 1);
		CLEARBIT(ezfs_sb->free_inodes, inode->i_ino - EZFS_ROOT_INODE_NUMBER);
		for (i = 0; i < inode->i_blocks / 8; i++) {
			CLEARBIT(ezfs_sb->free_data_blocks, data_blk_num -
					EZFS_ROOT_DATABLOCK_NUMBER + i);
		}
		mark_buffer_dirty(sb_bh);
	}

	/* required to be called by VFS, if not called, evict() will BUG out */
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	mutex_unlock(ezfs_sb->ezfs_lock);
}

int ezfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret = 0;
	struct buffer_head *i_bh = get_ezfs_i_bh(inode->i_sb);

	debug("[%s] ino=%ld\n", __func__, inode->i_ino);

	write_inode_helper(inode, get_ezfs_inode(inode));
	mark_buffer_dirty(i_bh);
	if (wbc->sync_mode == WB_SYNC_ALL) {
		sync_dirty_buffer(i_bh);
		if (buffer_req(i_bh) && !buffer_uptodate(i_bh)) {
			return -EIO;
		}
	}

	return ret;
}

static int ezfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct buffer_head *bh;
	struct ezfs_sb_buffer_heads *ezfs_sb_bufs = sb->s_fs_info;
	struct ezfs_super_block *ezfs_sb;
	struct inode *inode;

	sb->s_maxbytes		= EZFS_BLOCK_SIZE * EZFS_MAX_DATA_BLKS;
	sb->s_magic			= EZFS_MAGIC_NUMBER;
	sb->s_op			= &ezfs_sb_ops;
	sb->s_time_gran		= 1;
	sb->s_time_min		= 0;
	sb->s_time_max		= U32_MAX;

	/* if ezfs_fill_super fails, ezfs_free_fc will free allocated resources */
	if (!sb_set_blocksize(sb, EZFS_BLOCK_SIZE))
		return -EIO;
	bh = sb_bread(sb, EZFS_SUPERBLOCK_DATABLOCK_NUMBER);
	if (!bh)
		return -EIO;
	ezfs_sb_bufs->sb_bh = bh;

	ezfs_sb = get_ezfs_sb(sb);
	ezfs_sb->ezfs_lock = kzalloc(sizeof(struct mutex), GFP_KERNEL);
	if (!ezfs_sb->ezfs_lock)
		return -ENOMEM;
	mutex_init(ezfs_sb->ezfs_lock);
	if (ezfs_sb->magic != EZFS_MAGIC_NUMBER)
		return -EIO;

	bh = sb_bread(sb, EZFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!bh)
		return -EIO;
	ezfs_sb_bufs->i_store_bh = bh;

	inode = ezfs_iget(sb, EZFS_ROOT_INODE_NUMBER);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static void ezfs_free_fc(struct fs_context *fc)
{
	struct ezfs_sb_buffer_heads *ezfs_sb_bufs = fc->s_fs_info;
	struct ezfs_super_block *ezfs_sb;

	debug("[%s] %p\n", __func__, ezfs_sb_bufs);
	if (ezfs_sb_bufs) {
		if (ezfs_sb_bufs->i_store_bh)
			brelse(ezfs_sb_bufs->i_store_bh);
		if (ezfs_sb_bufs->sb_bh) {
			ezfs_sb = (struct ezfs_super_block *) ezfs_sb_bufs->sb_bh->b_data;
			if (ezfs_sb->ezfs_lock) {
				mutex_destroy(ezfs_sb->ezfs_lock);
				kfree(ezfs_sb->ezfs_lock);
			}
			brelse(ezfs_sb_bufs->sb_bh);
		}
		kfree(ezfs_sb_bufs);
	}
}

static int ezfs_get_tree(struct fs_context *fc)
{
	int ret = get_tree_bdev(fc, ezfs_fill_super);

	if (ret)
		debug("Failed to mount myezfs. Error:[%d]", ret);
	else
		debug("Successfully mount myezfs, EZFS_MAX_INODES %lu\n",
			   EZFS_MAX_INODES);

	return ret;
}

static const struct fs_context_operations ezfs_context_ops = {
	.free		= ezfs_free_fc,
	.get_tree	= ezfs_get_tree,
};

int ezfs_init_fs_context(struct fs_context *fc)
{
	struct ezfs_sb_buffer_heads *ezfs_sb_bufs;

	debug("[%s]\n", __func__);
	ezfs_sb_bufs = kzalloc(sizeof(*ezfs_sb_bufs), GFP_KERNEL);
	if (!ezfs_sb_bufs)
		return -ENOMEM;
	fc->s_fs_info = ezfs_sb_bufs;
	fc->ops = &ezfs_context_ops;
	return 0;
}

static void ezfs_kill_superblock(struct super_block *sb)
{
	struct ezfs_sb_buffer_heads *ezfs_sb_bufs = sb->s_fs_info;
	struct ezfs_super_block *ezfs_sb = (struct ezfs_super_block *)
										ezfs_sb_bufs->sb_bh->b_data;

	kill_block_super(sb);
	brelse(ezfs_sb_bufs->i_store_bh);
	mutex_destroy(ezfs_sb->ezfs_lock);
	kfree(ezfs_sb->ezfs_lock);
	brelse(ezfs_sb_bufs->sb_bh);
	kfree(ezfs_sb_bufs);
	debug("ezfs superblock destroyed. Unmount successful.\n");
}

struct file_system_type ezfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "ezfs",
	.init_fs_context = ezfs_init_fs_context,
	.kill_sb = ezfs_kill_superblock,
};

static int ezfs_init(void)
{
	int ret;

	ret = register_filesystem(&ezfs_fs_type);
	if (likely(ret == 0))
		debug("Successfully registered ezfs\n");
	else
		debug("Failed to register ezfs. Error:[%d]", ret);

	return ret;
}

static void ezfs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&ezfs_fs_type);

	if (likely(ret == 0))
		debug("Successfully unregistered ezfs\n");
	else
		debug("Failed to unregister ezfs. Error:[%d]", ret);
}

module_init(ezfs_init);
module_exit(ezfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sol");
