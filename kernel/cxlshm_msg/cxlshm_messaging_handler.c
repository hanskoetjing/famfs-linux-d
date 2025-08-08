
#include <linux/module.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/sprintf.h>
#include <linux/completion.h>
#include <vdso/limits.h>
#include <asm-generic/cacheflush.h>
#include <linux/pid.h>
#include <linux/pid_types.h>
#include <linux/kthread.h>
#include <linux/syscalls.h>
#include "conn_manager.h"
#include "../../mm/cxlshm/cxlshm_handler_private.h"

#define THIS_MOD "cxlshm_messaging_handler: "

const char * const enum_message_value[] = {"PAGE", "WHOLE_VMA", "DONE"};

int send_invalidation_message(struct ownership *owner, int type);

/*return 1 on ownership taken over, otherwise error*/
int send_invalidation_message(struct ownership *owner, int type) 
{
	int ret = 0;
	ret = _tcp_client_start(owner->ip_4_addr, owner->port);
	if (ret < 0) 
	{
		pr_info(THIS_MOD "can't connect to server %s %d\n", owner->ip_4_addr, owner->port);
		return 1;
	}
	else
	{
		char message_to_send[16] = {0};
		strscpy(message_to_send, enum_message_value[type], strlen(enum_message_value[type]) + 1);
		pr_info(THIS_MOD "message: %s\n", message_to_send);
		_send_message(message_to_send);
		char received_copy[MAX_BUFFER_NET] = {0};
		unsigned long timeout = msecs_to_jiffies(MAX_TIMEOUT_MSEC);
		long completion_ret_val = wait_for_completion_interruptible_timeout(&is_complete, timeout);
		if (completion_ret_val > 0) 
		{
			spin_lock(&client_lock);
			strscpy(received_copy, response_received, sizeof(response_received));
			memset(response_received, 0, sizeof(response_received));
			spin_unlock(&client_lock);
			if (strncmp(received_copy, "DONE", 4) == 0) {
				pr_info(THIS_MOD "ownership transfer completed\n");
				return 1;
			}
			else
			{
				pr_info(THIS_MOD "not a completion message, maybe handled later\n");
				return -EINVAL;
			}
		} 
		else if (completion_ret_val == 0) 
		{
			pr_info(THIS_MOD "timeout waiting for response\n");
			return 1;
		} 
		else 
		{
			pr_info(THIS_MOD "interrupted\n");
			return -EAGAIN;
		}
	}
	_tcp_client_stop();
}
EXPORT_SYMBOL(send_invalidation_message);

int get_message_type(char *message_str)
{
	int i = 0;
	for (; i < SENTINEL_ONLY; i++) 
	{
		if(strncmp(message_str, enum_message_value[i], strlen(enum_message_value[i])) == 0)
			break;
	}

	if (i == SENTINEL_ONLY) 
		return -EINVAL;
	else
		return i;
}
EXPORT_SYMBOL(get_message_type);
