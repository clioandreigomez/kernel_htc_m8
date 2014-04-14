/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * inode.c
 *
 * vfs' aops, fops, dops and iops
 *
 * Copyright (C) 2002, 2004 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>

#include <asm/byteorder.h>

#include <cluster/masklog.h>

#include "ocfs2.h"

#include "alloc.h"
#include "dir.h"
#include "blockcheck.h"
#include "dlmglue.h"
#include "extent_map.h"
#include "file.h"
#include "heartbeat.h"
#include "inode.h"
#include "journal.h"
#include "namei.h"
#include "suballoc.h"
#include "super.h"
#include "symlink.h"
#include "sysfile.h"
#include "uptodate.h"
#include "xattr.h"
#include "refcounttree.h"
#include "ocfs2_trace.h"

#include "buffer_head_io.h"

struct ocfs2_find_inode_args
{
	u64		fi_blkno;
	unsigned long	fi_ino;
	unsigned int	fi_flags;
	unsigned int	fi_sysfile_type;
};

static struct lock_class_key ocfs2_sysfile_lock_key[NUM_SYSTEM_INODES];

static int ocfs2_read_locked_inode(struct inode *inode,
				   struct ocfs2_find_inode_args *args);
static int ocfs2_init_locked_inode(struct inode *inode, void *opaque);
static int ocfs2_find_actor(struct inode *inode, void *opaque);
static int ocfs2_truncate_for_delete(struct ocfs2_super *osb,
				    struct inode *inode,
				    struct buffer_head *fe_bh);

void ocfs2_set_inode_flags(struct inode *inode)
{
	unsigned int flags = OCFS2_I(inode)->ip_attr;

	inode->i_flags &= ~(S_IMMUTABLE |
		S_SYNC | S_APPEND | S_NOATIME | S_DIRSYNC);

	if (flags & OCFS2_IMMUTABLE_FL)
		inode->i_flags |= S_IMMUTABLE;

	if (flags & OCFS2_SYNC_FL)
		inode->i_flags |= S_SYNC;
	if (flags & OCFS2_APPEND_FL)
		inode->i_flags |= S_APPEND;
	if (flags & OCFS2_NOATIME_FL)
		inode->i_flags |= S_NOATIME;
	if (flags & OCFS2_DIRSYNC_FL)
		inode->i_flags |= S_DIRSYNC;
}

void ocfs2_get_inode_flags(struct ocfs2_inode_info *oi)
{
	unsigned int flags = oi->vfs_inode.i_flags;

	oi->ip_attr &= ~(OCFS2_SYNC_FL|OCFS2_APPEND_FL|
			OCFS2_IMMUTABLE_FL|OCFS2_NOATIME_FL|OCFS2_DIRSYNC_FL);
	if (flags & S_SYNC)
		oi->ip_attr |= OCFS2_SYNC_FL;
	if (flags & S_APPEND)
		oi->ip_attr |= OCFS2_APPEND_FL;
	if (flags & S_IMMUTABLE)
		oi->ip_attr |= OCFS2_IMMUTABLE_FL;
	if (flags & S_NOATIME)
		oi->ip_attr |= OCFS2_NOATIME_FL;
	if (flags & S_DIRSYNC)
		oi->ip_attr |= OCFS2_DIRSYNC_FL;
}

struct inode *ocfs2_ilookup(struct super_block *sb, u64 blkno)
{
	struct ocfs2_find_inode_args args;

	args.fi_blkno = blkno;
	args.fi_flags = 0;
	args.fi_ino = ino_from_blkno(sb, blkno);
	args.fi_sysfile_type = 0;

	return ilookup5(sb, blkno, ocfs2_find_actor, &args);
}
struct inode *ocfs2_iget(struct ocfs2_super *osb, u64 blkno, unsigned flags,
			 int sysfile_type)
{
	struct inode *inode = NULL;
	struct super_block *sb = osb->sb;
	struct ocfs2_find_inode_args args;

	trace_ocfs2_iget_begin((unsigned long long)blkno, flags,
			       sysfile_type);

	if (blkno == 0) {
		inode = ERR_PTR(-EINVAL);
		mlog_errno(PTR_ERR(inode));
		goto bail;
	}

	args.fi_blkno = blkno;
	args.fi_flags = flags;
	args.fi_ino = ino_from_blkno(sb, blkno);
	args.fi_sysfile_type = sysfile_type;

	inode = iget5_locked(sb, args.fi_ino, ocfs2_find_actor,
			     ocfs2_init_locked_inode, &args);
	if (inode == NULL) {
		inode = ERR_PTR(-ENOMEM);
		mlog_errno(PTR_ERR(inode));
		goto bail;
	}
	trace_ocfs2_iget5_locked(inode->i_state);
	if (inode->i_state & I_NEW) {
		ocfs2_read_locked_inode(inode, &args);
		unlock_new_inode(inode);
	}
	if (is_bad_inode(inode)) {
		iput(inode);
		inode = ERR_PTR(-ESTALE);
		goto bail;
	}

bail:
	if (!IS_ERR(inode)) {
		trace_ocfs2_iget_end(inode, 
			(unsigned long long)OCFS2_I(inode)->ip_blkno);
	}

	return inode;
}



static int ocfs2_find_actor(struct inode *inode, void *opaque)
{
	struct ocfs2_find_inode_args *args = NULL;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);
	int ret = 0;

	args = opaque;

	mlog_bug_on_msg(!inode, "No inode in find actor!\n");

	trace_ocfs2_find_actor(inode, inode->i_ino, opaque, args->fi_blkno);

	if (oi->ip_blkno != args->fi_blkno)
		goto bail;

	ret = 1;
