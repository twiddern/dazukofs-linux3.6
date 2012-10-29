#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/user.h>
#include <linux/security.h>
#include <linux/unistd.h>
#include <linux/notifier.h>
#include <linux/version.h>

static void __exit cleanup(void)
{
}


static int __init startup(void)
{
}

module_init(startup);
module_exit(cleanup);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Someone Like You");

