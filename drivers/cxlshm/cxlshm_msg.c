// ffs_handler_ioctl_rw.c
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/maple_tree.h>
#include <linux/string.h>
#include <linux/socket.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/in.h>
#include <net/sock.h>
#include <linux/inet.h>
#include <linux/types.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/kstrtox.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/pid_types.h>
#include <vdso/limits.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/ioport.h>
#include "dax-private.h"
#include <asm-generic/cacheflush.h>
#include <linux/rcupdate.h>


#define DEVICE_NAME             "ffs_sync"
#define CLASS_NAME              "ffs_class"
#define DUMMY_FILE_PATH         "undefined file path, pls setup using ioctl"
#define FILE_PATH_LENGTH        128
#define OPEN_TCP_PORT           57580
#define MAX_BUFFER_NET          128
#define DEFAULT_PORT            57580
#define COMMAND_LENGTH          4
#define FAT_SIZE				2097152
#define FAT_OFFSET				FAT_SIZE / PAGE_SIZE

#define IOCTL_MAGIC             0xCD
#define IOCTL_SET_FILE_PATH     _IOW(IOCTL_MAGIC, 0x01, struct famfs_sync_control_struct)
#define IOCTL_SETUP_NETWORK     _IOW(IOCTL_MAGIC, 0x02, struct famfs_sync_control_struct)
#define IOCTL_TEST_NETWORK      _IOW(IOCTL_MAGIC, 0x69, struct famfs_sync_control_struct)//temporary


struct famfs_sync_control_struct {
	char path[FILE_PATH_LENGTH + 1];
	int port;
};

struct ownership { //TODO: add version to the struct...
	pid_t owner_pid;
	char ip_4_addr[17];
	pfn_t start;
	pfn_t end;
	unsigned long vm_start;
	unsigned long vm_end;
};

static DEFINE_SPINLOCK(ctr_lock);
static char *commands[] = {"SBGN", "REND", "SACK", "SNCK", NULL};
static char ffs_file_path[FILE_PATH_LENGTH + 1];
static int path_length;
static dev_t dev_num, dax_dev_num;
static struct cdev ffs_cdev;
static struct class *ffs_class;
static struct socket *server_socket;
static struct sockaddr_in sin;
static struct task_struct *my_kthread;
static int port = 57580;
static wait_queue_head_t wq;
static int ready = 0;
char message[MAX_BUFFER_NET] = {0};
static void *alloc_table_start;
struct task_struct *the_task;
struct pid *the_pid;
static struct dax_device *cxl_dax_device;
static char device_path[FILE_PATH_LENGTH];
struct ownership *owner_from_net;

int accept_connection(void *socket_in);
int check_commands(char *message);
struct task_struct *get_task_from_int_pid(pid_t pid);
int flush_mem_task(pid_t pid);
int get_cxl_device(void);
struct ownership *get_owner_on_mem(void);

int check_commands(char *message) {
	int result = -1;
	for (int i = 0; commands[i] != NULL; i++) {
		if (!strncmp(message, commands[i], 4)) {
			result = i;
			break;
		}
	}
	return result;
}

static int tcp_server_start(void) {
	int ret = 0;
	if (!server_socket) {
		pr_info("Start TCP server on port %d\n", port);
		
		//initialise socket address
		memset(&sin, 0, sizeof(sin));
		sin.sin_addr.s_addr = INADDR_ANY;
		sin.sin_family = AF_INET;
		sin.sin_port = htons(port);

		//create socket, bind, and listen
		ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &server_socket);
		if (ret < 0) return ret;
		ret = server_socket->ops->bind(server_socket, (struct sockaddr *)&sin, sizeof(sin));
		if (ret < 0) return ret;
		ret = server_socket->ops->listen(server_socket, 1);
		if (ret < 0) return ret;

		//accept connection inkernel_sendmsg separate thread
		my_kthread = kthread_run(accept_connection, (void *)server_socket, "accept_connection");
	}
	return ret;
}

