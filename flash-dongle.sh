#!/bin/bash

set -e

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 [--all]"
    echo "  (default)  flash the dongle only"
    echo "  --all      flash the dongle, then the left and right halves"
}

FLASH_HALVES=false
case "${1:-}" in
    "") ;;
    --all) FLASH_HALVES=true ;;
    -h|--help) usage; exit 0 ;;
    *) usage; exit 1 ;;
esac

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

TEMP_DIR=""

# Cleanup function to kill sudo keeper and remove downloads on exit
cleanup() {
    kill $SUDO_KEEPER_PID 2>/dev/null || true
    [ -n "$TEMP_DIR" ] && rm -rf "$TEMP_DIR"
}
trap cleanup EXIT

echo -e "${GREEN}✓ Sudo access granted${NC}\n"

# Step 1: Wait for GitHub Actions build to complete
echo -e "${YELLOW}[1/3] Checking GitHub Actions build status...${NC}"

# Get the latest workflow run from the build workflow
LATEST_RUN=$(gh run list --workflow "Build ZMK firmware" --limit 1 --json databaseId,status,conclusion --jq '.[0]')

if [ -z "$LATEST_RUN" ]; then
    echo -e "${RED}Error: No workflow runs found${NC}"
    exit 1
fi

RUN_ID=$(echo "$LATEST_RUN" | jq -r '.databaseId')
RUN_STATUS=$(echo "$LATEST_RUN" | jq -r '.status')
RUN_CONCLUSION=$(echo "$LATEST_RUN" | jq -r '.conclusion')

echo "Latest workflow run: #$RUN_ID"
echo "Status: $RUN_STATUS"

if [ "$RUN_STATUS" != "completed" ]; then
    echo -e "${YELLOW}Build is still running. Waiting for completion...${NC}"
    echo "(You can press Ctrl+C to cancel)"
    echo ""

    gh run watch "$RUN_ID" || {
        echo -e "${RED}Error: Failed to watch workflow run${NC}"
        exit 1
    }

    # Get the final conclusion
    RUN_CONCLUSION=$(gh run view "$RUN_ID" --json conclusion --jq '.conclusion')
fi

if [ "$RUN_CONCLUSION" != "success" ]; then
    echo -e "${RED}Error: Workflow run failed with conclusion: $RUN_CONCLUSION${NC}"
    echo "Please check the workflow logs on GitHub"
    exit 1
fi

echo -e "${GREEN}✓ Build completed successfully${NC}\n"

# Step 2: Download artifacts
echo -e "${YELLOW}[2/3] Downloading build artifacts...${NC}"

TEMP_DIR=$(mktemp -d)
echo "Using temp directory: $TEMP_DIR"

gh run download "$RUN_ID" --dir "$TEMP_DIR" 2>/dev/null || {
    echo -e "${RED}Error: Failed to download artifacts. Make sure 'gh' CLI is installed and authenticated.${NC}"
    echo "Run: gh auth login"
    exit 1
}

echo -e "${GREEN}✓ Download complete${NC}\n"

# Common bootloader labels
BOOTLOADER_TIMEOUT=120
BOOTLOADER_LABELS=("NICENANO" "NRF52BOOT" "NICE_NANO" "BOOT" "FEATHERBOOT")

find_firmware() {
    find "$TEMP_DIR" -name "$1" ! -name "*debug*.uf2" | head -n 1
}

