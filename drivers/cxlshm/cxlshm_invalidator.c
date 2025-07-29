#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/rcupdate.h>
#include <linux/sprintf.h>
#include <linux/completion.h>
#include <vdso/limits.h>
#include <asm-generic/cacheflush.h>
#include <linux/pid.h>
#include <linux/pid_types.h>
#include <linux/kthread.h>

#include <linux/cxlshm_msg.h>
#include "dax-private.h"
#include "conn_manager.h"
#include "cxlshm-private.h"

#define THIS_MOD "Mem Area Invalidator: "

static struct task_struct *invalidator_thread;
DECLARE_COMPLETION(ownership_transfer_arrived);

int invalidate_mem_area(void *data);
struct task_struct *get_task_from_int_pid(pid_t pid);
int flush_mem_task(pid_t pid);

int invalidate_mem_area(void *data) {
    int ret = 0;
    char *received_copy = kzalloc(MAX_BUFFER_NET * sizeof(char), GFP_NOWAIT); //using nowait as this is IO
    memset(received_copy, 0, MAX_BUFFER_NET * sizeof(char));
    while(!kthread_should_stop()) {
        long completion_ret_val = wait_for_completion_interruptible(&ownership_transfer_arrived);
        if (completion_ret_val >= 0) {
            spin_lock(&ctr_lock);
            strscpy(received_copy, ownership_transfer_message, sizeof(received_copy));
            memset(ownership_transfer_message, 0, sizeof(ownership_transfer_message));
            spin_unlock(&ctr_lock);
            if (strncmp(received_copy, "PID:", 4) == 0) {
                strsep(&received_copy, ":");
                pid_t pid_received = 0;
                int ret = kstrtoint(received_copy, 10, &pid_received);
                if (ret >= 0) {
                    flush_mem_task(pid_received);
                } else {
                    pr_info(THIS_MOD "failed to process PID: %s, returned: %d\n", received_copy, ret);
                }
                ret = send_message("DONE");
                pr_info(THIS_MOD "bytes send %d\n", ret);
            }
        } else {
            pr_info(THIS_MOD "interrupted\n");
            return -EINTR;
        }
    }
    pr_info(THIS_MOD "thread returns\n");
    return ret;
}

struct task_struct *get_task_from_int_pid(pid_t pid) {
	struct pid *the_pid = find_get_pid(pid);
	if (the_pid != NULL)
        return get_pid_task(the_pid, PIDTYPE_PID);
    else 
        return NULL;
}


int flush_mem_task(pid_t pid) {
	int ret = 0;
	struct vm_area_struct *this_vma = NULL;
    struct task_struct *the_task = NULL;
    volatile struct ownership *owner_on_mem;
	if (pid != -1) {
		the_task = get_task_from_int_pid(pid);
		if (the_task != NULL) {
            struct mm_struct *mm = the_task->mm;
            if (mm == NULL) return -1;
            struct vm_area_struct *vma;
            MA_STATE(mas, &mm->mm_mt, 0, 0);
            get_cxl_device();
            pid_t pid_on_mem = get_owner_on_mem(&owner_on_mem);
            unsigned long vm_from_mem = owner_on_mem->vm_start;
            pr_info(THIS_MOD "pid %d vm_start: 0x%lx\n", pid_on_mem, owner_on_mem->vm_start);
            mas_for_each(&mas, vma, ULONG_MAX) {
                if (vma->vm_start == vm_from_mem) {
                    this_vma = vma;
                    pr_info(THIS_MOD "found vma addr: 0x%lx\n", vma->vm_start);
                    flush_cache_range(this_vma, this_vma->vm_start, this_vma->vm_end);
                    zap_vma_ptes(this_vma, this_vma->vm_start, this_vma->vm_end - this_vma->vm_start); //temporary
                    break;
                }
            }
            if (this_vma) {
                pr_info(THIS_MOD "Flush CPU cache. Size: %ld\n", this_vma->vm_end - this_vma->vm_start);
            } else {
                pr_info(THIS_MOD "VMA not found\n");
                ret = -1;
            }
        } else {
            pr_info(THIS_MOD "task not found\n");
            ret = -1;
        }
	} else {
		ret = -1;
	}
	return ret;
}

static int __init cxlshm_invalidator_init(void) {	
    void *data = NULL;
    set_ownership_completion(&ownership_transfer_arrived);
    invalidator_thread = kthread_run(invalidate_mem_area, (void *)data, "invalidate_mem_area");
	//init done
	pr_info(THIS_MOD "loaded\n");
	return 0;
}

static void __exit cxlshm_invalidator_exit(void) {
    set_ownership_completion(NULL);
    if (task_is_running(invalidator_thread)) {
        pr_info(THIS_MOD "stop invalidator thread\n"); 
        kthread_stop(invalidator_thread);
    }
	//exit done
	pr_info(THIS_MOD ": unloaded\n"); 
}


module_init(cxlshm_invalidator_init);
module_exit(cxlshm_invalidator_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CXL shared memory area invalidator");
