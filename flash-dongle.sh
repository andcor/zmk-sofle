#!/bin/bash

set -e

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== ZMK Dongle Flasher ===${NC}\n"

# Request sudo access upfront (before keyboard stops working)
echo -e "${YELLOW}Requesting sudo access (needed for mounting)...${NC}"
sudo -v || {
    echo -e "${RED}Error: sudo access required${NC}"
    exit 1
}

# Keep sudo alive in the background
(while true; do sudo -n true; sleep 50; done) 2>/dev/null &
SUDO_KEEPER_PID=$!

# Cleanup function to kill sudo keeper on exit
cleanup() {
    kill $SUDO_KEEPER_PID 2>/dev/null || true
}
trap cleanup EXIT

echo -e "${GREEN}✓ Sudo access granted${NC}\n"

# Step 1: Download latest GitHub Actions artifacts
echo -e "${YELLOW}[1/4] Downloading latest build artifacts from GitHub Actions...${NC}"

# Create temp directory
TEMP_DIR=$(mktemp -d)
echo "Using temp directory: $TEMP_DIR"

# Download artifacts from the latest successful run
gh run download --dir "$TEMP_DIR" 2>/dev/null || {
    echo -e "${RED}Error: Failed to download artifacts. Make sure 'gh' CLI is installed and authenticated.${NC}"
    echo "Run: gh auth login"
    rm -rf "$TEMP_DIR"
    exit 1
}

echo -e "${GREEN}✓ Download complete${NC}\n"

# Step 2: Find and extract the dongle firmware
echo -e "${YELLOW}[2/4] Looking for dongle firmware...${NC}"

# Look for the dongle .uf2 file (could be in a subdirectory)
DONGLE_FILE=$(find "$TEMP_DIR" -name "*dongle*.uf2" -o -name "nice_nano_v2-eyelash_sofle_dongle.uf2" | head -n 1)

if [ -z "$DONGLE_FILE" ]; then
    echo -e "${RED}Error: Could not find dongle firmware file${NC}"
    echo "Contents of download directory:"
    ls -R "$TEMP_DIR"
    rm -rf "$TEMP_DIR"
    exit 1
fi

echo "Found firmware: $(basename "$DONGLE_FILE")"
echo -e "${GREEN}✓ Firmware ready${NC}\n"

# Step 3: Wait for bootloader mode
echo -e "${YELLOW}[3/4] Waiting for dongle to enter bootloader mode...${NC}"
echo "Please double-tap the reset button on your dongle now."
echo ""

# Common bootloader labels
BOOTLOADER_LABELS=("NICENANO" "NRF52BOOT" "NICE_NANO" "BOOT" "FEATHERBOOT")
BOOTLOADER_DEVICE=""
TIMEOUT=60
ELAPSED=0

while [ $ELAPSED -lt $TIMEOUT ]; do
    # Check /dev/disk/by-label/ for bootloader
    for label in "${BOOTLOADER_LABELS[@]}"; do
        if [ -L "/dev/disk/by-label/$label" ]; then
            BOOTLOADER_DEVICE=$(readlink -f "/dev/disk/by-label/$label")
            BOOTLOADER_LABEL="$label"
            break 2
        fi
    done

    # Visual progress indicator
    echo -ne "\rWaiting... ${ELAPSED}s / ${TIMEOUT}s "
    sleep 1
    ((ELAPSED++))
done

echo "" # New line after progress

if [ -z "$BOOTLOADER_DEVICE" ]; then
    echo -e "${RED}Error: Bootloader device not detected after ${TIMEOUT} seconds${NC}"
    echo "Make sure your dongle is connected and in bootloader mode"
    echo "Looked for labels: ${BOOTLOADER_LABELS[*]}"
    echo ""
    echo "Available devices:"
    lsblk -o NAME,LABEL,SIZE,TYPE
    rm -rf "$TEMP_DIR"
    exit 1
fi

echo -e "${GREEN}✓ Bootloader detected: $BOOTLOADER_DEVICE (label: $BOOTLOADER_LABEL)${NC}\n"

# Step 4: Mount and flash firmware
echo -e "${YELLOW}[4/4] Mounting and flashing firmware...${NC}"

# Create temporary mount point
MOUNT_POINT=$(mktemp -d)
echo "Mounting $BOOTLOADER_DEVICE to $MOUNT_POINT"

# Mount the device
sudo mount "$BOOTLOADER_DEVICE" "$MOUNT_POINT" || {
    echo -e "${RED}Error: Failed to mount bootloader${NC}"
    rm -rf "$TEMP_DIR" "$MOUNT_POINT"
    exit 1
}

# Copy firmware
cp "$DONGLE_FILE" "$MOUNT_POINT/" || {
    echo -e "${RED}Error: Failed to copy firmware${NC}"
    sudo umount "$MOUNT_POINT"
    rm -rf "$TEMP_DIR" "$MOUNT_POINT"
    exit 1
}

echo "Copied $(basename "$DONGLE_FILE") to $MOUNT_POINT"

# Sync and unmount
sync
sleep 1

sudo umount "$MOUNT_POINT" || echo -e "${YELLOW}Warning: Device may have already ejected itself${NC}"
rm -rf "$MOUNT_POINT"

echo -e "${GREEN}✓ Firmware flashed successfully${NC}\n"

# Cleanup
rm -rf "$TEMP_DIR"

echo -e "${GREEN}=== Flashing Complete! ===${NC}"
echo "The dongle should reboot automatically and your new firmware is ready."
echo ""