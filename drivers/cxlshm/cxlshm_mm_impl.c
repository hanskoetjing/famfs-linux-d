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
#define FILE_PATH_LENGTH        32
#define FAT_SIZE				2097152
#define FAT_OFFSET				FAT_SIZE / PAGE_SIZE

#define IOCTL_MAGIC             0xCC
#define IOCTL_SET_FILE_PATH     _IOW(IOCTL_MAGIC, 0x01, struct cxl_dev_path_struct)
#define IOCTL_SET_SRV_ADDR     	_IOW(IOCTL_MAGIC, 0x02, struct cxl_dev_path_struct)

struct cxl_dev_path_struct {
	char path[FILE_PATH_LENGTH];
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
int open_port = 57580;
char ip_4_addr[16] = {0};

static int mmap_helper(struct file *filp, struct vm_area_struct *vma);
static long cxl_range_helper_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
int get_cxl_device(void);
int is_owner(pid_t pid);
pid_t get_owner_on_mem(volatile struct ownership **owner_on_mem);
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
	volatile struct ownership *owner_on_mem;
	
	pr_info(THIS_MOD "page fault at user address 0x%lx (pgoff from userspace 0x%lx)\n",
		vmf->address, vmf->pgoff);
	dax_pgoff = vmf->pgoff + FAT_OFFSET;
	vma = this_vma = vmf->vma;
	task = rcu_dereference(vma->vm_mm->owner);
	pid_t owner_on_memory = 0;
	owner_on_memory = get_owner_on_mem(&owner_on_mem);
	if (task->pid == owner_on_memory)
		owned = 1;
	else if (owner_on_memory < 0)
		owned = 1;

	if (!owned) {
		pr_info(THIS_MOD "not owned. Current owner: %d caller PID: %d Try to send message to %s:%d\n", 
			owner_on_memory, task->pid, owner_on_mem->ip_4_addr, owner_on_mem->port);
		char pid_to_send[16] = {0};
		snprintf(pid_to_send, 15, "PID:%d", get_owner_on_mem(&owner_on_mem));
		tcp_client_start_d((char *)owner_on_mem->ip_4_addr, owner_on_mem->port);
		send_message_d(pid_to_send);
		int i = 0;
		char received_copy[MAX_BUFFER_NET] = {0};
		unsigned long timeout = msecs_to_jiffies(MAX_TIMEOUT_MSEC);
		long completion_ret_val = wait_for_completion_interruptible_timeout(&is_complete, timeout);
		if (completion_ret_val > 0) {
			spin_lock(&ctr_lock);
			strscpy(received_copy, message_received, sizeof(message_received));
			memset(message_received, 0, sizeof(message_received));
			spin_unlock(&ctr_lock);
			if (strncmp(received_copy, "DONE", 4) == 0) {
				owned = 1;
			} else {
				pr_info(THIS_MOD "not a completion message, maybe handled later %d\n", i);
			}
			tcp_client_stop_d();
		} else if (completion_ret_val == 0) {
			pr_info(THIS_MOD "timeout occured. retrying\n");
			return -EAGAIN;
		} else {
			pr_info(THIS_MOD "interrupted\n");
			return -EAGAIN;
		}
	}

	if (owned) {
		o.owner_pid = task->pid;
		o.vm_start = vmf->address;
		o.vm_end = vma->vm_end;
		strscpy(o.ip_4_addr, ip_4_addr, sizeof(o.ip_4_addr));
		o.port = open_port;

		unsigned long size = vma->vm_end - vma->vm_start;
		long nr_of_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE; 
		pr_info(THIS_MOD "fault region size: %lu, number of pages: %ld\n", size, nr_of_pages);

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
		pr_info(THIS_MOD "mapping pid %d 0x%lx from mem 0x%llx to 0x%llx (pgoff from user 0x%lx)\n", task->pid, vmf->address , o.start.val,
				o.end.val, vmf->pgoff);
		
		pr_info(THIS_MOD "now owned by pid: %d on host: %s\n", on_mem->owner_pid, on_mem->ip_4_addr);
	} else {
		pr_info(THIS_MOD "not yet impl\n");
	}
	return ret;
}

const struct vm_operations_struct cxl_helper_file_vm_ops = {
	.fault		= cxl_helper_filemap_fault
};


static int mmap_helper(struct file *filp, struct vm_area_struct *vma) {
	unsigned long size = vma->vm_end - vma->vm_start;
	pr_info(THIS_MOD "mmap region size: %lu\n", size);
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

int get_cxl_device(void) {
	int l = lookup_daxdev(device_path, &dax_dev_num);
	volatile struct ownership *owner_on_mem;
	if (!l) {
		pr_info(THIS_MOD "dax dev num: %d\n", dax_dev_num);
		cxl_dax_device = dax_dev_get(dax_dev_num);
		if (cxl_dax_device) {
			pr_info(THIS_MOD "got dax_device\n");
			int ret = read_allocation_table();
			if (ret < 0) return ret;
			end_pfn = begin_pfn;
			end_pfn.val = end_pfn.val + FAT_OFFSET - 1;
			pr_info(THIS_MOD "current owner on mem: %d\n", get_owner_on_mem(&owner_on_mem));
		} else {
			pr_info(THIS_MOD "no cxl_dax_device\n");
		}
		
	} else {
		pr_info("no dax dev num:\n");
	}
	
	return 0;
}
EXPORT_SYMBOL(get_cxl_device);

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
			//restart tcp server
			tcp_server_stop();
			set_port(open_port);
			tcp_server_start();
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

	//init cxl
	strscpy(device_path, "/dev/dax0.0", sizeof(device_path)); //default device, can be altered using ioctl
	pr_info(THIS_MOD "using default path: %s\n", device_path);
	ret = get_cxl_device();
	pr_info(THIS_MOD "initialise allocation table at 0x%llx to 0x%llx \n", begin_pfn.val, end_pfn.val);
	memset(alloc_table_start, 0, sizeof(struct ownership));

	//set default ip addr to localhost
	strscpy(ip_4_addr, "127.0.0.1", sizeof("127.0.0.1"));

	//init tcp server
	set_port(open_port);
	ret = tcp_server_start();

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

	//stopping tcp server
	tcp_server_stop();

	//exit done
	pr_info(THIS_MOD "unloaded\n"); 
}


module_init(cxl_range_helper_init);
module_exit(cxl_range_helper_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CXL shared memory area access helper (r/w for every node)");
