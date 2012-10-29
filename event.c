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

#include <linux/list.h>
#include <linux/sched.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/mount.h>
#include <linux/freezer.h>
#include <linux/cred.h>
#include <linux/pid.h>
#include <linux/slab.h>

#include "dev.h"
#include "dazukofs_fs.h"
#include "event.h"

struct dazukofs_proc {
	struct list_head list;
	struct pid *proc_id;
	int within_list;
};

struct dazukofs_event {
	unsigned long event_id;
	struct dentry *dentry;
	struct vfsmount *mnt;
	struct pid *proc_id;
	wait_queue_head_t queue;

	/* protects: deny, deprecated, assigned */
	struct mutex assigned_mutex;

	int deny;
	int deprecated;
	int assigned;
};

struct dazukofs_event_container {
	struct list_head list;
	struct dazukofs_event *event;
	struct file *file;
	int fd;
};

struct dazukofs_group {
	struct list_head list;
	char *name;
	size_t name_length;
	unsigned long group_id;
	struct dazukofs_event_container todo_list;
	wait_queue_head_t queue;
	wait_queue_head_t poll_queue;
	struct dazukofs_event_container working_list;
	atomic_t use_count;
	int tracking;
	int track_count;
	int deprecated;
};

static struct dazukofs_group group_list;
static int group_count;

/* protects: group_list, group_count */
static struct rw_semaphore group_count_sem;

/* protects: group_list, grp->members, last_event_id,
 *	     todo_list, working_list */
static struct mutex work_mutex;

static struct mutex proc_mutex;
static struct dazukofs_proc proc_list;

static struct kmem_cache *dazukofs_group_cachep;
static struct kmem_cache *dazukofs_event_container_cachep;
static struct kmem_cache *dazukofs_event_cachep;

static int last_event_id;

/**
 * dazukofs_init_events - initialize event handling infrastructure
 *
 * Description: This is called once to initialize all the structures
 * needed to manage event handling.
 *
 * Returns 0 on success.
 */
int dazukofs_init_events(void)
{
	mutex_init(&proc_mutex);
	mutex_init(&work_mutex);
	init_rwsem(&group_count_sem);

	INIT_LIST_HEAD(&proc_list.list);
	INIT_LIST_HEAD(&group_list.list);

	dazukofs_group_cachep =
		kmem_cache_create("dazukofs_group_cache",
				  sizeof(struct dazukofs_group), 0,
				  SLAB_HWCACHE_ALIGN, NULL);
	if (!dazukofs_group_cachep)
		goto error_out;

	dazukofs_event_container_cachep =
		kmem_cache_create("dazukofs_event_container_cache",
				  sizeof(struct dazukofs_event_container), 0,
				  SLAB_HWCACHE_ALIGN, NULL);
	if (!dazukofs_event_container_cachep)
		goto error_out;

	dazukofs_event_cachep =
		kmem_cache_create("dazukofs_event_cache",
				  sizeof(struct dazukofs_event), 0,
				  SLAB_HWCACHE_ALIGN, NULL);
	if (!dazukofs_event_cachep)
		goto error_out;

	return 0;

error_out:
	if (dazukofs_group_cachep)
		kmem_cache_destroy(dazukofs_group_cachep);
	if (dazukofs_event_container_cachep)
		kmem_cache_destroy(dazukofs_event_container_cachep);
	if (dazukofs_event_cachep)
		kmem_cache_destroy(dazukofs_event_cachep);
	return -ENOMEM;
}

/**
 * release_event - release (and possible free) an event
 * @evt: the event to release
 * @decrement_assigned: flag to signal if the assigned count should be
 *                      decremented (only for registered processes)
 * @deny: flag if file access event should be denied
 *
 * Description: This function will decrement the assigned count for the
 * event. The "decrement_assigned" flag is used to distinguish between
 * the anonymous process accessing a file and the registered process. The
 * assigned count is only incremented for registered process (although the
 * anonymous process will also have a handle to the event).
 *
 * For the anonymous process (decrement_assigned = false):
 * If the assigned count is not zero, there are registered processes that
 * have a handle to this event. The event is marked deprecated. Otherwise
 * we free the event.
 *
 * For a registered process (decrement_assigned = true):
 * The assigned count is decremented. If it is now zero and the event is
 * not deprecated, then the anonymous process still has a handle. In this
 * case we wake the anonymous process. Otherwise we free the event.
 *
 * Aside from releasing the event, the deny status of the event is also
 * updated. The "normal" release process involves the registered processes
 * first releasing (and providing their deny values) and finally the
 * anonymous process will release (and free) the event after reading the
 * deny value.
 */
