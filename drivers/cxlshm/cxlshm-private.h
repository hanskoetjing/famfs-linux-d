#ifndef CXLSHM_H   /* Include guard */
#define CXLSHM_H

#define MAX_TIMEOUT_MSEC		3000

struct ownership { //TODO: add version to the struct...
	pid_t owner_pid;
	char ip_4_addr[17];
    int port;
	pfn_t start;
	pfn_t end;
	unsigned long vm_start;
	unsigned long vm_end;
};

pid_t get_owner_on_mem(struct ownership **owner_on_mem);
int is_owner(pid_t pid);
int get_cxl_device(void);

#endif