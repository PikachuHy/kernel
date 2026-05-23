# macOS disk image creation — uses hdiutil + diskutil

disk_create() {
    local DISK_IMG="$1"
    local KERNEL_ELF="$2"
    local LIMINE_CONF="$3"

    LIMINE_DIR="$(brew --prefix limine)/share/limine"

    echo "==> Creating bootable disk image..."
    rm -f "$DISK_IMG"
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=64 2>/dev/null

    # Create FAT32 partition with MBR
    local DISK_DEV
    DISK_DEV=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$DISK_IMG" 2>/dev/null | awk '{print $1}')
    diskutil partitionDisk "$DISK_DEV" 1 MBR "MS-DOS FAT32" "KERNEL" 100% 2>/dev/null

    # Copy kernel, config, and Limine files
    local MOUNT_POINT="/Volumes/KERNEL"
    cp "$KERNEL_ELF" "$MOUNT_POINT/kernel.elf"
    cp "$LIMINE_CONF" "$MOUNT_POINT/limine.conf"
    cp "$LIMINE_DIR/limine-bios.sys" "$MOUNT_POINT/"

    hdiutil detach "$DISK_DEV" 2>/dev/null

    # Install Limine BIOS bootloader
    limine bios-install "$DISK_IMG"
}
