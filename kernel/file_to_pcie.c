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
#include <linux/pci.h>
#include <linux/version.h>

/* genhd.h provides disk_to_dev macro in older kernels */
/* In kernel 6.8+, genhd.h was removed and disk_to_dev moved to blkdev.h */
/* Check kernel version: KERNEL_VERSION(major, minor, patch) */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 8, 0)
#include <linux/genhd.h>
#endif
/* For kernel 6.8+, disk_to_dev is already defined in blkdev.h */
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
MODULE_AUTHOR("Stephen Bates <sbates@raithlin.com>");
MODULE_DESCRIPTION("Finds PCIe devices that provide DMA for file segments");
MODULE_VERSION("0.1");

static int major_number;
static struct class *file_to_pcie_class = NULL;
static struct device *file_to_pcie_device = NULL;
static struct cdev file_to_pcie_cdev;

/*
 * Get struct file* from file descriptor number
 * Uses fget() which is the standard kernel function for this purpose
 */
static struct file *get_file_from_fd(int fd)
{
    struct file *filp = NULL;

    if (fd < 0)
        return NULL;

    /* fget() gets the file from the current process's file descriptor table
     * and increments the reference count. We must call fput() when done. */
    rcu_read_lock();
    filp = fget(fd);
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

    /* Check filesystem type name first - this is the most reliable way
     * to identify pseudo filesystems. Some real filesystems (like btrfs)
     * may not set sb->s_bdev, so we need to check the type name before
     * checking s_bdev */
    if (sb->s_type && sb->s_type->name) {
        const char *name = sb->s_type->name;
        
        /* Known real filesystems that may not have sb->s_bdev set */
        if (strcmp(name, "btrfs") == 0 ||
            strcmp(name, "ext4") == 0 ||
            strcmp(name, "ext3") == 0 ||
            strcmp(name, "ext2") == 0 ||
            strcmp(name, "xfs") == 0)
            return false;
        
        /* Known pseudo filesystems */
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

    /* If we can't identify by name, check if it has a block device */
    /* Pseudo filesystems don't have a block device */
    if (!sb->s_bdev)
        return true;

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
        /* I_BDEV gets the block_device from the inode */
        /* For block device files, the inode's i_bdev field contains the block_device */
        /* Note: I_BDEV only works if the block device file has been opened */
        /* For raw block device files that haven't been opened, I_BDEV returns NULL */
        bdev = I_BDEV(inode);
        /* If I_BDEV returns NULL, the block device file hasn't been opened yet */
        /* In this case, we cannot get the block_device without opening the file */
        /* The user should open the block device file first, or use a file on a filesystem */
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
        /* For most filesystems, sb->s_bdev is set */
        /* Note: sb->s_bdev may be NULL if the filesystem hasn't been
         * fully initialized or if it's a filesystem type that doesn't
         * use a single block device (e.g., btrfs with multiple devices).
         * For ext4/xfs/etc. on a single device, sb->s_bdev should be set. */
        if (sb->s_bdev) {
            bdev = sb->s_bdev;
            /* sb->s_bdev already holds a reference, no need for bdget */
        }
        /* Note: If sb->s_bdev is NULL, we cannot get the block device */
        /* This can happen for some filesystems (like btrfs) that don't
         * set sb->s_bdev. For ext4/xfs/etc., sb->s_bdev should always be set.
         * If sb->s_bdev is NULL for ext4/xfs, this indicates a problem that
         * needs to be investigated (see dmesg for debug messages). */
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

    /* Get the device from the block device */
    if (!bdev->bd_disk)
        return -ENODEV;

    /* disk_to_dev macro:
     * - In kernels < 6.8: defined in genhd.h as &disk->part0.__dev
     * - In kernels >= 6.8: defined in blkdev.h as &disk->part0->bd_device
     * 
     * Safety: Validate disk and disk->part0 before using disk_to_dev macro
     */
    {
        struct gendisk *disk = bdev->bd_disk;
        
        /* Validate disk is a valid kernel pointer */
        if ((unsigned long)disk < 0xffff800000000000UL) {
            return -ENODEV;
        }
        
        /* Validate disk->part0 exists before accessing it */
        if (!disk->part0) {
            return -ENODEV;
        }
        
        /* Validate disk->part0 is a valid kernel pointer */
        if ((unsigned long)disk->part0 < 0xffff800000000000UL) {
            return -ENODEV;
        }
        
        /* Now safe to use disk_to_dev macro */
        dev = disk_to_dev(disk);
    }
    
    if (!dev)
        return -ENODEV;
    
    /* Validate dev is a valid kernel pointer */
    if ((unsigned long)dev < 0xffff800000000000UL) {
        return -ENODEV;
    }

    /* Walk up the device hierarchy to find PCI devices */
    while (dev && count < MAX_PCIE_DEVICES) {
        /* Safety check: validate dev is a valid kernel pointer before accessing ANY members */
        if ((unsigned long)dev < 0xffff800000000000UL) {
            break;
        }
        
        /* Check if this device is a PCI device using dev_is_pci */
        if (dev_is_pci(dev)) {
            pdev = to_pci_dev(dev);
            /* Safety check: pdev->bus should never be NULL for valid PCI devices */
            if (!pdev || !pdev->bus) {
                /* Move to parent device */
                struct device *next_dev = dev->parent;
                if (!next_dev || (unsigned long)next_dev < 0xffff800000000000UL) {
                    break;
                }
                dev = next_dev;
                continue;
            }
            
            /* Only include NVMe controllers, not PCIe bridges */
            /* NVMe controllers have PCI class code PCI_CLASS_STORAGE_EXPRESS (0x010802):
             *   Base class: 0x01 (Mass Storage Controller)
             *   Subclass: 0x08 (Non-Volatile Memory Controller)
             *   Programming interface: 0x02 (NVM Express)
             */
            if ((pdev->class & 0xffffff00) == (PCI_CLASS_STORAGE_EXPRESS & 0xffffff00)) {
                /* This is an NVMe controller - add it to the list */
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
                req->pcie_devices[count].file_offset_start = req->offset;
                req->pcie_devices[count].file_offset_end =
                    req->offset + req->length - 1;
                req->pcie_devices[count].sector_start = sector_start;
                req->pcie_devices[count].sector_end = sector_end;

                count++;
            }
            /* If it's not an NVMe controller, continue walking up the hierarchy
             * to find NVMe controllers (e.g., in RAID scenarios with multiple
             * controllers) */
        }

        /* Move to parent device */
        /* Validate dev->parent before accessing it */
        {
            struct device *next_dev = dev->parent;
            /* If next_dev is NULL or invalid, break the loop */
            if (!next_dev || (unsigned long)next_dev < 0xffff800000000000UL) {
                break;
            }
            dev = next_dev;
        }
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

        /* Debug: Log filesystem type if sb->s_bdev is NULL */
        if (sb && sb->s_type && sb->s_type->name) {
            pr_warn("file_to_pcie: sb->s_bdev is NULL for filesystem type: %s (dev: %u:%u)\n",
                    sb->s_type->name,
                    MAJOR(sb->s_dev), MINOR(sb->s_dev));
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
    /* No need to bdput - we didn't take an extra reference */
    /* sb->s_bdev already holds a reference */
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
    /* In kernel 6.8+, class_create only takes the class name */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
    file_to_pcie_class = class_create(CLASS_NAME);
#else
    file_to_pcie_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
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

