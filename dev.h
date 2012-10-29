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

#ifndef __DEV_H
#define __DEV_H

#include <linux/device.h>

#define DEVICE_NAME	"dazukofs"
#define GROUP_COUNT	10

extern int dazukofs_dev_init(void);
extern void dazukofs_dev_destroy(void);

extern int dazukofs_group_dev_init(int dev_major, int dev_minor_start,
				   struct class *dazukofs_class);
extern void dazukofs_group_dev_destroy(int dev_major, int dev_minor_start,
				       int dev_minor_end,
				       struct class *dazukofs_class);

extern int dazukofs_ctrl_dev_init(int dev_major, int dev_minor,
				  struct class *dazukofs_class);
extern void dazukofs_ctrl_dev_destroy(int dev_major, int dev_minor,
				      struct class *dazukofs_class);

extern int dazukofs_ign_dev_init(int dev_major, int dev_minor,
				 struct class *dazukofs_class);
extern void dazukofs_ign_dev_destroy(int dev_major, int dev_minor,
				     struct class *dazukofs_class);
extern int dazukofs_check_ignore_process(void);

#endif /* __DEV_H */
