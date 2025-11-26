#!/bin/bash
# run-qemu-tests.sh - Run QEMU tests with 6 NVMe SSDs
#
# This script launches QEMU directly with 6 NVMe SSDs,
# builds the kernel module and userspace code, and runs various tests.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="${SCRIPT_DIR}/.."
OUTPUT_DIR="/output"
VM_INFO_FILE="${OUTPUT_DIR}/vm-info.json"

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

# Check for qemu-system-x86_64
if ! command -v qemu-system-x86_64 &> /dev/null; then
    log_error "qemu-system-x86_64 not found in PATH"
    exit 1
fi

# Check for output directory and VM image
if [ ! -d "${OUTPUT_DIR}" ]; then
    log_error "Output directory not found: ${OUTPUT_DIR}"
    exit 1
fi

if [ ! -f "${VM_INFO_FILE}" ]; then
    log_error "VM info file not found: ${VM_INFO_FILE}"
    exit 1
fi

# Parse vm-info.json for connection details
log_info "Parsing VM info from ${VM_INFO_FILE}..."

# Extract values using jq or python (matching rocm-ernic pattern)
if command -v jq &> /dev/null; then
    VM_IMAGE=$(jq -r '.image_path // empty' "${VM_INFO_FILE}")
    SSH_USER=$(jq -r '.username // "root"' "${VM_INFO_FILE}")
    SSH_KEY_PRIVATE=$(jq -r '.ssh_keys.private_key_path // ""' "${VM_INFO_FILE}")
    SSH_KEY_PUBLIC=$(jq -r '.ssh_keys.public_key_path // ""' "${VM_INFO_FILE}")
    SSH_PORT=$(jq -r '.ssh_port // 2222' "${VM_INFO_FILE}")
    VM_HOST=$(jq -r '.host // "localhost"' "${VM_INFO_FILE}")
else
    # Fallback to python if jq not available
    log_info "Using Python to parse JSON..."
    VM_IMAGE=$(python3 -c "import json; print(json.load(open('${VM_INFO_FILE}')).get('image_path', ''))")
    SSH_USER=$(python3 -c "import json; print(json.load(open('${VM_INFO_FILE}')).get('username', 'root'))")
    SSH_KEY_PRIVATE=$(python3 -c "import json; d=json.load(open('${VM_INFO_FILE}')); print(d.get('ssh_keys', {}).get('private_key_path', ''))")
    SSH_KEY_PUBLIC=$(python3 -c "import json; d=json.load(open('${VM_INFO_FILE}')); print(d.get('ssh_keys', {}).get('public_key_path', ''))")
    SSH_PORT=$(python3 -c "import json; print(json.load(open('${VM_INFO_FILE}')).get('ssh_port', 2222))")
    VM_HOST=$(python3 -c "import json; print(json.load(open('${VM_INFO_FILE}')).get('host', 'localhost'))")
fi

if [ -z "${VM_IMAGE}" ]; then
    log_error "image_path not found in vm-info.json"
    exit 1
fi

# VM_IMAGE might be an absolute path or relative to /output
if [ -f "${VM_IMAGE}" ]; then
    VM_IMAGE_PATH="${VM_IMAGE}"
elif [ -f "${OUTPUT_DIR}/${VM_IMAGE}" ]; then
    VM_IMAGE_PATH="${OUTPUT_DIR}/${VM_IMAGE}"
else
    VM_IMAGE_PATH="${VM_IMAGE}"
fi
if [ ! -f "${VM_IMAGE_PATH}" ]; then
    log_error "VM image file not found: ${VM_IMAGE_PATH}"
    exit 1
fi

log_info "VM Image: ${VM_IMAGE_PATH}"
log_info "SSH User: ${SSH_USER}"
log_info "SSH Port: ${SSH_PORT}"
log_info "SSH Host: ${VM_HOST}"

# Create NVMe disk images in /tmp (qcow2 format, 1TiB each)
log_info "Creating NVMe disk images..."
NVME_IMAGES=()
for i in {0..5}; do
    NVME_IMG="/tmp/nvme${i}.qcow2"
    if [ ! -f "${NVME_IMG}" ]; then
        log_info "Creating NVMe disk image ${i} (qcow2, 1TiB)..."
        qemu-img create -f qcow2 "${NVME_IMG}" 1T || {
            log_error "Failed to create NVMe disk image ${i}"
            exit 1
        }
    else
        log_info "NVMe disk image ${i} already exists"
    fi
    NVME_IMAGES+=("${NVME_IMG}")
done