int accept_connection(void *socket_in) {
	int ret_val = 0;
	struct socket *srv_socket = (struct socket *)socket_in;
	struct socket *new_socket;
	char buf[MAX_BUFFER_NET] = {0};
	pr_info("Waiting for connection\n");
	while(!kthread_should_stop()) {
		kernel_accept(srv_socket, &new_socket, 0);
		if (new_socket) {
			struct sockaddr_in connected_client_addr;
			struct msghdr hdr;
			memset(&hdr, 0, sizeof(hdr));
			struct kvec iov = {
				.iov_base = buf,
				.iov_len = sizeof(buf) - 1
			};
			kernel_getpeername(new_socket, (struct sockaddr *)&connected_client_addr);
			pr_info("Connected! client: %pI4\n", &connected_client_addr.sin_addr);
			int len = -1;
			for(;;) {
				len = kernel_recvmsg(new_socket, &hdr, &iov, 1, sizeof(buf) - 1, 0);
				if (len > 0) {
					spin_lock(&ctr_lock);
					memset(message, 0, sizeof(message));
					strscpy(message, buf, sizeof(buf));
					ready = 1;
					spin_unlock(&ctr_lock);
					wake_up_interruptible(&wq);
					pr_info("Data: %s\n", buf);
					int tmp = 0;
					int res = kstrtoint(buf, 10, &tmp);
					if (res < 0) tmp = -1;
					flush_mem_task((pid_t) tmp);
				} else if (len == 0) {
					pr_info("Client closed connection.\n");
					break;
				} else if (len == -EAGAIN) {
					msleep(10);
					int accept_connection(void *socket_in);
				} else {
					ret_val = len;
					break;
				}
				//overwrite buf data with NULL char
				memset(buf, 0, sizeof(buf));			
			}
			sock_release(new_socket);
			new_socket = NULL;
			pr_info("Done receiving data\n");
		}
	}
	pr_info("Acceptor thread exit. Bye\n");
	return ret_val;
}

static void tcp_server_stop(void) {
	if (server_socket) {
		pr_info("Release server socket on port %d\n", OPEN_TCP_PORT);
		sock_release(server_socket);
		server_socket = NULL;
	}
}

struct task_struct *get_task_from_int_pid(pid_t pid) {
	the_pid = find_get_pid(pid);
	return get_pid_task(the_pid, PIDTYPE_PID);
}