static void release_event(struct dazukofs_event *evt, int decrement_assigned,
			  int deny)
{
	int free_event = 0;

	mutex_lock(&evt->assigned_mutex);
	if (deny)
		evt->deny |= 1;

	if (decrement_assigned) {
		evt->assigned--;
		if (evt->assigned == 0) {
			if (!evt->deprecated)
				wake_up(&evt->queue);
			else
				free_event = 1;
		}
	} else {
		if (evt->assigned == 0)
			free_event = 1;
		else
			evt->deprecated = 1;
	}
	mutex_unlock(&evt->assigned_mutex);

	if (free_event) {
		dput(evt->dentry);
		mntput(evt->mnt);
		put_pid(evt->proc_id);
		kmem_cache_free(dazukofs_event_cachep, evt);
	}
}

/**
 * __clear_group_event_list - cleanup/release event list
 * @event_list - the list to clear
 *
 * Description: All events (and their containers) will be released/freed
 * for the given event list. The event list will be an empty (yet still
 * valid) list after this function is finished.
 *
 * IMPORTANT: This function requires work_mutex to be held!
 */
static void __clear_group_event_list(struct list_head *event_list)
{
	struct dazukofs_event_container *ec;
	struct list_head *pos;
	struct list_head *q;

	list_for_each_safe(pos, q, event_list) {
		ec = list_entry(pos, struct dazukofs_event_container, list);
		list_del(pos);

		release_event(ec->event, 1, 0);

		kmem_cache_free(dazukofs_event_container_cachep, ec);
	}
}

/**
 * __remove_group - clear all activity associated with the group
 * @grp: the group to clear
 *
 * Description: All pending and in-progress events are released/freed.
 * Any processes waiting on the queue are woken.
 *
 * The actual group structure is not deleted, but rather marked as
 * deprecated. Deprecated group structures are deleted as new
 * groups are added.
 *
 * IMPORTANT: This function requires work_mutex to be held!
 */
static void __remove_group(struct dazukofs_group *grp)
{
	grp->deprecated = 1;
	group_count--;

	__clear_group_event_list(&grp->working_list.list);
	__clear_group_event_list(&grp->todo_list.list);

	/* notify all registered process waiting for an event */
	wake_up_all(&grp->queue);
	wake_up_all(&grp->poll_queue);
}

/**
 * dazukofs_destroy_events - cleanup/shutdown event handling infrastructure
 *
 * Description: Release all pending events, free all allocated structures.
 */
void dazukofs_destroy_events(void)
{
	struct dazukofs_group *grp;
	struct list_head *pos;
	struct list_head *q;

	/*
	 * We are not using any locks here because we assume
	 * everything else has been already cleaned up by
	 * the device layer.
	 */

	/* free the groups */
	list_for_each_safe(pos, q, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		list_del(pos);

		__remove_group(grp);

		/* free group name */
		kfree(grp->name);

		/* free group */
		kmem_cache_free(dazukofs_group_cachep, grp);
	}

	/* free everything else */
	kmem_cache_destroy(dazukofs_group_cachep);
	kmem_cache_destroy(dazukofs_event_container_cachep);
	kmem_cache_destroy(dazukofs_event_cachep);
}

/**
 * __check_for_group - check if a group exists and set tracking
 * @name: a group name to check for
 * @id: a group id to check for
 * @track: flag set if tracking is to be used
 * @already_exists: will be set if the group already exists
 *
 * Description: This function checks names and id's to see if a group may
 * be created. If the id already exists, but with a different group name,
 * the group cannot be created. If the group name exists, but with a
 * different id, the group cannot be created.
 *
 * If the group name exists and the id is already that which is requested,
 * the function returns success, but sets the already_exists flag.
 *
 * NOTE: Although the function name may imply read-only, this function
 *       _will_ set a group to track if the group is found to exist and
 *       tracking should be set. We do this because it is convenient
 *       since the work_mutex is already locked.
 *
 * IMPORTANT: This function requires work_mutex to be held!
 *
 * Returns 0 if the group exists or may be created.
 */
