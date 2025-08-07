#ifndef CXLSHM_HANDLER_H   /* Include guard */
#define CXLSHM_HANDLER_H

#include "../../drivers/dax/dax-private.h"
#include <linux/dax.h>

#define MAX_TIMEOUT_MSEC		3000
#define FILE_PATH_LENGTH        32
#define MAX(a, b) ((a) >= (b) ? (a) : (b))

struct ownership { //TODO: add version to the struct...
	pid_t owner_pid;
	char ip_4_addr[17];
    int port;
	pfn_t start;
	pfn_t end;
	unsigned long vm_start;
	unsigned long vm_end;
};

struct mem_alloc {
	pid_t owner_pid;
	char ip_4_addr[17];
    int port;
	struct dax_device *cxl_dax_dev;
};

//cxlshm_dax_handler
int get_cxl_dax_dev(char *device_path_param);
int alloc_mem_on_devdax(char *device_path_param, unsigned long len, void **dax_kaddr, pfn_t *dax_pfn);
int get_cxl_dax_device(char *device_path_param);
vm_fault_t handle_fault_on_cxldaxdev(struct vm_fault *vmf);
int is_allottable(pid_t requestor_pid);
int ask_for_permission(pid_t existing_owner, pid_t requestor_pid, char *address, int port);
void get_current_device_path(char **dev_path);
int get_owner_info_on_mem(struct ownership **owner);
int set_owner_info_on_mem(struct ownership *owner);
vm_fault_t handle_fault_on_cxldaxdev_prot(struct vm_fault *vmf, pgprot_t pgprot);
vm_fault_t handle_fault_on_cxldaxdev_mkwrite(struct vm_fault *vmf);


#endif