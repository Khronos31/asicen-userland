# Direct libusb link preparation trials

The primary PX-W3U3 USB function was detached from the isolated CentOS VM
and opened directly on Latitude (AnduinOS), bus 1 address 102, port 1-2.1.
Host usbmon was already recording before the handoff. The secondary function
was not changed. Shared demod initialization and T27 tuning with the existing
portable frontend reached terrestrial lock A9.

## Baseline and readback limitation

With controller 4a/05 set to 20 and verified, local 1, reset state 1,
four 4096-byte transfers, filter-start, and five seconds produced zero bytes.
All four cancelled transfers also reported zero actual length. Controller05
was restored to 00 and read back successfully.

The first seed diagnostic (`d34e69b`) passed its revision/type guard:
SysCtrl02 returned status/revision/tag `01 11 52`, controller09 was `1e`
(type 0f). All sixteen seed writes and the subsequent controller05=a0 write
returned status01 with their exact requested lengths. The diagnostic aborted
because the 16-byte seed readback did not match the supplied random seed.
Controller05 read back a0, then 00 after cleanup.

All three seed-window reads (before application, after application, and after
the attempted restoration) were identical. None of the sixteen returned
bytes matched the supplied seed. A separate DSC-stopped single-register
check also returned the unchanged value after writing its bitwise inverse.
Sixteen individual reads agreed with the contiguous read. This is evidence
that these reads do not verify the written seed state; it is not proof that
the seed writes have no effect.

The official `DTV_Set_EncSeedRegASV5606` at DTV_Lib.o .text 8a20–8b10 only
writes registers10..1f and checks write results. It does not read or compare
the seed. Consequently d34e69b's seed snapshot/restoration claim was too
strong: writing back this read window and observing it unchanged does not
establish restoration of the previous seed latch.

The revised experimental contract requires controller05=00 before applying
an ephemeral caller seed. It checks each write acknowledgement and verifies
controller output state, then validates the resulting transport offline.
Cleanup stops DSC, drains transfers, requests zero writes to the seed
registers, and restores/verifies controller05=00. Seed clearing itself cannot
be verified through these reads and must be reported as such. This replaces
the newly introduced seed-restoration requirement; it does not relax the
existing DSC, CF, output-state, or device-ownership checks.

Raw captures, random seeds, and traces remain private outside Git. No LNB or
GPIOEx writes are part of these trials.