bail:
	return ret;
}

static int ocfs2_init_locked_inode(struct inode *inode, void *opaque)
{
	struct ocfs2_find_inode_args *args = opaque;
	static struct lock_class_key ocfs2_quota_ip_alloc_sem_key,
				     ocfs2_file_ip_alloc_sem_key;

	inode->i_ino = args->fi_ino;
	OCFS2_I(inode)->ip_blkno = args->fi_blkno;
	if (args->fi_sysfile_type != 0)
		lockdep_set_class(&inode->i_mutex,
			&ocfs2_sysfile_lock_key[args->fi_sysfile_type]);
	if (args->fi_sysfile_type == USER_QUOTA_SYSTEM_INODE ||
	    args->fi_sysfile_type == GROUP_QUOTA_SYSTEM_INODE ||
	    args->fi_sysfile_type == LOCAL_USER_QUOTA_SYSTEM_INODE ||
	    args->fi_sysfile_type == LOCAL_GROUP_QUOTA_SYSTEM_INODE)
		lockdep_set_class(&OCFS2_I(inode)->ip_alloc_sem,
				  &ocfs2_quota_ip_alloc_sem_key);
	else
		lockdep_set_class(&OCFS2_I(inode)->ip_alloc_sem,
				  &ocfs2_file_ip_alloc_sem_key);

	return 0;
}

void ocfs2_populate_inode(struct inode *inode, struct ocfs2_dinode *fe,
			  int create_ino)
{
	struct super_block *sb;
	struct ocfs2_super *osb;
	int use_plocks = 1;

	sb = inode->i_sb;
	osb = OCFS2_SB(sb);

	if ((osb->s_mount_opt & OCFS2_MOUNT_LOCALFLOCKS) ||
	    ocfs2_mount_local(osb) || !ocfs2_stack_supports_plocks())
		use_plocks = 0;

	BUG_ON(!OCFS2_IS_VALID_DINODE(fe));  
	BUG_ON(!(fe->i_flags & cpu_to_le32(OCFS2_VALID_FL)));
	BUG_ON(le32_to_cpu(fe->i_fs_generation) != osb->fs_generation);


	OCFS2_I(inode)->ip_clusters = le32_to_cpu(fe->i_clusters);
	OCFS2_I(inode)->ip_attr = le32_to_cpu(fe->i_attr);
	OCFS2_I(inode)->ip_dyn_features = le16_to_cpu(fe->i_dyn_features);

	inode->i_version = 1;
	inode->i_generation = le32_to_cpu(fe->i_generation);
	inode->i_rdev = huge_decode_dev(le64_to_cpu(fe->id1.dev1.i_rdev));
	inode->i_mode = le16_to_cpu(fe->i_mode);
	inode->i_uid = le32_to_cpu(fe->i_uid);
	inode->i_gid = le32_to_cpu(fe->i_gid);

	
	if (S_ISLNK(inode->i_mode) && !fe->i_clusters)
		inode->i_blocks = 0;
	else
		inode->i_blocks = ocfs2_inode_sector_count(inode);
	inode->i_mapping->a_ops = &ocfs2_aops;
	inode->i_atime.tv_sec = le64_to_cpu(fe->i_atime);
	inode->i_atime.tv_nsec = le32_to_cpu(fe->i_atime_nsec);
	inode->i_mtime.tv_sec = le64_to_cpu(fe->i_mtime);
	inode->i_mtime.tv_nsec = le32_to_cpu(fe->i_mtime_nsec);
	inode->i_ctime.tv_sec = le64_to_cpu(fe->i_ctime);
	inode->i_ctime.tv_nsec = le32_to_cpu(fe->i_ctime_nsec);

	if (OCFS2_I(inode)->ip_blkno != le64_to_cpu(fe->i_blkno))
		mlog(ML_ERROR,
		     "ip_blkno %llu != i_blkno %llu!\n",
		     (unsigned long long)OCFS2_I(inode)->ip_blkno,
		     (unsigned long long)le64_to_cpu(fe->i_blkno));

	set_nlink(inode, ocfs2_read_links_count(fe));

	trace_ocfs2_populate_inode(OCFS2_I(inode)->ip_blkno,
				   le32_to_cpu(fe->i_flags));
	if (fe->i_flags & cpu_to_le32(OCFS2_SYSTEM_FL)) {
		OCFS2_I(inode)->ip_flags |= OCFS2_INODE_SYSTEM_FILE;
		inode->i_flags |= S_NOQUOTA;
	}
  
	if (fe->i_flags & cpu_to_le32(OCFS2_LOCAL_ALLOC_FL)) {
		OCFS2_I(inode)->ip_flags |= OCFS2_INODE_BITMAP;
	} else if (fe->i_flags & cpu_to_le32(OCFS2_BITMAP_FL)) {
		OCFS2_I(inode)->ip_flags |= OCFS2_INODE_BITMAP;
	} else if (fe->i_flags & cpu_to_le32(OCFS2_QUOTA_FL)) {
		inode->i_flags |= S_NOQUOTA;
	} else if (fe->i_flags & cpu_to_le32(OCFS2_SUPER_BLOCK_FL)) {
		BUG();
	}

	switch (inode->i_mode & S_IFMT) {
	    case S_IFREG:
		    if (use_plocks)
			    inode->i_fop = &ocfs2_fops;
		    else
			    inode->i_fop = &ocfs2_fops_no_plocks;
		    inode->i_op = &ocfs2_file_iops;
		    i_size_write(inode, le64_to_cpu(fe->i_size));
		    break;
	    case S_IFDIR:
		    inode->i_op = &ocfs2_dir_iops;
		    if (use_plocks)
			    inode->i_fop = &ocfs2_dops;
		    else
			    inode->i_fop = &ocfs2_dops_no_plocks;
		    i_size_write(inode, le64_to_cpu(fe->i_size));
		    OCFS2_I(inode)->ip_dir_lock_gen = 1;
		    break;
	    case S_IFLNK:
		    if (ocfs2_inode_is_fast_symlink(inode))
			inode->i_op = &ocfs2_fast_symlink_inode_operations;
		    else
			inode->i_op = &ocfs2_symlink_inode_operations;
		    i_size_write(inode, le64_to_cpu(fe->i_size));
		    break;
	    default:
		    inode->i_op = &ocfs2_special_file_iops;
		    init_special_inode(inode, inode->i_mode,
				       inode->i_rdev);
		    break;
	}

	if (create_ino) {
		inode->i_ino = ino_from_blkno(inode->i_sb,
			       le64_to_cpu(fe->i_blkno));

		BUG_ON(le32_to_cpu(fe->i_flags) & OCFS2_SYSTEM_FL);

		ocfs2_inode_lock_res_init(&OCFS2_I(inode)->ip_inode_lockres,
					  OCFS2_LOCK_TYPE_META, 0, inode);

		ocfs2_inode_lock_res_init(&OCFS2_I(inode)->ip_open_lockres,
					  OCFS2_LOCK_TYPE_OPEN, 0, inode);
	}

	ocfs2_inode_lock_res_init(&OCFS2_I(inode)->ip_rw_lockres,
				  OCFS2_LOCK_TYPE_RW, inode->i_generation,
				  inode);

	ocfs2_set_inode_flags(inode);

	OCFS2_I(inode)->ip_last_used_slot = 0;
	OCFS2_I(inode)->ip_last_used_group = 0;

	if (S_ISDIR(inode->i_mode))
		ocfs2_resv_set_type(&OCFS2_I(inode)->ip_la_data_resv,
				    OCFS2_RESV_FLAG_DIR);
}

