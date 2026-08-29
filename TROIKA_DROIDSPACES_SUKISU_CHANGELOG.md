# Troika Droidspaces + SukiSU release notes

## Included

- Droidspaces container support: PID, mount, UTS, IPC, network, and user
  namespaces; seccomp filtering; devtmpfs; loop, FUSE, OverlayFS, TUN/TAP,
  veth, bridge, and the required cgroup controllers.
- SukiSU Ultra built-in/manual-hook integration, pinned to the `builtin`
  implementation and adapted for this Linux 4.14 kernel.  The integration
  includes the legacy `path_umount` helper required by SukiSU's mounted-module
  handling.
- Kernel-side performance configuration: UFS SCSI multi-queue support from
  the preceding Troika r2 base, disabled SLUB per-CPU partial caching (aligned
  with Samsung's Exynos 9610 Mint configuration), and disabled scheduler
  statistics to avoid hot-path accounting overhead.

## Intentional limits

- SUSFS and KPM are disabled.  This release provides root functionality, not
  root-hiding hooks; KPM additionally depends on memory-management interfaces
  unavailable in this 4.14 kernel.
- Cgroup v2 and cgroup namespaces are optional Droidspaces facilities, but
  are absent from this legacy Linux 4.14 source tree and are not backported.
- No new DTB/DTBO performance tuning is included.  Flash the kernel Image
  using the device's established kernel-install procedure; this build does
  not require a DTBO change.

## Validation

- Final merged configuration was checked for Droidspaces' mandatory features:
  IPC namespace, devtmpfs, bridge/veth, seccomp, namespace support, cgroups,
  FUSE, OverlayFS, and networking facilities are enabled.
- The kernel is built as `Image dtbs` with Clang and LTO.  Device boot and
  benchmark validation remain necessary before claiming a measured performance
  improvement.
