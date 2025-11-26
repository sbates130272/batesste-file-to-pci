/*
 * file_to_pcie.c - Kernel module to find PCIe devices associated
 * with a file segment
 *
 * This module provides an ioctl interface that takes a file
 * descriptor, offset, and length, then walks from the struct file
 * to find PCIe devices associated with the underlying block device.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/rcupdate.h>
#include <linux/file.h>
#include <linux/bio.h>
#include <linux/blk_types.h>
#include <linux/string.h>
#include "file_to_pcie.h"

#define DEVICE_NAME "file_to_pcie"
#define CLASS_NAME "file_to_pcie"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Find PCIe devices for file segments");
MODULE_VERSION("0.1");

static int major_number;
static struct class *file_to_pcie_class = NULL;
static struct device *file_to_pcie_device = NULL;
static struct cdev file_to_pcie_cdev;

/*
 * Get struct file* from file descriptor number
 * This requires access to the current task's files_struct
 */
static struct file *get_file_from_fd(int fd)
{
    struct file *filp = NULL;
    struct files_struct *files;
    struct fdtable *fdt;

    if (fd < 0)
        return NULL;

    rcu_read_lock();
    files = current->files;
    if (!files)
        goto out_unlock;

    fdt = files_fdtable(files);
    if (fd >= fdt->max_fds)
        goto out_unlock;

    filp = rcu_dereference(fdt->fd[fd]);
    if (filp)
        get_file(filp);
    else
        filp = NULL;

out_unlock:
    rcu_read_unlock();
    return filp;
}

/*
 * Check if filesystem is a pseudo filesystem (no block device)
 */
static bool is_pseudo_filesystem(struct super_block *sb)
{
    if (!sb)
        return false;

    /* Pseudo filesystems don't have a block device */
    if (!sb->s_bdev)
        return true;

    /* Check filesystem type name for common pseudo filesystems */
    if (sb->s_type && sb->s_type->name) {
        const char *name = sb->s_type->name;
        if (strcmp(name, "proc") == 0 ||
            strcmp(name, "sysfs") == 0 ||
            strcmp(name, "tmpfs") == 0 ||
            strcmp(name, "devtmpfs") == 0 ||
            strcmp(name, "devpts") == 0 ||
            strcmp(name, "cgroup") == 0 ||
            strcmp(name, "cgroup2") == 0 ||
            strcmp(name, "pstore") == 0 ||
            strcmp(name, "debugfs") == 0 ||
            strcmp(name, "tracefs") == 0 ||
            strcmp(name, "securityfs") == 0 ||
            strcmp(name, "configfs") == 0 ||
            strcmp(name, "hugetlbfs") == 0 ||
            strcmp(name, "mqueue") == 0 ||
            strcmp(name, "bpf") == 0)
            return true;
    }

    return false;
}

/*
 * Check if filesystem is a network filesystem
 */
static bool is_network_filesystem(struct super_block *sb)
{
    if (!sb || !sb->s_type || !sb->s_type->name)
        return false;

    const char *name = sb->s_type->name;
    return (strcmp(name, "nfs") == 0 ||
            strcmp(name, "nfs4") == 0 ||
            strcmp(name, "cifs") == 0 ||
            strcmp(name, "smb3") == 0 ||
            strcmp(name, "9p") == 0 ||
            strcmp(name, "ceph") == 0 ||
            strcmp(name, "glusterfs") == 0 ||
            strcmp(name, "fuse") == 0);
}

/*
 * Walk from struct file to find the underlying block device
 * Supports both block device files and regular files on filesystems
 * Returns NULL if file is on pseudo filesystem or network filesystem
 */
static struct block_device *get_block_device_from_file(
    struct file *filp)
{
    struct inode *inode;
    struct super_block *sb;
    struct block_device *bdev = NULL;

    if (!filp)
        return NULL;

    inode = file_inode(filp);
    if (!inode)
        return NULL;

    /* Case 1: Block device file (e.g., /dev/sda1, /dev/md0) */
    if (S_ISBLK(inode->i_mode)) {
        bdev = I_BDEV(inode);
        if (bdev)
            bdget(bdev->bd_dev);
        return bdev;
    }

