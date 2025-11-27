# file_to_pcie - File to PCIe Device(s) Mapping Module

This kernel module provides an ioctl interface to find PCIe devices
associated with a file segment. Given a file descriptor, offset, and
length, it walks from the `struct file *` pointer to identify the
underlying PCIe device(s).

## Project Structure

```
.
├── include/          # Shared header files
│   └── file_to_pcie.h
├── kernel/           # Kernel module source code
│   ├── file_to_pcie.c
│   └── Makefile
├── user/             # Userspace test program
│   └── test_file_to_pcie.c
├── Makefile          # Top-level build file
└── README.md
```

## Overview

The Linux kernel module performs the following operations:

1. Takes a file descriptor, offset, and length via ioctl
2. Retrieves the `struct file *` associated with the file descriptor
3. Walks from the file to the underlying block device
4. Traverses the device hierarchy to find associated PCIe device(s)
5. Returns PCIe device information (vendor ID, device ID, bus,
   device, function, name) along with file offset and sector ranges

### Supported File Types

The module supports two types of file descriptors:

1. **Block Device Files** (e.g., `/dev/sda1`, `/dev/nvme0n1p1`)
   - Direct mapping: file offset maps directly to device sectors
   - Accurate sector range calculation

2. **Regular Files on Filesystems** (e.g., `/tmp/file.txt`, `/home/user/data.bin`)
   - Maps regular files to their underlying block device via the filesystem superblock
   - Approximate sector range calculation based on filesystem block size
   - Note: Sector ranges are approximations and don't account for:
     - Filesystem metadata (superblocks, inode tables, etc.)
     - File fragmentation
     - Filesystem-specific layout details

## Prerequisites

- Linux kernel headers matching your running kernel
- GCC compiler
- Make
- Root/sudo access (for installation and loading)

## Building

**Important**: Kernel modules should be built as a regular user (without sudo).
Only installation and loading require sudo privileges.

### Build Everything

Build both the kernel module and userspace test program:

```bash
make
```

This will create:
- `kernel/file_to_pcie.ko` - The kernel module
- `user/test_file_to_pcie` - The userspace test program

### Build Only the Kernel Module

```bash
make modules
```

**Note**: Do not use `sudo` when building. Building as root may fail due to
security restrictions preventing root from creating directories in user home
directories.

### Build Only the Test Program

```bash
make user/test_file_to_pcie
```

Or manually:

```bash
gcc -I. -o user/test_file_to_pcie user/test_file_to_pcie.c
```

## Installation

### Install the Module

After building (as a regular user), install the module to the system:

```bash
sudo make install
```

This will:
- Install the module to `/lib/modules/$(uname -r)/updates/file_to_pcie.ko`
- Update module dependencies with `depmod -a`
- Make the module available for loading with `modprobe`

**Note**: The module must be built before installation. Run `make modules` first
(without sudo), then `sudo make install`.

### Verify Installation

After installation, you can verify the module is available:

```bash
modinfo file_to_pcie
```

## Usage

### Load the Module

After installation, load the module:

```bash
sudo make load
# or
sudo modprobe file_to_pcie
```

If the module is not installed, you can load it directly:

```bash
sudo insmod kernel/file_to_pcie.ko
```

### Verify Module is Loaded

Check if the module is loaded:

```bash
lsmod | grep file_to_pcie
```

Check kernel messages:

```bash
dmesg | tail
```

### Run the Test Program

```bash
sudo ./user/test_file_to_pcie <file_path> <offset> <length>
```

Examples:

```bash
# Test with a block device
sudo ./user/test_file_to_pcie /dev/sda1 0 4096

# Test with a regular file
sudo ./user/test_file_to_pcie /tmp/testfile 0 1024

# Test with a specific offset
sudo ./user/test_file_to_pcie /dev/nvme0n1p1 1048576 4096
```

### Unload the Module

Unload the module when done:

```bash
sudo make unload
# or
sudo rmmod file_to_pcie
```

### Uninstall the Module

To remove the module from the system:

```bash
sudo make uninstall
```

This will:
- Remove the module from `/lib/modules/$(uname -r)/updates/` and `/lib/modules/$(uname -r)/extra/`
- Update module dependencies with `depmod -a`

**Note**: Unload the module first before uninstalling.

## How It Works

1. **File Descriptor Resolution**: The module uses the current task's
   `files_struct` to look up the `struct file *` from the file
   descriptor number.

2. **Block Device Discovery**:
   - **Block Device Files** (`/dev/sda1`, etc.): Directly accesses
     the block device via `I_BDEV(inode)`
   - **Regular Files** (`/tmp/file.txt`, etc.): Retrieves the
     underlying block device from the filesystem superblock via
     `inode->i_sb->s_bdev`

