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

char this_host[24] = {0};


int change_ownership(pid_t current_owner, pid_t requestor);
void set_host(char *host_id_string);
int is_allottable(pid_t requestor_pid);
int ask_for_permission(pid_t existing_owner, pid_t requestor_pid, char *address, int port);
void set_new_ownership(struct ownership **new_owner, char *address, int port);

int is_allottable(pid_t requestor_pid) {
	struct ownership *owner;
	pr_info(THIS_MOD "is_allottable function here %d\n", requestor_pid);
	get_owner_info_on_mem(&owner);
	char address[16] = {0};
	int port = 0;
	/*set host identifier on ownership data*/
	if (strlen(this_host) == 0)
	{
		strscpy(address, "127.0.0.1", 16);
		port = 57580;
	}
	else
	{
		char *temp_hostid_processing = (char *)kzalloc(sizeof(char) * strlen(this_host) + 1, GFP_KERNEL);
		
		strscpy(temp_hostid_processing, this_host, strlen(this_host) + 1);
		char *ip_4_addr_from_user = strsep(&temp_hostid_processing, ":");
		if (ip_4_addr_from_user != NULL) 
		{
			int port_from_user = 0;
			int strtoint_ret = kstrtoint(temp_hostid_processing, 10, &port_from_user);
			if (strtoint_ret >= 0) 
			{
				strscpy(address, ip_4_addr_from_user, sizeof(address));
				port = port_from_user;
			}
		}
		else
		{
			strscpy(address, "127.0.0.1", 16);
			port = 57580;
		}
	}
	pr_info(THIS_MOD "host id: %s %d\n", address, port);
	/*ownership checking here, also ask for permission if needed*/
	if (owner->owner_pid <= 0)
	{
		pr_info(THIS_MOD "nobody owns this area\n");
		owner->owner_pid = task_pid_nr(current);
		strscpy(owner->ip_4_addr, address, 16);
		owner->port = port;
		set_owner_info_on_mem(owner);
		return requestor_pid;
	}
	else
	{
		pr_info(THIS_MOD "owner on memory: %d. Requestor pid: %d\n", owner->owner_pid, requestor_pid);
		if (owner->owner_pid == requestor_pid && owner->port == port && strncmp(owner->ip_4_addr, address, MAX(strlen(owner->ip_4_addr), strlen(address))) == 0)
		{
			/* owned by itself */
			pr_info(THIS_MOD "owned by the caller\n");
			return requestor_pid;
		}
		else
		{
			/*to be checked later, but return 1 for now*/
			pr_info(THIS_MOD "owned by the other process, possibly in other host\n");
			return ask_for_permission(owner->owner_pid, requestor_pid, address, port);
		}
	}
}
EXPORT_SYMBOL(is_allottable);

void set_new_ownership(struct ownership **new_owner, char *address, int port) 
{
	(*new_owner)->owner_pid = task_pid_nr(current);
	strscpy((*new_owner)->ip_4_addr, address, strlen(address) + 1);
	(*new_owner)->port = port;
	set_owner_info_on_mem((*new_owner));
}

int ask_for_permission(pid_t existing_owner, pid_t requestor_pid, char *address, int port) {
	pr_info(THIS_MOD "ask_for_permission function here %d\n", requestor_pid);
	struct ownership *owner;
	int ret = 0;
	/*the messaging part will go here, but just return 1 for now */
	if (existing_owner <= 0)
	{
		pr_info(THIS_MOD "set ownership on memory to %d\n", requestor_pid);
		get_owner_info_on_mem(&owner);
		set_new_ownership(&owner, address, port);
		return requestor_pid;
	}
	else if (existing_owner > 0)
	{
		pr_info(THIS_MOD "invalidate vma of %d\n", existing_owner);
		get_owner_info_on_mem(&owner);
		char pid_to_send[16] = {0};
		snprintf(pid_to_send, 15, "PID:%d", owner->owner_pid);
        pr_info(THIS_MOD "message: %s\n", pid_to_send);

		ret = _tcp_client_start(owner->ip_4_addr, owner->port);
		if (ret < 0) 
		{
			pr_info(THIS_MOD "can't connect to server %s %d\n", owner->ip_4_addr, owner->port);
			set_new_ownership(&owner, address, port);//if can't connect, just take over
		}
		else
		{
			_send_message(pid_to_send);
			char received_copy[MAX_BUFFER_NET] = {0};
			unsigned long timeout = msecs_to_jiffies(MAX_TIMEOUT_MSEC);
			long completion_ret_val = wait_for_completion_interruptible_timeout(&is_complete, timeout);
			if (completion_ret_val > 0) {
				spin_lock(&client_lock);
				strscpy(received_copy, response_received, sizeof(response_received));
				memset(response_received, 0, sizeof(response_received));
				spin_unlock(&client_lock);
				if (strncmp(received_copy, "DONE", 4) == 0) {
					pr_info(THIS_MOD "ownership transfer completed\n");
					set_new_ownership(&owner, address, port);
					ret = requestor_pid;
				} else {
					pr_info(THIS_MOD "not a completion message, maybe handled later\n");
					return -EINVAL;
				}
			} else if (completion_ret_val == 0) {
				pr_info(THIS_MOD "timeout waiting for response\n");
				//automatically take over ownership if timeout occurred
				set_new_ownership(&owner, address, port);
				ret = 1;
			} else {
				pr_info(THIS_MOD "interrupted\n");
				ret = -EAGAIN;
			}
		}
		_tcp_client_stop();
		return requestor_pid;
	}
	else
	{
		return -EINVAL;
	}
	return ret;
}
EXPORT_SYMBOL(ask_for_permission);

void set_host(char *host_id_string) {
	strscpy(this_host, host_id_string, sizeof(this_host));
}
EXPORT_SYMBOL(set_host);

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