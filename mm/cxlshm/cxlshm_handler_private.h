#ifndef CXLSHM_HANDLER_H   /* Include guard */
#define CXLSHM_HANDLER_H

#include "../../drivers/dax/dax-private.h"
#include <linux/dax.h>

#define MAX_TIMEOUT_MSEC		3000
#define FILE_PATH_LENGTH        32
#define MAX_LOCATION_LENGTH        32
#define MAX(a, b) ((a) >= (b) ? (a) : (b))

struct ownership { //TODO: add version to the struct...
	pid_t owner_pid;
	char ip_4_addr[17];
    int port;
	unsigned long vm_start;
	unsigned long vm_end;
	unsigned long offset;
};


enum message_type {
	PAGE,
	WHOLE_VMA,
	DONE,
	SENTINEL_ONLY
};

extern const char * const enum_message_value[];
extern char this_host[MAX_LOCATION_LENGTH];

//cxlshm_dax_handler
int get_cxl_dax_dev(char *device_path_param);
int alloc_mem_on_devdax(char *device_path_param, unsigned long len, void **dax_kaddr, pfn_t *dax_pfn);
int get_cxl_dax_device(char *device_path_param);
void get_current_device_path(char **dev_path);
int get_owner_info_on_mem(struct ownership **owner);
int set_owner_info_on_mem(struct ownership *owner);
void set_new_owner_info(struct ownership **owner, pid_t pid, unsigned long vm_start, unsigned long vm_end, unsigned long offset);
pfn_t get_pfn_by_offset(unsigned long offset);
vm_fault_t handle_fault_on_cxldaxdev_prot(struct vm_fault *vmf, pgprot_t pgprot);
vm_fault_t handle_fault_on_cxldaxdev_mkwrite(struct vm_fault *vmf);


//ownership handler
int is_allottable(pid_t requestor_pid, struct vm_fault *vmf);
int ask_for_permission(pid_t existing_owner, pid_t requestor_pid, char *address, int port);
int is_allottable_page(pid_t requestor_pid, struct vm_fault *vmf);
void get_location_info(char **location_info_string);


#endif