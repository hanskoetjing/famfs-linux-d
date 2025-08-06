#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/mman.h> 
#include <linux/err.h>           
#include <linux/uaccess.h>       
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/dax.h>
#include "../../drivers/dax/dax-private.h"
#include "../../mm/cxlshm/cxlshm_handler_private.h"
//Tong Xing @UoE
//modified by Hans @UoE

#define THIS_MOD "cxl_alloc_dsm: "
#define MAX_ALLOC 4 * 1024 * 1024

extern pid_t owner_pid;

unsigned long __cxl_alloc_dsm(char *dax_device_path, unsigned long len);
static vm_fault_t cxl_dsm_fault_handler(struct vm_fault *vmf);
static vm_fault_t cxl_dsm_mkwrite_handler(struct vm_fault *vmf);

static vm_fault_t cxl_dsm_fault_handler(struct vm_fault *vmf) {
	int is_allottable_to_this_task = 1;
	pr_info(THIS_MOD "page fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
		vmf->address, vmf->pgoff);
	vm_fault_t vmfault_handled;
	
	is_allottable_to_this_task = 1;
	// /is_allottable_to_this_task = is_allottable(task_pid_nr(current));

	if(vmf->vma->vm_flags & MAP_CXLDSM)
		pr_info(THIS_MOD "memory on cxl device!\n");

	if (is_allottable_to_this_task > 0) 
	{
		vmfault_handled = handle_fault_on_cxldaxdev_prot(vmf, PAGE_READONLY);
		return vmfault_handled;
	} 
	else 
	{
		return VM_FAULT_RETRY;
	}
	
}

static vm_fault_t cxl_dsm_mkwrite_handler(struct vm_fault *vmf) 
{
	vm_fault_t vmfault_handled;
	pr_info(THIS_MOD "mkwrite fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
	vmf->address, vmf->pgoff);
	vmfault_handled = handle_fault_on_cxldaxdev_prot(vmf, PAGE_SHARED);
	return vmfault_handled;
	//return VM_FAULT_NOPAGE;
}


static const struct vm_operations_struct my_vm_ops = 
{
    .fault = cxl_dsm_fault_handler,
	.pfn_mkwrite = cxl_dsm_mkwrite_handler
};

/**
 * Custom memory allocation and mapping function.
 * This function allocates memory and maps it into the VMA of the calling process.
 *
 * @length: Length of memory to allocate and map.
 * @returns: The address of the allocated memory, or an error code.
 */
SYSCALL_DEFINE2(cxl_alloc_dsm, char __user *, dax_device_path, unsigned long, len)
{
    char message_buf[FILE_PATH_LENGTH] = {0};
	int ret = strncpy_from_user(message_buf, dax_device_path, FILE_PATH_LENGTH);
	if (ret < 0) return -EFAULT;
	if (ret >= sizeof(message_buf) || ret == 0) return -EINVAL;
	return __cxl_alloc_dsm(message_buf, len);
}

unsigned long __cxl_alloc_dsm(char *dax_device_path, unsigned long len) 
{
	owner_pid = task_pid_nr(current);
    //return 0; 
    unsigned long addr;
    const unsigned long prot  = PROT_READ | PROT_WRITE;
    const unsigned long flags = MAP_SHARED | MAP_ANONYMOUS | MAP_CXLDSM;
	vm_flags_t vm_flags = VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_USERMAP |
				VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE | VM_MIXEDMAP;
	void *kern_buf;
	struct vm_area_struct *vma;
	pfn_t dax_pfn;
	unsigned long populate = 0;
	int ret;
	if (strlen(dax_device_path) <= 0) {
		pr_info(THIS_MOD "invalid device path");
		return -EINVAL;
	}
	
	/* Reject nonsense or over‑large requests */
	if (!len || len > MAX_ALLOC)
		return -EINVAL;
	
	len = PAGE_ALIGN(len);
	printk(THIS_MOD "allocation of aligned %ld byte of cxl memory on device: %s\n", len, dax_device_path);

	ret = mmap_write_lock_killable(current->mm);
	if (ret) {
		return ret;
	}

	//do allocation on devdax
	ret = alloc_mem_on_devdax(dax_device_path, len, &kern_buf, &dax_pfn);
	addr = do_mmap(NULL, 0, len, prot, flags, vm_flags,
					0 /* pgoff */, &populate /* populate */, NULL /* uf */);
	pr_info("mmap-ed address: 0x%lx\n", addr);

	/* find the VMA we just created */
	vma = find_vma(current->mm, addr);
	if (!vma) {
		pr_info(THIS_MOD "failed to find vma");
		mmap_write_unlock(current->mm);
		return -EFAULT;
	}
	vma->vm_ops = &my_vm_ops;
	/*
		* remap_vmalloc_range() will map our vmalloc() buffer
		* into that VMA, page by page.
		
	*/
	pr_info("allocated pages on cxl dax: %d\n", ret);
	mmap_write_unlock(current->mm);
	
	if (ret < 0) {
		/* on failure, unmap the VMA and free the kernel buffer */
		vm_munmap(addr, len);
		return ret;
	}
	pr_info("allocation done\n");
	/* success → return user‑space VA */
	return addr;
}
