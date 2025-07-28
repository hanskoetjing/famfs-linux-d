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
#include <vdso/limits.h>
#include <asm-generic/cacheflush.h>
#include <linux/rcupdate.h>
#include <linux/pid.h>
#include <linux/pid_types.h>
#include <linux/kthread.h>

#include <linux/cxlshm_msg.h>
#include "dax-private.h"
#include "conn_manager.h"
#include "cxlshm-private.h"

#define THIS_MOD "Mem Area Invalidator: "

static struct task_struct *invalidator_thread;

int invalidate_mem_area(void *data);
struct task_struct *get_task_from_int_pid(pid_t pid);
int flush_mem_task(pid_t pid);

int invalidate_mem_area(void *data) {
    int ret = 0;
    char received_copy[MAX_BUFFER_NET] = {0};
    unsigned long timeout = msecs_to_jiffies(MAX_TIMEOUT_MSEC);

    while(!kthread_should_stop() && counter < 5) {
        long completion_ret_val = wait_for_completion_interruptible_timeout(&ownership_transfer_arrived, timeout);
        if (completion_ret_val > 0) {
            spin_lock(&ctr_lock);
            strscpy(received_copy, ownership_transfer_message, sizeof(received_copy));
            memset(ownership_transfer_message, 0, sizeof(ownership_transfer_message));
            spin_unlock(&ctr_lock);
            if (strncmp(received_copy, "PID:", 4) == 0) {
                send_message("DONE");
            } else {
                pr_info(THIS_MOD "not an ownership transfer message, maybe handled later\n");
            }
            counter = 0;
        } else if (completion_ret_val == 0) {
            //pr_info(THIS_MOD "timeout occured. retrying\n");
            //counter++;
        } else {
            pr_info(THIS_MOD "interrupted\n");
            return -EINTR;
        }
    }

    return 0;
}

struct task_struct *get_task_from_int_pid(pid_t pid) {
	struct pid *the_pid = find_get_pid(pid);
	return get_pid_task(the_pid, PIDTYPE_PID);
}


int flush_mem_task(pid_t pid) {
	int ret = 0;
	struct vm_area_struct *this_vma = NULL;
    struct task_struct *the_task;
    volatile struct ownership *owner_on_mem;
	if (pid != -1) {
		the_task = get_task_from_int_pid(pid);
		struct mm_struct *mm = the_task->mm;
		struct vm_area_struct *vma;
		MA_STATE(mas, &mm->mm_mt, 0, 0);
		get_cxl_device();
		pid_t pidd = get_owner_on_mem(&owner_on_mem);
        //temporary set to 0 since this func wont be called for now
		unsigned long vm_from_mem = 0;
		pr_info(THIS_MOD "pid %d vm_start: 0x%lx\n", 0, 0);
		int i = 0;
		mas_for_each(&mas, vma, ULONG_MAX) {
			pr_info("vma %d addr: 0x%lx\n", i, vma->vm_start);
			if (vma->vm_start == vm_from_mem) {
				this_vma = vma; 
				flush_cache_range(this_vma, this_vma->vm_start, this_vma->vm_end);
				zap_vma_ptes(this_vma, this_vma->vm_start, this_vma->vm_end - this_vma->vm_start); //temporary
				break;
			}
			i++;
		}
		if (this_vma) {
			pr_info(THIS_MOD "Flush CPU cache. Size: %ld\n", this_vma->vm_end - this_vma->vm_start);
		} else {
			pr_info(THIS_MOD "VMA not found\n");
		}
		//send_one_message(dest_ip_4_addr, dest_port, "DONE");
	} else {
		ret = -1;
	}
	return ret;
}

static int __init cxlshm_invalidator_init(void) {	
    void *data;
    invalidator_thread = kthread_run(invalidate_mem_area, (void *)data, "invalidate_mem_area");
	//init done
	pr_info(THIS_MOD ": loaded\n");
	return 0;
}

static void __exit cxlshm_invalidator_exit(void) {
    if (task_is_running(invalidator_thread) || invalidator_thread->__state == TASK_NORMAL) {
        kthread_stop(invalidator_thread);
    }
	//exit done
	pr_info(THIS_MOD ": unloaded\n"); 
}


module_init(cxlshm_invalidator_init);
module_exit(cxlshm_invalidator_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CXL shared memory area invalidator");