# Prepare SSH key if provided
SSH_OPTS=()
if [ -n "${SSH_KEY_PRIVATE}" ]; then
    # SSH key path might be absolute or relative to /output
    if [ -f "${SSH_KEY_PRIVATE}" ]; then
        SSH_KEY_PATH="${SSH_KEY_PRIVATE}"
    elif [ -f "${OUTPUT_DIR}/${SSH_KEY_PRIVATE}" ]; then
        SSH_KEY_PATH="${OUTPUT_DIR}/${SSH_KEY_PRIVATE}"
    else
        SSH_KEY_PATH="${SSH_KEY_PRIVATE}"
    fi
    
    if [ -f "${SSH_KEY_PATH}" ]; then
        chmod 600 "${SSH_KEY_PATH}" || true
        SSH_OPTS=(-i "${SSH_KEY_PATH}" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
        log_info "Using SSH private key: ${SSH_KEY_PATH}"
    else
        log_warn "SSH private key file not found: ${SSH_KEY_PRIVATE}"
    fi
fi

# Start QEMU in the background
log_info "Starting QEMU VM with 6 NVMe devices..."

# Build QEMU command line
# Optimize for GitHub Actions runners (standard: 2 vCPUs, 7GB RAM)
# Using TCG (software emulation) since KVM is not available in GitHub Actions
# Performance optimizations:
# - Use 'max' CPU model for better TCG performance
# - Allocate 2 vCPUs (matching runner cores) for better performance
# - Use 4GB RAM (leaving ~3GB for host system)
# - Use threads=2 for better SMP performance
QEMU_CMD=(
    qemu-system-x86_64
    -machine q35,accel=tcg
    -cpu max
    -smp 2,threads=2
    -m 4G
    -drive "file=${VM_IMAGE_PATH},format=qcow2,if=virtio"
    -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22
    -device virtio-net,netdev=net0
    -nographic
)

# Add NVMe devices
for i in {0..5}; do
    QEMU_CMD+=(
        -drive "file=${NVME_IMAGES[$i]},format=qcow2,if=none,id=nvme${i}"
        -device "nvme,drive=nvme${i},serial=NVME${i}"
    )
done

log_info "Launching QEMU..."
log_info "Command: ${QEMU_CMD[*]}"

# Start QEMU in background
"${QEMU_CMD[@]}" > /tmp/qemu.log 2>&1 &
QEMU_PID=$!

# Function to cleanup on exit
cleanup() {
    log_info "Cleaning up..."
    if kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" || true
    fi
}
trap cleanup EXIT

# Check if QEMU is still running after 5 seconds
log_info "Waiting 5 seconds to verify QEMU started successfully..."
sleep 5

if ! kill -0 "${QEMU_PID}" 2>/dev/null; then
    log_error "QEMU process died shortly after starting (PID: ${QEMU_PID})"
    log_error "QEMU output:"
    echo "----------------------------------------"
    cat /tmp/qemu.log || true
    echo "----------------------------------------"
    exit 1
fi

log_info "QEMU is running (PID: ${QEMU_PID})"

# Wait for VM to boot and SSH to be available
log_info "Waiting for VM to boot and SSH to be available..."
MAX_WAIT=120
WAIT_COUNT=0
while [ ${WAIT_COUNT} -lt ${MAX_WAIT} ]; do
    if ssh "${SSH_OPTS[@]}" -p "${SSH_PORT}" "${SSH_USER}@${VM_HOST}" "echo 'SSH ready'" > /dev/null 2>&1; then
        log_info "SSH is ready!"
        break
    fi
    sleep 2
    WAIT_COUNT=$((WAIT_COUNT + 2))
    if [ $((WAIT_COUNT % 10)) -eq 0 ]; then
        log_info "Still waiting for SSH... (${WAIT_COUNT}s)"
    fi
done

if [ ${WAIT_COUNT} -ge ${MAX_WAIT} ]; then
    log_error "Timeout waiting for SSH to become available"
    log_error "QEMU log:"
    tail -50 /tmp/qemu.log || true
    exit 1
fi

# Copy test script to VM
log_info "Copying test script to VM..."
scp "${SSH_OPTS[@]}" -P "${SSH_PORT}" \
    "${WORKSPACE_DIR}/scripts/test-inside-vm.sh" \
    "${SSH_USER}@${VM_HOST}:/tmp/test-inside-vm.sh" || {
    log_error "Failed to copy test script to VM"
    exit 1
}

# Copy workspace to VM (or mount it)
log_info "Copying workspace to VM..."
# Create a tarball of the workspace
TARBALL="/tmp/workspace.tar.gz"
tar czf "${TARBALL}" -C "${WORKSPACE_DIR}" . || {
    log_error "Failed to create workspace tarball"
    exit 1
}

scp "${SSH_OPTS[@]}" -P "${SSH_PORT}" \
    "${TARBALL}" \
    "${SSH_USER}@${VM_HOST}:/tmp/workspace.tar.gz" || {
    log_error "Failed to copy workspace to VM"
    exit 1
}

# Extract workspace in VM (use home directory or /tmp to avoid permission issues)
WORKSPACE_DIR_VM="/home/${SSH_USER}/workspace"
log_info "Extracting workspace to ${WORKSPACE_DIR_VM} in VM..."
ssh "${SSH_OPTS[@]}" -p "${SSH_PORT}" "${SSH_USER}@${VM_HOST}" \
    "mkdir -p ${WORKSPACE_DIR_VM} && tar xzf /tmp/workspace.tar.gz -C ${WORKSPACE_DIR_VM}" || {
    log_error "Failed to extract workspace in VM"
    exit 1
}

# Run the test script inside the VM
log_info "Running test script inside VM..."
ssh "${SSH_OPTS[@]}" -p "${SSH_PORT}" "${SSH_USER}@${VM_HOST}" \
    "cd ${WORKSPACE_DIR_VM} && WORKSPACE_DIR=${WORKSPACE_DIR_VM} bash /tmp/test-inside-vm.sh" || {
    log_error "Test script failed"
    exit 1
}

log_info "Tests completed successfully!"
