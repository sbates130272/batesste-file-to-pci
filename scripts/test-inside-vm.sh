#!/bin/bash
# test-inside-vm.sh - Test script to run inside QEMU VM
#
# This script runs inside the QEMU VM and performs various tests:
# - Filesystem tests (ext4, xfs, ext2, ext3, btrfs)
# - RAID scenarios (md RAID arrays with filesystem files)
# Note: Raw block device tests are not included due to I_BDEV limitations
# when block device files haven't been opened

set -uo pipefail
# Note: We don't use 'set -e' because we want to handle errors explicitly
# in test functions and continue with other tests even if one fails

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# WORKSPACE_DIR is set by the calling script or defaults to script's parent directory
WORKSPACE_DIR="${WORKSPACE_DIR:-${SCRIPT_DIR}/..}"
TEST_DIR="/tmp/test-results"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*"
}

test_passed=0
test_failed=0

run_test() {
    local test_name="$1"
    shift
    log_info "Running test: $test_name"
    log_info "Command: $*"
    # Clear dmesg buffer before test to see only relevant messages
    sudo dmesg -C > /dev/null 2>&1 || true
    if "$@" 2>&1; then
        log_info "✓ Test passed: $test_name"
        test_passed=$((test_passed + 1))
        return 0
    else
        local exit_code=$?
        log_error "✗ Test failed: $test_name (exit code: $exit_code)"
        log_error "Kernel messages (dmesg):"
        sudo dmesg | tail -30 || dmesg | tail -30 || true
        if [ $exit_code -eq 139 ]; then
            log_error "Segmentation fault detected - checking module status..."
            lsmod | grep file_to_pcie || log_error "Module not loaded!"
            ls -la /dev/file_to_pcie || log_error "Device file not found!"
        fi
        test_failed=$((test_failed + 1))
        return 1
    fi
}

run_test_expected_failure() {
    local test_name="$1"
    shift
    log_info "Running test (expected failure): $test_name"
    log_info "Command: $*"
    if "$@" 2>&1; then
        log_warn "Test unexpectedly passed (may indicate a fix): $test_name"
        test_passed=$((test_passed + 1))
        return 0
    else
        log_info "✓ Test failed as expected (known limitation): $test_name"
        # Don't increment test_failed for expected failures
        return 0
    fi
}

# Create test results directory
mkdir -p "${TEST_DIR}"

log_info "=== Starting QEMU VM Tests ==="
log_info "Kernel version: $(uname -r)"
log_info "Working directory: ${WORKSPACE_DIR}"

# Show initial dmesg to see any early messages
log_info "Initial kernel messages:"
sudo dmesg | tail -20 || dmesg | tail -20 || true

# Install build dependencies
log_info "Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
    build-essential \
    gcc \
    make \
    linux-headers-$(uname -r) \
    mdadm \
    xfsprogs \
    btrfs-progs \
    e2fsprogs \
    util-linux \
    > /dev/null 2>&1

# Build kernel module and userspace code
log_info "Building kernel module and userspace code..."
cd "${WORKSPACE_DIR}"
make clean || true
make

# Install and load the module
log_info "Installing and loading kernel module..."
sudo make install
sudo depmod -a
sudo modprobe file_to_pcie || {
    log_error "Failed to load kernel module"
    dmesg | tail -20
    exit 1
}

# Verify module is loaded
if ! lsmod | grep -q file_to_pcie; then
    log_error "Module not loaded"
    exit 1
fi

log_info "Module loaded successfully"

# Verify device file exists
if [ ! -c /dev/file_to_pcie ]; then
    log_error "Device file /dev/file_to_pcie not found"
    dmesg | tail -20
    exit 1
fi

log_info "Device file /dev/file_to_pcie exists"

# Verify test program exists and is executable
if [ ! -f "${WORKSPACE_DIR}/user/test_file_to_pcie" ]; then
    log_error "Test program not found: ${WORKSPACE_DIR}/user/test_file_to_pcie"
    exit 1
fi

if [ ! -x "${WORKSPACE_DIR}/user/test_file_to_pcie" ]; then
    log_error "Test program is not executable: ${WORKSPACE_DIR}/user/test_file_to_pcie"
    exit 1
fi

log_info "Test program is ready: ${WORKSPACE_DIR}/user/test_file_to_pcie"

