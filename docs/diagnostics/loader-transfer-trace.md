# Cold loader transfer trace

The original diagnostic increment was based on readiness candidate
`f643e1ea6537f6c944c0bf377095e0da638de695`. It does not fix the cold-loader
failure or qualify a release. For the later, narrowly scoped acquisition retry,
see [sibling-acquisition-retry.md](sibling-acquisition-retry.md).
The default post-gain readiness remains 340 ms;
the shared PX4 code, firmware bytes, LNA values and capture behavior are unchanged.

## Evidence and unresolved alternatives

The [saved E02 logs](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6152267732)
show that the failed daemon exited before the attempted SIGTERM. The existing
failure line identifies a loader vendor-OUT error but not its target or chunk.
Both failed and successful attempts include an intermediate sibling loader
followed by another disconnect/re-enumeration. That transition alone is not
a failure signature.

After primary upload and GPIO restore, the current resolver can accept the
first device at the sibling topology without proving that the previous
generation disappeared. Opening that old generation remains a possible race.
Primary final-AC acceptance followed by host-side IO is another possibility;
that path returns before this daemon performs GPIO restore. Existing logs
cannot distinguish them. The saved first-attempt script is not versioned, so
identical command lines between the two attempts are not established.

See [the analysis](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6152317028).
Do not treat IO as success, blindly resend firmware, add a recovery reset,
or change delays while collecting this evidence.

## Enablement and output

Set `ASICEN_DIAGNOSTIC_LOADER=1` on the actual daemon process that performs
firmware setup. Unset or `0` disables tracing. Other values are invalid.
Argument parsing, identity enumeration and endpoint setup may precede loader
diagnostic validation; an invalid value is not a no-device-I/O probe.

Keep `ASICEN_DIAGNOSTIC_POST_GAIN_MS` unset, so readiness follows the candidate's
normal behavior. These are separate diagnostic controls. Do not modify other
environment settings or dump the process's complete environment.

Record stderr directly to a regular file, not through a live consumer that
could block. The trace uses a fixed-capacity in-memory event buffer and emits
as the loader operation exits, after local USB handles are destroyed. Existing
error messages remain visible.
Logging has overhead and may affect a race; it cannot promise zero timing
impact. A hard crash or forced kill may prevent buffered events from appearing.

The trace records numeric target identity and phase results, including:

- selected primary/sibling index and cached bus/address/port/VID:PID
- resolution, open and interface claim results
- upload begin/result, primary runtime identity, GPIO restore begin/result
- failed or short vendor-OUT chunk/total, request/value/index, firmware offset,
  requested length, return code and relative start/end times
- bounded-buffer overflow and preserved failure information

No USB payload, firmware content, TS, card response, credential or arbitrary
environment value is part of the event format. Unknown identity fields remain
explicitly unknown rather than being inferred from a stale USB address.
The printed port is the component path (for example `2.1`); combine it with
`bus=1` to identify the full topology `1-2.1`. The final `load_status` belongs
to the loader helper. In particular, `1` means already-runtime/warm and is not
the daemon's process exit status. Keep the harness's actual daemon exit record.

## Clock interpretation

The startup anchor records wall time and available explicitly identified
Linux monotonic/boottime clocks, with read boundaries rather than an assumption
of atomic sampling. Linux time namespaces can offset monotonic and boottime;
the diagnostic accepts their numeric offsets only when active and
time-for-children namespace identities match and remain stable across the read.
Otherwise the offsets remain unknown. It does not modify any clock or namespace
setting.
See [Linux time_namespaces(7)](https://man7.org/linux/man-pages/man7/time_namespaces.7.html).

Do not add an unrelated current `btime` to historical kernel timestamps or
assume C++ steady_clock and dmesg have identical epochs. Correlate the recorded
clock anchors, namespace context and kernel-log clock first; suspend and wall
clock adjustments must not be silently ignored.

The [prepared E02 harness](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6152547272)
collects launch commands, exit results and pre/post clocks without a hardware
run yet. Its historical dmesg offset bounds are conditional estimates, not a
measured clock conversion to apply to new attempts. Kernel logging clocks can
differ from userspace monotonic clocks even when time-namespace offsets are zero.
The diagnostic does not require a writable `/dev/kmsg` or any permission change.
Confirm that the harness passes `ASICEN_DIAGNOSTIC_LOADER=1` through to the daemon
and records it in `launch.cmd`, while leaving post-gain diagnostics unset.

## Next naturally required cold startup

Use the verified diagnostic artifact and retain the previous binaries and logs.
Starting a physical reconnect/power cycle requires the owner's coordination;
this document does not request a new cycle solely to reproduce the error.
Do not reset a parent hub or affect another tuner.

For the next cold startup already needed by the test plan:

1. Confirm the selected enclosure and current primary/sibling port identities.
   Record pre-start list output, diagnostic archive/daemon hashes and command.
2. Use the existing bounded startup harness and exact firmware image with only
   loader tracing enabled. Preserve daemon exit status separately from the
   harness/queue status. A successful queue does not mean daemon startup passed.
3. Preserve the complete daemon log, launch/exit/check timestamps and relevant
   kernel USB lines in their original timestamp format. Record whether a stop
   signal was actually delivered or the process had already exited.
4. On startup failure, stop this attempt. Record post-start enumeration without
   automatically retrying upload or cycling power. Review the failed target,
   chunk and GPIO ordering before deciding on a recovery or software change.

No reception, LNB, card or soak test is needed to identify the failing loader
transfer. Do not publish raw firmware, TS or card data. Missing fields in older
logs remain unknown; this new trace cannot recover them retroactively.
