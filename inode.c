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
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/uaccess.h>
#include <linux/fs_stack.h>
#include <linux/slab.h>

#include "dazukofs_fs.h"

static struct inode_operations dazukofs_symlink_iops;
static struct inode_operations dazukofs_dir_iops;
static struct inode_operations dazukofs_main_iops;

static int dazukofs_inode_test(struct inode *inode,
			       void *candidate_lower_inode)
{
	if (get_lower_inode(inode) ==
	    (struct inode *)candidate_lower_inode) {
		return 1;
	}

	return 0;
}

static void dazukofs_init_inode(struct inode *inode, struct inode *lower_inode)
{
	set_lower_inode(inode, lower_inode);
	inode->i_ino = lower_inode->i_ino;
	inode->i_version++;
	inode->i_op = &dazukofs_main_iops;
	inode->i_fop = &dazukofs_main_fops;
	inode->i_mapping->a_ops = &dazukofs_aops;
}

static int dazukofs_inode_set(struct inode *inode, void *lower_inode)
{
	dazukofs_init_inode(inode, (struct inode *)lower_inode);
	return 0;
}

/**
 * dazukofs_interpose - fill in new dentry, linking it to the lower dentry
 * @lower_dentry: the corresponding lower dentry
 * @denty: the new DazukoFS dentry
 * @sb: super block of DazukoFS
 * @already_hashed: flag to signify if "dentry" is already hashed
 *
 * Description: This is the key function which sets up all the hooks to
 *              give DazukoFS control.
 *
 * Returns 0 on success.
 */
int dazukofs_interpose(struct dentry *lower_dentry, struct dentry *dentry,
		       struct super_block *sb, int already_hashed)
{
	struct inode *inode;
	struct inode *lower_inode = igrab(lower_dentry->d_inode);

	if (!lower_inode)
		return -ESTALE;

	if (lower_inode->i_sb != get_lower_sb(sb)) {
		iput(lower_inode);
		return -EXDEV;
	}

	inode = iget5_locked(sb, (unsigned long)lower_inode,
			     dazukofs_inode_test, dazukofs_inode_set,
			     lower_inode);

	if (!inode) {
		iput(lower_inode);
		return -EACCES;
	}

	if (inode->i_state & I_NEW) {
		unlock_new_inode(inode);
		/*
		 * This is a new node so we leave the lower_node "in use"
		 * and do not call iput().
		 */
	} else {
		/*
		 * This is not a new node so we decrement the usage count.
		 */
		iput(lower_inode);
	}

	if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &dazukofs_symlink_iops;
	else if (S_ISDIR(lower_inode->i_mode))
		inode->i_op = &dazukofs_dir_iops;

	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &dazukofs_dir_fops;

	if (special_file(lower_inode->i_mode)) {
		init_special_inode(inode, lower_inode->i_mode,
				   lower_inode->i_rdev);
	}

	dentry->d_op = &dazukofs_dops;

	if (already_hashed)
		d_add(dentry, inode);
	else
		d_instantiate(dentry, inode);

	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);
	return 0;
}

