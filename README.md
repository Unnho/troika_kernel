# Troika Kernel

Custom kernel for the Motorola One Vision (troika) and Motorola One Action (kane), based on [LineageOS 23.2](https://wiki.lineageos.org/) for the Samsung Exynos 9610 SoC.

## Supported devices

| Device | Codename | Boot image |
|---|---|---|
| Motorola One Vision | troika | [`troika-boot.img`](https://github.com/Unnho/troika_kernel/releases/latest/download/troika-boot.img) |
| Motorola One Action | kane | [`kane-boot.img`](https://github.com/Unnho/troika_kernel/releases/latest/download/kane-boot.img) |

## Features

### ARM64 ASM LZ4 decompression

Hardware-accelerated LZ4 decompression via ARM64 v8 SIMD instructions. Touches `lib/lz4`, `lib/decompress_unlz4`, `crypto/lz4`, and `erofs` for measurable decompression speedup on the Exynos 9610.

### ECC __int128 optimization

Replaces 64-bit multiply-with-carry emulation with native `__int128` for faster CRC/ECC computation.

### Droidspaces container runtime support

Enables namespace, cgroup, OverlayFS, and NAT/veth container modes required by [Droidspaces](https://droidspaces.com) legacy-kernel container runtime.

### SukiSU Ultra

KernelSU built-in via manual-hook integration. SUSFS is deliberately disabled — this kernel ships root support, not root-hiding hooks. KPM is disabled because Linux 4.14 lacks the `set_memory` API it requires.

### SLUB low-latency tuning

Follows the Exynos 9610 Mint kernel's low-latency configuration:
- `CONFIG_SLUB_CPU_PARTIAL` disabled
- `CONFIG_SCHEDSTATS` disabled

## Installation

> **Do not flash via fastboot.** There is a known issue with fastboot flashing on these devices that can result in a boot loop.

1. Download the boot image for your device from the [latest release](https://github.com/Unnho/troika_kernel/releases/latest).
2. Boot into TWRP recovery.
3. Use TWRP to flash the `.img` file to the **boot** partition.
4. Reboot.

## Building

```bash
# Install dependencies
sudo apt install build-essential flex bison bc libssl-dev libelf-dev

# Clone
git clone https://github.com/Unnho/troika_kernel.git
cd troika_kernel
git checkout troika-mint-optimizations

# Set up Clang toolchain (r563880c)
export PATH=/path/to/clang-r563880c/bin:$PATH

# Generate config
make ARCH=arm64 CC=clang LD=ld.lld exynos9610_defconfig

# For Motorola One Vision (troika):
cat arch/arm64/configs/ext_config/troika.config >> .config

# For Motorola One Action (kane):
cat arch/arm64/configs/ext_config/kane.config >> .config

make ARCH=arm64 CC=clang LD=ld.lld olddefconfig

# Build
make ARCH=arm64 CC=clang LD=ld.lld \
  AR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy \
  OBJDUMP=llvm-objdump STRIP=llvm-strip \
  HOSTCC=clang HOSTCXX=clang++ HOSTLD=ld.lld \
  HOSTAR=llvm-ar LLVM=1 LLVM_IAS=1 -j$(nproc) Image
```

## Release history

See [GitHub Releases](https://github.com/Unnho/troika_kernel/releases) for pre-built boot images and changelogs.

## Credits

- [LineageOS](https://wiki.lineageos.org/) — base kernel source
- [FreshROMs Mint kernel](https://github.com/nicholaschum/android_kernel_samsung_exynos9610) — LZ4 ASM acceleration, ECC optimization
- [Droidspaces](https://droidspaces.com) — container runtime support
- [SukiSU Ultra](https://github.com/SukiSU-Ultra/SukiSU_Ultra_KernelSU) — KernelSU integration

## License

Kernel is licensed under the GNU General Public License version 2, as published by the Free Software Foundation.
