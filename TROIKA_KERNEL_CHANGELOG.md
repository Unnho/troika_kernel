# Troika kernel release changelog

## 2026-08-24 — LineageOS 23.2 build release

### Source baseline

- Upstream: `LineageOS/android_kernel_motorola_exynos9610`
- Branch: `lineage-23.2`
- Baseline commit: `41fbcdf3676a4c5743428a8d37fa21c6a553b0a2`
- Kernel version: `4.14.357-openela`

### Build configuration

- Base configuration: `arch/arm64/configs/exynos9610_defconfig`
- Device fragment: `arch/arm64/configs/ext_config/troika.config`
- Toolchain: Android 16 `clang-r563880c` (Clang 21)
- Build targets: `Image`, `dtbs`, and separated `dtbo.img`

### Troika-enabled components

- Samsung SCSC WLAN/BT platform: `leman_s620_troika_dualfem`
- Troika camera CIS, actuator, and EEPROM configuration
- Exynos9610 DTB and Troika DTBO overlay packaging

### Local source changes

None. No MIF, UFS, scheduler, camera, WLAN, or other kernel-source tuning patches were applied. The only addition in this repository is this release changelog.

### Licensing and attribution

This repository is a derivative distribution of LineageOS and other upstream work. Existing copyright notices, license files, and attribution remain intact. This changelog does not replace or alter those rights.
