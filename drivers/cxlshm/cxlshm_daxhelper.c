#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/sprintf.h>
#include "dax-private.h"
#include "cxlshm-private.h"

#define THIS_MOD				"cxlshm_dax_helper: "
#define FAT_SIZE				2097152
#define FAT_OFFSET				FAT_SIZE / PAGE_SIZE

static char device_path[FILE_PATH_LENGTH] = "/dev/dax0.0";
static struct dax_device *cxl_dax_device = NULL;
static dev_t dax_dev_num;
static pfn_t begin_pfn, end_pfn;
static void *alloc_table_start = NULL;

int lookup_daxdev(const char *pathname, dev_t *devno);
pid_t get_owner_on_mem(volatile struct ownership **owner_on_mem);
int read_allocation_table(void);
int get_cxl_device(void);
vm_fault_t handle_fault_on_cxldax(struct vm_fault *vmf);
pid_t get_owner_pid_on_mem(void);
void get_dest_host(char **dest_ip_4_address, int *dest_port);

//taken from famfs kernel code
int lookup_daxdev(const char *pathname, dev_t *devno) {
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

int read_allocation_table(void) {
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


int get_cxl_device(void) {
	int lookup_result = lookup_daxdev(device_path, &dax_dev_num);
    int ret = 0;
	if (!lookup_result) {
		pr_info(THIS_MOD "dax dev num: %d\n", dax_dev_num);
		cxl_dax_device = dax_dev_get(dax_dev_num);
		if (cxl_dax_device) {
			pr_info(THIS_MOD "got cxl_dax_device\n");
		} else {
			pr_info(THIS_MOD "no cxl_dax_device\n");
		}
	} else {
		pr_info(THIS_MOD "can't dax dev num\n");
        cxl_dax_device = NULL;
        ret = -ENXIO;
	}
	
	return ret;
}
EXPORT_SYMBOL(get_cxl_device);

//with no ownership and messaging at first. just try to separate this.
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