/**
 * Description: Called when the VFS needs to look up an inode in a parent
 * directory. The name to look for is found in the dentry. This method
 * must call d_add() to insert the found inode into the dentry. The
 * "i_count" field in the inode structure should be incremented. If the
 * named inode does not exist a NULL inode should be inserted into the
 * dentry (this is called a negative dentry). Returning an error code
 * from this routine must only be done on a real error, otherwise
 * creating inodes with system calls like create(2), mknod(2), mkdir(2)
 * and so on will fail. If you wish to overload the dentry methods then
 * you should initialise the "d_dop" field in the dentry; this is a
 * pointer to a struct "dentry_operations". This method is called with
 * the directory inode semaphore held.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static struct dentry* dazukofs_lookup(struct inode *dir, struct dentry *dentry,
 unsigned int flags)
#else
static struct dentry *dazukofs_lookup(struct inode *dir, struct dentry *dentry,
				      struct nameidata *nd)
#endif
{
	struct dentry *lower_dentry;
	struct dentry *lower_dentry_parent;
	struct vfsmount *lower_mnt;
	int err = 0;

	/* check for "." or ".." (they are not relevant here) */
	if (dentry->d_name.len == 1) {
		if (dentry->d_name.name[0] == '.') {
			d_drop(dentry);
			goto out;
		}
	} else if (dentry->d_name.len == 2) {
		if (dentry->d_name.name[0] == '.' &&
		    dentry->d_name.name[1] == '.') {
			d_drop(dentry);
			goto out;
		}
	}

	dentry->d_op = &dazukofs_dops;

	lower_dentry_parent = get_lower_dentry(dentry->d_parent);
	mutex_lock(&lower_dentry_parent->d_inode->i_mutex);
	lower_dentry = lookup_one_len(dentry->d_name.name,
				      lower_dentry_parent,
				      dentry->d_name.len);
	mutex_unlock(&lower_dentry_parent->d_inode->i_mutex);
	if (IS_ERR(lower_dentry)) {
		err = PTR_ERR(lower_dentry);
		d_drop(dentry);
		goto out;
	}

	BUG_ON(!lower_dentry->d_count);

	set_dentry_private(dentry,
			   kmem_cache_zalloc(dazukofs_dentry_info_cachep,
					     GFP_KERNEL));
	if (!get_dentry_private(dentry)) {
		err = -ENOMEM;
		goto out_dput;
	}

	fsstack_copy_attr_atime(dir, lower_dentry_parent->d_inode);

	lower_mnt = mntget(get_lower_mnt(dentry->d_parent));
	set_lower_dentry(dentry, lower_dentry, lower_mnt);

	if (!lower_dentry->d_inode) {
		/*
		 * We want to add because we could not find in lower.
		 */
		d_add(dentry, NULL);
		goto out;
	}

	err = dazukofs_interpose(lower_dentry, dentry, dir->i_sb, 1);
	if (err)
		goto out_dput;
	goto out;

out_dput:
	dput(lower_dentry);
	d_drop(dentry);
out:
	return ERR_PTR(err);
}

/**
 * Description: Called by the mknod(2) system call to create a device
 * (char, block) inode or a named pipe (FIFO) or socket. Only required if
 * you want to support creating these types of inodes. You will probably
 * need to call d_instantiate() just as you would in the create() method.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static int dazukofs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
						  dev_t dev)
#else
static int dazukofs_mknod(struct inode *dir, struct dentry *dentry, int mode,
			  dev_t dev)
#endif
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct dentry *lower_dentry_parent = dget(lower_dentry->d_parent);
	struct inode *lower_dentry_parent_inode = lower_dentry_parent->d_inode;
	int err;

	mutex_lock_nested(&(lower_dentry_parent_inode->i_mutex),
			  I_MUTEX_PARENT);

	err = vfs_mknod(lower_dentry_parent_inode, lower_dentry, mode, dev);
	if (err)
		goto out;

	err = dazukofs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, lower_dentry_parent_inode);
	fsstack_copy_inode_size(dir, lower_dentry_parent_inode);
out:
	mutex_unlock(&(lower_dentry_parent_inode->i_mutex));
	dput(lower_dentry_parent);
	return err;
}

/**
 * Description: Called by the mkdir(2) system call. Only required if you
 * want to support creating subdirectories. You will probably need to call
 * d_instantiate() just as you would in the create() method.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
static int dazukofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
#else
static int dazukofs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
#endif
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct dentry *lower_dentry_parent = dget(lower_dentry->d_parent);
	struct inode *lower_dentry_parent_inode = lower_dentry_parent->d_inode;
	int err;

	mutex_lock_nested(&(lower_dentry_parent_inode->i_mutex),
			  I_MUTEX_PARENT);

	err = vfs_mkdir(lower_dentry_parent_inode, lower_dentry, mode);
	if (err)
		goto out;

	err = dazukofs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, lower_dentry_parent_inode);
	fsstack_copy_inode_size(dir, lower_dentry_parent_inode);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
	set_nlink(dir, lower_dentry_parent_inode->i_nlink);
#else
	dir->nlink = lower_dentry_parent_inode->i_nlink;
#endif
out:
	mutex_unlock(&(lower_dentry_parent_inode->i_mutex));
	dput(lower_dentry_parent);
	return err;
}

/**
 * Description: Called by the open(2) and creat(2) system calls. Only
 * required if you want to support regular files. The dentry you get
 * should not have an inode (i.e. it should be a negative dentry). Here
 * you will probably call d_instantiate() with the dentry and the newly
 * created inode.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
static int dazukofs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
 bool excl)
#else
static int dazukofs_create(struct inode *dir, struct dentry *dentry, int mode,
			   struct nameidata *nd)
#endif
{
	struct vfsmount *lower_mnt = get_lower_mnt(dentry);
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct dentry *lower_dentry_parent = dget(lower_dentry->d_parent);
	struct inode *lower_dentry_parent_inode = lower_dentry_parent->d_inode;
	int err;

	mutex_lock_nested(&(lower_dentry_parent_inode->i_mutex),
			  I_MUTEX_PARENT);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 6, 0)
	struct vfsmount *vfsmount_save;
	struct dentry *dentry_save;
	
	vfsmount_save = nd->path.mnt;
	dentry_save = nd->path.dentry;

	nd->path.mnt = mntget(lower_mnt);
	nd->path.dentry = dget(lower_dentry);
	
	err = vfs_create(lower_dentry_parent_inode, lower_dentry, mode, nd);
#else
	err = vfs_create(lower_dentry_parent_inode, lower_dentry, mode, excl);
#endif

	mntput(lower_mnt);
	dput(lower_dentry);
	
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 6, 0)
	nd->path.mnt = vfsmount_save;
	nd->path.dentry = dentry_save;
#endif

	if (err)
		goto out;

	err = dazukofs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, lower_dentry_parent_inode);
	fsstack_copy_inode_size(dir, lower_dentry_parent_inode);
out:
	mutex_unlock(&(lower_dentry_parent_inode->i_mutex));
	dput(lower_dentry_parent);
	return err;
}

/**
 * Description: Called by the symlink(2) system call. Only required if you
 * want to support symlinks. You will probably need to call d_instantiate()
 * just as you would in the create() method.
 */
