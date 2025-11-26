/*
 * test_file_to_pcie.c - Userspace test program for file_to_pcie
 * kernel module
 *
 * This program opens a file, calls the ioctl with the file
 * descriptor, offset, and length, then displays the PCIe devices
 * found.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>
#include "file_to_pcie.h"

#ifndef ENOTSUPP
#define ENOTSUPP 524
#endif

#define DEVICE_PATH "/dev/file_to_pcie"

static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s <file_path> <offset> <length>\n",
            prog_name);
    fprintf(stderr, "\n");
    fprintf(stderr, "Example: %s /dev/sda1 0 4096\n", prog_name);
    fprintf(stderr, "         %s /tmp/testfile 0 1024\n",
            prog_name);
}

static void print_pcie_devices(struct file_to_pcie_request *req)
{
    int i;

    printf("Found %d PCIe device(s):\n", req->pcie_count);
    printf("----------------------------------------\n");

    for (i = 0; i < req->pcie_count; i++) {
        printf("Device %d:\n", i + 1);
        printf("  Name: %s\n", req->pcie_devices[i].name);
        printf("  Vendor ID: 0x%04x\n",
               req->pcie_devices[i].vendor_id);
        printf("  Device ID: 0x%04x\n",
               req->pcie_devices[i].device_id);
        printf("  Bus: 0x%02x\n", req->pcie_devices[i].bus);
        printf("  Device: 0x%02x\n",
               req->pcie_devices[i].device);
        printf("  Function: 0x%02x\n",
               req->pcie_devices[i].function);
        printf("  File Offset Range: %lld - %lld (length: %lld)\n",
               (long long)req->pcie_devices[i].file_offset_start,
               (long long)req->pcie_devices[i].file_offset_end,
               (long long)(req->pcie_devices[i].file_offset_end -
                           req->pcie_devices[i].file_offset_start + 1));
        printf("  Sector Range: %lld - %lld\n",
               (long long)req->pcie_devices[i].sector_start,
               (long long)req->pcie_devices[i].sector_end);
        printf("\n");
    }
}

int main(int argc, char *argv[])
{
    int dev_fd, file_fd;
    struct file_to_pcie_request req;
    long offset;
    size_t length;
    int ret;

    if (argc != 4) {
        print_usage(argv[0]);
        return 1;
    }

    /* Parse arguments */
    offset = strtol(argv[2], NULL, 0);
    length = strtoul(argv[3], NULL, 0);

    if (offset < 0) {
        fprintf(stderr, "Error: offset must be >= 0\n");
        return 1;
    }

    if (length == 0) {
        fprintf(stderr, "Error: length must be > 0\n");
        return 1;
    }

    /* Open the device file */
    dev_fd = open(DEVICE_PATH, O_RDWR);
    if (dev_fd < 0) {
        perror("Failed to open device");
        fprintf(stderr, "Make sure the module is loaded and "
                "device exists: %s\n", DEVICE_PATH);
        return 1;
    }

    /* Open the target file */
    file_fd = open(argv[1], O_RDONLY);
    if (file_fd < 0) {
        perror("Failed to open target file");
        close(dev_fd);
        return 1;
    }

    /* Prepare request */
    memset(&req, 0, sizeof(req));
    req.fd = file_fd;
    req.offset = offset;
    req.length = length;

    printf("Querying PCIe devices for:\n");
    printf("  File: %s\n", argv[1]);
    printf("  Offset: %ld\n", offset);
    printf("  Length: %zu\n", length);
    printf("\n");

    /* Call ioctl */
    ret = ioctl(dev_fd, FILE_TO_PCIE_IOCTL_GET_PCIE, &req);
    if (ret < 0) {
        switch (errno) {
        case ENOTSUPP:
            fprintf(stderr, "Error: File is on a pseudo filesystem "
                    "(proc, sysfs, tmpfs, etc.) or network filesystem "
                    "(NFS, CIFS, etc.).\n");
            fprintf(stderr, "This operation is not supported for "
                    "these filesystem types.\n");
            break;
        case ENODEV:
            fprintf(stderr, "Error: No block device found for this file.\n");
            fprintf(stderr, "The file may be on a virtual filesystem "
                    "without a backing block device.\n");
            break;
        case EBADF:
            fprintf(stderr, "Error: Invalid file descriptor.\n");
            break;
        default:
            perror("ioctl failed");
            fprintf(stderr, "Error code: %d\n", errno);
            break;
        }
        close(file_fd);
        close(dev_fd);
        return 1;
    }

    /* Print results */
    if (req.pcie_count == 0) {
        printf("No PCIe devices found for this file segment.\n");
        printf("The file is on a block device, but the device is not "
               "connected via PCIe\n");
        printf("(e.g., USB, SCSI, or other bus types).\n");
    } else {
        print_pcie_devices(&req);
    }

    close(file_fd);
    close(dev_fd);

    return 0;
}