static int __check_for_group(const char *name, int id, int track,
			     int *already_exists)
{
	struct dazukofs_group *grp;
	struct list_head *pos;
	struct list_head *q;
	int id_available = 1;

	*already_exists = 0;

	list_for_each_safe(pos, q, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (grp->deprecated) {
			/* cleanup deprecated groups */
			if (atomic_read(&grp->use_count) == 0) {
				list_del(pos);
				kfree(grp->name);
				kmem_cache_free(dazukofs_group_cachep, grp);
			}
		} else {
			if (strcmp(name, grp->name) == 0) {
				*already_exists = 1;
				if (track)
					grp->tracking = 1;
				break;
			} else if (grp->group_id == id) {
				id_available = 0;
				break;
			}
		}
	}

	if (*already_exists)
		return 0;

	if (id_available) {
		/* we have found a free id */
		return 0;
	}

	return -1;
}

/**
 * __create_group - allocate and initialize a group structure
 * @name: the name of the new group
 * @id: the id of the new group
 * @track: flag set if tracking is to be used
 *
 * Description: This function allocates and initializes a group
 * structure. The group_count should be locked to ensure that
 * the group id remains available until the group can be
 * added to the group list.
 *
 * Returns the newly created and initialized group structure.
 */
static struct dazukofs_group *__create_group(const char *name, int id,
					     int track)
{
	struct dazukofs_group *grp;

	grp = kmem_cache_zalloc(dazukofs_group_cachep, GFP_KERNEL);
	if (!grp)
		return NULL;

	atomic_set(&grp->use_count, 0);
	grp->group_id = id;
	grp->name = kstrdup(name, GFP_KERNEL);
	if (!grp->name) {
		kmem_cache_free(dazukofs_group_cachep, grp);
		return NULL;
	}
	grp->name_length = strlen(name);
	init_waitqueue_head(&grp->queue);
	init_waitqueue_head(&grp->poll_queue);
	INIT_LIST_HEAD(&grp->todo_list.list);
	INIT_LIST_HEAD(&grp->working_list.list);
	if (track)
		grp->tracking = 1;
	return grp;
}

/**
 * dazukofs_add_group - add a new group
 * @name: the name of the group to add
 * @track: flag set if tracking is to be used
 *
 * Description: This function is called by the device layer to add a new
 * group. It returns success if the group has been successfully created
 * or if the group already exists.
 *
 * If the group already exists and is not tracking, but "track" is set,
 * the group will be changed to start tracking (actually done in the
 * function __check_for_group()).
 *
 * Returns 0 on success.
 */
int dazukofs_add_group(const char *name, int track)
{
	int ret = 0;
	int already_exists;
	int available_id = 0;
	struct dazukofs_group *grp;

	down_write(&group_count_sem);

	mutex_lock(&work_mutex);
	while (__check_for_group(name, available_id, track,
				 &already_exists) != 0) {
		/* try again with the next id */
		available_id++;
	}
	mutex_unlock(&work_mutex);

	if (already_exists)
		goto out;

	/* if we are here, the group doesn't already exist */

	/* do we have room for a new group? */
	if (group_count == GROUP_COUNT) {
		ret = -EPERM;
		goto out;
	}

	grp = __create_group(name, available_id, track);
	if (!grp) {
		ret = -ENOMEM;
		goto out;
	}

	mutex_lock(&work_mutex);
	list_add_tail(&grp->list, &group_list.list);
	mutex_unlock(&work_mutex);

	group_count++;
out:
	up_write(&group_count_sem);
	return ret;
}

/**
 * dazukofs_remove_group - remove a group
 * @name: the name of the group to remove
 * @unsued: argument not used
 *
 * Description: This function is called by the device layer to remove a
 * group. It returns success if the group has been deleted or the group
 * does not exist.
 *
 * The unused argument exists for convenience to the device layer.
 *
 * Returns 0 on success.
 */
int dazukofs_remove_group(const char *name, int unused)
{
	int ret = 0;
	struct dazukofs_group *grp;
	struct list_head *pos;

	down_write(&group_count_sem);

	if (group_count == 0)
		goto out;

	mutex_lock(&work_mutex);
	/* set group deprecated */
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated && strcmp(name, grp->name) == 0) {
			__remove_group(grp);
			break;
		}
	}
	mutex_unlock(&work_mutex);
out:
	up_write(&group_count_sem);
	return ret;
}

