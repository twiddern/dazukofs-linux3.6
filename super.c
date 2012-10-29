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
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/mm.h>

#include "dazukofs_fs.h"
#include "dev.h"

static struct kmem_cache *dazukofs_inode_info_cachep;
static struct kmem_cache *dazukofs_sb_info_cachep;
struct kmem_cache *dazukofs_dentry_info_cachep;
struct kmem_cache *dazukofs_file_info_cachep;

static struct inode *dazukofs_alloc_inode(struct super_block *sb)
{
	struct dazukofs_inode_info *inodei =
		kmem_cache_alloc(dazukofs_inode_info_cachep, GFP_KERNEL);
	if (!inodei)
		return NULL;

	/*
	 * The inode is embedded within the dazukofs_inode_info struct.
	 */
	return &(inodei->vfs_inode);
}

static void dazukofs_destroy_inode(struct inode *inode)
{
	/*
	 * The inode is embedded within the dazukofs_inode_info struct.
	 */
	kmem_cache_free(dazukofs_inode_info_cachep,
			get_inode_private(inode));
}

static int dazukofs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct dentry *lower_dentry = dget(get_lower_dentry(dentry));

	if (!lower_dentry->d_sb->s_op->statfs)
		return -ENOSYS;

	return lower_dentry->d_sb->s_op->statfs(lower_dentry, buf);
}

static void dazukofs_evict_inode(struct inode *inode)
{
	truncate_inode_pages(&inode->i_data, 0);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	clear_inode(inode);
#else
	end_writeback(inode);
#endif
	iput(get_lower_inode(inode));
}

static void dazukofs_put_super(struct super_block *sb)
{
	struct dazukofs_sb_info *sbi = get_sb_private(sb);
	if (sbi)
		kmem_cache_free(dazukofs_sb_info_cachep, sbi);
}

/**
 * Unused operations:
 *   - dirty_inode
 *   - write_inode
 *   - put_inode
 *   - drop_inode
 *   - delete_inode
 *   - write_super
 *   - sync_fs
 *   - write_super_lockfs
 *   - unlockfs
 *   - remount_fs
 *   - umount_begin
 *   - show_options
 *   - show_stats
 *   - quota_read
 *   - quota_write
 */
static struct super_operations dazukofs_sops = {
	.alloc_inode	= dazukofs_alloc_inode,
	.destroy_inode	= dazukofs_destroy_inode,
	.put_super	= dazukofs_put_super,
	.statfs		= dazukofs_statfs,
	.evict_inode	= dazukofs_evict_inode,
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 39)
static int dazukofs_parse_mount_options(char *options, struct super_block *sb)
{
	return 0;
}
#endif

static int dazukofs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct dazukofs_sb_info *sbi;
	struct dentry *root;
	static const struct qstr name = { .name = "/", .len = 1 };
	struct dazukofs_dentry_info *di;

	sbi =  kmem_cache_zalloc(dazukofs_sb_info_cachep, GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_op = &dazukofs_sops;

	root = d_alloc(NULL, &name);
	if (!root) {
		kmem_cache_free(dazukofs_sb_info_cachep, sbi);
		return -ENOMEM;
	}

	sb->s_root = root;

	sb->s_root->d_op = &dazukofs_dops;
	sb->s_root->d_sb = sb;
	sb->s_root->d_parent = sb->s_root;

	di = kmem_cache_zalloc(dazukofs_dentry_info_cachep, GFP_KERNEL);
	if (!di) {
		kmem_cache_free(dazukofs_sb_info_cachep, sbi);
		dput(sb->s_root);
		return -ENOMEM;
	}

	set_dentry_private(sb->s_root, di);

	set_sb_private(sb, sbi);

	return 0;
}

static int dazukofs_read_super(struct super_block *sb, const char *dev_name)
{
	struct nameidata nd;
	struct dentry *lower_root;
	struct vfsmount *lower_mnt;
	int err;

	memset(&nd, 0, sizeof(struct nameidata));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 39)
	err = kern_path(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &nd.path);
#else
	err = path_lookup(dev_name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &nd);
#endif
	if (err)
		return err;

	lower_root = dget(nd.path.dentry);
	lower_mnt = mntget(nd.path.mnt);

	if (IS_ERR(lower_root)) {
		err = PTR_ERR(lower_root);
		goto out_put;
	}

	if (!lower_root->d_inode) {
		err = -ENOENT;
		goto out_put;
	}

	if (!S_ISDIR(lower_root->d_inode->i_mode)) {
		err = -EINVAL;
		goto out_put;
	}

	set_lower_sb(sb, lower_root->d_sb);
	sb->s_maxbytes = lower_root->d_sb->s_maxbytes;
	set_lower_dentry(sb->s_root, lower_root, lower_mnt);

	err = dazukofs_interpose(lower_root, sb->s_root, sb, 0);
	if (err)
		goto out_put;
	goto out;