int flush_mem_task(pid_t pid) {
	int ret = 0;
	struct vm_area_struct *this_vma = NULL;
	pr_info("pid: %d\n", pid);
	if (pid != -1) {
		the_task = get_task_from_int_pid(pid);
		pr_info("task: %d\n", the_task->pid);
		struct mm_struct *mm = the_task->mm;
		struct vm_area_struct *vma;
		MA_STATE(mas, &mm->mm_mt, 0, 0);

		get_cxl_device();
		struct ownership *o = get_owner_on_mem();
		pr_info("vm_start: 0x%lx\n", o->vm_start);
		int i = 0;
		mas_for_each(&mas, vma, ULONG_MAX) {
			pr_info("vma %d addr: 0x%lx\n", i, vma->vm_start);
			if (vma->vm_start == o->vm_start) {
				this_vma = vma; 
				break;
			}
			i++;
		}
		if (this_vma) {
			flush_cache_range(this_vma, this_vma->vm_start, this_vma->vm_end);
			zap_vma_ptes(this_vma, this_vma->vm_start, this_vma->vm_end - this_vma->vm_start); //temporary
			pr_info("Flush CPU cache. Size: %ld\n", this_vma->vm_end - this_vma->vm_start);
		} else {
			pr_info("VMA not found\n");
		}

		
	} else {
		ret = -1;
	}
	return ret;
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

struct ownership *get_owner_on_mem(void) {
	int ret = 0;
	if (!cxl_dax_device) return NULL;
	if (!dax_alive(cxl_dax_device))
		run_dax(cxl_dax_device);
	pfn_t pfn;
	ret = dax_direct_access(cxl_dax_device, 0, FAT_OFFSET, DAX_ACCESS, &alloc_table_start, &pfn);
	if (ret < 0) return NULL;
	volatile struct ownership *owner_on_mem = (volatile struct ownership *)alloc_table_start;
	return owner_on_mem;

}

int get_cxl_device(void) {
	int l = lookup_daxdev(device_path, &dax_dev_num);
	if (!l) {
		pr_info("dax dev num: %d\n", dax_dev_num);
		cxl_dax_device = dax_dev_get(dax_dev_num);
		if (cxl_dax_device) {
			pr_info("got dax_device\n");
			dax_write_cache(cxl_dax_device, false);
			if (!dax_alive(cxl_dax_device))
				run_dax(cxl_dax_device);
		} else {
			pr_info("no cxl_dax_device\n");
		}
		
	} else {
		pr_info("no dax dev num:\n");
	}
	
	return 0;
}


static long ffs_helper_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
	struct famfs_sync_control_struct rw;

	if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
		return -EFAULT;

	pr_info("Path: %s\n", rw.path);

	switch (cmd) {
		case IOCTL_SET_FILE_PATH:
			path_length = strscpy(ffs_file_path, rw.path, FILE_PATH_LENGTH);
			pr_info("%d char copied to file_path. File path: %s\n", path_length, ffs_file_path);
			break;
		case IOCTL_SETUP_NETWORK:
			if (!rw.port) return -EINVAL;
			port = rw.port;
			pr_info("Server port: %d\n", port);
			tcp_server_stop();
			tcp_server_start();
			break;
		default:
			return -ENOTTY;
	}

	return 0;
}

static unsigned int ffs_helper_poll(struct file *file, poll_table *poll) {
	poll_wait(file, &wq, poll);
	if (ready) {
		return POLLIN | POLLRDNORM; // Data ready to read
	}
	return -EAGAIN;
}

static long int ffs_helper_read(struct file *file, char __user *buf, size_t length, loff_t *offset) {
	if (!ready) 
		return -EAGAIN;
	spin_lock(&ctr_lock);
	ready = !ready;
	int msg_size = sizeof(message);
	if (copy_to_user(buf, message, msg_size) != 0) {
		return -EFAULT;
	}
	memset(message, 0, sizeof(message));
	spin_unlock(&ctr_lock);
	return msg_size;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ffs_helper_ioctl,
	.poll = ffs_helper_poll,
	.read = ffs_helper_read
};

static int __init ffs_helper_init(void) {	
	//init char device
	alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
	cdev_init(&ffs_cdev, &fops);
	cdev_add(&ffs_cdev, dev_num, 1);
	ffs_class = class_create(CLASS_NAME);
	device_create(ffs_class, NULL, dev_num, NULL, DEVICE_NAME);

	//init tcp and poll
	tcp_server_start();
	init_waitqueue_head(&wq);

	//init others
	strscpy(ffs_file_path, DUMMY_FILE_PATH, 64);
	strscpy(device_path, "/dev/dax0.0", 64);
	pr_info("famfs_sync_helper: loaded\n");
	pr_info("%s\n", ffs_file_path);

	return 0;
}

static void __exit ffs_helper_exit(void) {
	//stopping tcp connection stuff
	kthread_stop(my_kthread);
	tcp_server_stop();

	//destroying char devices
	device_destroy(ffs_class, dev_num);
	class_destroy(ffs_class);
	cdev_del(&ffs_cdev);
	unregister_chrdev_region(dev_num, 1);
	pr_info("famfs_sync_helper: unloaded\n"); 
}

module_init(ffs_helper_init);
module_exit(ffs_helper_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FAMFS sync helper for multi-host configuration (r/w for all, not only master)");