/**
 * dazukofs_get_groups - get the names and id's of active groups as strings
 * @buf: to be assigned the list of groups as a single printable string
 *
 * Description: This function will allocate a string that includes all the
 * active (not deprecated) groups and their id's. This function is called
 * by the device layer for presenting userspace with the list of groups.
 *
 * This function will allocate memory that must be freed by the caller.
 *
 * Returns 0 on success.
 */
int dazukofs_get_groups(char **buf)
{
	struct dazukofs_group *grp;
	char *tmp;
	struct list_head *pos;
	size_t buflen;
	size_t allocsize = 256;

tryagain:
	*buf = kzalloc(allocsize, GFP_KERNEL);
	if (!*buf)
		return -ENOMEM;
	tmp = *buf;
	buflen = 1;

	mutex_lock(&work_mutex);
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated)
			buflen += grp->name_length + 3;
	}
	if (buflen < allocsize) {
		list_for_each(pos, &group_list.list) {
			grp = list_entry(pos, struct dazukofs_group, list);
			if (!grp->deprecated) {
				snprintf(tmp, (allocsize - 1) - (tmp - *buf),
					 "%lu:%s\n", grp->group_id,
					 grp->name);
				tmp += grp->name_length + 3;
			}
		}
		mutex_unlock(&work_mutex);
	} else {
		mutex_unlock(&work_mutex);
		allocsize *= 2;
		kfree(*buf);
		goto tryagain;
	}

	return 0;
}

/**
 * check_recursion - check if current process is recursing
 *
 * Description: A list of anonymous processes is managed to prevent
 * access event recursion. This function checks if the current process is
 * a part of that list.
 *
 * If the current process is found in the process list, it is removed.
 *
 * NOTE: The proc structure is not freed. It is only removed from the
 *       list. Since it is a recursive call, the caller can free the
 *       structure after the call chain is finished.
 *
 * Returns 0 if this is a recursive process call.
 */
static int check_recursion(void)
{
	struct dazukofs_proc *proc;
	struct list_head *pos;
	int found = 0;
	struct pid *cur_proc_id = get_pid(task_pid(current));

	mutex_lock(&proc_mutex);
	list_for_each(pos, &proc_list.list) {
		proc = list_entry(pos, struct dazukofs_proc, list);
		if (proc->proc_id == cur_proc_id) {
			found = 1;
			list_del(pos);
			proc->within_list = 0;
			put_pid(proc->proc_id);
			break;
		}
	}
	mutex_unlock(&proc_mutex);

	put_pid(cur_proc_id);

	/* process event if not found */
	return !found;
}

/**
 * event_assigned - check if event is (still) assigned
 * @event: event to check
 *
 * Description: This function checks if an event is still assigned. An
 * assigned event means that it is sitting on the todo or working list
 * of a group.
 *
 * Returns the number assigned count.
 */
static int event_assigned(struct dazukofs_event *event)
{
	int val;
	mutex_lock(&event->assigned_mutex);
	val = event->assigned;
	mutex_unlock(&event->assigned_mutex);
	return val;
}

/**
 * check_access_precheck - check if an access event should be generated
 * @grp_count: the current number of groups
 *
 * Description: Check if the current process should cause an access event
 * to be generated.
 *
 * Returns 0 if an access event should be generated.
 */
static int check_access_precheck(int grp_count)
{
	/* do we have any groups? */
	if (grp_count == 0)
		return -1;

	/* am I a recursion process? */
	if (!check_recursion())
		return -1;

	/* am I an ignored process? */
	if (!dazukofs_check_ignore_process())
		return -1;

	return 0;
}

/**
 * assign_event_to_groups - post an event to be processed
 * @evt: the event to be posted
 * @ec_array: the containers for the event
 *
 * Description: This function will assign a unique id to the event.
 * The event will be associated with each container and the container is
 * placed on each group's todo list. Each group will also be woken to
 * handle the new event.
 */
static void
assign_event_to_groups(struct dazukofs_event *evt,
		       struct dazukofs_event_container *ec_array[])
{
	struct dazukofs_group *grp;
	struct list_head *pos;
	int i;

	mutex_lock(&work_mutex);
	mutex_lock(&evt->assigned_mutex);

	/* assign the event a "unique" id */

	last_event_id++;
	evt->event_id = last_event_id;

