#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/ioport.h>
#include <asm-generic/cacheflush.h>
#include <linux/rcupdate.h>
#include <linux/sprintf.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/completion.h>

#include <linux/cxlshm_msg.h>
#include "dax-private.h"
#include "conn_manager.h"
#include "cxlshm-private.h"

static int __init cxlshm_invalidator_init(void) {	

	//init done
	pr_info("cxlshm_invalidator: loaded\n");
	return 0;
}

static void __exit cxlshm_invalidator_exit(void) {




	//exit done
	pr_info("cxlshm_invalidator: unloaded\n"); 
}


module_init(cxlshm_invalidator_init);
module_exit(cxlshm_invalidator_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CXL shared memory area invalidator");
