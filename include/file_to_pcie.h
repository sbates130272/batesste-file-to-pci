/*
 * file_to_pcie.h - Shared header for file_to_pcie kernel module
 * and userspace programs
 */

#ifndef FILE_TO_PCIE_H
#define FILE_TO_PCIE_H

#include <linux/ioctl.h>

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/fs.h>
typedef loff_t file_offset_t;
#else
#include <stdint.h>
#include <sys/types.h>
typedef int64_t file_offset_t;
#endif

#define MAX_PCIE_DEVICES 16

struct file_to_pcie_request {
    int fd;
    file_offset_t offset;
    size_t length;
    int pcie_count;
    struct {
        unsigned short vendor_id;
        unsigned short device_id;
        unsigned char bus;
        unsigned char device;
        unsigned char function;
        char name[64];
        /* File offset range on this PCIe device */
        file_offset_t file_offset_start;
        file_offset_t file_offset_end;
        /* Block device sector range (for reference) */
        file_offset_t sector_start;
        file_offset_t sector_end;
    } pcie_devices[MAX_PCIE_DEVICES];
};

#define FILE_TO_PCIE_IOC_MAGIC 'f'
#define FILE_TO_PCIE_IOCTL_GET_PCIE \
    _IOWR(FILE_TO_PCIE_IOC_MAGIC, 1, \
          struct file_to_pcie_request)

#endif /* FILE_TO_PCIE_H */

