#ifndef CXLSHM_H   /* Include guard */
#define CXLSHM_H

#define MAX_TIMEOUT_MSEC		3000
#define FILE_PATH_LENGTH        32

struct ownership { //TODO: add version to the struct...
	pid_t owner_pid;
	char ip_4_addr[17];
    int port;
	pfn_t start;
	pfn_t end;
	unsigned long vm_start;
	unsigned long vm_end;
};

//cxlshm_daxhelper
pid_t get_owner_on_mem(volatile struct ownership **owner_on_mem);
int get_cxl_device(void);
vm_fault_t handle_fault_on_cxldax(struct vm_fault *vmf);
pid_t get_owner_pid_on_mem(void);
void get_dest_host(char **dest_ip_4_address, int *dest_port);
int reset_ownership(void);
int set_ownership(pid_t pid, char *connection_string, unsigned long vma_start, unsigned long vma_end);

//cxlshm_ownership_helper
int change_ownership(pid_t current_owner, pid_t requestor);
void set_host(char *host_id_string);

#endif