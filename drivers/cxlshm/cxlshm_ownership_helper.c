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

#define THIS_MOD "cxlshm_ownership_helper: "

int change_ownership(pid_t current_owner, pid_t requestor);

int change_ownership(pid_t current_owner, pid_t requestor) {
    int ret = 0;
    pr_info(THIS_MOD "current owner pid: %d, requestor pid: %d\n", current_owner, requestor);
    if (current_owner == requestor) {
        pr_info(THIS_MOD "same pid. Do nothing and continue\n");
        ret = 1;
    } else {
        pr_info(THIS_MOD "different pid. takeover ownership\n");
        char pid_to_send[16] = {0};
		//snprintf(pid_to_send, 15, "PID:%d", get_owner_pid_on_mem());
		snprintf(pid_to_send, 15, "PID:%d", requestor);
        pr_info(THIS_MOD "message: %s\n", pid_to_send);
        int dest_port = 0;
        char *dest_ip_4_addr = kzalloc(sizeof(char) * 16, GFP_KERNEL);
        memset(dest_ip_4_addr, 0, sizeof(char) * 16);
        get_dest_host(&dest_ip_4_addr, &dest_port);
        pr_info(THIS_MOD "%s %d\n", dest_ip_4_addr, dest_port);
		//tcp_client_start_d((char *)owner_on_mem->ip_4_addr, owner_on_mem->port);
        tcp_client_start_d("127.0.0.1", 57580);
		send_message_d(pid_to_send);
		char received_copy[MAX_BUFFER_NET] = {0};
		unsigned long timeout = msecs_to_jiffies(MAX_TIMEOUT_MSEC);
		long completion_ret_val = wait_for_completion_interruptible_timeout(&is_complete, timeout);
		if (completion_ret_val > 0) {
			spin_lock(&ctr_lock);
			strscpy(received_copy, message_received, sizeof(message_received));
			memset(message_received, 0, sizeof(message_received));
			spin_unlock(&ctr_lock);
			if (strncmp(received_copy, "DONE", 4) == 0) {
				ret = 1;
			} else {
				pr_info(THIS_MOD "not a completion message, maybe handled later\n");
				return -EINVAL;
			}
			tcp_client_stop_d();
		} else if (completion_ret_val == 0) {
			pr_info(THIS_MOD "timeout occured\n");
			return -EAGAIN;
		} else {
			pr_info(THIS_MOD "interrupted\n");
			return -EAGAIN;
		}
    }
    return ret;
}
EXPORT_SYMBOL(change_ownership);