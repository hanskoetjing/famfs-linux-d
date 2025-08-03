#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/sprintf.h>

#include <linux/dax.h>
#include "../../drivers/dax/dax-private.h"
#include "cxlshm_handler_private.h"

#define THIS_MOD				"cxlshm_dax_handler: "
#define DEFAULT_PATH			"/dev/dax0.0"
#define FAT_SIZE				2097152
#define FAT_OFFSET				FAT_SIZE >> PAGE_SHIFT

static char device_path[FILE_PATH_LENGTH] = {0};
static struct dax_device *cxl_dax_device = NULL;
static dev_t dax_dev_num;
static pfn_t begin_pfn, end_pfn;
static void *alloc_table_start = NULL;

int lookup_daxdev(const char *pathname, dev_t *devno);
int get_cxl_dax_dev(char *device_path_param);
int lookup_daxdevice(const char *pathname, struct dax_device **daxdevice);
/*
pid_t get_owner_on_mem(volatile struct ownership **owner_on_mem);
int read_allocation_table(void);
int get_cxl_device(void);
vm_fault_t handle_fault_on_cxldax(struct vm_fault *vmf);
pid_t get_owner_pid_on_mem(void);
void get_dest_host(char **dest_ip_4_address, int *dest_port);
int reset_ownership(void);
int set_ownership(pid_t pid, char *connection_string, unsigned long vma_start, unsigned long vma_end);
*/
/*
//fault handler. owner checking is handled in other c source
vm_fault_t handle_fault_on_cxldax(struct vm_fault *vmf) {
    int ret = 0;
    struct vm_area_struct *vma = vmf->vma;
    unsigned long size = vma->vm_end - vma->vm_start;
    long nr_of_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    pfn_t pfn_dax;
    void *kaddr = NULL;
    pgoff_t dax_pgoff = vmf->pgoff + FAT_OFFSET;
    pr_info(THIS_MOD "dax area page fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
		vmf->address, vmf->pgoff);
    if (!cxl_dax_device) {
        get_cxl_device();
        if (!cxl_dax_device)
            return -ENXIO;
    }
    if (!dax_alive(cxl_dax_device)) {
        run_dax(cxl_dax_device);
        if (!dax_alive(cxl_dax_device))
            return -ENXIO;
    }
    ret = dax_direct_access(cxl_dax_device, dax_pgoff, nr_of_pages, DAX_ACCESS, &kaddr, &pfn_dax);
    pr_info(THIS_MOD "got pfn at: 0x%llx\n", pfn_dax.val);
    ret = vmf_insert_pfn(vmf->vma, vmf->address, pfn_dax.val);
    pr_info(THIS_MOD "insert pfn to vmf done\n");
    return ret;
}
EXPORT_SYMBOL(handle_fault_on_cxldax);
*/


//taken from famfs kernel code
int lookup_daxdev(const char *pathname, dev_t *devno) 
{
	struct inode *inode;
	struct path path;
	int err;

	if (!pathname || !*pathname)
		return -EINVAL;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = d_backing_inode(path.dentry);
	if (!S_ISCHR(inode->i_mode)) {
		err = -EINVAL;
		goto out_path_put;
	}
	//may_open_dev is taken out
	 /* if it's dax, i_rdev is struct dax_device */
	*devno = inode->i_rdev;

out_path_put:
	path_put(&path);
	return err;
}

int lookup_daxdevice(const char *pathname, struct dax_device **daxdevice) 
{
	struct inode *inode;
	struct path path;
	int err;

	if (!pathname || !*pathname)
		return -EINVAL;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = d_backing_inode(path.dentry);
	if (!S_ISCHR(inode->i_mode)) 
	{
		err = -EINVAL;
		goto out_path_put;
	}
	(*daxdevice) = inode_dax(inode);

	if (dax_alive((*daxdevice)))
	{
		err = -ENXIO;
		goto out_path_put;
	}

out_path_put:
	path_put(&path);
	return err;
}

int read_owner_info_on_mem(void) 
{
    int ret = 0;
	if (!cxl_dax_device) 
        ret = get_cxl_device();
	if (ret == 0 && !dax_alive(cxl_dax_device))
		run_dax(cxl_dax_device);
    ret = dax_direct_access(cxl_dax_device, 0, FAT_OFFSET, DAX_ACCESS, &alloc_table_start, &begin_pfn);
    end_pfn = begin_pfn;
	end_pfn.val = end_pfn.val + FAT_OFFSET - 1;
	return ret;
}