static int dazukofs_symlink(struct inode *dir, struct dentry *dentry,
			    const char *symname)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct dentry *lower_dentry_parent = dget(lower_dentry->d_parent);
	struct inode *lower_dentry_parent_inode = lower_dentry_parent->d_inode;
	int err;

	mutex_lock_nested(&(lower_dentry_parent_inode->i_mutex),
			  I_MUTEX_PARENT);

	err = vfs_symlink(lower_dentry_parent_inode, lower_dentry, symname);
	if (err)
		goto out;

	err = dazukofs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, lower_dentry_parent_inode);
	fsstack_copy_inode_size(dir, lower_dentry_parent_inode);
out:
	mutex_unlock(&(lower_dentry_parent_inode->i_mutex));
	dput(lower_dentry_parent);
	return err;
}

/**
 * Description: Called by the readlink(2) system call. Only required if
 * you want to support reading symbolic links.
 */
static int dazukofs_readlink(struct dentry *dentry, char __user *buf,
			     int bufsiz)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct inode *lower_dentry_inode = lower_dentry->d_inode;
	int err;

	if (!lower_dentry_inode) {
		err = -ENOENT;
		d_drop(dentry);
		goto out;
	}

	if (!lower_dentry_inode->i_op->readlink) {
		err = -EINVAL;
		goto out;
	}

	err = lower_dentry_inode->i_op->readlink(lower_dentry, buf, bufsiz);
	if (err)
		goto out;

	fsstack_copy_attr_times(dentry->d_inode, lower_dentry_inode);
out:
	return err;
}

/**
 * Description: Called by the VFS to follow a symbolic link to the inode
 * it points to. Only required if you want to support symbolic links. This
 * method returns a void pointer cookie that is passed to put_link().
 */
static void *dazukofs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	mm_segment_t fs_save;
	int rc;
	char *buf;
	int len = PAGE_SIZE;
	int err = 0;

	/*
	 * Released in dazukofs_put_link(). Only release here on error.
	 */
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto out;
	}

	fs_save = get_fs();
	set_fs(get_ds());
	rc = dazukofs_readlink(dentry, (char __user *)buf, len);
	set_fs(fs_save);

	if (rc < 0) {
		err = rc;
		goto out_free;
	}
	buf[rc] = 0;

	nd_set_link(nd, buf);
	goto out;

out_free:
	kfree(buf);
out:
	return ERR_PTR(err);
}

