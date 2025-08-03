#ifndef CXLSHM_HANDLER_H   /* Include guard */
#define CXLSHM_HANDLER_H

#include "../../drivers/dax/dax-private.h"
#include <linux/dax.h>

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

struct mem_alloc {
	pid_t owner_pid;
	char ip_4_addr[17];
    int port;
	struct dax_device *cxl_dax_dev;
};

//cxlshm_dax_handler
int get_cxl_dax_dev(char *device_path_param);
int alloc_mem_on_devdax(char *device_path_param, unsigned long len);
int get_cxl_dax_device(char *device_path_param);

#endif