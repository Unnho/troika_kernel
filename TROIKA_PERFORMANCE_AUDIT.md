# Troika MIF and UFS tuning audit

This release was reviewed against the LineageOS 23.2 Troika kernel source,
not applied from the supplied AI-generated recommendations verbatim.

## Accepted changes

### SCSI multi-queue for UFS

`CONFIG_SCSI_MQ_DEFAULT=y` enables the block multi-queue path for the UFS
SCSI host. The configuration already contains `CONFIG_MQ_IOSCHED_DEADLINE=y`
and `CONFIG_MQ_IOSCHED_KYBER=y`. For a single-hardware-queue MQ device this
4.14 block layer selects `mq-deadline` by default; no device-side sysfs hack
is required.

### Big-cluster to MIF constraints

The `cpufreq_domain1` `mif-perf` table is active in `exynos9610.dtsi`. Only
the interactive big-core range was raised: 1.248–1.456 GHz now requests
845–1014 MHz MIF; 1.508–1.664 GHz requests 1352 MHz; 1.898 GHz requests the
validated 2093 MHz OPP. The MIF idle floor remains 546 MHz, and the
little-core table is unchanged. This bounds the battery/thermal trade-off
while avoiding the report's much more aggressive, system-wide uplift.

## Rejected recommendations

- The reports identify the kernel as 5.10; `Makefile` identifies this source
  as 4.14.357-openela. Version-dependent conclusions were rechecked.
- Raising the global MIF `default_qos` would create a permanent boot-time
  PM-QoS minimum request, so it was not changed.
- The UFS `freq-table-hz`, Gear 3/one-lane link limits, aggregation threshold,
  Hibern8/clock gating, and Exynos quirks are controller-specific. They are
  already set by the active UFS driver and were left intact.
- Forced `read_ahead_kb` and `nr_requests` settings belong in device init
  policy and require on-device benchmarking; they were not hard-coded into the
  kernel.
- No speculative upstream cherry-picks were selected. A boot-critical storage
  driver must not receive unrelated patches without a known provenance and
  device testing.

## Validation required on device

After flashing, confirm `/sys/class/scsi_host/host*/use_blk_mq` reports `1`,
and inspect `/sys/block/sda/queue/scheduler` for `[mq-deadline]`. Benchmark
MIF and UFS while monitoring temperature and battery use before treating the
change as a permanent daily-driver policy.
