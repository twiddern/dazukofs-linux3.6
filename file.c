/* dazukofs: access control stackable filesystem

   Copyright (C) 1997-2004 Erez Zadok
   Copyright (C) 2001-2004 Stony Brook University
   Copyright (C) 2004-2007 International Business Machines Corp.
   Copyright (C) 2008-2010 John Ogness
     Author: John Ogness <dazukocode@ogness.net>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <linux/fs_stack.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/pagemap.h>

#include "dazukofs_fs.h"
#include "event.h"

/**
 * Description: Called when the VFS needs to move the file position index.
 */
static loff_t dazukofs_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t retval;
	struct file *lower_file = get_lower_file(file);

	lower_file->f_pos = file->f_pos;

	memcpy(&(lower_file->f_ra), &(file->f_ra),
	       sizeof(struct file_ra_state));

	if (lower_file->f_op && lower_file->f_op->llseek)
		retval = lower_file->f_op->llseek(lower_file, offset, origin);
	else
		retval = generic_file_llseek(lower_file, offset, origin);

	if (retval >= 0) {
		file->f_pos = lower_file->f_pos;
		file->f_version = lower_file->f_version;
	}
	return retval;
}

/**
 * Description: Called by read(2) and related system calls.
 */
static ssize_t dazukofs_read(struct file *file, char *buf, size_t count,
			     loff_t *ppos)
{
	int err;
	struct file *lower_file = get_lower_file(file);
	loff_t pos_copy = *ppos;

	if (!lower_file->f_op || !lower_file->f_op->read)
		return -EINVAL;

	err = vfs_read(lower_file, buf, count, &pos_copy);

	lower_file->f_pos = pos_copy;
	*ppos = pos_copy;

	if (err >= 0) {
		fsstack_copy_attr_atime(file->f_dentry->d_inode,
					lower_file->f_dentry->d_inode);
	}

	memcpy(&(file->f_ra), &(lower_file->f_ra),
	       sizeof(struct file_ra_state));
	return err;
}

/** 
 * Description:
 * Get all existing pages of the given interval of the given 
 * file and clear its "Uptodate" flag.
 */ 
static void mark_pages_outdated(struct file *file, size_t count, loff_t pos)
{
	struct page *page;
	pgoff_t start;
	pgoff_t end;
	pgoff_t i;

	if (unlikely(!count))
		return;

	start = pos >> PAGE_SHIFT;
	end = (pos + count) >> PAGE_SHIFT;

	for (i = start; i <= end; i++) {
		page = find_lock_page(file->f_mapping, i);
		if (!page) {
			/* not yet accessed, get next one */
			continue;
		}
		ClearPageUptodate(page);
		unlock_page(page);
	}
}

/**
 * Description: Called by write(2) and related system calls.
 */
static ssize_t dazukofs_write(struct file *file, const char *buf,
			      size_t count, loff_t *ppos)
{
	struct file *lower_file = get_lower_file(file);
	struct inode *inode = file->f_dentry->d_inode;
	struct inode *lower_inode = get_lower_inode(inode);
	loff_t pos_copy = *ppos;
	ssize_t ret;

	if (!lower_file->f_op || !lower_file->f_op->write)
		return -EINVAL;

	mutex_lock(&inode->i_mutex);
	ret = vfs_write(lower_file, buf, count, &pos_copy);

	lower_file->f_pos = pos_copy;

	if (ret >= 0) {
		/*
		 * Mark all upper pages concerned by the write to
		 * lower file as not Uptodate.
		 *
		 * NOTE: We don't use the offset given with ppos, but
		 * calculate it from the _returned_ value, since in some
		 * cases (O_APPEND) the file pointer will have been modified
		 * subsequently.
		 */
		mark_pages_outdated(file, ret, pos_copy - ret);
		fsstack_copy_attr_atime(inode, lower_inode);
	}

	*ppos = pos_copy;
	memcpy(&(file->f_ra), &(lower_file->f_ra),
	       sizeof(struct file_ra_state));

	i_size_write(inode, i_size_read(lower_inode));
	mutex_unlock(&inode->i_mutex);

	return ret;
}

/**
 * Description: Called when the VFS needs to read the directory contents.
 */
static int dazukofs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	int err;
	struct file *lower_file = get_lower_file(file);
	struct inode *inode = file->f_dentry->d_inode;

	lower_file->f_pos = file->f_pos;

	err = vfs_readdir(lower_file, filldir, dirent);

	file->f_pos = lower_file->f_pos;

	if (err >= 0)
		fsstack_copy_attr_atime(inode, lower_file->f_dentry->d_inode);

	return err;
}

/**
 * Description: Called by the ioctl(2) system call.
 */
static long dazukofs_unlocked_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	struct file *lower_file = get_lower_file(file);

	if (!lower_file->f_op || !lower_file->f_op->unlocked_ioctl)
		return -ENOTTY;

	return lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);
}

#ifdef CONFIG_COMPAT
static long dazukofs_compat_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct file *lower_file = get_lower_file(file);

	if (!lower_file->f_op || !lower_file->f_op->compat_ioctl)
		return -ENOIOCTLCMD;

	return lower_file->f_op->compat_ioctl(lower_file, cmd, arg);
}
#endif