static int ocfs2_read_locked_inode(struct inode *inode,
				   struct ocfs2_find_inode_args *args)
{
	struct super_block *sb;
	struct ocfs2_super *osb;
	struct ocfs2_dinode *fe;
	struct buffer_head *bh = NULL;
	int status, can_lock;
	u32 generation = 0;

	status = -EINVAL;
	if (inode == NULL || inode->i_sb == NULL) {
		mlog(ML_ERROR, "bad inode\n");
		return status;
	}
	sb = inode->i_sb;
	osb = OCFS2_SB(sb);

	if (!args) {
		mlog(ML_ERROR, "bad inode args\n");
		make_bad_inode(inode);
		return status;
	}

	can_lock = !(args->fi_flags & OCFS2_FI_FLAG_SYSFILE)
		&& !(args->fi_flags & OCFS2_FI_FLAG_ORPHAN_RECOVERY)
		&& !ocfs2_mount_local(osb);

	trace_ocfs2_read_locked_inode(
		(unsigned long long)OCFS2_I(inode)->ip_blkno, can_lock);

	if (args->fi_flags & OCFS2_FI_FLAG_SYSFILE)
		generation = osb->fs_generation;

	ocfs2_inode_lock_res_init(&OCFS2_I(inode)->ip_inode_lockres,
				  OCFS2_LOCK_TYPE_META,
				  generation, inode);

	ocfs2_inode_lock_res_init(&OCFS2_I(inode)->ip_open_lockres,
				  OCFS2_LOCK_TYPE_OPEN,
				  0, inode);

	if (can_lock) {
		status = ocfs2_open_lock(inode);
		if (status) {
			make_bad_inode(inode);
			mlog_errno(status);
			return status;
		}
		status = ocfs2_inode_lock(inode, NULL, 0);
		if (status) {
			make_bad_inode(inode);
			mlog_errno(status);
			return status;
		}
	}

	if (args->fi_flags & OCFS2_FI_FLAG_ORPHAN_RECOVERY) {
		status = ocfs2_try_open_lock(inode, 0);
		if (status) {
			make_bad_inode(inode);
			return status;
		}
	}

	if (can_lock) {
		status = ocfs2_read_inode_block_full(inode, &bh,
						     OCFS2_BH_IGNORE_CACHE);
	} else {
		status = ocfs2_read_blocks_sync(osb, args->fi_blkno, 1, &bh);
		if (!status && !buffer_jbd(bh))
			status = ocfs2_validate_inode_block(osb->sb, bh);
	}
	if (status < 0) {
		mlog_errno(status);
		goto bail;
	}

	status = -EINVAL;
	fe = (struct ocfs2_dinode *) bh->b_data;

	mlog_bug_on_msg(!!(fe->i_flags & cpu_to_le32(OCFS2_SYSTEM_FL)) !=
			!!(args->fi_flags & OCFS2_FI_FLAG_SYSFILE),
			"Inode %llu: system file state is ambigous\n",
			(unsigned long long)args->fi_blkno);

	if (S_ISCHR(le16_to_cpu(fe->i_mode)) ||
	    S_ISBLK(le16_to_cpu(fe->i_mode)))
		inode->i_rdev = huge_decode_dev(le64_to_cpu(fe->id1.dev1.i_rdev));