	/* assign the event to each group */
	i = 0;
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated) {
			ec_array[i]->event = evt;

			evt->assigned++;
			list_add_tail(&ec_array[i]->list,
				      &grp->todo_list.list);

			/* notify someone to handle the event */
			wake_up(&grp->queue);
			wake_up(&grp->poll_queue);

			i++;
		}
	}

	mutex_unlock(&evt->assigned_mutex);
	mutex_unlock(&work_mutex);
}

/**
 * allocate_event_and_containers - allocate an event and event containers
 * @evt: event pointer to be assigned a new event
 * @ec: event container array to be filled with new array of containers
 * @grp_count: the number of groups (size of the array)
 *
 * Description: New event and event container structures are allocated
 * and initialized.
 *
 * Returns 0 on success.
 */
static int
allocate_event_and_containers(struct dazukofs_event **evt,
			      struct dazukofs_event_container *ec_array[],
			      int grp_count)
{
	int i;

	*evt = kmem_cache_zalloc(dazukofs_event_cachep, GFP_KERNEL);
	if (!*evt)
		return -1;
	init_waitqueue_head(&(*evt)->queue);
	mutex_init(&(*evt)->assigned_mutex);

	/* allocate containers now while we don't have a lock */
	for (i = 0; i < grp_count; i++) {
		ec_array[i] = kmem_cache_zalloc(
				dazukofs_event_container_cachep, GFP_KERNEL);
		if (!ec_array[i])
			goto error_out;
	}

	return 0;

error_out:
	for (i--; i >= 0; i--) {
		kmem_cache_free(dazukofs_event_container_cachep, ec_array[i]);
		ec_array[i] = NULL;
	}
	kmem_cache_free(dazukofs_event_cachep, *evt);
	*evt = NULL;
	return -1;
}

/**
 * dazukofs_check_access - check for allowed file access
 * @dentry: the dentry associated with the file access
 * @mnt: the vfsmount associated with the file access
 *
 * Description: This is the only function used by the stackable filesystem
 * layer to check if a file may be accessed.
 *
 * Returns 0 if the file access is allowed.
 */
int dazukofs_check_access(struct dentry *dentry, struct vfsmount *mnt)
{
	struct dazukofs_event_container *ec_array[GROUP_COUNT];
	struct dazukofs_event *evt;
	int err = 0;

	down_read(&group_count_sem);

	if (check_access_precheck(group_count)) {
		up_read(&group_count_sem);
		return 0;
	}

	/* at this point, the access should be handled */

	if (allocate_event_and_containers(&evt, ec_array, group_count)) {
		up_read(&group_count_sem);
		err = -ENOMEM;
		goto out;
	}

	evt->dentry = dget(dentry);
	evt->mnt = mntget(mnt);
	evt->proc_id = get_pid(task_pid(current));

	assign_event_to_groups(evt, ec_array);

	up_read(&group_count_sem);

	/* wait (uninterruptible) until event completely processed */
	wait_event(evt->queue, event_assigned(evt) == 0);

	if (evt->deny)
		err = -EPERM;

	release_event(evt, 0, 0);
out:
	return err;
}

/**
 * dazukofs_group_open_tracking - begin tracking this process
 * @group_id: id of the group we belong to
 *
 * Description: This function is called by the device layer to begin
 * tracking the current process (if tracking for that group is enabled).
 *
 * Tracking simply means to keep track if there are any processes still
 * registered with the group, so we use a simple counter for that.
 * dazukofs_group_release_tracking() must be called when this process
 * unregisters.
 *
 * Returns 0 if tracking is _not_ enabled.
 */
int dazukofs_group_open_tracking(unsigned long group_id)
{
	struct dazukofs_group *grp;
	struct list_head *pos;
	int tracking = 0;

	mutex_lock(&work_mutex);
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated && grp->group_id == group_id) {
			if (grp->tracking) {
				atomic_inc(&grp->use_count);
				grp->track_count++;
				tracking = 1;
			}
			break;
		}
	}
	mutex_unlock(&work_mutex);
	return tracking;
}

/**
 * dazukofs_group_release_tracking - stop tracking this process
 * @group_id: id of the group we belong to
 *
 * Description: This function is called by the device layer when a process
 * is no longer registered and thus tracking for this process should end
 * (if tracking for the group is enabled).
 */
void dazukofs_group_release_tracking(unsigned long group_id)
{
	struct dazukofs_group *grp;
	struct list_head *pos;

	mutex_lock(&work_mutex);
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated && grp->group_id == group_id) {
			if (grp->tracking) {
				atomic_dec(&grp->use_count);
				grp->track_count--;
				if (grp->track_count == 0)
					__remove_group(grp);
			}
			break;
		}
	}
	mutex_unlock(&work_mutex);
}