/**
 * Description: Called by the VFS when an inode should be opened. When the
 * VFS opens a file, it creates a new "struct file". It then calls the open
 * method for the newly allocated file structure. You might think that the
 * open method really belongs in "struct inode_operations", and you may be
 * right. I think it's done the way it is because it makes filesystems
 * simpler to implement. The open() method is a good place to initialize
 * the "private_data" member in the file structure if you want to point to
 * a device structure.
 */
static int dazukofs_open(struct inode *inode, struct file *file)
{
	struct dentry *dentry = file->f_dentry;
	struct dentry *lower_dentry = dget(get_lower_dentry(dentry));
	struct vfsmount *lower_mnt = mntget(get_lower_mnt(dentry));
	struct file *lower_file;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
	struct path path;
#endif
	int err;

	err = dazukofs_check_access(file->f_dentry, file->f_vfsmnt);
	if (err)
		goto error_out1;

	set_file_private(file, kmem_cache_zalloc(dazukofs_file_info_cachep,
						 GFP_KERNEL));
	if (!get_file_private(file)) {
		err = -ENOMEM;
		goto error_out1;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
	path.dentry = lower_dentry;
	path.mnt = lower_mnt;
	
	lower_file = dentry_open(&path, file->f_flags, current_cred());
#else
	lower_file = dentry_open (lower_dentry, lower_mnt, file->f_flags,
				 current_cred());
#endif
	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		/* dentry_open() already did dput() and mntput() */
		goto error_out2;
	}

	set_lower_file(file, lower_file);

	return err;

error_out1:
	dput(lower_dentry);
	mntput(lower_mnt);
error_out2:
	return err;
}

/**
 * Description: Called by the close(2) system call to flush a file.
 */
static int dazukofs_flush(struct file *file, fl_owner_t td)
{
	struct file *lower_file = get_lower_file(file);

	if (!lower_file->f_op || !lower_file->f_op->flush)
		return 0;

	return lower_file->f_op->flush(lower_file, td);
}

/**
 * Description: Called when the last reference to an open file is closed.
 */
static int dazukofs_release(struct inode *inode, struct file *file)
{
	struct inode *lower_inode = get_lower_inode(inode);

	fput(get_lower_file(file));
	inode->i_blocks = lower_inode->i_blocks;

	kmem_cache_free(dazukofs_file_info_cachep, get_file_private(file));
	return 0;
}

/**
 * Description: Called by the fsync(2) system call.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static int dazukofs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct file *lower_file = get_lower_file(file);

	if (!lower_file->f_op || !lower_file->f_op->fsync)
		return -EINVAL;

	return vfs_fsync_range(lower_file, start, end, datasync);
}
#else
static int dazukofs_fsync(struct file *file, int datasync)
{
	struct file *lower_file = get_lower_file(file);

	if (!lower_file->f_op || !lower_file->f_op->fsync)
		return -EINVAL;

	return vfs_fsync(lower_file, datasync);
}
#endif

/**
 * Description: .called by the fcntl(2) system call when asynchronous
 * (non-blocking) mode is enabled for a file.
 */
static int dazukofs_fasync(int fd, struct file *file, int flag)
{
	struct file *lower_file = get_lower_file(file);

	if (!lower_file->f_op || !lower_file->f_op->fasync)
		return 0;

	return lower_file->f_op->fasync(fd, lower_file, flag);
}

static int dazukofs_mmap(struct file *file, struct vm_area_struct *vm)
{
	struct file *lower_file = get_lower_file(file);

	/* If lower fs does not support mmap, we dont call generic_mmap(), since
	 * this would result in calling lower readpage(), which might not be defined
	 * by lower fs, since mmap is not supported. */
	if (!lower_file->f_op || !lower_file->f_op->mmap)
		return -ENODEV;

	return generic_file_mmap(file, vm);
}

/**
 * Unused operations:
 *   - owner
 *   - aio_read
 *   - aio_write
 *   - poll
 *   - aio_fsync
 *   - lock
 *   - sendpage
 *   - get_unmapped_area
 *   - check_flags
 *   - dir_notify
 *   - flock
 *   - splice_write
 *   - splice_read
 *   - setlease
 */
const struct file_operations dazukofs_main_fops = {
	.llseek		= dazukofs_llseek,
	.read		= dazukofs_read,
	.write		= dazukofs_write,
	.readdir	= dazukofs_readdir,
	.unlocked_ioctl	= dazukofs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= dazukofs_compat_ioctl,
#endif
	.mmap		= dazukofs_mmap,
	.open		= dazukofs_open,
	.flush		= dazukofs_flush,
	.release	= dazukofs_release,
	.fsync		= dazukofs_fsync,
	.fasync		= dazukofs_fasync,
};

/**
 * Unused operations:
 *   - owner
 *   - llseek
 *   - read
 *   - write
 *   - aio_read
 *   - aio_write
 *   - poll
 *   - aio_fsync
 *   - lock
 *   - sendpage
 *   - get_unmapped_area
 *   - check_flags
 *   - dir_notify
 *   - flock
 *   - splice_write
 *   - splice_read
 *   - setlease
 */
const struct file_operations dazukofs_dir_fops = {
	.read		= dazukofs_read,
	.readdir	= dazukofs_readdir,
	.unlocked_ioctl	= dazukofs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= dazukofs_compat_ioctl,
#endif
	.mmap		= dazukofs_mmap,
	.open		= dazukofs_open,
	.flush		= dazukofs_flush,
	.release	= dazukofs_release,
	.fsync		= dazukofs_fsync,
	.fasync		= dazukofs_fasync,
};