	ocfs2_populate_inode(inode, fe, 0);

	BUG_ON(args->fi_blkno != le64_to_cpu(fe->i_blkno));

	status = 0;

bail:
	if (can_lock)
		ocfs2_inode_unlock(inode, 0);

	if (status < 0)
		make_bad_inode(inode);

	if (args && bh)
		brelse(bh);

	return status;
}

void ocfs2_sync_blockdev(struct super_block *sb)
{
	sync_blockdev(sb->s_bdev);
}

static int ocfs2_truncate_for_delete(struct ocfs2_super *osb,
				     struct inode *inode,
				     struct buffer_head *fe_bh)
{
	int status = 0;
	struct ocfs2_dinode *fe;
	handle_t *handle = NULL;

	fe = (struct ocfs2_dinode *) fe_bh->b_data;

	if (fe->i_clusters) {
		if (ocfs2_should_order_data(inode))
			ocfs2_begin_ordered_truncate(inode, 0);

		handle = ocfs2_start_trans(osb, OCFS2_INODE_UPDATE_CREDITS);
		if (IS_ERR(handle)) {
			status = PTR_ERR(handle);
			handle = NULL;
			mlog_errno(status);
			goto out;
		}

		status = ocfs2_journal_access_di(handle, INODE_CACHE(inode),
						 fe_bh,
						 OCFS2_JOURNAL_ACCESS_WRITE);
		if (status < 0) {
			mlog_errno(status);
			goto out;
		}

		i_size_write(inode, 0);

		status = ocfs2_mark_inode_dirty(handle, inode, fe_bh);
		if (status < 0) {
			mlog_errno(status);
			goto out;
		}

		ocfs2_commit_trans(osb, handle);
		handle = NULL;

		status = ocfs2_commit_truncate(osb, inode, fe_bh);
		if (status < 0) {
			mlog_errno(status);
			goto out;
		}
	}

out:
	if (handle)
		ocfs2_commit_trans(osb, handle);
	return status;
}

static int ocfs2_remove_inode(struct inode *inode,
			      struct buffer_head *di_bh,
			      struct inode *orphan_dir_inode,
			      struct buffer_head *orphan_dir_bh)
{
	int status;
	struct inode *inode_alloc_inode = NULL;
	struct buffer_head *inode_alloc_bh = NULL;
	handle_t *handle;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct ocfs2_dinode *di = (struct ocfs2_dinode *) di_bh->b_data;

	inode_alloc_inode =
		ocfs2_get_system_file_inode(osb, INODE_ALLOC_SYSTEM_INODE,
					    le16_to_cpu(di->i_suballoc_slot));
	if (!inode_alloc_inode) {
		status = -EEXIST;
		mlog_errno(status);
		goto bail;
	}

	mutex_lock(&inode_alloc_inode->i_mutex);
	status = ocfs2_inode_lock(inode_alloc_inode, &inode_alloc_bh, 1);
	if (status < 0) {
		mutex_unlock(&inode_alloc_inode->i_mutex);

		mlog_errno(status);
		goto bail;
	}

	handle = ocfs2_start_trans(osb, OCFS2_DELETE_INODE_CREDITS +
				   ocfs2_quota_trans_credits(inode->i_sb));
	if (IS_ERR(handle)) {
		status = PTR_ERR(handle);
		mlog_errno(status);
		goto bail_unlock;
	}

	if (!(OCFS2_I(inode)->ip_flags & OCFS2_INODE_SKIP_ORPHAN_DIR)) {
		status = ocfs2_orphan_del(osb, handle, orphan_dir_inode, inode,
					  orphan_dir_bh);
		if (status < 0) {
			mlog_errno(status);
			goto bail_commit;
		}
	}

	
	status = ocfs2_journal_access_di(handle, INODE_CACHE(inode), di_bh,
					 OCFS2_JOURNAL_ACCESS_WRITE);
	if (status < 0) {
		mlog_errno(status);
		goto bail_commit;
	}

	di->i_dtime = cpu_to_le64(CURRENT_TIME.tv_sec);
	di->i_flags &= cpu_to_le32(~(OCFS2_VALID_FL | OCFS2_ORPHANED_FL));
	ocfs2_journal_dirty(handle, di_bh);

	ocfs2_remove_from_cache(INODE_CACHE(inode), di_bh);
	dquot_free_inode(inode);

	status = ocfs2_free_dinode(handle, inode_alloc_inode,
				   inode_alloc_bh, di);
	if (status < 0)
		mlog_errno(status);

bail_commit:
	ocfs2_commit_trans(osb, handle);
bail_unlock:
	ocfs2_inode_unlock(inode_alloc_inode, 1);
	mutex_unlock(&inode_alloc_inode->i_mutex);
	brelse(inode_alloc_bh);
bail:
	iput(inode_alloc_inode);

	return status;
}

static int ocfs2_check_orphan_recovery_state(struct ocfs2_super *osb,
					     int slot)
{
	int ret = 0;

	spin_lock(&osb->osb_lock);
	if (ocfs2_node_map_test_bit(osb, &osb->osb_recovering_orphan_dirs, slot)) {
		ret = -EDEADLK;
		goto out;
	}
	osb->osb_orphan_wipes[slot]++;
out:
	spin_unlock(&osb->osb_lock);
	trace_ocfs2_check_orphan_recovery_state(slot, ret);
	return ret;
}

