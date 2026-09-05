.. SPDX-License-Identifier: GPL-2.0-only

Apple J713 PMP thermal policy
============================

``CONFIG_APPLE_PMP_THERMAL`` provides an opt-in Linux CPU cooling policy for
the validated J713/T8132 board. It does not change PMP firmware, the SMC, or
any firmware protection threshold. The 70 C passive trip and approximately
80 C operating target are Linux policy choices, not Apple specifications.

The module reuses the same read-only record decoder as the ``apple_pmp``
hwmon provider. The six-channel hwmon ABI is unchanged. ``apple-pmp-die``
is an additional thermal zone whose sensor is the aggregate maximum of
all valid documented records, including the normally invalid sixth record.
No individual record is identified as a CPU cluster, GPU, or other block.

The zone polls every 25 ms and uses ``step_wise`` with 10 C hysteresis.
The large release hysteresis avoids repeatedly restoring full frequency
when the hotspot drops abruptly under a cap but sustained load remains.
The early 70 C trip leaves headroom for the first rapid load transient.
Both CPU clusters have standard cpufreq cooling devices. The initial
cooling floors correspond to 2616000 kHz for the four-core P cluster and
1860000 kHz for the six-core E cluster; further cooling can reach their
minimum OPPs. Below the hysteresis threshold the governor can remove the
cooling requests. ``schedutil`` and userspace policy limits remain in force:
thermal limits are additional frequency-QoS maximum requests, not writes
that replace userspace's ``scaling_max_freq`` configuration.

Registration starts with minimum-frequency guards on both clusters. Read
failures reinstate those guards without reporting an invented temperature.
Recovery requires three valid reads and, when hot, active cooling on both
clusters. Disabling the thermal zone also retains minimum-frequency guards.
This cannot detect firmware that freezes with coherent, plausible but stale
records: the documented record ABI contains no freshness counter.

The report device must already be bound and the exact J713 CPU policy
topology and cooling-floor OPPs must exist. No other Apple board is enabled
implicitly. Because this is a board operating policy using existing devices,
loading ``apple-pmp-thermal.ko`` needs no device-tree overlay or PMP rebind.
Built-in use requires the suppliers to be initialized before this client;
module use after boot is the validated deployment path.

Unloading unregisters the zone, joins its polling work, removes the cooling
devices and this module's QoS requests, and releases its mappings/references.
It removes Linux thermal protection: unload only while idle with another
recovery path available. It must never force-unload a module or reset PMP.

An 80 C hard ceiling is not guaranteed. Sensor refresh, polling latency,
thermal inertia, workloads outside the CPUs, and minimum-OPP cooling capacity
can cause overshoot. This driver neither invents an emergency shutdown trip
nor replaces autonomous firmware protection. Independent actual-clock and
full firmware thermal-protection validation remain separate work.
