// SPDX-License-Identifier: GPL-2.0
/*
 * Intel Security Manager for FPGA
 *
 * Copyright (C) 2019-2020 Intel Corporation, Inc.
 */

#include <linux/fpga/ifpga-sec-mgr.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

static DEFINE_IDA(ifpga_sec_mgr_ida);
static struct class *ifpga_sec_mgr_class;

static ssize_t show_canceled_csk(struct ifpga_sec_mgr *imgr,
				 sysfs_csk_hndlr_t get_csk,
				 sysfs_csk_nbits_t get_csk_nbits,
				 char *buf)
{
	unsigned long *csk_map = NULL;
	unsigned int nbits;
	int cnt, ret;

	ret = get_csk_nbits(imgr);
	if (ret < 0)
		return ret;

	nbits = (unsigned int)ret;
	csk_map = vmalloc(sizeof(unsigned long) * BITS_TO_LONGS(nbits));
	if (!csk_map)
		return -ENOMEM;

	ret = get_csk(imgr, csk_map, nbits);
	if (ret)
		goto vfree_exit;

	cnt = bitmap_print_to_pagebuf(1, buf, csk_map, nbits);

vfree_exit:
	vfree(csk_map);
	return ret ? : cnt;
}

static ssize_t show_root_entry_hash(struct ifpga_sec_mgr *imgr,
				    sysfs_reh_hndlr_t get_reh,
				    sysfs_reh_size_t get_reh_size,
				    char *buf)
{
	unsigned int size, i;
	int ret, cnt = 0;
	u8 *hash;

	ret = get_reh_size(imgr);
	if (ret < 0)
		return ret;
	else if (!ret)
		return sprintf(buf, "hash not programmed\n");

	size = (unsigned int)ret;
	hash = vmalloc(size);
	if (!hash)
		return -ENOMEM;

	ret = get_reh(imgr, hash, size);
	if (ret)
		goto vfree_exit;

	cnt += sprintf(buf, "0x");
	for (i = 0; i < size; i++)
		cnt += sprintf(buf + cnt, "%02x", hash[i]);
	cnt += sprintf(buf + cnt, "\n");

vfree_exit:
	vfree(hash);
	return ret ? : cnt;
}

#define to_sec_mgr(d) container_of(d, struct ifpga_sec_mgr, dev)

