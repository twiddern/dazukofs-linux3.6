/* dazukofs: access control stackable filesystem

   Copyright (C) 2008-2009 John Ogness
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

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/sched.h>

#include "dazukofs_fs.h"
#include "dev.h"

struct dazukofs_proc {
	struct list_head list;
	struct pid *proc_id;
};

static struct dazukofs_proc ign_list;
static struct mutex ign_list_mutex;
static struct kmem_cache *dazukofs_ign_cachep;

int dazukofs_check_ignore_process(void)
{
	struct dazukofs_proc *proc;
	struct list_head *pos;
	int found = 0;
	struct pid *cur_proc_id = find_get_pid(task_pid_nr(current));

	mutex_lock(&ign_list_mutex);
	list_for_each(pos, &ign_list.list) {
		proc = list_entry(pos, struct dazukofs_proc, list);
		if (proc->proc_id == cur_proc_id) {
			found = 1;
			break;
		}
	}
	mutex_unlock(&ign_list_mutex);

	put_pid(cur_proc_id);

	return !found;
}

static int dazukofs_add_ign(struct file *file)
{
	struct dazukofs_proc *proc =
		kmem_cache_zalloc(dazukofs_ign_cachep, GFP_KERNEL);
	if (!proc) {
		file->private_data = NULL;
		return -ENOMEM;
	}

	file->private_data = proc;
	proc->proc_id = find_get_pid(task_pid_nr(current));

	mutex_lock(&ign_list_mutex);
	list_add(&proc->list, &ign_list.list);
	mutex_unlock(&ign_list_mutex);

	return 0;
}

static void dazukofs_remove_ign(struct file *file)
{
	struct list_head *pos;
	struct dazukofs_proc *proc = NULL;
	struct dazukofs_proc *check_proc = file->private_data;
	int found = 0;

	if (!check_proc)
		return;

	mutex_lock(&ign_list_mutex);
	list_for_each(pos, &ign_list.list) {
		proc = list_entry(pos, struct dazukofs_proc, list);
		if (proc->proc_id == check_proc->proc_id) {
			found = 1;
			put_pid(proc->proc_id);
			list_del(pos);
			break;
		}
	}
	mutex_unlock(&ign_list_mutex);

	if (found) {
		file->private_data = NULL;
		kmem_cache_free(dazukofs_ign_cachep, proc);
	}
}

static int dazukofs_ign_open(struct inode *inode, struct file *file)
{
	return dazukofs_add_ign(file);
}

static int dazukofs_ign_release(struct inode *inode, struct file *file)
{
	dazukofs_remove_ign(file);
	return 0;
}

static void dazukofs_destroy_ignlist(void)
{
	struct list_head *pos;
	struct list_head *q;
	struct dazukofs_proc *proc;

	list_for_each_safe(pos, q, &ign_list.list) {
		proc = list_entry(pos, struct dazukofs_proc, list);
		list_del(pos);
		kmem_cache_free(dazukofs_ign_cachep, proc);
	}
}

static struct cdev ign_cdev;

static const struct file_operations ign_fops = {
	.owner		= THIS_MODULE,
	.open		= dazukofs_ign_open,
	.release	= dazukofs_ign_release,
};

int dazukofs_ign_dev_init(int dev_major, int dev_minor,
			  struct class *dazukofs_class)
{
	int err = 0;
	struct device *dev;

	INIT_LIST_HEAD(&ign_list.list);
	mutex_init(&ign_list_mutex);

	dazukofs_ign_cachep =
		kmem_cache_create("dazukofs_ign_cache",
				  sizeof(struct dazukofs_proc), 0,
				  SLAB_HWCACHE_ALIGN, NULL);
	if (!dazukofs_ign_cachep) {
		err = -ENOMEM;
		goto error_out1;
	}

	/* setup cdev for ignore */
	cdev_init(&ign_cdev, &ign_fops);
	ign_cdev.owner = THIS_MODULE;
	err = cdev_add(&ign_cdev, MKDEV(dev_major, dev_minor), 1);
	if (err)
		goto error_out2;

	/* create ignore device */
	dev = device_create(dazukofs_class, NULL, MKDEV(dev_major, dev_minor),
			    NULL, "%s.ign", DEVICE_NAME);
	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto error_out3;
	}

	return 0;

error_out3:
	cdev_del(&ign_cdev);
error_out2:
	dazukofs_destroy_ignlist();
	kmem_cache_destroy(dazukofs_ign_cachep);
error_out1:
	return err;
}

void dazukofs_ign_dev_destroy(int dev_major, int dev_minor,
			      struct class *dazukofs_class)
{
	device_destroy(dazukofs_class, MKDEV(dev_major, dev_minor));
	cdev_del(&ign_cdev);
	dazukofs_destroy_ignlist();
	kmem_cache_destroy(dazukofs_ign_cachep);
}