static void ocfs2_signal_wipe_completion(struct ocfs2_super *osb,
					 int slot)
{
	spin_lock(&osb->osb_lock);
	osb->osb_orphan_wipes[slot]--;
	spin_unlock(&osb->osb_lock);

	wake_up(&osb->osb_wipe_event);
}

static int ocfs2_wipe_inode(struct inode *inode,
			    struct buffer_head *di_bh)
{
	int status, orphaned_slot = -1;
	struct inode *orphan_dir_inode = NULL;
	struct buffer_head *orphan_dir_bh = NULL;
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);
	struct ocfs2_dinode *di = (struct ocfs2_dinode *) di_bh->b_data;

	if (!(OCFS2_I(inode)->ip_flags & OCFS2_INODE_SKIP_ORPHAN_DIR)) {
		orphaned_slot = le16_to_cpu(di->i_orphaned_slot);

		status = ocfs2_check_orphan_recovery_state(osb, orphaned_slot);
		if (status)
			return status;

		orphan_dir_inode = ocfs2_get_system_file_inode(osb,
							       ORPHAN_DIR_SYSTEM_INODE,
							       orphaned_slot);
		if (!orphan_dir_inode) {
			status = -EEXIST;
			mlog_errno(status);
			goto bail;
		}

		mutex_lock(&orphan_dir_inode->i_mutex);
		status = ocfs2_inode_lock(orphan_dir_inode, &orphan_dir_bh, 1);
		if (status < 0) {
			mutex_unlock(&orphan_dir_inode->i_mutex);

			mlog_errno(status);
			goto bail;
		}
	}

	status = ocfs2_truncate_for_delete(osb, inode, di_bh);
	if (status < 0) {
		mlog_errno(status);
		goto bail_unlock_dir;
	}

	
	if (S_ISDIR(inode->i_mode)) {
		status = ocfs2_dx_dir_truncate(inode, di_bh);
		if (status) {
			mlog_errno(status);
			goto bail_unlock_dir;
		}
	}

	
	status = ocfs2_xattr_remove(inode, di_bh);
	if (status < 0) {
		mlog_errno(status);
		goto bail_unlock_dir;
	}

	status = ocfs2_remove_refcount_tree(inode, di_bh);
	if (status < 0) {
		mlog_errno(status);
		goto bail_unlock_dir;
	}

	status = ocfs2_remove_inode(inode, di_bh, orphan_dir_inode,
				    orphan_dir_bh);
	if (status < 0)
		mlog_errno(status);

bail_unlock_dir:
	if (OCFS2_I(inode)->ip_flags & OCFS2_INODE_SKIP_ORPHAN_DIR)
		return status;

	ocfs2_inode_unlock(orphan_dir_inode, 1);
	mutex_unlock(&orphan_dir_inode->i_mutex);
	brelse(orphan_dir_bh);
bail:
	iput(orphan_dir_inode);
	ocfs2_signal_wipe_completion(osb, orphaned_slot);

	return status;
}

static int ocfs2_inode_is_valid_to_delete(struct inode *inode)
{
	int ret = 0;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);
	struct ocfs2_super *osb = OCFS2_SB(inode->i_sb);

	trace_ocfs2_inode_is_valid_to_delete(current, osb->dc_task,
					     (unsigned long long)oi->ip_blkno,
					     oi->ip_flags);

	if (inode == osb->root_inode) {
		mlog(ML_ERROR, "Skipping delete of root inode.\n");
		goto bail;
	}

	if (current == osb->dc_task)
		goto bail;

	spin_lock(&oi->ip_lock);
	if (oi->ip_flags & OCFS2_INODE_SYSTEM_FILE) {
		mlog(ML_ERROR, "Skipping delete of system file %llu\n",
		     (unsigned long long)oi->ip_blkno);
		goto bail_unlock;
	}

	if (oi->ip_flags & OCFS2_INODE_SKIP_DELETE)
		goto bail_unlock;

	ret = 1;
bail_unlock:
	spin_unlock(&oi->ip_lock);
bail:
	return ret;
}

static int ocfs2_query_inode_wipe(struct inode *inode,
				  struct buffer_head *di_bh,
				  int *wipe)
{
	int status = 0, reason = 0;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);
	struct ocfs2_dinode *di;

	*wipe = 0;

	trace_ocfs2_query_inode_wipe_begin((unsigned long long)oi->ip_blkno,
					   inode->i_nlink);

	if (!ocfs2_inode_is_valid_to_delete(inode)) {
		reason = 1;
		goto bail;
	}

	if (inode->i_nlink)
		goto bail;

	
	di = (struct ocfs2_dinode *) di_bh->b_data;
	if (!(di->i_flags & cpu_to_le32(OCFS2_ORPHANED_FL)) &&
	    !(oi->ip_flags & OCFS2_INODE_SKIP_ORPHAN_DIR)) {
		if (di->i_dyn_features & cpu_to_le16(OCFS2_HAS_REFCOUNT_FL)) {
			reason = 2;
			goto bail;
		}

		
		status = -EEXIST;
		mlog(ML_ERROR,
		     "Inode %llu (on-disk %llu) not orphaned! "
		     "Disk flags  0x%x, inode flags 0x%x\n",
		     (unsigned long long)oi->ip_blkno,
		     (unsigned long long)le64_to_cpu(di->i_blkno),
		     le32_to_cpu(di->i_flags), oi->ip_flags);
		goto bail;
	}

	
	if (di->i_dtime) {
		status = -EEXIST;
		mlog_errno(status);
		goto bail;
	}

	status = ocfs2_try_open_lock(inode, 1);
	if (status == -EAGAIN) {
		status = 0;
		reason = 3;
		goto bail;
	}
	if (status < 0) {
		mlog_errno(status);
		goto bail;
	}

	*wipe = 1;
	trace_ocfs2_query_inode_wipe_succ(le16_to_cpu(di->i_orphaned_slot));