/**
 * Description: Called by the VFS to release resources allocated by
 * follow_link(). The cookie returned by follow_link() is passed to this
 * method as the last parameter. It is used by filesystems such as NFS
 * where page cache is not stable (i.e. page that was installed when the
 * symbolic link walk started might not be in the page cache at the end
 * of the walk).
 */
static void dazukofs_put_link(struct dentry *dentry, struct nameidata *nd,
			      void *ptr)
{
	/*
	 * Release the char* from dazukofs_follow_link().
	 */
	kfree(nd_get_link(nd));
}

/**
 * Description: Called by the VFS to check for access rights on a
 * POSIX-like filesystem.
 */
static int dazukofs_permission(struct inode *inode, int mask)
{
	return inode_permission(get_lower_inode(inode), mask);
}

/**
 * Description: Called by the VFS to get attributes for a file. 
 */
static int dazukofs_getattr(struct vfsmount *mnt, struct dentry *dentry, 
			    struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct inode *lower_inode = get_lower_inode(inode);
	struct dentry *lower_dentry;
	struct vfsmount *lower_mnt;
	int err;

	if (!lower_inode->i_op->getattr) {
		generic_fillattr(inode, stat);
		return 0;
	}

	lower_mnt = mntget(get_lower_mnt(dentry));
	lower_dentry = get_lower_dentry(dentry);
	err = lower_inode->i_op->getattr(lower_mnt, lower_dentry, stat);
	mntput(lower_mnt);

	return err;
}

/**
 * Description: Called by the VFS to set attributes for a file. This method
 * is called by chmod(2) and related system calls.
 */
static int dazukofs_setattr(struct dentry *dentry, struct iattr *ia)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct inode *inode = dentry->d_inode;
	struct inode *lower_inode = get_lower_inode(inode);
	int err;

	/*
	 * mode change is for clearing setuid/setgid bits. Allow lower fs
	 * to interpret this in its own way.
	 */
	if (ia->ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		ia->ia_valid &= ~ATTR_MODE;

	mutex_lock(&lower_inode->i_mutex);
	err = notify_change(lower_dentry, ia);
	mutex_unlock(&lower_inode->i_mutex);

	fsstack_copy_attr_all(inode, lower_inode);
	fsstack_copy_inode_size(inode, lower_inode);
	return err;
}

/**
 * Description: Called by the VFS to set an extended attribute for a file.
 * Extended attribute is a name:value pair associated with an inode. This
 * method is called by setxattr(2) system call.
 */
static int dazukofs_setxattr(struct dentry *dentry, const char *name,
			     const void *value, size_t size, int flags)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct inode *lower_dentry_inode = lower_dentry->d_inode;
	int err;

	if (!lower_dentry_inode) {
		err = -ENOENT;
		d_drop(dentry);
		goto out;
	}

	if (!lower_dentry_inode->i_op->setxattr) {
		err = -EOPNOTSUPP;
		goto out;
	}

	mutex_lock(&lower_dentry_inode->i_mutex);
	err = lower_dentry_inode->i_op->setxattr(lower_dentry, name, value,
						 size, flags);
	mutex_unlock(&lower_dentry_inode->i_mutex);

	fsstack_copy_attr_all(dentry->d_inode, lower_dentry_inode);
	fsstack_copy_inode_size(dentry->d_inode, lower_dentry_inode);
out:
	return err;
}

/**
 * Description: Called by the VFS to retrieve the value of an extended
 * attribute name. This method is called by getxattr(2) function call.
 */
static ssize_t dazukofs_getxattr(struct dentry *dentry, const char *name,
				 void *value, size_t size)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct inode *lower_dentry_inode = lower_dentry->d_inode;
	ssize_t err;

	if (!lower_dentry_inode) {
		err = -ENOENT;
		d_drop(dentry);
		goto out;
	}

	if (!lower_dentry_inode->i_op->getxattr) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = lower_dentry_inode->i_op->getxattr(lower_dentry, name,
						 value, size);
out:
	return err;
}

/**
 * Description: Called by the VFS to list all extended attributes for a
 * given file. This method is called by listxattr(2) system call.
 */
static ssize_t dazukofs_listxattr(struct dentry *dentry, char *list,
				  size_t size)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct inode *lower_dentry_inode = lower_dentry->d_inode;
	int err;

	if (!lower_dentry_inode) {
		err = -ENOENT;
		d_drop(dentry);
		goto out;
	}

	if (!lower_dentry_inode->i_op->listxattr) {
		err = -EOPNOTSUPP;
		goto out;
	}

	err = lower_dentry_inode->i_op->listxattr(lower_dentry, list, size);