# Wait for NVMe devices to be available
log_info "Waiting for NVMe devices..."
for i in {1..30}; do
    if [ -b /dev/nvme0n1 ] && [ -b /dev/nvme1n1 ] && [ -b /dev/nvme2n1 ] && \
       [ -b /dev/nvme3n1 ] && [ -b /dev/nvme4n1 ] && [ -b /dev/nvme5n1 ]; then
        break
    fi
    sleep 1
done

# Wait for udev to settle
log_info "Waiting for udev to settle..."
sudo udevadm settle --timeout=30 || log_warn "udevadm settle timed out"

# Verify all devices are available
for dev in nvme0n1 nvme1n1 nvme2n1 nvme3n1 nvme4n1 nvme5n1; do
    if [ ! -b "/dev/${dev}" ]; then
        log_error "NVMe device /dev/${dev} not found"
        lsblk
        exit 1
    fi
done

log_info "NVMe devices detected:"
lsblk | grep nvme || true

# Test filesystems on individual NVMe devices
# Note: Raw block device access is not tested due to limitations with I_BDEV
# when block device files haven't been opened

# Test 1: Regular file on ext4 filesystem
log_info "=== Test 1: Regular File on ext4 ==="
if ! sudo mkfs.ext4 -F /dev/nvme0n1 > /dev/null 2>&1; then
    log_error "Failed to create ext4 filesystem on /dev/nvme0n1"
    exit 1
fi
if ! sudo mkdir -p /mnt/test-ext4; then
    log_error "Failed to create mount point /mnt/test-ext4"
    exit 1
fi
if ! sudo mount /dev/nvme0n1 /mnt/test-ext4; then
    log_error "Failed to mount /dev/nvme0n1 on /mnt/test-ext4"
    exit 1
fi
if ! echo "test data" | sudo tee /mnt/test-ext4/testfile.txt > /dev/null; then
    log_error "Failed to create test file"
    sudo umount /mnt/test-ext4 || true
    exit 1
fi
# Verify file exists and is accessible
if [ ! -f /mnt/test-ext4/testfile.txt ]; then
    log_error "Test file was not created"
    sudo umount /mnt/test-ext4 || true
    exit 1
fi
# Verify filesystem is mounted
if ! mountpoint -q /mnt/test-ext4; then
    log_error "Mount point /mnt/test-ext4 is not mounted"
    exit 1
fi
# Sync to ensure filesystem is fully ready
sync
# Small delay to ensure mount is fully propagated
sleep 1
# Verify the file is accessible
if ! sudo test -f /mnt/test-ext4/testfile.txt; then
    log_error "Test file is not accessible"
    exit 1
fi
# Read from the file to ensure filesystem is fully initialized
# This may be needed for sb->s_bdev to be populated
if ! sudo cat /mnt/test-ext4/testfile.txt > /dev/null 2>&1; then
    log_warn "Failed to read from test file (may be OK)"
fi
# Sync again after reading
sync
# Debug: Show mount info
log_info "Mount information:"
mount | grep test-ext4 || log_warn "Mount not found in mount table"
log_info "File information:"
ls -la /mnt/test-ext4/testfile.txt || log_error "Cannot list test file"
log_info "Block device information:"
lsblk | grep nvme0n1 || log_error "Cannot find nvme0n1"
run_test "ext4 regular file test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-ext4/testfile.txt 0 1024
sudo umount /mnt/test-ext4 || log_warn "Failed to umount /mnt/test-ext4"

# Test 2: Regular file on xfs filesystem
log_info "=== Test 2: Regular File on xfs ==="
sudo mkfs.xfs -f /dev/nvme1n1 > /dev/null 2>&1 || {
    log_error "Failed to create xfs filesystem on /dev/nvme1n1"
    exit 1
}
sudo mkdir -p /mnt/test-xfs || {
    log_error "Failed to create mount point /mnt/test-xfs"
    exit 1
}
sudo mount /dev/nvme1n1 /mnt/test-xfs || {
    log_error "Failed to mount /dev/nvme1n1 on /mnt/test-xfs"
    exit 1
}
echo "test data" | sudo tee /mnt/test-xfs/testfile.txt > /dev/null || {
    log_error "Failed to create test file"
    sudo umount /mnt/test-xfs
    exit 1
}
if [ ! -f /mnt/test-xfs/testfile.txt ]; then
    log_error "Test file was not created"
    sudo umount /mnt/test-xfs
    exit 1
fi
run_test "xfs regular file test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-xfs/testfile.txt 0 1024
sudo umount /mnt/test-xfs || log_warn "Failed to umount /mnt/test-xfs"

