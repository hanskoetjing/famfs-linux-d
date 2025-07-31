#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/ioport.h>
#include <asm-generic/cacheflush.h>
#include <linux/rcupdate.h>
#include <linux/sprintf.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/completion.h>

#include <linux/cxlshm_msg.h>
#include "dax-private.h"
#include "conn_manager.h"
#include "cxlshm-private.h"

#define THIS_MOD				"cxlshm_mm: "
#define DEVICE_NAME             "cxl_mmap"
#define CLASS_NAME              "cxl_mmap_class"

#define IOCTL_MAGIC             0xCC
#define IOCTL_SET_FILE_PATH     _IOW(IOCTL_MAGIC, 0x01, struct cxl_dev_path_struct)
#define IOCTL_SET_SRV_ADDR     	_IOW(IOCTL_MAGIC, 0x02, struct cxl_dev_path_struct)

struct cxl_dev_path_struct {
	char path[FILE_PATH_LENGTH];
};

static char device_path[FILE_PATH_LENGTH];
static dev_t dev_num;
static struct cdev ffs_cdev;
static struct class *ffs_class;
static struct vm_area_struct *this_vma;
static struct ownership o;
int open_port = 57580;
char ip_4_addr[16] = {0};

static int mmap_helper(struct file *filp, struct vm_area_struct *vma);
static long cxl_range_helper_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.mmap = mmap_helper,
	.unlocked_ioctl = cxl_range_helper_ioctl
};

//with no ownership and messaging at first. just try to separate this.
static vm_fault_t cxl_helper_fault(struct vm_fault *vmf) {
	int is_allocatable_to_this_task = 0;
	pr_info(THIS_MOD "page fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
		vmf->address, vmf->pgoff);
	vm_fault_t vmfault_handled;

	is_allocatable_to_this_task = change_ownership(get_owner_pid_on_mem(), current->pid);

	if(vmf->vma->vm_flags | MAP_CXLSHM)
		pr_info(THIS_MOD "it's me\n");

	if (is_allocatable_to_this_task) {
		vmfault_handled = handle_fault_on_cxldax(vmf);
		return vmfault_handled;
	} else {
		return VM_FAULT_RETRY;
	}
	
}

const struct vm_operations_struct cxl_helper_vm_ops = {
	.fault		= cxl_helper_fault
};


static int mmap_helper(struct file *filp, struct vm_area_struct *vma) {
	unsigned long size = vma->vm_end - vma->vm_start;
	pr_info(THIS_MOD "mmap region size: %lu\n", size);
	if (size == 0)
		return -EINVAL;
	vma->vm_ops = &cxl_helper_vm_ops;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP | MAP_CXLSHM);
	//not remap_pfn_range in here, will be handled by page fault function
	return 0;
}


static long cxl_range_helper_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	struct cxl_dev_path_struct rw;

	if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
		return -EFAULT;

	pr_info(THIS_MOD "data from userspace: %s\n", rw.path);

	switch (cmd) {
		case IOCTL_SET_FILE_PATH:
			int path_length = strscpy(device_path, rw.path, FILE_PATH_LENGTH);
			pr_info(THIS_MOD "%d char copied to file_path. File path: %s\n", path_length, device_path);
			get_cxl_device();
			break;
		case IOCTL_SET_SRV_ADDR: //as ioctl 
			if (!strchr(rw.path, ':')) 
				return -EINVAL;
			int strlen = strlen(rw.path);
			char *temporary_data = kzalloc(sizeof(char) * (strlen + 1), GFP_KERNEL);
			memset(temporary_data, 0, sizeof(char) * (strlen + 1));
			strscpy(temporary_data, rw.path, strlen + 1);
			char *ip_4_addr_from_user = strsep(&temporary_data, ":");
			int port_from_user = 0;
			int ret = kstrtoint(temporary_data, 10, &port_from_user);
			if (ret >= 0) {
				strscpy(ip_4_addr, ip_4_addr_from_user, sizeof(ip_4_addr));
				open_port = port_from_user;
			}
			break;
		default:
			return -ENOTTY;
	}

	return 0;
}

static int __init cxl_range_helper_init(void) {	
	int ret = 0;
	//init char device
	alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
	cdev_init(&ffs_cdev, &fops);
	cdev_add(&ffs_cdev, dev_num, 1);
	ffs_class = class_create(CLASS_NAME);
	device_create(ffs_class, NULL, dev_num, NULL, DEVICE_NAME);
	//init done
	pr_info(THIS_MOD "loaded\n");
	return ret;
}

static void __exit cxl_range_helper_exit(void) {
	//destroying char devices
	device_destroy(ffs_class, dev_num);
	class_destroy(ffs_class);
	cdev_del(&ffs_cdev);
	unregister_chrdev_region(dev_num, 1);
	//exit done
	pr_info(THIS_MOD "unloaded\n"); 
}


module_init(cxl_range_helper_init);
module_exit(cxl_range_helper_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CXL shared memory area access helper (r/w for every node)");