    /* Case 2: Regular file on a filesystem */
    if (S_ISREG(inode->i_mode)) {
        sb = inode->i_sb;
        if (!sb)
            return NULL;

        /* Check for pseudo filesystems (proc, sysfs, tmpfs, etc.) */
        if (is_pseudo_filesystem(sb))
            return NULL;

        /* Check for network filesystems */
        if (is_network_filesystem(sb))
            return NULL;

        /* Get the underlying block device from the superblock */
        if (sb->s_bdev) {
            bdev = sb->s_bdev;
            bdget(bdev->bd_dev);
        }
    }

    return bdev;
}

/*
 * Calculate block device sector range for a file segment
 * Returns 0 on success, negative error code on failure
 *
 * For block device files: Direct byte-to-sector mapping
 * For regular files: Approximate mapping based on filesystem block size
 *                    Note: This is an approximation. Actual physical
 *                    sectors depend on filesystem layout, fragmentation,
 *                    and metadata placement.
 */
static int calculate_sector_range(struct file *filp,
                                  loff_t file_offset,
                                  size_t length,
                                  loff_t *sector_start,
                                  loff_t *sector_end)
{
    struct inode *inode;
    struct super_block *sb;
    unsigned int blkbits;
    loff_t start_sector, end_sector;
    sector_t logical_block_start, logical_block_end;

    if (!filp || !sector_start || !sector_end)
        return -EINVAL;

    inode = file_inode(filp);
    if (!inode)
        return -ENODEV;

    /* Case 1: Block device files (e.g., /dev/sda1) */
    /* Offset directly maps to device sectors */
    if (S_ISBLK(inode->i_mode)) {
        *sector_start = file_offset >> 9; /* Convert bytes to sectors */
        *sector_end = (file_offset + length - 1) >> 9;
        return 0;
    }

    /* Case 2: Regular files on filesystems (e.g., /tmp/file.txt) */
    /* Convert file offset to filesystem logical blocks, then to sectors */
    if (S_ISREG(inode->i_mode)) {
        sb = inode->i_sb;
        if (!sb || !sb->s_bdev)
            return -ENODEV;

        blkbits = sb->s_blocksize_bits;
        if (blkbits < 9) /* Sanity check - blocksize must be >= 512 */
            return -EINVAL;

        /* Calculate filesystem logical block numbers */
        logical_block_start = file_offset >> blkbits;
        logical_block_end = (file_offset + length - 1) >> blkbits;

        /* Convert filesystem blocks to device sectors */
        /* Note: This doesn't account for:
         * - Filesystem metadata (superblock, inode tables, etc.)
         * - File fragmentation
         * - Partition offsets (handled by block device layer)
         */
        start_sector = logical_block_start << (blkbits - 9);
        end_sector = ((logical_block_end + 1) << (blkbits - 9)) - 1;

        *sector_start = start_sector;
        *sector_end = end_sector;
        return 0;
    }

    return -ENODEV;
}

/*
 * Find PCIe devices associated with a block device and map file segment
 * This walks through the device hierarchy to find PCI devices and
 * calculates which parts of the file segment map to each device
 */
static int find_pcie_devices_for_bdev(struct block_device *bdev,
                                      struct file *filp,
                                      struct file_to_pcie_request *req)
{
    struct device *dev;
    struct pci_dev *pdev;
    loff_t sector_start, sector_end;
    int count = 0;
    int ret;

    if (!bdev || !req)
        return -EINVAL;

    /* Calculate sector range for the file segment */
    ret = calculate_sector_range(filp, req->offset, req->length,
                                 &sector_start, &sector_end);
    if (ret < 0)
        return ret;

    /* Get the gendisk from the block device */
    if (!bdev->bd_disk)
        return -ENODEV;

    dev = disk_to_dev(bdev->bd_disk);
    if (!dev)
        return -ENODEV;

    /* Walk up the device hierarchy to find PCI devices */
    while (dev && count < MAX_PCIE_DEVICES) {
        /* Check if this device is a PCI device */
        pdev = to_pci_dev(dev);
        if (pdev) {
            req->pcie_devices[count].vendor_id = pdev->vendor;
            req->pcie_devices[count].device_id = pdev->device;
            req->pcie_devices[count].bus = pdev->bus->number;
            req->pcie_devices[count].device =
                PCI_SLOT(pdev->devfn);
            req->pcie_devices[count].function =
                PCI_FUNC(pdev->devfn);
            snprintf(req->pcie_devices[count].name,
                     sizeof(req->pcie_devices[count].name),
                     "%s", pci_name(pdev));

            /* Map the entire file segment to this PCIe device */
            /* For now, all segments map to the same device(s) */
            req->pcie_devices[count].file_offset_start = req->offset;
            req->pcie_devices[count].file_offset_end =
                req->offset + req->length - 1;
            req->pcie_devices[count].sector_start = sector_start;
            req->pcie_devices[count].sector_end = sector_end;

            count++;
        }

        /* Move to parent device */
        dev = dev->parent;
    }

