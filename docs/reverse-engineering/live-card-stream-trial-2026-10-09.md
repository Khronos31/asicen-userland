# Live reception and internal card decoding

This increment combines the existing px4-derived tuner and card services in
one direct-libusb daemon. It retains one primary frontend lease at a time:
receiver0 satellite or receiver1/T27. Card transactions serialize with USB
control operations, while bulk callbacks and resubmission remain independent.
Hardware shutdown follows both service workers; failures quarantine normal
operations while allowing bounded cleanup and GPIO restoration.
The implementation is committed as `00c6628`.

## Initial live trials

Latitude/AnduinOS, Release build, both USB functions on the host, private
mount namespace and private pcscd/IFD configuration. The system pcscd
configuration and installed recisdb were not changed. All USB/card/TS payloads
are retained privately outside Git.

With the installed recisdb1.2.4, both T27 and BS01_0 captured200,000 packets
(37,600,000 bytes), with zero TEI/continuity errors. Live decoded output had
199,999 packets, zero scrambled packets and valid PAT/PMT CRCs. All three
pipeline processes exited0. Thus exit status alone did not establish complete
output. There were10 card submits inside each active DSC reception window,
which establishes live card access rather than just offline decoding.

Decoding the same T27 input as a regular file twice produced all200,000
packets. The live decoded file was exactly the byte prefix of the complete
offline output; its missing last packet was PID0x0100. No padding packets were
added and the equal-count acceptance criterion was retained.

The local recisdb source already contains commit
`265fa62d5b076129e5f0f8ae68969385a87de495` (flush decoded stdin output).
A clean `badc171` source build (`cargo build --release --locked -p recisdb`)
was copied to an isolated executable on Latitude. Its first T27 live run
returned all200,000 decoded packets, with zero scrambled/TEI/continuity
errors. That run exposed a separate ASICEN terminal protocol failure after
the full capture: client exit6, STOP_STREAM statistics followed by
STREAM_END error255. Daemon cleanup and USB requests succeeded. This failure
is retained as evidence, not counted as a passing pipeline.

With the fixed recisdb executable, BS01_0 subsequently passed the complete
pipeline:200,000 packets before and after decoding, zero TEI/continuity
errors,195,351 scrambled input packets and none in output,190 valid PAT and
570 valid PMT sections, one video frame decoded, all processes exit0.
Its trace contains10 card submits during18.966 seconds of active satellite
DSC,37,646,336 bulk bytes, no vendor errors and no LNB/GPIOEx writes.
GPIO readbacks wereff before/after; controller05 returned to00.

The terminal failure was traced to `HardwareStreamService::detach()` exposing
detached state before joining the worker and publishing `final_`.
A concurrent reader could trigger STREAM_END using the initial empty
snapshot while STOP_STREAM later reported the completed capture. The fix
keeps reads pending until the final snapshot is published and rejects
premature snapshot access; its regression holds the worker inside cleanup
to exercise that interval.

## Final combined rerun

Source snapshot SHA-256:
`ef4789fedf3752d139cfc7cee21497c91d0106d2e98810c6db0f7e10b2accf46`.
Isolated recisdb executable SHA-256:
`ad8120dace2896fadbf00ae556c7ea353cb2b23113f0ff38ca4970730e41b704`.

The same daemon served T27 followed by BS01_0, with private PC/SC and recisdb
running during each capture. All capture/tee/decoder/video processes exited0.

| Measurement | T27 | BS01_0 |
| --- | ---: | ---: |
| Input and decoded packets | 200,000 each | 200,000 each |
| Input and decoded bytes | 37,600,000 each | 37,600,000 each |
| Scrambled input packets | 191,230 | 195,525 |
| Scrambled decoded packets | 0 | 0 |
| Invalid sync / TEI / CC discontinuities | 0 / 0 / 0 | 0 / 0 / 0 |
| CRC-valid PAT / PMT | 224 / 514 | 189 / 567 |
| Decoded video frames checked | 1 | 1 |
| Card submits inside active DSC | 12 | 12 |
| Active DSC seconds | 22.486 | 19.047 |

The trace has41,540 records, zero matched vendor errors, no LNB-mask or
GPIOEx writes, GPIOff before/after and controller05 restored to00 after
each capture. Both DSC windows closed. Daemon, pcscd and monitor exited0.

In a separate live T27 run, SIGTERM was sent to the daemon after at least
30,000 packets had reached the pipeline. The daemon exited0; the interrupted
capture client returned7, while tee and recisdb exited0. Input and decoded
files both contain5,701,476 bytes. The trace has4,276 records, two card
submits during active capture, no vendor errors or LNB/GPIOEx writes,
closed DSC, GPIOff→ff and controller05 restored to00.

## Recovery and remaining quality issue

After SIGTERM, two fresh-daemon T27 captures each returned30,000 packets
and exited0, with valid PAT/PMT and zero TEI/invalid packets. Their continuity
discontinuities were27 (packet indices4–220) and7 (indices3–334), respectively;
no later CC event occurred in either file. These are whole-file results:
startup packets were not discarded. Reacquisition works, but startup quality
is unresolved and must not be described as error-free recovery. The existing
daemon quality counters remain placeholders; the independent file validator
establishes these counts.

Each recovery trace has zero matched vendor errors, closed DSC, no LNB-mask
or GPIOEx write, GPIOff before/after and controller05 restored to00. All
isolated processes stopped; system pcscd.service remained inactive and its
socket remained active. Both USB functions were returned to the CentOS6.3
VM, where runtime0b06:0005 devices018/019 enumerated.

Private evidence is backed up under `/config/.tools/asicen-work/` in
`live-initial-trials-private-20261009.tar` and
`live-final-trials-private-20261009.tar` (mode0600). No raw TS, USB/card
payload or vendor binary is committed. Next reception work must distinguish
startup contamination/loss from sustained reception and retain the failing
samples. Secondary receivers and broader terrestrial tuning are still pending.

## Offline verification

The initial libusb-ON suite passed39/40; the unchanged card-only service test
intermittently observes the background detection timeout instead of its
expected initialization timeout. An independent clean be6b1ca baseline
reproduced the same assertion4/20 times. No expectation was weakened.
Libusb-OFF passed37/37. Focused production capture lifecycle tests also passed
under Clang19 ThreadSanitizer, including concurrent card/control and real
completion/consume/resubmit transitions, quarantine, cancellation, deadline
accounting and independent GPIO restoration after a cleanup failure.

After the snapshot-publication fix, a complete rebuilt Release/libusb-ON
suite passed40/40. An earlier invocation against a partial, stale build
directory had four missing test executables; that invocation is not used as
verification. The rebuild supplied them and tested the current source.
Focused session/IPC tests passed for libusb-OFF and under TSAN; Latitude's
fresh Release lifecycle/session/IPC tests passed3/3. The previously observed
card-only timing flake remains documented despite the final full-suite pass.

The experiment plan and unresolved test limitation are recorded in
[the acceptance plan](../live-card-stream-plan-2026-10-09.md).