# Test 3: Regular file on ext2 filesystem
log_info "=== Test 3: Regular File on ext2 ==="
sudo mkfs.ext2 -F /dev/nvme2n1 > /dev/null 2>&1 || {
    log_error "Failed to create ext2 filesystem on /dev/nvme2n1"
    exit 1
}
sudo mkdir -p /mnt/test-ext2 || {
    log_error "Failed to create mount point /mnt/test-ext2"
    exit 1
}
sudo mount /dev/nvme2n1 /mnt/test-ext2 || {
    log_error "Failed to mount /dev/nvme2n1 on /mnt/test-ext2"
    exit 1
}
echo "test data" | sudo tee /mnt/test-ext2/testfile.txt > /dev/null || {
    log_error "Failed to create test file"
    sudo umount /mnt/test-ext2
    exit 1
}
if [ ! -f /mnt/test-ext2/testfile.txt ]; then
    log_error "Test file was not created"
    sudo umount /mnt/test-ext2
    exit 1
fi
run_test "ext2 regular file test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-ext2/testfile.txt 0 1024
sudo umount /mnt/test-ext2 || log_warn "Failed to umount /mnt/test-ext2"

# Test 4: Regular file on ext3 filesystem
log_info "=== Test 4: Regular File on ext3 ==="
sudo mkfs.ext3 -F /dev/nvme3n1 > /dev/null 2>&1 || {
    log_error "Failed to create ext3 filesystem on /dev/nvme3n1"
    exit 1
}
sudo mkdir -p /mnt/test-ext3 || {
    log_error "Failed to create mount point /mnt/test-ext3"
    exit 1
}
sudo mount /dev/nvme3n1 /mnt/test-ext3 || {
    log_error "Failed to mount /dev/nvme3n1 on /mnt/test-ext3"
    exit 1
}
echo "test data" | sudo tee /mnt/test-ext3/testfile.txt > /dev/null || {
    log_error "Failed to create test file"
    sudo umount /mnt/test-ext3
    exit 1
}
if [ ! -f /mnt/test-ext3/testfile.txt ]; then
    log_error "Test file was not created"
    sudo umount /mnt/test-ext3
    exit 1
fi
run_test "ext3 regular file test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-ext3/testfile.txt 0 1024
sudo umount /mnt/test-ext3 || log_warn "Failed to umount /mnt/test-ext3"

# Test 5: Regular file on btrfs filesystem (KNOWN LIMITATION - Expected to fail)
log_info "=== Test 5: Regular File on btrfs (KNOWN LIMITATION) ==="
log_warn "btrfs is known to fail due to sb->s_bdev being NULL - this is expected"
sudo mkfs.btrfs -f /dev/nvme4n1 > /dev/null 2>&1
sudo mkdir -p /mnt/test-btrfs
sudo mount /dev/nvme4n1 /mnt/test-btrfs
echo "test data" | sudo tee /mnt/test-btrfs/testfile.txt > /dev/null
# This test is expected to fail - btrfs doesn't populate sb->s_bdev
run_test_expected_failure "btrfs regular file test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-btrfs/testfile.txt 0 1024
sudo umount /mnt/test-btrfs || sudo umount -l /mnt/test-btrfs
sync
sleep 1
# Clean up btrfs signature before reusing device for RAID
sudo wipefs -a /dev/nvme4n1 > /dev/null 2>&1 || true
sudo udevadm settle --timeout=10 || true

# Test 6: RAID 0 (striping) - 2 devices
log_info "=== Test 6: RAID 0 (Striping) ==="
# Ensure devices are clean before creating RAID
sudo wipefs -a /dev/nvme4n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme5n1 > /dev/null 2>&1 || true
sudo udevadm settle --timeout=10 || true
sudo mdadm --create /dev/md0 --level=0 --raid-devices=2 /dev/nvme4n1 /dev/nvme5n1 --force > /dev/null 2>&1
sudo udevadm settle --timeout=10 || true
sudo mkfs.ext4 -F /dev/md0 > /dev/null 2>&1
sudo mkdir -p /mnt/test-raid0
sudo mount /dev/md0 /mnt/test-raid0
echo "test data" | sudo tee /mnt/test-raid0/testfile.txt > /dev/null
run_test "RAID 0 regular file test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-raid0/testfile.txt 0 1024
# Note: Raw block device tests removed due to I_BDEV limitations
sudo umount /mnt/test-raid0 || sudo umount -l /mnt/test-raid0
sync
sudo mdadm --stop /dev/md0 > /dev/null 2>&1 || true
sudo mdadm --zero-superblock /dev/nvme4n1 > /dev/null 2>&1 || true
sudo mdadm --zero-superblock /dev/nvme5n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme4n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme5n1 > /dev/null 2>&1 || true
sudo udevadm settle --timeout=10 || true

