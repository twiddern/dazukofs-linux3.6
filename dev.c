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

#include "dazukofs_fs.h"
#include "event.h"
#include "dev.h"

static struct class *dazukofs_class;

static int dev_major;
static int dev_minor_start;
static int dev_minor_end;

int dazukofs_dev_init(void)
{
	int err;
	dev_t devt;

	err = dazukofs_init_events();
	if (err)
		goto error_out1;

	err = alloc_chrdev_region(&devt, 0, 2 + GROUP_COUNT, DEVICE_NAME);
	if (err)
		goto error_out2;
	dev_major = MAJOR(devt);
	dev_minor_start = MINOR(devt);

	dazukofs_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(dazukofs_class)) {
		err = PTR_ERR(dazukofs_class);
		goto error_out3;
	}

	err = dazukofs_ctrl_dev_init(dev_major, dev_minor_start,
				     dazukofs_class);
	if (err)
		goto error_out4;

	err = dazukofs_ign_dev_init(dev_major, dev_minor_start + 1,
				    dazukofs_class);
	if (err)
		goto error_out5;

	dev_minor_end = dazukofs_group_dev_init(dev_major,
						dev_minor_start + 2,
						dazukofs_class);
	if (dev_minor_end < 0) {
		err = dev_minor_end;
		goto error_out6;
	}

	return 0;

error_out6:
	dazukofs_ign_dev_destroy(dev_major, dev_minor_start + 1,
				 dazukofs_class);
error_out5:
	dazukofs_ctrl_dev_destroy(dev_major, dev_minor_start, dazukofs_class);
error_out4:
	class_destroy(dazukofs_class);
error_out3:
	unregister_chrdev_region(MKDEV(dev_major, dev_minor_start),
				 2 + GROUP_COUNT);
error_out2:
	dazukofs_destroy_events();
error_out1:
	return err;
}

void dazukofs_dev_destroy(void)
{
	dazukofs_group_dev_destroy(dev_major, dev_minor_start + 2,
				   dev_minor_end, dazukofs_class);
	dazukofs_ign_dev_destroy(dev_major, dev_minor_start + 1,
				 dazukofs_class);
	dazukofs_ctrl_dev_destroy(dev_major, dev_minor_start, dazukofs_class);
	class_destroy(dazukofs_class);
	unregister_chrdev_region(MKDEV(dev_major, dev_minor_start),
				 2 + GROUP_COUNT);
	dazukofs_destroy_events();
}