bail:
	trace_ocfs2_query_inode_wipe_end(status, reason);
	return status;
}

static void ocfs2_cleanup_delete_inode(struct inode *inode,
				       int sync_data)
{
	trace_ocfs2_cleanup_delete_inode(
		(unsigned long long)OCFS2_I(inode)->ip_blkno, sync_data);
	if (sync_data)
		filemap_write_and_wait(inode->i_mapping);
	truncate_inode_pages(&inode->i_data, 0);
}

static void ocfs2_delete_inode(struct inode *inode)
{
	int wipe, status;
	sigset_t oldset;
	struct buffer_head *di_bh = NULL;

	trace_ocfs2_delete_inode(inode->i_ino,
				 (unsigned long long)OCFS2_I(inode)->ip_blkno,
				 is_bad_inode(inode));

	if (is_bad_inode(inode) || !OCFS2_I(inode)->ip_blkno)
		goto bail;

	dquot_initialize(inode);

	if (!ocfs2_inode_is_valid_to_delete(inode)) {
		ocfs2_cleanup_delete_inode(inode, 0);
		goto bail;
	}

	ocfs2_block_signals(&oldset);

	status = ocfs2_nfs_sync_lock(OCFS2_SB(inode->i_sb), 0);
	if (status < 0) {
		mlog(ML_ERROR, "getting nfs sync lock(PR) failed %d\n", status);
		ocfs2_cleanup_delete_inode(inode, 0);
		goto bail_unblock;
	}
	status = ocfs2_inode_lock(inode, &di_bh, 1);
	if (status < 0) {
		if (status != -ENOENT)
			mlog_errno(status);
		ocfs2_cleanup_delete_inode(inode, 0);
		goto bail_unlock_nfs_sync;
	}

	status = ocfs2_query_inode_wipe(inode, di_bh, &wipe);
	if (!wipe || status < 0) {
		if (status < 0)
			mlog_errno(status);

		ocfs2_cleanup_delete_inode(inode, 1);
		goto bail_unlock_inode;
	}

	ocfs2_cleanup_delete_inode(inode, 0);

	status = ocfs2_wipe_inode(inode, di_bh);
	if (status < 0) {
		if (status != -EDEADLK)
			mlog_errno(status);
		goto bail_unlock_inode;
	}

	OCFS2_I(inode)->ip_flags |= OCFS2_INODE_DELETED;

bail_unlock_inode:
	ocfs2_inode_unlock(inode, 1);
	brelse(di_bh);

bail_unlock_nfs_sync:
	ocfs2_nfs_sync_unlock(OCFS2_SB(inode->i_sb), 0);

bail_unblock:
	ocfs2_unblock_signals(&oldset);
bail:
	return;
}

static void ocfs2_clear_inode(struct inode *inode)
{
	int status;
	struct ocfs2_inode_info *oi = OCFS2_I(inode);

	end_writeback(inode);
	trace_ocfs2_clear_inode((unsigned long long)oi->ip_blkno,
				inode->i_nlink);

	mlog_bug_on_msg(OCFS2_SB(inode->i_sb) == NULL,
			"Inode=%lu\n", inode->i_ino);

	dquot_drop(inode);

	ocfs2_open_unlock(inode);

	ocfs2_mark_lockres_freeing(&oi->ip_rw_lockres);
	ocfs2_mark_lockres_freeing(&oi->ip_inode_lockres);
	ocfs2_mark_lockres_freeing(&oi->ip_open_lockres);

	ocfs2_resv_discard(&OCFS2_SB(inode->i_sb)->osb_la_resmap,
			   &oi->ip_la_data_resv);
	ocfs2_resv_init_once(&oi->ip_la_data_resv);

	if (!(OCFS2_I(inode)->ip_flags & OCFS2_INODE_DELETED))
		ocfs2_checkpoint_inode(inode);

	mlog_bug_on_msg(!list_empty(&oi->ip_io_markers),
			"Clear inode of %llu, inode has io markers\n",
			(unsigned long long)oi->ip_blkno);

	ocfs2_extent_map_trunc(inode, 0);

	status = ocfs2_drop_inode_locks(inode);
	if (status < 0)
		mlog_errno(status);

	ocfs2_lock_res_free(&oi->ip_rw_lockres);
	ocfs2_lock_res_free(&oi->ip_inode_lockres);
	ocfs2_lock_res_free(&oi->ip_open_lockres);

	ocfs2_metadata_cache_exit(INODE_CACHE(inode));

	mlog_bug_on_msg(INODE_CACHE(inode)->ci_num_cached,
			"Clear inode of %llu, inode has %u cache items\n",
			(unsigned long long)oi->ip_blkno,
			INODE_CACHE(inode)->ci_num_cached);

	mlog_bug_on_msg(!(INODE_CACHE(inode)->ci_flags & OCFS2_CACHE_FL_INLINE),
			"Clear inode of %llu, inode has a bad flag\n",
			(unsigned long long)oi->ip_blkno);

	mlog_bug_on_msg(spin_is_locked(&oi->ip_lock),
			"Clear inode of %llu, inode is locked\n",
			(unsigned long long)oi->ip_blkno);

	mlog_bug_on_msg(!mutex_trylock(&oi->ip_io_mutex),
			"Clear inode of %llu, io_mutex is locked\n",
			(unsigned long long)oi->ip_blkno);
	mutex_unlock(&oi->ip_io_mutex);

	mlog_bug_on_msg(!down_write_trylock(&oi->ip_alloc_sem),
			"Clear inode of %llu, alloc_sem is locked\n",
			(unsigned long long)oi->ip_blkno);
	up_write(&oi->ip_alloc_sem);

	mlog_bug_on_msg(oi->ip_open_count,
			"Clear inode of %llu has open count %d\n",
			(unsigned long long)oi->ip_blkno, oi->ip_open_count);

	
	oi->ip_flags = 0;
	oi->ip_dir_start_lookup = 0;
	oi->ip_blkno = 0ULL;

	jbd2_journal_release_jbd_inode(OCFS2_SB(inode->i_sb)->journal->j_journal,
				       &oi->ip_jinode);
}

