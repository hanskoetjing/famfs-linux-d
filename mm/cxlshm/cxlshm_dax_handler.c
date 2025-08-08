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
/*to check pages*/
#include <linux/pfn_t.h>
#include <asm-generic/memory_model.h>
#include <linux/mm_types.h>
#include <asm-generic/int-ll64.h>
#include <vdso/limits.h>

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
static pfn_t begin_pfn, end_pfn, start_data_pfn;
static void *alloc_table_start = NULL;
extern pid_t owner_pid;

int lookup_daxdev(const char *pathname, dev_t *devno);
int get_cxl_dax_dev(char *device_path_param);
int read_owner_info_on_mem(char *device_path_param);
int alloc_mem_on_devdax(char *device_path_param, unsigned long len, void **dax_kaddr, pfn_t *dax_pfn);
void get_current_device_path(char **dev_path);
int process_location_info(char **address, int *port);
int get_owner_info_on_mem(struct ownership **owner);
int set_owner_info_on_mem(struct ownership *owner);
void set_new_owner_info(struct ownership **owner, pid_t pid, unsigned long vm_start, unsigned long vm_end, unsigned long offset);
vm_fault_t handle_fault_on_cxldaxdev_prot(struct vm_fault *vmf, pgprot_t pgprot);
vm_fault_t handle_fault_on_cxldaxdev_mkwrite(struct vm_fault *vmf);

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
	start_data_pfn.val = end_pfn.val + FAT_OFFSET + 1;
	return ret;
}

int get_owner_info_on_mem(struct ownership **owner)
{
	read_owner_info_on_mem(device_path);
	pr_info(THIS_MOD "get owner info from mem\n");
	volatile struct ownership *owner_on_memory = (volatile struct ownership *)alloc_table_start;
	if (owner_on_memory != NULL && owner_on_memory->owner_pid > 0) 
	{
		pr_info(THIS_MOD "found ownership info. %d is the owner\n", owner_on_memory->owner_pid);
		(*owner) = (struct ownership *)owner_on_memory;
	}
	else
	{
		pr_info(THIS_MOD "no ownership info found. initialising a new one\n");
		struct ownership *new_owner = (struct ownership *)kzalloc(sizeof(struct ownership), GFP_KERNEL);
		new_owner->owner_pid = 0;
		new_owner->port = 0;
		memcpy((void *)owner_on_memory, (void *)new_owner, sizeof(struct ownership));
		(*owner) = new_owner;
	}
	return 0;
}
EXPORT_SYMBOL(get_owner_info_on_mem);

void set_new_owner_info(struct ownership **owner, pid_t pid, unsigned long vm_start, unsigned long vm_end, unsigned long offset) 
{
	struct ownership *new_owner = kzalloc(sizeof(struct ownership), GFP_KERNEL);
	new_owner->owner_pid = pid;
	char *ip_4_addr;
	int owner_port;
	process_location_info(&ip_4_addr, &owner_port);
	new_owner->port = owner_port;
	struct mm_struct *mm = current->mm;
	if (mm == NULL) 
		return -1;
	new_owner->vm_start = vm_start;
	new_owner->vm_end = vm_end;
	new_owner->offset = offset;
	pr_info(THIS_MOD "vm_start: 0x%lx vm_end: 0x%lx\n", new_owner->vm_start, new_owner->vm_end);
	strscpy(new_owner->ip_4_addr, ip_4_addr, 16);
	*owner = new_owner;
	kfree(ip_4_addr);
}
EXPORT_SYMBOL(set_new_owner_info);

int set_owner_info_on_mem(struct ownership *owner)
{
	read_owner_info_on_mem(device_path);
	if (owner == NULL)
	{
		return -EINVAL;
	}
	else
	{
		if (owner->owner_pid <= 0)
		{
			return -EINVAL;
		}
		pr_info(THIS_MOD "writing owner info on special area on mem\n");
		memcpy(alloc_table_start, owner, sizeof(struct ownership));
	}
	return 0;
}
EXPORT_SYMBOL(set_owner_info_on_mem);

