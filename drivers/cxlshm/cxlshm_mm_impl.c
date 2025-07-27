#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/ioport.h>
#include "dax-private.h"
#include <linux/cxlshm_msg.h>
#include <asm-generic/cacheflush.h>
#include <linux/rcupdate.h>
#include <linux/sprintf.h>
#include "conn_manager.h"
#include <linux/delay.h>


#define DEVICE_NAME             "cxl_mmap"
#define CLASS_NAME              "cxl_mmap_class"
#define FILE_PATH_LENGTH        32
#define FAT_SIZE				2097152
#define FAT_OFFSET				FAT_SIZE / PAGE_SIZE

#define IOCTL_MAGIC             0xCC
#define IOCTL_SET_FILE_PATH     _IOW(IOCTL_MAGIC, 0x01, struct cxl_dev_path_struct)
#define IOCTL_FLUSH_CACHE     	_IOR(IOCTL_MAGIC, 0x02, struct cxl_dev_path_struct)

struct cxl_dev_path_struct {
	char path[FILE_PATH_LENGTH];
};

struct ownership { //TODO: add version to the struct...
	pid_t owner_pid;
	char ip_4_addr[17];
	pfn_t start;
	pfn_t end;
	unsigned long vm_start;
	unsigned long vm_end;
};

static char device_path[FILE_PATH_LENGTH];
static dev_t dev_num, dax_dev_num;
static struct cdev ffs_cdev;
static struct class *ffs_class;
static struct dax_device *cxl_dax_device = NULL;
static pfn_t begin_pfn, end_pfn;
static struct vm_area_struct *this_vma;
static void *alloc_table_start;
static struct ownership o;
int dest_port = 57580;

static int mmap_helper(struct file *filp, struct vm_area_struct *vma);
static long cxl_range_helper_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int get_cxl_device(void);
int is_owner(pid_t pid);
pid_t get_owner_on_mem(void);
int read_allocation_table(void);
int lookup_daxdev(const char *pathname, dev_t *devno);


static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.mmap = mmap_helper,
	.unlocked_ioctl = cxl_range_helper_ioctl
};

static vm_fault_t cxl_helper_filemap_fault(struct vm_fault *vmf)
{
	pfn_t pf;
    void *kaddr;
    long nr_pages_avail;
	struct vm_area_struct *vma;
	int owned = 1;
	vm_fault_t ret = 0;
	pgoff_t dax_pgoff; 
	struct task_struct *task;
	volatile struct ownership *on_mem = (volatile struct ownership *) alloc_table_start;
	
	pr_info("Page fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
		vmf->address, vmf->pgoff);
	dax_pgoff = vmf->pgoff + FAT_OFFSET;
	vma = this_vma = vmf->vma;
	task = rcu_dereference(vma->vm_mm->owner);
	owned = is_owner(task->pid);

	if (!owned) { //should sleep. maybe using fsleep??? too fast -> the receiver cant update 
		pr_info("Not owned. Current owner: %d caller PID: %d Try to send message\n", get_owner_on_mem(), task->pid);
		char pid_to_send[16] = {0};
		snprintf(pid_to_send, 15, "%d", get_owner_on_mem());
		send_one_message(o.ip_4_addr, dest_port, pid_to_send);
		int i = 0;
		while (i < 10) {
			pr_info("received %s\n", message_received);
			if (strncmp(message_received, "DONE", sizeof(message_received)) == 0) {
				break;
			} else {
				pr_info("waiting response %d\n", i);
				i++;
				msleep(1000);
			}
		}
	}
	o.owner_pid = task->pid;
	o.vm_start = vmf->address;
	o.vm_end = vma->vm_end;
	strscpy(o.ip_4_addr, "127.0.0.1", sizeof(o.ip_4_addr));

	unsigned long size = vma->vm_end - vma->vm_start;
	long nr_of_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE; 
	pr_info("cxl: fault region size: %lu, number of pages: %ld\n", size, nr_of_pages);

	if (!dax_alive(cxl_dax_device))
		run_dax(cxl_dax_device);
	
	nr_pages_avail = dax_direct_access(cxl_dax_device, dax_pgoff, nr_of_pages, DAX_ACCESS, &kaddr, &pf);
	if (nr_pages_avail < 0) return -ENXIO;
	//pr_info("Num of page(s) %ld, pfn: 0x%llx, kaddr %p\n", nr_pages_avail, pf.val, kaddr);
	o.start = pf;
	o.end.val = pf.val + nr_of_pages - 1;
	ret = vmf_insert_pfn(vmf->vma, vmf->address, pf.val);
	if (ret < 0) return ret; 
	*on_mem = o;
	pr_info("Mapping pid %d 0x%lx from mem 0x%llx to 0x%llx (pgoff from user 0x%lx)\n", task->pid, vmf->address , o.start.val,
			o.end.val, vmf->pgoff);
	
	pr_info("Now owned by pid: %d on host: %s\n", on_mem->owner_pid, on_mem->ip_4_addr);
	return ret;
}

