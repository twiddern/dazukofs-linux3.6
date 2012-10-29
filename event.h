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

#ifndef __EVENT_H
#define __EVENT_H

#include <linux/poll.h>

typedef enum {
	ALLOW,
	DENY,
	REPOST,
} dazukofs_response_t;

extern int dazukofs_init_events(void);
extern void dazukofs_destroy_events(void);

extern unsigned int dazukofs_poll(unsigned long group_id,
				  struct file *dev_file, poll_table *wait);
extern int dazukofs_get_event(unsigned long group_id,
			      unsigned long *event_id, int *fd, pid_t *pid);
extern int dazukofs_return_event(unsigned long group_id,
				 unsigned long event_id,
				 dazukofs_response_t response);

extern int dazukofs_check_access(struct dentry *dentry, struct vfsmount *mnt);

extern int dazukofs_group_open_tracking(unsigned long group_id);
extern void dazukofs_group_release_tracking(unsigned long group_id);

extern int dazukofs_get_groups(char **buf);
extern int dazukofs_add_group(const char *name, int track);
extern int dazukofs_remove_group(const char *name, int unused);

#endif /* __EVENT_H */