void ocfs2_evict_inode(struct inode *inode)
{
	if (!inode->i_nlink ||
	    (OCFS2_I(inode)->ip_flags & OCFS2_INODE_MAYBE_ORPHANED)) {
		ocfs2_delete_inode(inode);
	} else {
		truncate_inode_pages(&inode->i_data, 0);
	}
	ocfs2_clear_inode(inode);
}

int ocfs2_drop_inode(struct inode *inode)
{
	struct ocfs2_inode_info *oi = OCFS2_I(inode);
	int res;

	trace_ocfs2_drop_inode((unsigned long long)oi->ip_blkno,
				inode->i_nlink, oi->ip_flags);

	if (oi->ip_flags & OCFS2_INODE_MAYBE_ORPHANED)
		res = 1;
	else
		res = generic_drop_inode(inode);

	return res;
}

int ocfs2_inode_revalidate(struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	int status = 0;

	trace_ocfs2_inode_revalidate(inode,
		inode ? (unsigned long long)OCFS2_I(inode)->ip_blkno : 0ULL,
		inode ? (unsigned long long)OCFS2_I(inode)->ip_flags : 0);

	if (!inode) {
		status = -ENOENT;
		goto bail;
	}

	spin_lock(&OCFS2_I(inode)->ip_lock);
	if (OCFS2_I(inode)->ip_flags & OCFS2_INODE_DELETED) {
		spin_unlock(&OCFS2_I(inode)->ip_lock);
		status = -ENOENT;
		goto bail;
	}
	spin_unlock(&OCFS2_I(inode)->ip_lock);

	status = ocfs2_inode_lock(inode, NULL, 0);
	if (status < 0) {
		if (status != -ENOENT)
			mlog_errno(status);
		goto bail;
	}
	ocfs2_inode_unlock(inode, 0);
bail:
	return status;
}

int ocfs2_mark_inode_dirty(handle_t *handle,
			   struct inode *inode,
			   struct buffer_head *bh)
{
	int status;
	struct ocfs2_dinode *fe = (struct ocfs2_dinode *) bh->b_data;

	trace_ocfs2_mark_inode_dirty((unsigned long long)OCFS2_I(inode)->ip_blkno);

	status = ocfs2_journal_access_di(handle, INODE_CACHE(inode), bh,
					 OCFS2_JOURNAL_ACCESS_WRITE);
	if (status < 0) {
		mlog_errno(status);
		goto leave;
	}

	spin_lock(&OCFS2_I(inode)->ip_lock);
	fe->i_clusters = cpu_to_le32(OCFS2_I(inode)->ip_clusters);
	ocfs2_get_inode_flags(OCFS2_I(inode));
	fe->i_attr = cpu_to_le32(OCFS2_I(inode)->ip_attr);
	fe->i_dyn_features = cpu_to_le16(OCFS2_I(inode)->ip_dyn_features);
	spin_unlock(&OCFS2_I(inode)->ip_lock);

	fe->i_size = cpu_to_le64(i_size_read(inode));
	ocfs2_set_links_count(fe, inode->i_nlink);
	fe->i_uid = cpu_to_le32(inode->i_uid);
	fe->i_gid = cpu_to_le32(inode->i_gid);
	fe->i_mode = cpu_to_le16(inode->i_mode);
	fe->i_atime = cpu_to_le64(inode->i_atime.tv_sec);
	fe->i_atime_nsec = cpu_to_le32(inode->i_atime.tv_nsec);
	fe->i_ctime = cpu_to_le64(inode->i_ctime.tv_sec);
	fe->i_ctime_nsec = cpu_to_le32(inode->i_ctime.tv_nsec);
	fe->i_mtime = cpu_to_le64(inode->i_mtime.tv_sec);
	fe->i_mtime_nsec = cpu_to_le32(inode->i_mtime.tv_nsec);

	ocfs2_journal_dirty(handle, bh);
leave:
	return status;
}

void ocfs2_refresh_inode(struct inode *inode,
			 struct ocfs2_dinode *fe)
{
	spin_lock(&OCFS2_I(inode)->ip_lock);