#define DEVICE_ATTR_SEC_CSK(_name) \
static ssize_t _name##_canceled_csks_show(struct device *dev, \
					  struct device_attribute *attr, \
					  char *buf) \
{ \
	struct ifpga_sec_mgr *imgr = to_sec_mgr(dev); \
	return show_canceled_csk(imgr, \
	       imgr->iops->_name##_canceled_csks, \
	       imgr->iops->_name##_canceled_csk_nbits, buf); \
} \
static DEVICE_ATTR_RO(_name##_canceled_csks)

#define DEVICE_ATTR_SEC_ROOT_ENTRY_HASH(_name) \
static ssize_t _name##_root_entry_hash_show(struct device *dev, \
				     struct device_attribute *attr, \
				     char *buf) \
{ \
	struct ifpga_sec_mgr *imgr = to_sec_mgr(dev); \
	return show_root_entry_hash(imgr, \
	       imgr->iops->_name##_root_entry_hash, \
	       imgr->iops->_name##_reh_size, buf); \
} \
static DEVICE_ATTR_RO(_name##_root_entry_hash)

#define DEVICE_ATTR_SEC_FLASH_CNT(_name) \
static ssize_t _name##_flash_count_show(struct device *dev, \
		    struct device_attribute *attr, char *buf) \
{ \
	struct ifpga_sec_mgr *imgr = to_sec_mgr(dev); \
	int cnt = imgr->iops->_name##_flash_count(imgr); \
	return cnt < 0 ? cnt : sprintf(buf, "%d\n", cnt); \
} \
static DEVICE_ATTR_RO(_name##_flash_count)

DEVICE_ATTR_SEC_ROOT_ENTRY_HASH(sr);
DEVICE_ATTR_SEC_ROOT_ENTRY_HASH(pr);
DEVICE_ATTR_SEC_ROOT_ENTRY_HASH(bmc);
DEVICE_ATTR_SEC_FLASH_CNT(user);
DEVICE_ATTR_SEC_FLASH_CNT(bmc);
DEVICE_ATTR_SEC_CSK(sr);
DEVICE_ATTR_SEC_CSK(pr);
DEVICE_ATTR_SEC_CSK(bmc);

static struct attribute *sec_mgr_security_attrs[] = {
	&dev_attr_user_flash_count.attr,
	&dev_attr_bmc_flash_count.attr,
	&dev_attr_bmc_root_entry_hash.attr,
	&dev_attr_sr_root_entry_hash.attr,
	&dev_attr_pr_root_entry_hash.attr,
	&dev_attr_sr_canceled_csks.attr,
	&dev_attr_pr_canceled_csks.attr,
	&dev_attr_bmc_canceled_csks.attr,
	NULL,
};

#define check_attr(attribute, _name) \
	((attribute) == &dev_attr_##_name.attr && imgr->iops->_name)

static umode_t sec_mgr_visible(struct kobject *kobj,
			       struct attribute *attr, int n)
{
	struct ifpga_sec_mgr *imgr = to_sec_mgr(kobj_to_dev(kobj));

	if (check_attr(attr, user_flash_count) ||
	    check_attr(attr, bmc_flash_count) ||
	    check_attr(attr, bmc_root_entry_hash) ||
	    check_attr(attr, sr_root_entry_hash) ||
	    check_attr(attr, pr_root_entry_hash) ||
	    check_attr(attr, sr_canceled_csks) ||
	    check_attr(attr, pr_canceled_csks) ||
	    check_attr(attr, bmc_canceled_csks))
		return attr->mode;

	return 0;
}

static struct attribute_group sec_mgr_security_attr_group = {
	.name = "security",
	.attrs = sec_mgr_security_attrs,
	.is_visible = sec_mgr_visible,
};

static ssize_t name_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct ifpga_sec_mgr *imgr = to_sec_mgr(dev);

	return sprintf(buf, "%s\n", imgr->name);
}
static DEVICE_ATTR_RO(name);

static struct attribute *sec_mgr_attrs[] = {
	&dev_attr_name.attr,
	NULL,
};

static struct attribute_group sec_mgr_attr_group = {
	.attrs = sec_mgr_attrs,
};

static const struct attribute_group *ifpga_sec_mgr_attr_groups[] = {
	&sec_mgr_attr_group,
	&sec_mgr_security_attr_group,
	NULL,
};

static bool check_sysfs_handler(struct device *dev,
				void *sysfs_handler, void *size_handler,
				const char *sysfs_handler_name,
				const char *size_handler_name)
{
	if (sysfs_handler) {
		if (!size_handler) {
			dev_err(dev, "%s registered without %s\n",
				sysfs_handler_name, size_handler_name);
			return false;
		}
	} else if (size_handler) {
		dev_err(dev, "%s registered without %s\n",
			size_handler_name, sysfs_handler_name);
		return false;
	}
	return true;
}

#define check_reh_handler(_dev, _iops, _name) \
	check_sysfs_handler(_dev, _iops->_name##_root_entry_hash, \
			    _iops->_name##_reh_size, \
			    __stringify(_name##_root_entry_hash), \
			    __stringify(_name##_reh_size))

#define check_csk_handler(_dev, _iops, _name) \
	check_sysfs_handler(_dev, _iops->_name##_canceled_csks, \
			    _iops->_name##_canceled_csk_nbits, \
			    __stringify(_name##_canceled_csks), \
			    __stringify(_name##_canceled_csk_nbits))

/**
 * ifpga_sec_mgr_register - register an IFPGA security manager struct
 *
 * @dev:  create ifpga security manager device from pdev
 * @name: ifpga security manager name
 * @iops: pointer to a structure of ifpga callback functions
 * @priv: ifpga security manager private data
 *
 * Returns &struct ifpga_sec_mgr pointer on success, or ERR_PTR() on error.
 */
struct ifpga_sec_mgr *
ifpga_sec_mgr_register(struct device *dev, const char *name,
		       const struct ifpga_sec_mgr_ops *iops, void *priv)
{
	struct ifpga_sec_mgr *imgr;
	int id, ret;

	if (!check_reh_handler(dev, iops, bmc) ||
	    !check_reh_handler(dev, iops, sr) ||
	    !check_reh_handler(dev, iops, pr) ||
	    !check_csk_handler(dev, iops, bmc) ||
	    !check_csk_handler(dev, iops, sr) ||
	    !check_csk_handler(dev, iops, pr)) {
		return ERR_PTR(-EINVAL);
	}

	if (!name || !strlen(name)) {
		dev_err(dev, "Attempt to register with no name!\n");
		return ERR_PTR(-EINVAL);
	}

	imgr = kzalloc(sizeof(*imgr), GFP_KERNEL);
	if (!imgr)
		return ERR_PTR(-ENOMEM);

	imgr->name = name;
	imgr->priv = priv;
	imgr->iops = iops;
	mutex_init(&imgr->lock);

	id = ida_simple_get(&ifpga_sec_mgr_ida, 0, 0, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto exit_free;
	}

	imgr->dev.class = ifpga_sec_mgr_class;
	imgr->dev.parent = dev;
	imgr->dev.id = id;

	ret = dev_set_name(&imgr->dev, "ifpga_sec%d", id);
	if (ret) {
		dev_err(dev, "Failed to set device name: ifpga_sec%d\n", id);
		ida_simple_remove(&ifpga_sec_mgr_ida, id);
		goto exit_free;
	}

	ret = device_register(&imgr->dev);
	if (ret) {
		put_device(&imgr->dev);
		return ERR_PTR(ret);
	}

	return imgr;

exit_free:
	kfree(dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(ifpga_sec_mgr_register);

/**
 * ifpga_sec_mgr_unregister - unregister a IFPGA security manager
 *
 * @mgr: fpga manager struct
 *
 * This function is intended for use in a IFPGA security manager
 * driver's remove() function.
 */
void ifpga_sec_mgr_unregister(struct ifpga_sec_mgr *imgr)
{
	dev_info(&imgr->dev, "%s %s\n", __func__, imgr->name);

	device_unregister(&imgr->dev);
}
EXPORT_SYMBOL_GPL(ifpga_sec_mgr_unregister);

static void ifpga_sec_mgr_dev_release(struct device *dev)
{
	struct ifpga_sec_mgr *imgr = to_sec_mgr(dev);

	mutex_destroy(&imgr->lock);
	ida_simple_remove(&ifpga_sec_mgr_ida, imgr->dev.id);
	kfree(imgr);
}

static int __init ifpga_sec_mgr_class_init(void)
{
	pr_info("Intel FPGA Security Manager\n");

	ifpga_sec_mgr_class = class_create(THIS_MODULE, "ifpga_sec_mgr");
	if (IS_ERR(ifpga_sec_mgr_class))
		return PTR_ERR(ifpga_sec_mgr_class);

	ifpga_sec_mgr_class->dev_groups = ifpga_sec_mgr_attr_groups;
	ifpga_sec_mgr_class->dev_release = ifpga_sec_mgr_dev_release;

	return 0;
}

static void __exit ifpga_sec_mgr_class_exit(void)
{
	class_destroy(ifpga_sec_mgr_class);
	ida_destroy(&ifpga_sec_mgr_ida);
}

MODULE_DESCRIPTION("Intel FPGA Security Manager Driver");
MODULE_LICENSE("GPL v2");

subsys_initcall(ifpga_sec_mgr_class_init);
module_exit(ifpga_sec_mgr_class_exit)