wait_for_bootloader() {
    local timeout=$BOOTLOADER_TIMEOUT
    local elapsed=0
    BOOTLOADER_DEVICE=""
    BOOTLOADER_LABEL=""

    while [ $elapsed -lt $timeout ]; do
        for label in "${BOOTLOADER_LABELS[@]}"; do
            [ -L "/dev/disk/by-label/$label" ] || continue
            local device block_device is_removable size_mb
            device=$(readlink -f "/dev/disk/by-label/$label" 2>/dev/null) || continue

            # Skip non-removable devices such as internal disks
            [[ "$device" == *"nvme"* || "$device" == *"mmcblk"* ]] && continue

            block_device=$(lsblk -no PKNAME "$device" 2>/dev/null) || block_device=""
            if [ -z "$block_device" ]; then
                block_device=$(basename "$device" | sed 's/[0-9]*$//' | sed 's/p$//')
            fi
            [ -z "$block_device" ] && continue

            is_removable=$(cat "/sys/block/$block_device/removable" 2>/dev/null) || continue
            [ "$is_removable" == "1" ] || continue

            # Bootloaders are typically very small (< 100MB)
            size_mb=$(( $(lsblk -bno SIZE "$device" 2>/dev/null || echo 0) / 1024 / 1024 ))
            if [ "$size_mb" -lt 100 ] && [ "$size_mb" -gt 0 ]; then
                BOOTLOADER_DEVICE="$device"
                BOOTLOADER_LABEL="$label"
                echo ""
                return 0
            fi
        done

        echo -ne "\rWaiting... ${elapsed}s / ${timeout}s "
        sleep 1
        elapsed=$((elapsed + 1))
    done

    echo ""
    return 1
}

# Wait until the previous bootloader drive disappears so it isn't picked up again
wait_for_eject() {
    local device="$1"
    for _ in $(seq 1 15); do
        [ -e "$device" ] || return 0
        sleep 1
    done
    echo -e "${YELLOW}Warning: $device still present after flashing${NC}"
}

# flash_device <name> <firmware glob>
flash_device() {
    local name="$1"
    local firmware
    firmware=$(find_firmware "$2")

    echo -e "${YELLOW}=== Flashing $name ===${NC}"

    if [ -z "$firmware" ]; then
        echo -e "${RED}Error: Could not find $name firmware ($2)${NC}"
        echo "Available .uf2 files:"
        find "$TEMP_DIR" -name "*.uf2"
        exit 1
    fi
    echo "Found firmware: $(basename "$firmware")"

    echo "Connect the $name via USB and put it in bootloader mode (double-tap reset, or the &bootloader key on layer 2)."
    set +e
    wait_for_bootloader
    local found=$?
    set -e

    if [ $found -ne 0 ]; then
        echo -e "${RED}Error: Bootloader device not detected after ${BOOTLOADER_TIMEOUT} seconds${NC}"
        echo "Make sure the $name is connected and in bootloader mode"
        echo "Looked for labels: ${BOOTLOADER_LABELS[*]}"
        echo ""
        echo "Available devices:"
        lsblk -o NAME,LABEL,SIZE,TYPE
        exit 1
    fi

    echo -e "${GREEN}✓ Bootloader detected: $BOOTLOADER_DEVICE (label: $BOOTLOADER_LABEL)${NC}"

    local mount_point
    mount_point=$(mktemp -d)
    sudo mount "$BOOTLOADER_DEVICE" "$mount_point" || {
        echo -e "${RED}Error: Failed to mount bootloader${NC}"
        rm -rf "$mount_point"
        exit 1
    }

    sudo cp "$firmware" "$mount_point/" || {
        echo -e "${RED}Error: Failed to copy firmware${NC}"
        sudo umount "$mount_point"
        rm -rf "$mount_point"
        exit 1
    }

    sync
    sleep 1
    sudo umount "$mount_point" 2>/dev/null || echo -e "${YELLOW}Warning: Device may have already ejected itself${NC}"
    rm -rf "$mount_point"

    wait_for_eject "$BOOTLOADER_DEVICE"
    echo -e "${GREEN}✓ $name flashed${NC}\n"
}

# Step 3: Flash devices
echo -e "${YELLOW}[3/3] Flashing firmware...${NC}\n"

flash_device "dongle" "*dongle*.uf2"
if $FLASH_HALVES; then
    flash_device "left half" "*eyelash_sofle_studio_left*.uf2"
    flash_device "right half" "*eyelash_sofle_right*.uf2"
fi

echo -e "${GREEN}=== Flashing Complete! ===${NC}"
echo "Devices reboot automatically and the new firmware is ready."
echo ""
