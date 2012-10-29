/* simple program using dazukofs

   Copyright (C) 2008 John Ogness 
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

#include "dazukofs.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

static int running = 1;

static void print_access(struct dazukofs_access *acc)
{
	char filename[1024];

	if (dazukofs_get_filename(acc, filename, sizeof(filename)) > 0) {
		printf("pid:%05lu file:%s\n", acc->pid, filename);
	} else {
		fprintf(stderr, "dazukofs_get_filename() failed: %s\n",
			strerror(errno));
		printf("pid:%05lu file:???\n", acc->pid);
	}
}

static void sigterm(int sig)
{
	running = 0;
	signal(sig, sigterm);
}

int main(void)
{
	dazukofs_handle_t hndl;
	struct dazukofs_access acc;

	signal(SIGHUP, sigterm);
	signal(SIGINT, sigterm);
	signal(SIGTERM, sigterm);

	hndl = dazukofs_open("dazukofs_example", DAZUKOFS_TRACK_GROUP);
	if (!hndl) {
		fprintf(stderr, "dazukofs_open() failed: %s\n",
			strerror(errno));
		return -1;
	}

	while (running) {
		if (dazukofs_get_access(hndl, &acc) != 0) {
			if (running) {
				fprintf(stderr,
					"dazukofs_get_access() failed: %s\n",
					strerror(errno));
			}
			break;
		}

		print_access(&acc);

		if (dazukofs_return_access(hndl, &acc) != 0) {
			if (running) {
				fprintf(stderr,
					"dazukofs_return_access() failed: %s\n",
					strerror(errno));
			}
			break;
		}
	}

	if (dazukofs_close(hndl, DAZUKOFS_REMOVE_GROUP) != 0) {
		fprintf(stderr, "dazukofs_close() failed: %s\n",
			strerror(errno));
	}

	printf("\nGoodbye.\n");

	return 0;
}
