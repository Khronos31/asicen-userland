# LNB requests and hardware capabilities

Implementation and static review: dot (OpenAI). This extension starts from
`origin/dot/usb-model-support-0.1.0`, commit
`9954e1572db7c9c315d1da6beb47c72a3c87afeb`, preserving its Android/Termux work.
No physical receiver or vendor executable was run for this change.

## Same client operation as px4-userland

`asicen-ts` accepts `--lnb-voltage 0|15` for ISDB-S, with default `0`, and
transmits the request through the existing tune IPC field. Other voltages,
duplicate options, and an explicit LNB option on terrestrial channels are
invalid, including an explicit terrestrial `--lnb-voltage 0`.

```sh
asicen-ts --instance tuner --receiver 0 --channel BS01_0 \
  --lnb-voltage 15 --duration-seconds 10 --output capture.ts
```

On the three software-controlled models, use `--lnb-voltage 0` when this
receiver should not supply antenna power. On S3U/S3U2 it means no software
power selection and cannot promise that the antenna connector is unpowered.
The AC adapter is required when enabling LNB power according to the official
W3U2/W3U3/V2 instructions. Neither a successful USB transfer nor a GPIO readback
measures the voltage at the antenna connector.

## Five-model result

| Model | Physical LNB power | Software request in this backend |
|---|---|---|
| PX-S3U | Official specification: external DC15V | `15`: UNSUPPORTED; `0`: legacy no-selection |
| PX-S3U2 | Official manual describes LNB power | `15`: UNSUPPORTED; `0`: legacy no-selection |
| PX-W3U2 | Official utility documents ON/OFF | Source-backed GPIO20 active-low switching |
| PX-W3U3 | Official manual documents ON/OFF | Source-backed GPIO20 active-low switching |
| PX-W3U3 V2.0 | Official utility documents ON/OFF | Source-backed GPIO20 active-high switching |

For S3U/S3U2, `0` is **not a guarantee of zero volts or physical power OFF**.
The original software setters are no-ops in the recovered drivers. Their
external supply behavior still needs electrical verification. Do not copy
W3U3 GPIO values to these boards or report a successful software switch that
does not occur. The request to make all five models software-switchable could
therefore only be completed for the three models with proven control paths.

The S3U2 package's LNB registry files actually target W3U2 (`0B06_0004`), and
its manual also mixes model names. Those files are not evidence of a working
S3U2 software switch. The physical-support statement remains separate.

## Power transaction contract

The transaction follows the pinned px4-userland implementation: both ON and
OFF requests are applied before tuning, including a 15V-to-0V transition.
The requested setting is committed after a successful tune. Failed retuning restores
the prior committed setting. Closing the hardware receiver and terminal
shutdown disable software-controlled power rather than restoring a previously
powered GPIO snapshot. Cleanup failures remain visible and quarantine the
backend instead of reporting safe shutdown as successful.

V2 also requires the source driver's GPIO80-dependent feedback cutoff while
power is enabled. This digital condition is not identified here as a measured
overcurrent threshold. A cutoff must not be undone by a stale tune rollback.
The dedicated static audit records the read request and polling cadence.

Mock mode exercises synthetic requests and lifecycle behavior only. It cannot
establish electrical support, and may emulate requests that a particular real
model rejects. The existing one-active-capture and primary-only receiver
restrictions still apply.

## Primary sources

- [S3U official shop specification, archived image](https://web.archive.org/web/20131226071800id_/http://www.plexshop.jp/design/gmosp1096/PX-S3U/pxs3u_02.jpg): external DC15V LNB power. Its 500mA figure is labelled maximum LNB consumption, not a measured output rating.
- [S3U2 official manual](https://plex-net.co.jp/plex/px-s3u2/px-s3u2_manual.pdf), page 3: antenna power, AC adapter requirement, inconsistent utility filenames.
- [W3U2 official package](https://plex-net.co.jp/plex/px-w3u2/PX-W3U2.Driver_Utility_Package_Ver.1.0.3.zip), root readme and Utility/LNB files.
- [W3U3 official manual](https://plex-net.co.jp/plex/px-w3u3/px-w3u3_manual.pdf), page 3.
- [V2 official package](https://plex-net.co.jp/plex/px-w3u3v2/Driver_PX-W3U3_V2_Ver1.0.zip), root readme and V2 LNB utility files.

Exact binary/control-flow evidence is recorded in
[the static LNB audit](reverse-engineering/lnb-control-facts.md).

## Offline verification

- Release builds with libusb enabled and disabled succeeded.
- The enabled local suite passed 43/50 cases. The remaining seven fail on the
  unchanged base as well: six require AF_UNIX sockets denied in this sandbox,
  and the inherited USB-fd test cannot initialize system libusb here.
- The disabled local suite passed 40/46 cases; the same six socket cases are
  blocked. These are not represented as passed tests.
- All six focused model/IPC/LNB/session/capture suites passed after the final
  backend changes. The two new LNB suites passed AddressSanitizer and
  UndefinedBehaviorSanitizer with leak detection disabled for this environment.
- Independent review and repeated LNB lifecycle execution found no remaining
  P1/P2 issue. Tests cover polarity, numeric setup packets, transaction rollback,
  failed restoration, V2 cutoff during long gate-held work, cleanup and
  unsupported-model rejection.
- GitHub Actions results on the PR establish platform integration separately;
  none of these tests establishes antenna connector voltage.
