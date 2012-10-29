#
# Makefile for the dazukofs-filesystem routines.
#

DAZUKOFS_KERNEL_SRC     = /lib/modules/`uname -r`/build
DAZUKOFS_KERNEL_INSTALL = /lib/modules/`uname -r`/kernel/fs/dazukofs

obj-m += dazukofs.o

dazukofs-objs := super.o inode.o file.o dentry.o mmap.o group_dev.o ign_dev.o ctrl_dev.o dev.o event.o

dazukofs_modules:
	make -C $(DAZUKOFS_KERNEL_SRC) SUBDIRS=$(PWD) modules

dazukofs_install: dazukofs_modules
	mkdir -p $(DAZUKOFS_KERNEL_INSTALL)
	cp dazukofs.ko $(DAZUKOFS_KERNEL_INSTALL)
	/sbin/depmod -a

clean:
	rm -f Module.symvers
	make -C $(DAZUKOFS_KERNEL_SRC) SUBDIRS=$(PWD) clean

.PHONY: dazukofs_modules dazukofs_install clean