out:
	return err;
}

/**
 * Description: Called by the VFS to remove an extended attribute from a
 * file. This method is called by removexattr(2) system call.
 */
static int dazukofs_removexattr(struct dentry *dentry, const char *name)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct inode *lower_dentry_inode = lower_dentry->d_inode;
	int err;

	if (!lower_dentry_inode) {
		err = -ENOENT;
		d_drop(dentry);
		goto out;
	}

	if (!lower_dentry_inode->i_op->removexattr) {
		err = -EOPNOTSUPP;
		goto out;
	}

	mutex_lock(&lower_dentry_inode->i_mutex);
	err = lower_dentry_inode->i_op->removexattr(lower_dentry, name);
	mutex_unlock(&lower_dentry_inode->i_mutex);
out:
	return err;
}

/**
 * Description: Called by the link(2) system call. Only required if you want
 * to support hard links. You will probably need to call d_instantiate()
 * just as you would in the create() method.
 */
static int dazukofs_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *new_dentry)
{
	struct dentry *lower_old_dentry = get_lower_dentry(old_dentry);
	struct dentry *lower_new_dentry = get_lower_dentry(new_dentry);
	struct dentry *lower_dentry_parent = dget(lower_new_dentry->d_parent);
	struct inode *lower_dentry_parent_inode = lower_dentry_parent->d_inode;
	int err;

	mutex_lock_nested(&(lower_dentry_parent_inode->i_mutex),
			  I_MUTEX_PARENT);

	err = vfs_link(lower_old_dentry, lower_dentry_parent_inode,
		       lower_new_dentry);
	if (err)
		goto out;

	err = dazukofs_interpose(lower_new_dentry, new_dentry, dir->i_sb, 0);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, lower_dentry_parent_inode);
	fsstack_copy_inode_size(dir, lower_dentry_parent_inode);
out:
	mutex_unlock(&(lower_dentry_parent_inode->i_mutex));
	dput(lower_dentry_parent);
	return err;
}

/**
 * Description: Called by the unlink(2) system call. Only required if you
 * want to support deleting inodes.
 */
static int dazukofs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct dentry *lower_dentry_parent = dget(lower_dentry->d_parent);
	struct inode *lower_dentry_parent_inode = lower_dentry_parent->d_inode;
	int err;

	mutex_lock_nested(&(lower_dentry_parent_inode->i_mutex),
			  I_MUTEX_PARENT);

	err = vfs_unlink(lower_dentry_parent_inode, lower_dentry);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, lower_dentry_parent_inode);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
	set_nlink(dentry->d_inode, get_lower_inode(dentry->d_inode)->i_nlink);
#else
	dentry->d_inode->n_link = get_lower_inode(dentry->d_inode)->i_nlink;
#endif
	fsstack_copy_attr_times(dentry->d_inode, dir);
out:
	mutex_unlock(&(lower_dentry_parent_inode->i_mutex));
	dput(lower_dentry_parent);
	return err;
}

/**
 * Description: Called by the rmdir(2) system call. Only required if you
 * want to support deleting subdirectories.
 */
static int dazukofs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	struct dentry *lower_dentry_parent = dget(lower_dentry->d_parent);
	struct inode *lower_dentry_parent_inode = lower_dentry_parent->d_inode;
	int err;

	mutex_lock_nested(&(lower_dentry_parent_inode->i_mutex),
			  I_MUTEX_PARENT);

	err = vfs_rmdir(lower_dentry_parent_inode, lower_dentry);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, lower_dentry_parent_inode);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
	set_nlink(dir, lower_dentry_parent_inode->i_nlink);
	set_nlink(dentry->d_inode, get_lower_inode(dentry->d_inode)->i_nlink);
#else
	dir->i_nlink = lower_dentry_parent_inode->i_nlink;
	dentry->d_inode->i_nlink = get_lower_inode(dentry->d_inode)->i_nlink;
#endif
out:
	mutex_unlock(&(lower_dentry_parent_inode->i_mutex));
	dput(lower_dentry_parent);

	if (!err)
		d_drop(dentry);
	return err;
}

