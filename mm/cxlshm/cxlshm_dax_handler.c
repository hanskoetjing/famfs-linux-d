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
#define FAT_OFFSET				(FAT_SIZE >> PAGE_SHIFT)

static char device_path[FILE_PATH_LENGTH] = {0};
static struct dax_device *cxl_dax_device = NULL;
static dev_t dax_dev_num;
static pfn_t begin_pfn, end_pfn;
static void *alloc_table_start = NULL;
extern pid_t owner_pid;

int lookup_daxdev(const char *pathname, dev_t *devno);
int get_cxl_dax_dev(char *device_path_param);
int read_owner_info_on_mem(char *device_path_param);
int alloc_mem_on_devdax(char *device_path_param, unsigned long len, void **dax_kaddr, pfn_t *dax_pfn);
vm_fault_t handle_fault_on_cxldaxdev(struct vm_fault *vmf);
void get_current_device_path(char **dev_path);
int get_owner_info_on_mem(struct ownership *owner);
int set_owner_info_on_mem(struct ownership *owner);

void get_current_device_path(char **dev_path) 
{
	strscpy((*dev_path), device_path, strlen(device_path) + 1);
}
EXPORT_SYMBOL(get_current_device_path);

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

int read_owner_info_on_mem(char *device_path_param) 
{
    int ret = 0;
	if (!cxl_dax_device) 
        ret = get_cxl_dax_dev(device_path_param);
	if (ret == 0 && !dax_alive(cxl_dax_device))
		run_dax(cxl_dax_device);
    ret = dax_direct_access(cxl_dax_device, 0, FAT_OFFSET, DAX_ACCESS, &alloc_table_start, &begin_pfn);
    end_pfn = begin_pfn;
	end_pfn.val = end_pfn.val + FAT_OFFSET - 1;
	pr_info(THIS_MOD "read owner info on 0x%llx to 0x%llx\n", begin_pfn.val, end_pfn.val);
	pr_info(THIS_MOD "read owner info with kaddr 0x%p\n", alloc_table_start);
	return ret;
}

int get_owner_info_on_mem(struct ownership *owner)
{
	read_owner_info_on_mem(device_path);
	pr_info(THIS_MOD "get owner info from mem\n");
	volatile struct ownership * owner_on_memory = (volatile struct ownership *)alloc_table_start;
	pr_info(THIS_MOD "get owner info from mem 0x%p\n", alloc_table_start);
	pr_info(THIS_MOD "get owner info from memm 0x%d\n", *owner_on_memory);
	if (!(*owner_on_memory)) 
		pr_info(THIS_MOD "owner is empty\n");
	if (owner_on_memory != NULL && owner_on_memory->owner_pid > 0) 
	{
		pr_info(THIS_MOD "found ownership info. %d is the owner\n", owner->owner_pid);
		owner = (struct ownership *)owner_on_memory;
	}
	else
	{
		pr_info(THIS_MOD "no ownership info found. initialising a new one\n");
		owner = (struct ownership *)kzalloc(sizeof(struct ownership), GFP_KERNEL);
		owner->owner_pid = 0;
		owner->port = 0;
	}
	return 0;
}
EXPORT_SYMBOL(get_owner_info_on_mem);

int set_owner_info_on_mem(struct ownership *owner)
{
	read_owner_info_on_mem(device_path);
	if (owner->owner_pid <= 0)
	{
		return -EINVAL;
	}
	pr_info(THIS_MOD "writing owner info on special area on mem\n");
	memcpy(alloc_table_start, owner, sizeof(struct ownership));
	return 0;
}
EXPORT_SYMBOL(set_owner_info_on_mem);

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

int alloc_mem_on_devdax(char *device_path_param, unsigned long len, void **dax_kaddr, pfn_t *dax_pfn) 
{
	int ret = 0;
	unsigned long nr_pages = len >> PAGE_SHIFT;
	if (!cxl_dax_device)
	{
		ret = get_cxl_dax_dev(device_path_param);
		if (ret < 0) 
		{
			return ret;
		}
	}
	ret = dax_direct_access(cxl_dax_device, FAT_OFFSET, nr_pages, DAX_ACCESS, dax_kaddr, dax_pfn);
	return ret;
}
EXPORT_SYMBOL(alloc_mem_on_devdax);

//fault handler. owner checking is handled in other c source
vm_fault_t handle_fault_on_cxldaxdev(struct vm_fault *vmf) {
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
        get_cxl_dax_dev(device_path);
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
EXPORT_SYMBOL(handle_fault_on_cxldaxdev);
