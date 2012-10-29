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
#include <linux/syscalls.h>

#include "dazukofs_fs.h"
#include "event.h"
#include "dev.h"

static int dazukofs_group_open(int group_id, struct inode *inode,
			       struct file *file)
{
	if (dazukofs_group_open_tracking(group_id))
		file->private_data = file;
	else
		file->private_data = NULL;
	return 0;
}

static int dazukofs_group_release(int group_id, struct inode *inode,
				  struct file *file)
{
	if (file->private_data)
		dazukofs_group_release_tracking(group_id);
	return 0;
}

static ssize_t dazukofs_group_read(int group_id, struct file *file,
				   char __user *buffer, size_t length,
				   loff_t *pos)
{
#define DAZUKOFS_MIN_READ_BUFFER 43
	char tmp[DAZUKOFS_MIN_READ_BUFFER];
	ssize_t tmp_used;
	pid_t pid;
	int fd;
	int err;
	unsigned long event_id;

	if (*pos > 0)
		return 0;

	if (length < DAZUKOFS_MIN_READ_BUFFER)
		return -EINVAL;

	err = dazukofs_get_event(group_id, &event_id, &fd, &pid);
	if (err) {
		/* convert some errors to acceptable read(2) errno values */
		if (err == -ERESTARTSYS)
			return -EINTR;
		else if (err == -ENFILE)
			return -EIO;
		return err;
	}

	tmp_used = snprintf(tmp, sizeof(tmp)-1, "id=%lu\nfd=%d\npid=%d\n",
			    event_id, fd, pid);
	if (tmp_used >= sizeof(tmp)) {
		sys_close(fd);
		dazukofs_return_event(group_id, event_id, REPOST);
		return -EINVAL;
	}

	if (copy_to_user(buffer, tmp, tmp_used)) {
		sys_close(fd);
		dazukofs_return_event(group_id, event_id, REPOST);
		return -EFAULT;
	}

	*pos = tmp_used;

	return tmp_used;
}

static ssize_t dazukofs_group_write(int group_id, struct file *file,
				    const char __user *buffer, size_t length,
				    loff_t *pos)
{
#define DAZUKOFS_MAX_WRITE_BUFFER 19
	char tmp[DAZUKOFS_MAX_WRITE_BUFFER];
	dazukofs_response_t response;
	unsigned long event_id;
	char *p;
	char *p2;
	int ret;

	if (length >= DAZUKOFS_MAX_WRITE_BUFFER)
		length = DAZUKOFS_MAX_WRITE_BUFFER - 1;

	if (copy_from_user(tmp, buffer, length))
		return -EFAULT;
	tmp[length] = 0;

	p = strstr(tmp, "id=");
	if (!p)
		return -EINVAL;
	event_id = simple_strtoul(p + 3, &p2, 10);

	/*
	 * checkpatch.pl recommends using strict_strtoul() instead of
	 * simple_strtoul(). However, we _want_ a function that stops
	 * on non-number characters rather than errors out.
	 */

	p = strstr(p2, "r=");
	if (!p)
		return -EINVAL;
	if ((*(p + 2)) - '0' != 0)
		response = DENY;
	else
		response = ALLOW;

	ret = dazukofs_return_event(group_id, event_id, response);
	if (ret == 0) {
		*pos += length;
		ret = length;
	} else if (ret == -ERESTARTSYS) {
		ret = -EINTR;
	}

	return ret;
}

static unsigned int dazukofs_group_poll(int group_id, struct file *file,
					poll_table *wait)
{
	return dazukofs_poll(group_id, file, wait);
}

#define DECLARE_GROUP_FOPS(group_id) \
static int \
dazukofs_group_open_##group_id(struct inode *inode, struct file *file) \
{ \
	return dazukofs_group_open(group_id, inode, file); \
} \
static int \
dazukofs_group_release_##group_id(struct inode *inode, struct file *file) \
{ \
	return dazukofs_group_release(group_id, inode, file); \
} \
static ssize_t \
dazukofs_group_read_##group_id(struct file *file, char __user *buffer, \
			       size_t length, loff_t *pos) \
{ \
	return dazukofs_group_read(group_id, file, buffer, length, pos); \
} \
static ssize_t \
dazukofs_group_write_##group_id(struct file *file, \
				const char __user *buffer, size_t length, \
				loff_t *pos) \
{ \
	return dazukofs_group_write(group_id, file, buffer, length, pos); \
} \
static unsigned int \
dazukofs_group_poll_##group_id(struct file *file, poll_table *wait) \
{ \
	return dazukofs_group_poll(group_id, file, wait); \
} \
static const struct file_operations group_fops_##group_id = { \
	.owner		= THIS_MODULE, \
	.open		= dazukofs_group_open_##group_id, \
	.release	= dazukofs_group_release_##group_id, \
	.read		= dazukofs_group_read_##group_id, \
	.write		= dazukofs_group_write_##group_id, \
	.poll		= dazukofs_group_poll_##group_id, \
};

DECLARE_GROUP_FOPS(0)
DECLARE_GROUP_FOPS(1)
DECLARE_GROUP_FOPS(2)
DECLARE_GROUP_FOPS(3)
DECLARE_GROUP_FOPS(4)
DECLARE_GROUP_FOPS(5)
DECLARE_GROUP_FOPS(6)
DECLARE_GROUP_FOPS(7)
DECLARE_GROUP_FOPS(8)
DECLARE_GROUP_FOPS(9)

static struct cdev groups_cdev[GROUP_COUNT];

static const struct file_operations *group_fops[GROUP_COUNT] = {
	&group_fops_0,
	&group_fops_1,
	&group_fops_2,
	&group_fops_3,
	&group_fops_4,
	&group_fops_5,
	&group_fops_6,
	&group_fops_7,
	&group_fops_8,
	&group_fops_9,
};

int dazukofs_group_dev_init(int dev_major, int dev_minor_start,
			    struct class *dazukofs_class)
{
	int err;
	struct device *dev;
	int i;
	int cdev_count;
	int dev_minor_end = dev_minor_start;

	/* setup cdevs for groups */
	for (cdev_count = 0; cdev_count < GROUP_COUNT; cdev_count++) {
		cdev_init(&groups_cdev[cdev_count], group_fops[cdev_count]);
		groups_cdev[cdev_count].owner = THIS_MODULE;
		err = cdev_add(&groups_cdev[cdev_count],
			       MKDEV(dev_major, dev_minor_start + cdev_count),
			       GROUP_COUNT);
		if (err)
			goto error_out1;
	}

	/* create group devices */
	for (i = 0; i < GROUP_COUNT; i++) {
		dev = device_create(dazukofs_class, NULL,
				    MKDEV(dev_major, dev_minor_end), NULL,
				    "%s.%d", DEVICE_NAME, i);
		if (IS_ERR(dev)) {
			err = PTR_ERR(dev);
			goto error_out2;
		}
		dev_minor_end++;
	}

	return dev_minor_end;

error_out2:
	for (i = dev_minor_start; i < dev_minor_end; i++)
		device_destroy(dazukofs_class, MKDEV(dev_major, i));
error_out1:
	for (i = 0; i < cdev_count; i++)
		cdev_del(&groups_cdev[i]);
	return err;
}

void dazukofs_group_dev_destroy(int dev_major, int dev_minor_start,
				int dev_minor_end,
				struct class *dazukofs_class)
{
	int i;

	for (i = dev_minor_start; i < dev_minor_end; i++)
		device_destroy(dazukofs_class, MKDEV(dev_major, i));

	for (i = 0; i < GROUP_COUNT; i++)
		cdev_del(&groups_cdev[i]);
}