/**
 * Description: Called by the rename(2) system call to rename the object to
 * have the parent and name given by the second inode and dentry.
 */
static int dazukofs_rename(struct inode *old_dir, struct dentry *old_dentry,
			   struct inode *new_dir, struct dentry *new_dentry)
{
	struct dentry *lower_old_dentry = get_lower_dentry(old_dentry);
	struct dentry *lower_new_dentry = get_lower_dentry(new_dentry);
	struct dentry *lower_old_dentry_parent =
		dget(lower_old_dentry->d_parent);
	struct dentry *lower_new_dentry_parent =
		dget(lower_new_dentry->d_parent);
	struct inode *lower_old_dentry_parent_inode =
		lower_old_dentry_parent->d_inode;
	struct inode *lower_new_dentry_parent_inode =
		lower_new_dentry_parent->d_inode;
	int err = -ENOENT;

	if (!lower_old_dentry_parent_inode) {
		d_drop(old_dentry);
		goto out;
	}

	if (!lower_new_dentry_parent_inode) {
		d_drop(new_dentry);
		goto out;
	}

	lock_rename(lower_old_dentry_parent, lower_new_dentry_parent);
	err = vfs_rename(lower_old_dentry_parent_inode, lower_old_dentry,
			 lower_new_dentry_parent_inode, lower_new_dentry);
	unlock_rename(lower_old_dentry_parent, lower_new_dentry_parent);

	if (err)
		goto out;

	fsstack_copy_attr_all(new_dir, lower_new_dentry_parent_inode);
	if (new_dir != old_dir)
		fsstack_copy_attr_all(old_dir, lower_old_dentry_parent_inode);
out:
	dput(lower_old_dentry_parent);
	dput(lower_new_dentry_parent);
	return err;
}

/**
 * Unused operations:
 *   - create
 *   - lookup
 *   - link
 *   - unlink
 *   - symlink
 *   - mkdir
 *   - rmdir
 *   - mknod
 *   - rename
 *   - truncate
 *   - truncate_range
 *   - fallocate
 */
static struct inode_operations dazukofs_symlink_iops = {
	.readlink	= dazukofs_readlink,
	.follow_link	= dazukofs_follow_link,
	.put_link	= dazukofs_put_link,
	.permission	= dazukofs_permission,
	.getattr	= dazukofs_getattr,
	.setattr	= dazukofs_setattr,
	.setxattr	= dazukofs_setxattr,
	.getxattr	= dazukofs_getxattr,
	.listxattr	= dazukofs_listxattr,
	.removexattr	= dazukofs_removexattr,
};

/**
 * Unused operations:
 *   - readlink
 *   - follow_link
 *   - put_link
 *   - truncate
 *   - truncate_range
 *   - fallocate
 */
static struct inode_operations dazukofs_dir_iops = {
	.create		= dazukofs_create,
	.lookup		= dazukofs_lookup,
	.link		= dazukofs_link,
	.unlink		= dazukofs_unlink,
	.symlink	= dazukofs_symlink,
	.mkdir		= dazukofs_mkdir,
	.rmdir		= dazukofs_rmdir,
	.mknod		= dazukofs_mknod,
	.rename		= dazukofs_rename,
	.permission	= dazukofs_permission,
	.getattr	= dazukofs_getattr,
	.setattr	= dazukofs_setattr,
	.setxattr	= dazukofs_setxattr,
	.getxattr	= dazukofs_getxattr,
	.listxattr	= dazukofs_listxattr,
	.removexattr	= dazukofs_removexattr,
};

/**
 * Unused operations:
 *   - create
 *   - lookup
 *   - link
 *   - unlink
 *   - symlink
 *   - mkdir
 *   - rmdir
 *   - mknod
 *   - rename
 *   - readlink
 *   - follow_link
 *   - put_link
 *   - truncate
 *   - truncate_range
 *   - fallocate
 */
static struct inode_operations dazukofs_main_iops = {
	.permission	= dazukofs_permission,
	.getattr	= dazukofs_getattr,
	.setattr	= dazukofs_setattr,
	.setxattr	= dazukofs_setxattr,
	.getxattr	= dazukofs_getxattr,
	.listxattr	= dazukofs_listxattr,
	.removexattr	= dazukofs_removexattr,
};
