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
#include <linux/syscalls.h>

#include <linux/cxlshm_msg.h>
#include "../../drivers/dax/dax-private.h"
#include "../../kernel/cxlshm_msg/conn_manager.h"
#include "cxlshm_handler_private.h"

#define THIS_MOD "cxlshm_ownership_handler: "

char this_host[MAX_LOCATION_LENGTH] = {0};
EXPORT_SYMBOL(this_host);


int change_ownership(pid_t current_owner, pid_t requestor);
void set_host(char *host_id_string);
int is_allottable(pid_t requestor_pid, struct vm_fault *vmf, int type);
void set_new_ownership(struct ownership **new_owner, char *address, int port);
int ask_for_permission_page(pid_t existing_owner, pid_t requestor_pid, char *address, int port, struct vm_fault *vmf);
void get_location_info(char **location_info_string);

int is_allottable(pid_t requestor_pid, struct vm_fault *vmf, int type) 
{
	struct ownership *owner;
	pr_info(THIS_MOD "is_allottable function here %d\n", requestor_pid);
	get_owner_info_on_mem(&owner);
	/*ownership checking here, also ask for permission if needed*/
	if (owner->owner_pid <= 0)
	{
		pr_info(THIS_MOD "nobody owns this area\n");
		set_new_owner_info(&owner, task_pid_nr(current), vmf->vma->vm_start, vmf->vma->vm_end, vmf->pgoff);
		set_owner_info_on_mem(owner);
		kfree(owner);
		return requestor_pid;
	}
	else
	{
		pr_info(THIS_MOD "owner on memory: %d. Requestor pid: %d\n", owner->owner_pid, requestor_pid);
		if (owner->owner_pid == requestor_pid)
		{
			/* owned by itself */
			pr_info(THIS_MOD "owned by the caller\n");
			return requestor_pid;
		}
		else
		{
			pr_info(THIS_MOD "owned by the other process, possibly in other host\n");
			int ownership_transfer_status = send_invalidation_message(owner, WHOLE_VMA);
			if(ownership_transfer_status == 1)
			{
				set_new_owner_info(&owner, task_pid_nr(current), vmf->vma->vm_start, vmf->vma->vm_end, vmf->pgoff);
				set_owner_info_on_mem(owner);
				kfree(owner);
			}
			return requestor_pid;
		}
	}
}
EXPORT_SYMBOL(is_allottable);

void set_host(char *host_id_string) 
{
	strscpy(this_host, host_id_string, sizeof(this_host));
}
EXPORT_SYMBOL(set_host);

void get_location_info(char **location_info_string) 
{
	*location_info_string = (char *)kzalloc(sizeof(char) * MAX_LOCATION_LENGTH, GFP_KERNEL);
	if (*location_info_string != NULL)
		strscpy(*location_info_string, this_host, sizeof(this_host));
}
EXPORT_SYMBOL(get_location_info);

/*syscall to set host id. format: IP_V4:PORT*/
SYSCALL_DEFINE1(set_process_identification, char __user *, process_id)
{
	char message_buf[24] = {0};
	int ret = strncpy_from_user(message_buf, process_id, sizeof(message_buf));
	if (ret < 0) return -EFAULT;
	if (ret >= sizeof(message_buf) || ret == 0) return -EINVAL;
	set_host(message_buf);
	
	return 0;
}