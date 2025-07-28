#ifndef CXLSHM_H   /* Include guard */
#define CXLSHM_H
struct ownership { //TODO: add version to the struct...
	pid_t owner_pid;
	char ip_4_addr[17];
    int port;
	pfn_t start;
	pfn_t end;
	unsigned long vm_start;
	unsigned long vm_end;
};

#endif