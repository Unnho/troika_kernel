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

- Enabled `CONFIG_SCSI_MQ_DEFAULT`: Troika's UFS host now uses the supported
  SCSI multi-queue path. `mq-deadline` is already built into this tree and is
  selected automatically for a single-hardware-queue MQ device.
- Moderately raised only the big-cluster CPU-to-MIF minimum constraints in the
  interactive range (1.248–1.898 GHz). This reduces memory-bandwidth drops
  during foreground work without raising the MIF idle floor or altering the
  little-cluster policy.
- Kept the vendor UFS link configuration, Gear 3/one-lane limit, interrupt
  aggregation, Hibern8 clock gating, DMA coherency, queue-depth policy, and
  hardware quirks unchanged. These are hardware/firmware-specific and were
  already correctly configured.

### Tuning review

The accompanying `TROIKA_PERFORMANCE_AUDIT.md` records the source-level
verification. In particular, the input reports incorrectly identify this as a
5.10 kernel and claim MQ schedulers are absent; this source is 4.14.357 and
already builds `mq-deadline` and Kyber. No unverified cherry-picks were used.

### Licensing and attribution

This repository is a derivative distribution of LineageOS and other upstream work. Existing copyright notices, license files, and attribution remain intact. This changelog does not replace or alter those rights.
