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

#define THIS_MOD "cxl_alloc: "
#define MAX_ALLOC 4 * 1024 * 1024

struct dax_device *cxl_dax_device = NULL;

static vm_fault_t adaptive_vma_fault(struct vm_fault *vmf)
{
    struct page *page;

    printk("Custom fault at address: %lx\n", vmf->address);

    // Example: fill in a dummy zero page
    page = ZERO_PAGE(0);  // or alloc_page(), etc.
    get_page(page);
    vmf->page = page;

    return 0;  // VM_FAULT_NOPAGE
}

static const struct vm_operations_struct my_vm_ops = {
    .fault = adaptive_vma_fault,
};



//extern int stupid;
/**
 * Custom memory allocation and mapping function.
 * This function allocates memory and maps it into the VMA of the calling process.
 *
 * @length: Length of memory to allocate and map.
 * @returns: The address of the allocated memory, or an error code.
 */
SYSCALL_DEFINE2(cxl_alloc, char __user *, dax_device_path, unsigned long, len)
{
        //stupid = task_pid_nr(current);
        //return 0; 
        unsigned long addr;
        const unsigned long prot  = PROT_READ | PROT_WRITE;
        const unsigned long flags = MAP_SHARED | MAP_ANONYMOUS;
		vm_flags_t vmf = VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_USERMAP |
                 VM_READ | VM_WRITE | VM_MAYREAD | VM_MAYWRITE;
		void *kern_buf;
		struct vm_area_struct *vma;
		struct dax_device *cxl_dax_device = NULL;
		unsigned long populate = 0;
	    int ret;
		if (strlen(dax_device_path) <= 0) {
			pr_info(THIS_MOD "invalid device path");
			return -EINVAL;
		}
		printk(THIS_MOD "allocation %ld byte of cxl memory on device: %s\n", len, dax_device_path);
        /* Reject nonsense or over‑large requests */
        if (!len || len > MAX_ALLOC)
            return -EINVAL;
		
		len = PAGE_ALIGN(len);

		//do allocation on devdax
		int ret = alloc_mem_on_devdax(dax_device_path, len);
        addr = do_mmap(NULL, 0, len, prot, flags, vmf,
                       0 /* pgoff */, &populate /* populate */, NULL /* uf */);

		/* find the VMA we just created */
		vma = find_vma(current->mm, addr);
	    if (!vma) {
		    mmap_write_unlock(current->mm);
			vfree(kern_buf);
	        return -EFAULT;
		}
		vma->vm_ops = &my_vm_ops;
		/*
		 * remap_vmalloc_range() will map our vmalloc() buffer
	     * into that VMA, page by page.
		 */
	    ret = remap_vmalloc_range(vma, kern_buf, 0);
		mmap_write_unlock(current->mm);
	
	    if (ret) {
		    /* on failure, unmap the VMA and free the kernel buffer */
	        vm_munmap(addr, len);
			vfree(kern_buf);
		    return ret;
	    }

		/* success → return user‑space VA */
	    return addr;
}