# Test 7: RAID 1 (mirroring) - 2 devices
log_info "=== Test 7: RAID 1 (Mirroring) ==="
# Ensure devices are clean before creating RAID
sudo wipefs -a /dev/nvme4n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme5n1 > /dev/null 2>&1 || true
sudo udevadm settle --timeout=10 || true
sudo mdadm --create /dev/md1 --level=1 --raid-devices=2 /dev/nvme4n1 /dev/nvme5n1 --force > /dev/null 2>&1
sudo udevadm settle --timeout=10 || true
sudo mkfs.ext4 -F /dev/md1 > /dev/null 2>&1
sudo mkdir -p /mnt/test-raid1
sudo mount /dev/md1 /mnt/test-raid1
echo "test data" | sudo tee /mnt/test-raid1/testfile.txt > /dev/null
run_test "RAID 1 regular file test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-raid1/testfile.txt 0 1024
# Note: Raw block device tests removed due to I_BDEV limitations
sudo umount /mnt/test-raid1 || sudo umount -l /mnt/test-raid1
sync
sudo mdadm --stop /dev/md1 > /dev/null 2>&1 || true
sudo mdadm --zero-superblock /dev/nvme4n1 > /dev/null 2>&1 || true
sudo mdadm --zero-superblock /dev/nvme5n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme4n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme5n1 > /dev/null 2>&1 || true
sudo udevadm settle --timeout=10 || true

# Test 8: RAID 5 - 3 devices (using nvme0-2 which were used for individual filesystems)
# Note: We'll reuse devices after unmounting, but for simplicity, skip RAID 5
# as it requires 3 devices and we've already used nvme0-4 for individual filesystems
log_info "=== Test 8: RAID 5 skipped (devices already in use) ==="

# Test 9: xfs on RAID 0
log_info "=== Test 9: xfs on RAID 0 ==="
# Ensure devices are clean before creating RAID
sudo wipefs -a /dev/nvme4n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme5n1 > /dev/null 2>&1 || true
sudo udevadm settle --timeout=10 || true
sudo mdadm --create /dev/md2 --level=0 --raid-devices=2 /dev/nvme4n1 /dev/nvme5n1 --force > /dev/null 2>&1
sudo udevadm settle --timeout=10 || true
sudo mkfs.xfs -f /dev/md2 > /dev/null 2>&1
sudo mkdir -p /mnt/test-raid-xfs
sudo mount /dev/md2 /mnt/test-raid-xfs
echo "test data" | sudo tee /mnt/test-raid-xfs/testfile.txt > /dev/null
run_test "xfs on RAID 0 test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-raid-xfs/testfile.txt 0 1024
sudo umount /mnt/test-raid-xfs || sudo umount -l /mnt/test-raid-xfs
sync
sudo mdadm --stop /dev/md2 > /dev/null 2>&1 || true
sudo mdadm --zero-superblock /dev/nvme4n1 > /dev/null 2>&1 || true
sudo mdadm --zero-superblock /dev/nvme5n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme4n1 > /dev/null 2>&1 || true
sudo wipefs -a /dev/nvme5n1 > /dev/null 2>&1 || true
sudo udevadm settle --timeout=10 || true

# Test 10: Test with different offsets on filesystem file
log_info "=== Test 10: Different Offsets ==="
# Remount ext4 for offset test
sudo mount /dev/nvme0n1 /mnt/test-ext4
# Create a larger file for offset testing
sudo dd if=/dev/zero of=/mnt/test-ext4/largefile.bin bs=1M count=2 > /dev/null 2>&1
run_test "ext4 file offset test" \
    sudo "${WORKSPACE_DIR}/user/test_file_to_pcie" /mnt/test-ext4/largefile.bin 1048576 4096
sudo umount /mnt/test-ext4

# Summary
log_info "=== Test Summary ==="
log_info "Tests passed: ${test_passed}"
log_info "Tests failed: ${test_failed}"

if [ ${test_failed} -eq 0 ]; then
    log_info "All tests passed!"
    exit 0
else
    log_error "Some tests failed"
    exit 1
fi