    req->pcie_count = count;
    return count;
}

/*
 * IOCTL handler
 */
static long file_to_pcie_ioctl(struct file *filp, unsigned int cmd,
                                unsigned long arg)
{
    struct file_to_pcie_request req;
    struct file *target_file = NULL;
    struct block_device *bdev = NULL;
    long ret = 0;

    if (_IOC_TYPE(cmd) != FILE_TO_PCIE_IOC_MAGIC)
        return -ENOTTY;

    if (_IOC_NR(cmd) != 1)
        return -ENOTTY;

    /* Copy request from userspace */
    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    /* Get the struct file* from the file descriptor */
    target_file = get_file_from_fd(req.fd);
    if (!target_file) {
        ret = -EBADF;
        goto out;
    }

    /* Get the block device from the file */
    bdev = get_block_device_from_file(target_file);
    if (!bdev) {
        struct inode *inode = file_inode(target_file);
        struct super_block *sb = NULL;

        if (inode && S_ISREG(inode->i_mode))
            sb = inode->i_sb;

        /* Check if this is a pseudo filesystem */
        if (sb && is_pseudo_filesystem(sb)) {
            ret = -ENOTSUPP; /* Operation not supported */
            goto out_file;
        }

        /* Check if this is a network filesystem */
        if (sb && is_network_filesystem(sb)) {
            ret = -ENOTSUPP; /* Operation not supported */
            goto out_file;
        }

        /* No block device found */
        ret = -ENODEV;
        goto out_file;
    }

    /* Find PCIe devices */
    ret = find_pcie_devices_for_bdev(bdev, target_file, &req);
    if (ret < 0)
        goto out_bdev;

    /* If no PCIe devices found, that's OK - return success with count=0 */
    /* This handles non-PCIe block devices (e.g., USB, SCSI, etc.) */
    req.pcie_count = (ret >= 0) ? ret : 0;

    /* Copy results back to userspace */
    if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
        ret = -EFAULT;
        goto out_bdev;
    }

    ret = 0; /* Success */

out_bdev:
    if (bdev)
        bdput(bdev);
out_file:
    if (target_file)
        fput(target_file);
out:
    return ret;
}

/*
 * File operations
 */
static int file_to_pcie_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int file_to_pcie_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = file_to_pcie_open,
    .release = file_to_pcie_release,
    .unlocked_ioctl = file_to_pcie_ioctl,
};

/*
 * Module initialization
 */
static int __init file_to_pcie_init(void)
{
    dev_t dev = 0;

    /* Allocate major number */
    if (alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME) < 0) {
        pr_err("Failed to allocate chrdev region\n");
        return -1;
    }
    major_number = MAJOR(dev);

    /* Initialize cdev */
    cdev_init(&file_to_pcie_cdev, &fops);
    if (cdev_add(&file_to_pcie_cdev, dev, 1) < 0) {
        pr_err("Failed to add cdev\n");
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    /* Create device class */
    file_to_pcie_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(file_to_pcie_class)) {
        pr_err("Failed to create device class\n");
        cdev_del(&file_to_pcie_cdev);
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    /* Create device */
    file_to_pcie_device = device_create(file_to_pcie_class, NULL, dev,
                                        NULL, DEVICE_NAME);
    if (IS_ERR(file_to_pcie_device)) {
        pr_err("Failed to create device\n");
        class_destroy(file_to_pcie_class);
        cdev_del(&file_to_pcie_cdev);
        unregister_chrdev_region(dev, 1);
        return -1;
    }

    pr_info("file_to_pcie module loaded (major %d)\n", major_number);
    return 0;
}

/*
 * Module cleanup
 */
static void __exit file_to_pcie_exit(void)
{
    dev_t dev = MKDEV(major_number, 0);

    device_destroy(file_to_pcie_class, dev);
    class_destroy(file_to_pcie_class);
    cdev_del(&file_to_pcie_cdev);
    unregister_chrdev_region(dev, 1);

    pr_info("file_to_pcie module unloaded\n");
}

module_init(file_to_pcie_init);
module_exit(file_to_pcie_exit);

