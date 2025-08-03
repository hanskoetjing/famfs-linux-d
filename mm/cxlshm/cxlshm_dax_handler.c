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
extern pid_t owner_pid;

int lookup_daxdev(const char *pathname, dev_t *devno);
int get_cxl_dax_dev(char *device_path_param);
int lookup_daxdevice(const char *pathname, struct dax_device **daxdevice);
int read_owner_info_on_mem(char *device_path_param);
int get_cxl_dax_device(char *device_path_param);

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

int read_owner_info_on_mem(char *device_path_param) 
{
    int ret = 0;
	if (!cxl_dax_device) 
        ret = get_cxl_dax_device(device_path_param);
	if (ret == 0 && !dax_alive(cxl_dax_device))
		run_dax(cxl_dax_device);
    ret = dax_direct_access(cxl_dax_device, 0, FAT_OFFSET, DAX_ACCESS, &alloc_table_start, &begin_pfn);
    end_pfn = begin_pfn;
	end_pfn.val = end_pfn.val + FAT_OFFSET - 1;
	return ret;
}

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
	int lookup_result = lookup_daxdevice(device_path, &cxl_dax_device);
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
		ret = get_cxl_dax_dev(device_path_param);
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