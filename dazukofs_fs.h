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

#ifndef __DAZUKOFS_FS_H
#define __DAZUKOFS_FS_H

#define DAZUKOFS_VERSION "3.1.4"

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/dcache.h>
#include <linux/mount.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>

extern struct kmem_cache *dazukofs_dentry_info_cachep;
extern struct kmem_cache *dazukofs_file_info_cachep;
extern const struct file_operations dazukofs_main_fops;
extern const struct file_operations dazukofs_dir_fops;
extern struct dentry_operations dazukofs_dops;
extern const struct address_space_operations dazukofs_aops;

extern int dazukofs_interpose(struct dentry *lower_dentry,
			      struct dentry *dentry, struct super_block *sb,
			      int already_hashed);

struct dazukofs_sb_info {
	struct super_block *lower_sb;
};

struct dazukofs_inode_info {
	struct inode *lower_inode;

	/*
	 * the inode (embedded)
	 */
	struct inode vfs_inode;
};

struct dazukofs_dentry_info {
	struct dentry *lower_dentry;
	struct vfsmount *lower_mnt;
};

struct dazukofs_file_info {
	struct file *lower_file;
};

static inline struct dazukofs_sb_info *get_sb_private(
						struct super_block *upper_sb)
{
	return upper_sb->s_fs_info;
}

static inline void set_sb_private(struct super_block *upper_sb,
				  struct dazukofs_sb_info *sbi)
{
	upper_sb->s_fs_info = sbi;
}

static inline struct super_block *get_lower_sb(struct super_block *upper_sb)
{
	return get_sb_private(upper_sb)->lower_sb;
}

static inline void set_lower_sb(struct super_block *upper_sb,
				struct super_block *lower_sb)
{
	struct dazukofs_sb_info *sbi = get_sb_private(upper_sb);
	sbi->lower_sb = lower_sb;
}

static inline struct dazukofs_inode_info *get_inode_private(
						struct inode *upper_inode)
{
	return container_of(upper_inode, struct dazukofs_inode_info,
			    vfs_inode);
}

static inline struct inode *get_lower_inode(struct inode *upper_inode)
{
	return get_inode_private(upper_inode)->lower_inode;
}

static inline void set_lower_inode(struct inode *upper_inode,
				   struct inode *lower_inode)
{
	struct dazukofs_inode_info *dii = get_inode_private(upper_inode);
	dii->lower_inode = lower_inode;
}

static inline struct dazukofs_dentry_info *get_dentry_private(
						struct dentry *upper_dentry)
{
	return upper_dentry->d_fsdata;
}

static inline void set_dentry_private(struct dentry *upper_dentry,
				      struct dazukofs_dentry_info *dentryi)
{
	upper_dentry->d_fsdata = dentryi;
}

static inline struct dentry *get_lower_dentry(struct dentry *upper_dentry)
{
	return get_dentry_private(upper_dentry)->lower_dentry;
}

static inline struct vfsmount *get_lower_mnt(struct dentry *upper_dentry)
{
	return get_dentry_private(upper_dentry)->lower_mnt;
}

static inline void set_lower_dentry(struct dentry *upper_dentry,
				    struct dentry *lower_dentry,
				    struct vfsmount *lower_mnt)
{
	struct dazukofs_dentry_info *dii = get_dentry_private(upper_dentry);
	dii->lower_dentry = lower_dentry;
	dii->lower_mnt = lower_mnt;
}

static inline struct dazukofs_file_info *get_file_private(
						struct file *upper_file)
{
	return upper_file->private_data;
}

static inline void set_file_private(struct file *upper_file,
				    struct dazukofs_file_info *filei)
{
	upper_file->private_data = filei;
}

static inline struct file *get_lower_file(struct file *upper_file)
{
	return get_file_private(upper_file)->lower_file;
}

static inline void set_lower_file(struct file *upper_file,
				  struct file *lower_file)
{
	struct dazukofs_file_info *dfi = get_file_private(upper_file);
	dfi->lower_file = lower_file;
}

#endif  /* __DAZUKOFS_FS_H */