	OCFS2_I(inode)->ip_clusters = le32_to_cpu(fe->i_clusters);
	OCFS2_I(inode)->ip_attr = le32_to_cpu(fe->i_attr);
	OCFS2_I(inode)->ip_dyn_features = le16_to_cpu(fe->i_dyn_features);
	ocfs2_set_inode_flags(inode);
	i_size_write(inode, le64_to_cpu(fe->i_size));
	set_nlink(inode, ocfs2_read_links_count(fe));
	inode->i_uid = le32_to_cpu(fe->i_uid);
	inode->i_gid = le32_to_cpu(fe->i_gid);
	inode->i_mode = le16_to_cpu(fe->i_mode);
	if (S_ISLNK(inode->i_mode) && le32_to_cpu(fe->i_clusters) == 0)
		inode->i_blocks = 0;
	else
		inode->i_blocks = ocfs2_inode_sector_count(inode);
	inode->i_atime.tv_sec = le64_to_cpu(fe->i_atime);
	inode->i_atime.tv_nsec = le32_to_cpu(fe->i_atime_nsec);
	inode->i_mtime.tv_sec = le64_to_cpu(fe->i_mtime);
	inode->i_mtime.tv_nsec = le32_to_cpu(fe->i_mtime_nsec);
	inode->i_ctime.tv_sec = le64_to_cpu(fe->i_ctime);
	inode->i_ctime.tv_nsec = le32_to_cpu(fe->i_ctime_nsec);

	spin_unlock(&OCFS2_I(inode)->ip_lock);
}

int ocfs2_validate_inode_block(struct super_block *sb,
			       struct buffer_head *bh)
{
	int rc;
	struct ocfs2_dinode *di = (struct ocfs2_dinode *)bh->b_data;

	trace_ocfs2_validate_inode_block((unsigned long long)bh->b_blocknr);

	BUG_ON(!buffer_uptodate(bh));

	rc = ocfs2_validate_meta_ecc(sb, bh->b_data, &di->i_check);
	if (rc) {
		mlog(ML_ERROR, "Checksum failed for dinode %llu\n",
		     (unsigned long long)bh->b_blocknr);
		goto bail;
	}


	rc = -EINVAL;

	if (!OCFS2_IS_VALID_DINODE(di)) {
		ocfs2_error(sb, "Invalid dinode #%llu: signature = %.*s\n",
			    (unsigned long long)bh->b_blocknr, 7,
			    di->i_signature);
		goto bail;
	}

	if (le64_to_cpu(di->i_blkno) != bh->b_blocknr) {
		ocfs2_error(sb, "Invalid dinode #%llu: i_blkno is %llu\n",
			    (unsigned long long)bh->b_blocknr,
			    (unsigned long long)le64_to_cpu(di->i_blkno));
		goto bail;
	}

	if (!(di->i_flags & cpu_to_le32(OCFS2_VALID_FL))) {
		ocfs2_error(sb,
			    "Invalid dinode #%llu: OCFS2_VALID_FL not set\n",
			    (unsigned long long)bh->b_blocknr);
		goto bail;
	}

	if (le32_to_cpu(di->i_fs_generation) !=
	    OCFS2_SB(sb)->fs_generation) {
		ocfs2_error(sb,
			    "Invalid dinode #%llu: fs_generation is %u\n",
			    (unsigned long long)bh->b_blocknr,
			    le32_to_cpu(di->i_fs_generation));
		goto bail;
	}

	rc = 0;

bail:
	return rc;
}

int ocfs2_read_inode_block_full(struct inode *inode, struct buffer_head **bh,
				int flags)
{
	int rc;
	struct buffer_head *tmp = *bh;

	rc = ocfs2_read_blocks(INODE_CACHE(inode), OCFS2_I(inode)->ip_blkno,
			       1, &tmp, flags, ocfs2_validate_inode_block);

	
	if (!rc && !*bh)
		*bh = tmp;

	return rc;
}

int ocfs2_read_inode_block(struct inode *inode, struct buffer_head **bh)
{
	return ocfs2_read_inode_block_full(inode, bh, 0);
}


static u64 ocfs2_inode_cache_owner(struct ocfs2_caching_info *ci)
{
	struct ocfs2_inode_info *oi = cache_info_to_inode(ci);

	return oi->ip_blkno;
}

static struct super_block *ocfs2_inode_cache_get_super(struct ocfs2_caching_info *ci)
{
	struct ocfs2_inode_info *oi = cache_info_to_inode(ci);

	return oi->vfs_inode.i_sb;
}

static void ocfs2_inode_cache_lock(struct ocfs2_caching_info *ci)
{
	struct ocfs2_inode_info *oi = cache_info_to_inode(ci);

	spin_lock(&oi->ip_lock);
}

static void ocfs2_inode_cache_unlock(struct ocfs2_caching_info *ci)
{
	struct ocfs2_inode_info *oi = cache_info_to_inode(ci);

	spin_unlock(&oi->ip_lock);
}

static void ocfs2_inode_cache_io_lock(struct ocfs2_caching_info *ci)
{
	struct ocfs2_inode_info *oi = cache_info_to_inode(ci);

	mutex_lock(&oi->ip_io_mutex);
}

static void ocfs2_inode_cache_io_unlock(struct ocfs2_caching_info *ci)
{
	struct ocfs2_inode_info *oi = cache_info_to_inode(ci);

	mutex_unlock(&oi->ip_io_mutex);
}

const struct ocfs2_caching_operations ocfs2_inode_caching_ops = {
	.co_owner		= ocfs2_inode_cache_owner,
	.co_get_super		= ocfs2_inode_cache_get_super,
	.co_cache_lock		= ocfs2_inode_cache_lock,
	.co_cache_unlock	= ocfs2_inode_cache_unlock,
	.co_io_lock		= ocfs2_inode_cache_io_lock,
	.co_io_unlock		= ocfs2_inode_cache_io_unlock,
};