int process_location_info(char **address, int *port)
{
	/*set host identifier on ownership data*/
	char *temp_location_processing;
	get_location_info(&temp_location_processing);
	*port = 57580;
	*address = (char *)kzalloc(sizeof(char) * 16, GFP_KERNEL);
	if (strlen(temp_location_processing) == 0)
	{
		strscpy(*address, "127.0.0.1", 16);
	}
	else 
	{
		pr_info(THIS_MOD "location: %s\n", temp_location_processing);
		char *ip_4_addr_from_user = strsep(&temp_location_processing, ":");
		if (ip_4_addr_from_user != NULL) 
		{
			int port_from_user = 0;
			int strtoint_ret = kstrtoint(temp_location_processing, 10, &port_from_user);
			if (strtoint_ret >= 0) 
			{
				strscpy(*address, ip_4_addr_from_user, 16);
				*port = port_from_user;
			}
		}
		else
		{
			strscpy(*address, "127.0.0.1", 16);
		}
	}
	
	kfree(temp_location_processing);
	return 0;

}

int get_cxl_dax_dev(char *device_path_param) 
{
	int char_copied_length = -1;
	if (strlen(device_path) != 0) {
		char_copied_length = strscpy(device_path, device_path_param, FILE_PATH_LENGTH);
	}
	if (char_copied_length < 0) {
		char_copied_length = strscpy(device_path, DEFAULT_PATH, FILE_PATH_LENGTH);
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

//pfn fault handler. insert the pfn as read-only, write will trigger the other func
vm_fault_t handle_fault_on_cxldaxdev_prot(struct vm_fault *vmf, pgprot_t pgprot) {
    int ret = 0;
	struct ownership *owner;
    struct vm_area_struct *vma = vmf->vma;
    pgoff_t dax_pgoff = vmf->pgoff;
    pr_info(THIS_MOD "dax area page fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
		vmf->address, vmf->pgoff);
	read_owner_info_on_mem("");
	start_data_pfn.val += (u64)dax_pgoff;
    pr_info(THIS_MOD "got pfn at: 0x%llx\n", start_data_pfn.val);
    ret = vmf_insert_pfn_prot(vmf->vma, vmf->address, start_data_pfn.val, pgprot);
    pr_info(THIS_MOD "insert pfn to vmf done\n");
    return ret;
}
EXPORT_SYMBOL(handle_fault_on_cxldaxdev_prot);

//pfn write fault handler
vm_fault_t handle_fault_on_cxldaxdev_mkwrite(struct vm_fault *vmf) {
    int ret = 0;
    struct vm_area_struct *vma = vmf->vma;
    pgoff_t dax_pgoff = vmf->pgoff;
    pr_info(THIS_MOD "dax area write fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
		vmf->address, vmf->pgoff);
	struct mm_struct *mm = vmf->vma->vm_mm;
	if (vmf->pte != NULL)
	{
		pr_info(THIS_MOD "pte writable? %d\n", pte_write(*(vmf->pte)));
		down_read(&(mm->mmap_lock));
		spin_lock(vmf->ptl);
		pte_t *ptep = vmf->pte;
		pte_t new_pte = *ptep;
		new_pte = pte_mkwrite(new_pte, vma);
		u64 pfn = pte_pfn(new_pte);
		set_pte(ptep, new_pte);
		flush_tlb_page(vma, vmf->address);
    	update_mmu_cache(vma, vmf->address, ptep);
		spin_unlock(vmf->ptl);
		up_read(&(mm->mmap_lock));
		pr_info(THIS_MOD "pte writable now? %d\n", pte_write(*(vmf->pte)));
	}

	/*
	pte_t pte_from_vmf = *(vmf->pte);
	unsigned long pfn_from_vmf = pte_pfn(pte_from_vmf);
	pfn_t pfn_to_insert;
	pfn_to_insert.val = pfn_from_vmf;
    ret = vmf_insert_mixed_mkwrite(vmf->vma, vmf->address, pfn_to_insert);
    pr_info(THIS_MOD "insert pfn as writable to vmf done\n");*/
    return VM_FAULT_NOPAGE;
}
EXPORT_SYMBOL(handle_fault_on_cxldaxdev_mkwrite);