/**
 * unclaim_event - return an event to the todo list
 * @grp: group to which the event is assigned
 * @ec: event container of the event to be returned
 *
 * Description: This function removes the given event container from its
 * current list, puts it on the todo list, and wakes the group.
 */
static void unclaim_event(struct dazukofs_group *grp,
			  struct dazukofs_event_container *ec)
{
	/* put the event on the todo list */
	mutex_lock(&work_mutex);
	list_del(&ec->list);
	list_add(&ec->list, &grp->todo_list.list);
	mutex_unlock(&work_mutex);

	/* wake up someone else to handle the event */
	wake_up(&grp->queue);
	wake_up(&grp->poll_queue);
}

/**
 * dazukofs_return_event - return checked file access results
 * @group_id: id of the group the event came from
 * @event_id: the id of the event
 * @deny: a flag indicating if file access should be denied
 *
 * Description: This function is called by the device layer when returning
 * results from a checked file access event. If the event_id was valid, the
 * event container will be freed and the event released.
 *
 * Returns 0 on success.
 */
int dazukofs_return_event(unsigned long group_id, unsigned long event_id,
			  dazukofs_response_t response)
{
	struct dazukofs_group *grp;
	struct dazukofs_event_container *ec = NULL;
	struct dazukofs_event *evt = NULL;
	struct list_head *pos;
	int found = 0;
	int ret = 0;

	mutex_lock(&work_mutex);
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated && grp->group_id == group_id) {
			atomic_inc(&grp->use_count);
			found = 1;
			break;
		}
	}

	if (!found) {
		mutex_unlock(&work_mutex);
		return -EINVAL;
	}

	found = 0;
	list_for_each(pos, &grp->working_list.list) {
		ec = list_entry(pos, struct dazukofs_event_container, list);
		evt = ec->event;
		if (evt->event_id == event_id) {
			found = 1;
			if (response != REPOST) {
				list_del(pos);
				kmem_cache_free(
					dazukofs_event_container_cachep, ec);
			}
			break;
		}
	}
	mutex_unlock(&work_mutex);

	if (found) {
		if (response == REPOST)
			unclaim_event(grp, ec);
		else
			release_event(evt, 1, response == DENY);
	} else {
		ret = -EINVAL;
	}
	atomic_dec(&grp->use_count);

	return ret;
}

/**
 * claim_event - grab an event from the todo list
 * @grp: the group
 *
 * Description: Take the first event from the todo list and move it to the
 * working list. The event is then returned to its called for processing.
 *
 * Returns the claimed event.
 */
static struct dazukofs_event_container *claim_event(struct dazukofs_group *grp)
{
	struct dazukofs_event_container *ec = NULL;

	/* move first todo-item to working list */
	mutex_lock(&work_mutex);
	if (!list_empty(&grp->todo_list.list)) {
		ec = list_first_entry(&grp->todo_list.list,
				      struct dazukofs_event_container, list);
		list_del(&ec->list);
		list_add(&ec->list, &grp->working_list.list);
	}
	mutex_unlock(&work_mutex);

	return ec;
}

/**
 * mask_proc - mask the current process
 * @proc: process structure to use for the list
 *
 * Description: Assign the current process to the provided proc structure
 * and add the structure to the list. The list is used to prevent
 * generating recursive file access events. The process is removed from
 * the list with the check_recursion() function.
 */
static void mask_proc(struct dazukofs_proc *proc)
{
	proc->proc_id = get_pid(task_pid(current));
	mutex_lock(&proc_mutex);
	list_add(&proc->list, &proc_list.list);
	proc->within_list = 1;
	mutex_unlock(&proc_mutex);
}

/**
 * open_file - open a file for the current process (avoiding recursion)
 * @ec: event container to store opened file descriptor
 *
 * Description: This function will open a file using the information within
 * the provided event container. The calling process will be temporarily
 * masked so that the file open does not generate a file access event.
 *
 * Returns 0 on success.
 */
