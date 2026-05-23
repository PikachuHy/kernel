# Linux disk image creation — uses parted + mkfs.fat + mcopy (no sudo)

disk_create() {
    local DISK_IMG="$1"
    local KERNEL_ELF="$2"
    local LIMINE_CONF="$3"

    # Find Limine share directory
    if [ -d "/usr/local/share/limine" ]; then
        LIMINE_DIR="/usr/local/share/limine"
    elif [ -d "/usr/share/limine" ]; then
        LIMINE_DIR="/usr/share/limine"
    else
        echo "Error: cannot find Limine share directory" >&2
        echo "  Tried: /usr/local/share/limine, /usr/share/limine" >&2
        exit 1
    fi

    echo "==> Creating bootable disk image..."
    mkdir -p "$(dirname "$DISK_IMG")"
    rm -f "$DISK_IMG"
    dd if=/dev/zero of="$DISK_IMG" bs=1M count=64 2>/dev/null

    # Create MBR partition table with one FAT32 partition starting at sector 2048
    parted -s "$DISK_IMG" mklabel msdos
    parted -s "$DISK_IMG" mkpart primary fat32 2048s 100%
    parted -s "$DISK_IMG" set 1 boot on

    # Format partition at offset 2048 sectors (1MiB)
    mkfs.fat -F 32 --offset 2048 "$DISK_IMG" >/dev/null

    # Copy files using mtools (access partition at 1MiB offset)
    local PART_OFFSET="@@1M"
    mcopy -i "$DISK_IMG$PART_OFFSET" "$KERNEL_ELF" ::/kernel.elf
    mcopy -i "$DISK_IMG$PART_OFFSET" "$LIMINE_CONF" ::/limine.conf
    mcopy -i "$DISK_IMG$PART_OFFSET" "$LIMINE_DIR/limine-bios.sys" ::/limine-bios.sys

    # Install Limine BIOS bootloader
    limine bios-install "$DISK_IMG"
}
