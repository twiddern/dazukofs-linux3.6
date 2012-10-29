/* dazukofs: access control stackable filesystem

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

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/module.h>

#include "event.h"
#include "dev.h"

static int dazukofs_ctrl_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static int dazukofs_ctrl_release(struct inode *inode, struct file *file)
{
	/*
	 * checkpatch.pl recommends not checking for NULL before freeing
	 * the data because kfree(NULL) is allowed. However, that is
	 * poor style and leads to sloppy programming.
	 */

	if (file->private_data)
		kfree(file->private_data);

	return 0;
}

static ssize_t dazukofs_ctrl_read(struct file *file, char __user *buffer,
				  size_t length, loff_t *pos)
{
	char *buf = file->private_data;
	size_t buflen;
	int err;

	if (!file->private_data) {
		err = dazukofs_get_groups(&buf);
		if (err)
			return err;
		file->private_data = buf;
	}
	buflen = strlen(buf);

	if (*pos >= buflen)
		return 0;

	if (length > buflen - *pos)
		length = buflen - *pos;

	if (copy_to_user(buffer, buf + *pos, length))
		return -EFAULT;

	*pos += length;

	return length;
}

#define DAZUKOFS_ALLOWED_GROUPCHARS \
	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"
static int is_valid_char(char c)
{
	if (strchr(DAZUKOFS_ALLOWED_GROUPCHARS, c) != NULL)
		return 1;
	return 0;
}

static int process_command(char *buf, const char *key,
			   int (*func)(const char *, int), int arg2,
			   int *retcode)
{
	char *p;
	char *p2;

	p = strstr(buf, key);
	if (!p)
		return -1;

	p += strlen(key);

	for (p2 = p; is_valid_char(*p2); p2++)
		;

	if (p == p2) {
		*retcode = -EINVAL;
	} else {
		*p2 = 0;
		*retcode = func(p, arg2);
		*p2 = ' ';
	}

	return 0;
}

static ssize_t dazukofs_ctrl_write(struct file *file,
				   const char __user *buffer, size_t length,
				   loff_t *pos)
{
#define DAZUKOFS_MAX_WRITE_BUFFER 32
	char tmp[DAZUKOFS_MAX_WRITE_BUFFER];
	int match = 0;
	int ret = -EINVAL;
	int cp_len = length;

	cp_len = (length >= DAZUKOFS_MAX_WRITE_BUFFER) ?
				(DAZUKOFS_MAX_WRITE_BUFFER - 1) : length;
	if (copy_from_user(tmp, buffer, cp_len))
		return -EFAULT;

	tmp[cp_len] = 0;

	if (!match || (match && ret >= 0)) {
		if (process_command(tmp, "del=",
				    dazukofs_remove_group, 0, &ret) == 0) {
			match = 1;
		}
	}

	if (!match || (match && ret >= 0)) {
		if (process_command(tmp, "add=",
				    dazukofs_add_group, 0, &ret) == 0) {
			match = 1;
		}
	}

	if (!match || (match && ret >= 0)) {
		if (process_command(tmp, "addtrack=",
				    dazukofs_add_group, 1, &ret) == 0) {
			match = 1;
		}
	}

	if (ret >= 0) {
		*pos += length;
		ret = length;
	}

	return ret;
}

static struct cdev ctrl_cdev;

static const struct file_operations ctrl_fops = {
	.owner		= THIS_MODULE,
	.open		= dazukofs_ctrl_open,
	.release	= dazukofs_ctrl_release,
	.read		= dazukofs_ctrl_read,
	.write		= dazukofs_ctrl_write,
};

int dazukofs_ctrl_dev_init(int dev_major, int dev_minor,
			   struct class *dazukofs_class)
{
	int err = 0;
	struct device *dev;

	/* setup cdev for control */
	cdev_init(&ctrl_cdev, &ctrl_fops);
	ctrl_cdev.owner = THIS_MODULE;
	err = cdev_add(&ctrl_cdev, MKDEV(dev_major, dev_minor), 1);
	if (err)
		goto error_out1;

	/* create control device */
	dev = device_create(dazukofs_class, NULL, MKDEV(dev_major, dev_minor),
			    NULL, "%s.ctrl", DEVICE_NAME);
	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto error_out2;
	}

	return 0;

error_out2:
	cdev_del(&ctrl_cdev);
error_out1:
	return err;
}

void dazukofs_ctrl_dev_destroy(int dev_major, int dev_minor,
			       struct class *dazukofs_class)
{
	device_destroy(dazukofs_class, MKDEV(dev_major, dev_minor));
	cdev_del(&ctrl_cdev);
}