static int open_file(struct dazukofs_event_container *ec)
{
	struct dazukofs_event *evt = ec->event;
	struct dazukofs_proc proc;
	int ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
	struct path path;
#endif

	/* open the file read-only */

	ec->fd = get_unused_fd();
	if (ec->fd < 0) {
		ret = ec->fd;
		goto error_out1;
	}

	/* add self to be ignored on file open (to avoid recursion) */
	mask_proc(&proc);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0)
	path.dentry = dget(evt->dentry);
	path.mnt = mntget(evt->mnt);
	
	ec->file = dentry_open(&path, O_RDONLY | O_LARGEFILE, current_cred());
#else
	ec->file = dentry_open(dget(evt->dentry), mntget(evt->mnt),
			       O_RDONLY | O_LARGEFILE, current_cred());
#endif

	/* If dentry_open() was successful, it should have removed us from
	 * proc_list. If it didn't do this, we do it now ourselves. */
	if (proc.within_list)
		check_recursion();

	if (IS_ERR(ec->file)) {
		ret = PTR_ERR(ec->file);
		goto error_out2;
	}

	fd_install(ec->fd, ec->file);

	return 0;

error_out2:
	put_unused_fd(ec->fd);
error_out1:
	return ret;
}

/**
 * is_event_available - check if an event is available for processing
 * @grp: the group
 *
 * Description: This function simply checks if there are any events posted
 * in the group's todo list.
 *
 * Returns 0 if there are no events in the todo list.
 */
static int is_event_available(struct dazukofs_group *grp)
{
	int ret = 0;

	mutex_lock(&work_mutex);
	if (!list_empty(&grp->todo_list.list))
		ret = 1;
	mutex_unlock(&work_mutex);

	return ret;
}

/**
 * dazukofs_poll - poll for an available event
 * @group_id: id of the group we belong to
 * @dev_file: the device file being polled
 * @wait: the poll table
 * @mask: to be filled the poll results
 *
 * Description: This function is called by the device layer to get poll
 * for an available file access event. It waits until an event has been
 * posted in the todo list.
 *
 * Returns the poll result.
 */
unsigned int dazukofs_poll(unsigned long group_id, struct file *dev_file,
			   poll_table *wait)
{
	struct dazukofs_group *grp = NULL;
	struct list_head *pos;
	unsigned int mask = 0;
	int found = 0;

	mutex_lock(&work_mutex);
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated && grp->group_id == group_id) {
			atomic_inc(&grp->use_count);
			found = 1;
			break;
		}
	}
	mutex_unlock(&work_mutex);

	if (!found)
		return POLLERR;

	poll_wait(dev_file, &grp->poll_queue, wait);
	if (is_event_available(grp))
		mask = POLLIN | POLLRDNORM;

	atomic_dec(&grp->use_count);

	return mask;
}

/**
 * dazukofs_get_event - get an event to process
 * @group_id: id of the group we belong to
 * @event_id: to be filled in with the new event id
 * @fd: to be filled in with the opened file descriptor
 * @pid: to be filled in with the pid of the process generating the event
 *
 * Description: This function is called by the device layer to get a new
 * file access event to process. It waits until an event has been
 * posted in the todo list (and is successfully claimed by this process).
 *
 * Returns 0 on success.
 */
int dazukofs_get_event(unsigned long group_id, unsigned long *event_id,
		       int *fd, pid_t *pid)
{
	struct dazukofs_group *grp = NULL;
	struct dazukofs_event_container *ec;
	struct list_head *pos;
	int found = 0;
	int ret = 0;

	mutex_lock(&work_mutex);
	list_for_each(pos, &group_list.list) {
		grp = list_entry(pos, struct dazukofs_group, list);
		if (!grp->deprecated && grp->group_id == group_id) {
			atomic_inc(&grp->use_count);
			found = 1;
			break;
		}
	}
	mutex_unlock(&work_mutex);

	if (!found)
		return -EINVAL;

	while (1) {
		ret = wait_event_freezable(grp->queue,
					   is_event_available(grp) ||
					   grp->deprecated);
		if (ret != 0)
			break;

		if (grp->deprecated) {
			ret = -EINVAL;
			break;
		}

		ec = claim_event(grp);
		if (ec) {
			ret = open_file(ec);
			if (ret == 0) {
				*event_id = ec->event->event_id;
				*fd = ec->fd;

				/* set to 0 if not within namespace */
				*pid = pid_vnr(ec->event->proc_id);
				break;
			} else {
				unclaim_event(grp, ec);
				/* The registered process was not allowed
				 * to open the file! */
				break;
			}
		}
	}
	atomic_dec(&grp->use_count);

	return ret;
}