3. **Sector Range Calculation**:
   - **Block Device Files**: Direct byte-to-sector conversion
     (`offset >> 9`)
   - **Regular Files**: Approximate calculation based on filesystem
     block size. Converts file offset to filesystem logical blocks,
     then to device sectors. Note: This is an approximation and
     doesn't account for fragmentation or filesystem metadata.

4. **PCIe Device Traversal**: Starting from the block device's gendisk,
   it walks up the device hierarchy using `dev->parent` to find PCI
   devices.

5. **Device Information**: For each PCI device found, it extracts:
   - Vendor ID
   - Device ID
   - Bus number
   - Device number
   - Function number
   - Device name (via `pci_name()`)
   - File offset range (start and end offsets in the file)
   - Sector range (start and end sectors on the block device)

## Supported Filesystem Types

### Fully Supported

- **Block Device Files**: Direct access to block devices (e.g., `/dev/sda1`,
  `/dev/nvme0n1p1`, `/dev/md0` for RAID arrays)
  - Accurate sector mapping
  - Works with RAID arrays (md) - walks up device hierarchy to find
    underlying PCIe devices

- **Regular Files on Local Filesystems**: Files on ext4, xfs, btrfs, etc.
  - Maps to underlying block device via filesystem superblock
  - Approximate sector range calculation

### Error Handling

- **Pseudo Filesystems**: Returns `ENOTSUPP` for files on pseudo filesystems
  (proc, sysfs, tmpfs, devtmpfs, devpts, cgroup, etc.)
  - These filesystems don't have a backing block device

- **Network Filesystems**: Returns `ENOTSUPP` for files on network filesystems
  (NFS, CIFS/SMB, 9p, Ceph, GlusterFS, FUSE, etc.)
  - These filesystems don't have a local block device

- **Non-PCIe Block Devices**: Returns success with `pcie_count = 0` for block
  devices not connected via PCIe (e.g., USB, SCSI, etc.)
  - The device is found, but no PCIe devices are associated with it

## Limitations

- **Regular Files**: Sector ranges are approximations based on filesystem
  block size:
  - Does not account for file fragmentation
  - Does not account for filesystem metadata (superblocks, inode tables,
    etc.)
  - Actual physical sectors may differ from reported ranges
- Requires root privileges to load the module and run tests
- Maximum of 16 PCIe devices can be returned per request

## IOCTL Interface

The ioctl command is `FILE_TO_PCIE_IOCTL_GET_PCIE` with the following
structure:

```c
struct file_to_pcie_request {
    int fd;              // File descriptor (block device or regular file)
    loff_t offset;       // Offset in file
    size_t length;      // Length of segment
    int pcie_count;     // Number of PCIe devices found
    struct {
        unsigned short vendor_id;
        unsigned short device_id;
        unsigned char bus;
        unsigned char device;
        unsigned char function;
        char name[64];
        // File offset range on this PCIe device
        loff_t file_offset_start;
        loff_t file_offset_end;
        // Block device sector range (for reference)
        loff_t sector_start;
        loff_t sector_end;
    } pcie_devices[MAX_PCIE_DEVICES];
};
```

**Note**: The `fd` parameter can be either:
- A file descriptor for a block device file (e.g., `open("/dev/sda1")`)
- A file descriptor for a regular file on a filesystem (e.g., `open("/tmp/file.txt")`)

## Error Codes

The ioctl may return the following error codes:

- **EBADF** (-9): Invalid file descriptor
- **ENODEV** (-19): No block device found for the file (virtual filesystem
  without backing block device)
- **ENOTSUPP** (-524): Operation not supported (pseudo filesystem or
  network filesystem)
- **EFAULT** (-14): Invalid memory access (userspace copy failed)
- **ENOTTY** (-25): Invalid ioctl command

On success, returns 0. If no PCIe devices are found but a block device
exists (e.g., USB or SCSI device), returns 0 with `pcie_count = 0`.

## Troubleshooting

- **Module fails to load**: Check kernel logs with `dmesg` for error
  messages
- **ENOTSUPP error**: File is on a pseudo filesystem (proc, sysfs,
  tmpfs) or network filesystem (NFS, CIFS). Use a file on a local
  block device filesystem instead.
- **ENODEV error**: File is on a virtual filesystem without a backing
  block device. Try a file on a regular filesystem or a block device
  file directly.
- **No PCIe devices found** (pcie_count = 0): The block device exists
  but is not connected via PCIe (e.g., USB, SCSI, or other bus types).
  This is normal and not an error.
- **Permission denied**: Ensure you're running with root privileges
- **Device not found**: Make sure the module is loaded and `/dev/
  file_to_pcie` exists

## License

GPL v2 (as required for kernel modules)