/*
int reset_ownership(void) {
	int ret = 0;
	ret = read_allocation_table();
	if (ret < 0) return ret;
	memset(alloc_table_start, 0, sizeof(struct ownership));
	return 0;
}
EXPORT_SYMBOL(reset_ownership);
*/
/*
int set_ownership(pid_t pid, char *connection_string, unsigned long vma_start, unsigned long vma_end) {
	int ret = 0;
	ret = read_allocation_table();
	volatile struct ownership *owner_on_mem = (volatile struct ownership *)alloc_table_start;
	owner_on_mem->owner_pid = pid;
	owner_on_mem->vm_start = vma_start;
	owner_on_mem->vm_end = vma_end;
	int strlen = strlen(connection_string);
	char *temporary_data = kzalloc(sizeof(char) * (strlen + 1), GFP_KERNEL);
	memset(temporary_data, 0, sizeof(char) * (strlen + 1));
	strscpy(temporary_data, connection_string, strlen + 1);
	if (!strchr(temporary_data, ':')) 
		return -EINVAL;
	char *ip_4_addr_from_user = strsep(&temporary_data, ":");
	int port_from_user = 0;
	ret = kstrtoint(temporary_data, 10, &port_from_user);
	if (ret >= 0) {
		strscpy((char * const)owner_on_mem->ip_4_addr, ip_4_addr_from_user, sizeof(owner_on_mem->ip_4_addr));
		owner_on_mem->port = port_from_user;
	}
	return 0;
}
EXPORT_SYMBOL(set_ownership);
*/
/*
pid_t get_owner_on_mem(volatile struct ownership **owner_on_mem) {
	int ret = 0;
	ret = read_allocation_table();
	if (ret < 0) return ret;
	*owner_on_mem = (volatile struct ownership *)alloc_table_start;
	if (*owner_on_mem && (*owner_on_mem)->owner_pid > 0) {
		return (pid_t) ((*owner_on_mem)->owner_pid);
	} else {
		return -ENXIO;
	}

}
EXPORT_SYMBOL(get_owner_on_mem);
*/
/*
pid_t get_owner_pid_on_mem(void) {
	int ret = 0;
    pr_info(THIS_MOD "get owner pid in mem\n");
	ret = read_allocation_table();
	if (ret < 0) return ret;
	volatile struct ownership *owner_on_mem = (volatile struct ownership *)alloc_table_start;
    
    pr_info(THIS_MOD "owner on mem: %d\n", owner_on_mem->owner_pid);
	if (owner_on_mem && owner_on_mem->owner_pid > 0) {
		return (pid_t)owner_on_mem->owner_pid;
	} else if (!owner_on_mem || owner_on_mem == NULL) {
        return -EFAULT;
    } else {
		return -ENXIO;
	}
}
EXPORT_SYMBOL(get_owner_pid_on_mem);
*/
/*
void get_dest_host(char **dest_ip_4_address, int *dest_port) {
	volatile struct ownership *owner_data = NULL;
    get_owner_on_mem(&owner_data);
	pr_info(THIS_MOD "owner host on mem: %s:%d", owner_data->ip_4_addr, owner_data->port);
    int address_length = strlen((const char *)owner_data->ip_4_addr);
    (*dest_ip_4_address) = kzalloc(sizeof(char) * (address_length + 1), GFP_KERNEL);
    memset((*dest_ip_4_address), 0, sizeof(char) * (address_length + 1));
    strscpy((*dest_ip_4_address), (const char *)owner_data->ip_4_addr, (address_length + 1));
    *dest_port = owner_data->port;
}
EXPORT_SYMBOL(get_dest_host);
*/

int get_cxl_dax_dev(char *device_path_param) 
{
	int char_copied_length = strscpy(device_path, device_path_param, FILE_PATH_LENGTH);
	if (char_copied_length < 0) {
		strscpy(device_path, DEFAULT_PATH, FILE_PATH_LENGTH);
	}
	int lookup_result = lookup_daxdev(device_path, &dax_dev_num);
    int ret = 0;
	if (!lookup_result) {
		pr_info(THIS_MOD "dax dev num: %d\n", dax_dev_num);
		cxl_dax_device = dax_dev_get(dax_dev_num);
		if (cxl_dax_device) {
			pr_info(THIS_MOD "cxl dax device is ready\n");
		} else {
			pr_info(THIS_MOD "cxl dax device is not ready\n");
			ret = -ENXIO;
		}
	} else {
		pr_info(THIS_MOD "can't get dax dev num\n");
        cxl_dax_device = NULL;
        ret = -ENXIO;
	}
	
	return ret;
}
EXPORT_SYMBOL(get_cxl_dax_dev);

int get_cxl_dax_device(char *device_path_param) 
{
	int char_copied_length = strscpy(device_path, device_path_param, FILE_PATH_LENGTH);
	if (char_copied_length < 0) {
		strscpy(device_path, DEFAULT_PATH, FILE_PATH_LENGTH);
	}
	int lookup_result = lookup_daxdevice(device_path, &dax_dev_num);
    int ret = 0;
	if (!lookup_result) {
		pr_info(THIS_MOD "dax dev num: %d\n", dax_dev_num);
		cxl_dax_device = dax_dev_get(dax_dev_num);
		if (cxl_dax_device) {
			pr_info(THIS_MOD "cxl dax device is ready\n");
		} else {
			pr_info(THIS_MOD "cxl dax device is not ready\n");
			ret = -ENXIO;
		}
	} else {
		pr_info(THIS_MOD "can't get dax dev num\n");
        cxl_dax_device = NULL;
        ret = -ENXIO;
	}
	
	return ret;
}
EXPORT_SYMBOL(get_cxl_dax_device);

int alloc_mem_on_devdax(char *device_path_param, unsigned long len) 
{
	int ret = 0;
	unsigned long nr_pages = len >> PAGE_SHIFT;
	if (!cxl_dax_device)
	{
		ret = get_cxl_dax_device(device_path_param);
		if (ret < 0) 
		{
			return ret;
		}
	}
	void *dax_kaddr;
	pfn_t dax_pfn;
	ret = dax_direct_access(cxl_dax_device, FAT_OFFSET, len, DAX_ACCESS, &dax_kaddr, &dax_pfn);
	return ret;
}
EXPORT_SYMBOL(alloc_mem_on_devdax);