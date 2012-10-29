/* dazukofs: access control stackable filesystem

   Copyright (C) 1997-2003 Erez Zadok
   Copyright (C) 2001-2003 Stony Brook University
   Copyright (C) 2004-2006 International Business Machines Corp.
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
#include <linux/fs_stack.h>

#include "dazukofs_fs.h"

/**
 * dazukofs_d_revalidate - revalidate a dentry found in the dcache
 * @dentry: dentry to revalidate
 * @nd: nameidata associated with dentry
 *
 * Description: Called when the VFS needs to revalidate a dentry. This is
 * called whenever a name look-up finds a dentry in the dcache. Most
 * filesystems leave this as NULL, because all their dentries in the dcache
 * are valid.
 *
 * Call d_revalidate() on the lower dentry if available. The mnt/dentry
 * (path) data in the nameidata needs to be temporarily swapped out for the
 * lower call.
 *
 * After the call, the original path data is restored and the dentry's inode
 * attributes are updated to match the lower inode.
 *
 * Returns 1 if dentry is valid, otherwise 0.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
static int dazukofs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct vfsmount *lower_mnt_parent;
	struct dentry *lower_dentry_parent, *lower_dentry;
	int valid, err;
	struct nameidata new_nd;
		
	lower_dentry = get_lower_dentry(dentry);
	if (!lower_dentry->d_op || !lower_dentry->d_op->d_revalidate)
		return 1;
	
	lower_mnt_parent = get_lower_mnt(dentry->d_parent);
	lower_dentry_parent = get_lower_dentry(dentry->d_parent);
	err = vfs_path_lookup(lower_dentry_parent,
							  lower_mnt_parent,
							  dentry->d_name.name, 0, &(new_nd.path));
	if (err)
		return -EINVAL;
	
	valid = lower_dentry->d_op->d_revalidate(lower_dentry, 0);
	path_put(&new_nd.path);
	
	/* update the inode, even if d_revalidate() != 1 */
	if (dentry->d_inode) {
		struct inode *lower_inode = get_lower_inode(dentry->d_inode);
		fsstack_copy_attr_all(dentry->d_inode, lower_inode);
	}
	return valid;
}
#else
static int dazukofs_d_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	int valid;

	if (!lower_dentry->d_op || !lower_dentry->d_op->d_revalidate)
		return 1;

	if (!nd) {
		/* No nameidata provided. We are probably
		 * in a lookup_one_len() calling stack.
		 * Setup new nameidata ourselves. */
		struct nameidata new_nd;
		struct vfsmount *lower_mnt_parent =
					get_lower_mnt(dentry->d_parent);
		struct dentry *lower_dentry_parent =
					get_lower_dentry(dentry->d_parent);
		int err = vfs_path_lookup(lower_dentry_parent,
					  lower_mnt_parent,
					  dentry->d_name.name, 0, &new_nd);
		if (err)
			return -EINVAL;

		valid = lower_dentry->d_op->d_revalidate(lower_dentry,
				&new_nd);
		path_put(&new_nd.path);
	} else {
		struct vfsmount *lower_mnt = get_lower_mnt(dentry);
		struct vfsmount *vfsmount_save = nd->path.mnt;
		struct dentry *dentry_save = nd->path.dentry;

		nd->path.mnt = mntget(lower_mnt);
		nd->path.dentry = dget(lower_dentry);

		valid = lower_dentry->d_op->d_revalidate(lower_dentry, nd);

		mntput(lower_mnt);
		dput(lower_dentry);

		nd->path.mnt = vfsmount_save;
		nd->path.dentry = dentry_save;
	}

	/* update the inode, even if d_revalidate() != 1 */
	if (dentry->d_inode) {
		struct inode *lower_inode = get_lower_inode(dentry->d_inode);
		fsstack_copy_attr_all(dentry->d_inode, lower_inode);
	}
	return valid;
}
#endif

/**
 * dazukofs_d_hash - hash the given name
 * @dentry: the parent dentry
 * @name: the name to hash
 *
 * Description: Called when the VFS adds a dentry to the hash table.
 *
 * Call d_hash() on the lower dentry if available. Otherwise dazukofs
 * does nothing. This is ok because the VFS will compute a default
 * hash.
 *
 * Returns 0 on success.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
static int dazukofs_d_hash(const struct dentry *dentry, 
						   const struct inode *inode, struct qstr *name)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);
	
	if (!lower_dentry->d_op || !lower_dentry->d_op->d_hash)
		return 0;
	
	return lower_dentry->d_op->d_hash(lower_dentry, inode, name);
}
#else
static int dazukofs_d_hash(struct dentry *dentry, struct qstr *name)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);

	if (!lower_dentry->d_op || !lower_dentry->d_op->d_hash)
		return 0;

	return lower_dentry->d_op->d_hash(lower_dentry, name);
}
#endif

/**
 * dazukofs_d_release - clean up dentry
 * @dentry: the dentry that will be released
 *
 * Description: Called when a dentry is really deallocated.
 *
 * Release our hold on the lower dentry and mnt. Then free the structure
 * (from the cache) containing the lower data for this dentry.
 */
static void dazukofs_d_release(struct dentry *dentry)
{
	if (get_dentry_private(dentry)) {
		dput(get_lower_dentry(dentry));
		mntput(get_lower_mnt(dentry));

		kmem_cache_free(dazukofs_dentry_info_cachep,
				get_dentry_private(dentry));
	}
}

/**
 * dazukofs_d_compare - used to compare dentry's
 * @dentry: the parent dentry
 * @a: qstr of an existing dentry
 * @b: qstr of a second dentry (dentry may not be valid)
 *
 * Description: Called when a dentry should be compared with another.
 *
 * Call d_compare() on the lower dentry if available. Otherwise, perform
 * some basic comparisons between the two qstr's.
 *
 * Returns 0 if they are the same, otherwise 1.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
static int dazukofs_d_compare(const struct dentry *parent,
							  const struct inode *pinode,
							  const struct dentry *dentry, const struct inode *inode,
							  unsigned int len, const char *str, const struct qstr *name)
{
	if (len != name->len || memcmp(str, name->name, len))
		return 1;
	return 0;
}
#else
static int dazukofs_d_compare(struct dentry *dentry, struct qstr *a,
							  struct qstr *b)
{
	struct dentry *lower_dentry = get_lower_dentry(dentry);

	if (lower_dentry->d_op && lower_dentry->d_op->d_compare)
		return lower_dentry->d_op->d_compare(lower_dentry, a, b);

	if (a->len != b->len)
		return 1;
	if (memcmp(a->name, b->name, a->len))
		return 1;
	return 0;
}
#endif

/**
 * Unused operations:
 *   - d_delete
 *   - d_iput
 *   - d_dname
 */
struct dentry_operations dazukofs_dops = {
	.d_revalidate	= dazukofs_d_revalidate,
	.d_hash		= dazukofs_d_hash,
	.d_release	= dazukofs_d_release,
	.d_compare	= dazukofs_d_compare,
};