const struct vm_operations_struct cxl_helper_file_vm_ops = {
	.fault		= cxl_helper_filemap_fault
};


static int mmap_helper(struct file *filp, struct vm_area_struct *vma) {
	unsigned long size = vma->vm_end - vma->vm_start;
	pr_info("cxl: mmap region size: %lu\n", size);
	if (size == 0)
		return -EINVAL;
	vma->vm_ops = &cxl_helper_file_vm_ops;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	//not remap_pfn_range in here, will be handled by page fault function
	return 0;
}

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
	if (!cxl_dax_device) return -ENXIO;
	if (!dax_alive(cxl_dax_device))
		run_dax(cxl_dax_device);
	return dax_direct_access(cxl_dax_device, 0, FAT_OFFSET, DAX_ACCESS, &alloc_table_start, &begin_pfn);
}

pid_t get_owner_on_mem(void) {
	int ret = 0;
	ret = read_allocation_table();
	if (ret < 0) return ret;
	volatile struct ownership *owner_on_mem = (volatile struct ownership *)alloc_table_start;
	if (owner_on_mem && owner_on_mem->owner_pid > 0) {
		return owner_on_mem->owner_pid;
	} else {
		return -ENXIO;
	}

}

int is_owner(pid_t pid) {
	int ret = 0;
	pid_t owner_on_memory = get_owner_on_mem();
	if (owner_on_memory > 0 && owner_on_memory == pid) ret = 1;
	else if (owner_on_memory < 0) ret = (int) owner_on_memory;
	return ret;
}

int get_cxl_device(void) {
	int l = lookup_daxdev(device_path, &dax_dev_num);
	if (!l) {
		pr_info("dax dev num: %d\n", dax_dev_num);
		cxl_dax_device = dax_dev_get(dax_dev_num);
		if (cxl_dax_device) {
			pr_info("got dax_device\n");
			int ret = read_allocation_table();
			if (ret < 0) return ret;
			end_pfn = begin_pfn;
			end_pfn.val = end_pfn.val + FAT_OFFSET - 1;
			pr_info("Current owner on mem: %d\n", get_owner_on_mem());
		} else {
			pr_info("no cxl_dax_device\n");
		}
		
	} else {
		pr_info("no dax dev num:\n");
	}
	
	return 0;
}

static long cxl_range_helper_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	struct cxl_dev_path_struct rw;

	if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
		return -EFAULT;

	pr_info("Path: %s\n", rw.path);

	switch (cmd) {
		case IOCTL_SET_FILE_PATH:
			int path_length = strscpy(device_path, rw.path, FILE_PATH_LENGTH);
			pr_info("%d char copied to file_path. File path: %s\n", path_length, device_path);
			get_cxl_device();
			break;
		case IOCTL_FLUSH_CACHE: //as ioctl (temporary manual invoke)
			pr_info("this ioctl done nothing now. page invalidation done by message\n");
			break;
		default:
			return -ENOTTY;
	}

	return 0;
}

static int __init cxl_range_helper_init(void) {	
	//init char device
	alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
	cdev_init(&ffs_cdev, &fops);
	cdev_add(&ffs_cdev, dev_num, 1);
	ffs_class = class_create(CLASS_NAME);
	device_create(ffs_class, NULL, dev_num, NULL, DEVICE_NAME);

	//init cxl
	strscpy(device_path, "/dev/dax0.0", sizeof(device_path)); //default device, can be altered using ioctl
	pr_info("using default path: %s\n", device_path);
	get_cxl_device();
	pr_info("Initialise allocation table at 0x%llx to 0x%llx \n", begin_pfn.val, end_pfn.val);
	memset(alloc_table_start, 0, sizeof(struct ownership));

	//init tcp server
	set_port(57581);
	tcp_server_start();

	//init done
	pr_info("cxlshm_mm: loaded\n");
	return 0;
}

static void __exit cxl_range_helper_exit(void) {
	//destroying char devices
	device_destroy(ffs_class, dev_num);
	class_destroy(ffs_class);
	cdev_del(&ffs_cdev);
	unregister_chrdev_region(dev_num, 1);

	//stopping tcp server
	tcp_server_stop();

	//exit done
	pr_info("cxlshm_mm: unloaded\n"); 
}


module_init(cxl_range_helper_init);
module_exit(cxl_range_helper_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CXL shared memory area access helper (r/w for every node)");