out_put:
	dput(lower_root);
	mntput(lower_mnt);
out:
	path_put(&nd.path);
	return err;
}

// FIXME!
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 39)

static struct dentry* dazukofs_get_sb(struct file_system_type *fs_type, int flags,
						   const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, dazukofs_fill_super);
}
#else
static int dazukofs_get_sb(struct file_system_type *fs_type, int flags,
			   const char *dev_name, void *data,
			   struct vfsmount *mnt)
{
	struct super_block *sb;
	int err;

	err = get_sb_nodev(fs_type, flags, data, dazukofs_fill_super, mnt);

	if (err)
		goto out;
	
	sb = mnt->mnt_sb;

	err = dazukofs_parse_mount_options(data, sb);
	if (err)
		goto out_abort;

	err = dazukofs_read_super(sb, dev_name);
	if (err)
		goto out_abort;

	goto out;

out_abort:
	dput(sb->s_root);
	up_write(&sb->s_umount);
	deactivate_super(sb);
out:
	return err;
}
#endif

static void init_once(void *data)
{
	struct dazukofs_inode_info *inode_info =
		(struct dazukofs_inode_info *)data;

	memset(inode_info, 0, sizeof(struct dazukofs_inode_info));
	inode_init_once(&(inode_info->vfs_inode));
}

static void destroy_caches(void)
{
	if (dazukofs_inode_info_cachep) {
		kmem_cache_destroy(dazukofs_inode_info_cachep);
		dazukofs_inode_info_cachep = NULL;
	}

	if (dazukofs_sb_info_cachep) {
		kmem_cache_destroy(dazukofs_sb_info_cachep);
		dazukofs_sb_info_cachep = NULL;
	}

	if (dazukofs_dentry_info_cachep) {
		kmem_cache_destroy(dazukofs_dentry_info_cachep);
		dazukofs_dentry_info_cachep = NULL;
	}

	if (dazukofs_file_info_cachep) {
		kmem_cache_destroy(dazukofs_file_info_cachep);
		dazukofs_file_info_cachep = NULL;
	}
}

static int init_caches(void)
{
	dazukofs_inode_info_cachep =
		kmem_cache_create("dazukofs_inode_info_cache",
				  sizeof(struct dazukofs_inode_info), 0,
				  SLAB_HWCACHE_ALIGN,
				  init_once);
	if (!dazukofs_inode_info_cachep)
		goto out_nomem;

	dazukofs_sb_info_cachep =
		kmem_cache_create("dazukofs_sb_info_cache",
				  sizeof(struct dazukofs_sb_info), 0,
				  SLAB_HWCACHE_ALIGN,
				  NULL);
	if (!dazukofs_sb_info_cachep)
		goto out_nomem;

	dazukofs_dentry_info_cachep =
		kmem_cache_create("dazukofs_dentry_info_cache",
				  sizeof(struct dazukofs_dentry_info), 0,
				  SLAB_HWCACHE_ALIGN,
				  NULL);
	if (!dazukofs_dentry_info_cachep)
		goto out_nomem;

	dazukofs_file_info_cachep =
		kmem_cache_create("dazukofs_file_info_cache",
				  sizeof(struct dazukofs_file_info), 0,
				  SLAB_HWCACHE_ALIGN,
				  NULL);
	if (!dazukofs_file_info_cachep)
		goto out_nomem;

	return 0;

out_nomem:
	destroy_caches();
	return -ENOMEM;
}

static struct file_system_type dazukofs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "dazukofs",
	.mount		= dazukofs_get_sb,
	/*
	 * XXX: We are using kill_anon_super() instead of my own function.
	 *      Is this OK?
	 */
	.kill_sb	= kill_anon_super,
	.fs_flags	= 0,
};

static int __init init_dazukofs_fs(void)
{
	int err;

	err = dazukofs_dev_init();
	if (err)
		goto error_out1;

	err = init_caches();
	if (err)
		goto error_out2;

	err = register_filesystem(&dazukofs_fs_type);
	if (err)
		goto error_out3;

	printk(KERN_INFO "dazukofs: loaded, version=%s\n", DAZUKOFS_VERSION);
	return 0;

error_out3:
	destroy_caches();
error_out2:
	dazukofs_dev_destroy();
error_out1:
	return err;
}

static void __exit exit_dazukofs_fs(void)
{
	unregister_filesystem(&dazukofs_fs_type);
	destroy_caches();
	dazukofs_dev_destroy();
	printk(KERN_INFO "dazukofs: unloaded, version=%s\n", DAZUKOFS_VERSION);
}

MODULE_AUTHOR("John Ogness");
MODULE_DESCRIPTION("access control stackable filesystem");
MODULE_LICENSE("GPL");
module_init(init_dazukofs_fs)
module_exit(exit_dazukofs_fs)
