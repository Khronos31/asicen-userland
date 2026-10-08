# Receive preparation and concurrent bridge reads after guarded RF lock

Reviewer: **dot (OpenAI)**. Date: 2026-10-08.
Evidence checkpoint: [c0de090](https://github.com/Khronos31/asicen-userland/tree/c0de090eed591bd50c5caa6cb28aacb513c8d034).
This is static review of the reported trial, its sanitized trace, and the original
Linux/Windows binaries. No hardware or vendor code was executed by this review.
Binary provenance is recorded in [the first audit](static-audit-2026-10-08.md#evidence-identity-and-addressing).
The Linux archive SHA-256 is e1d68db2e09c584912a60363d89e56271ff3ad3b1bd0a319a7076fe914dad2c0.
LNB and GPIOEx exclusions remain in force; no guessed seed/key input or flag patch
is proposed. Addresses and register values below are hexadecimal; byte counts and times are decimal.

## Result and corrected scope

The reported Customer_Info trial validates the earlier missing preparation API:
both Init calls now succeed. The later diagnostic genuinely retuned T27 and
observed RF lock, but its eight bulk completions all reported zero bytes.
Neither result is yet a successful complete official application receive path.

A further preparation phase is source-proven. GenEncSeed contains conditional real controller
programming before clearing the public API gate. The earlier observation that
Start/Init do not internally call this API cannot establish that a normal
application need not call it separately. Conversely, finding an omitted phase
does not establish that it causes the zero-byte result.

## Linux: flag transition follows hardware programming

`TF_DTV_GenEncSeed` is publicly declared with APEncSeed and PCKey buffers and
lengths. The recovered implementation requires both lengths to be 16 bytes
(0x10) and validates
the application seed. The supplied archive contains no application caller from
which to recover the legitimate input contract. No key constants or table-derived
input values are reproduced here.

`DTV_Lib.o:DTV_GenEncSeed`:
- `8d85`: sets control+1c70=1 after the validation branch.
- `8dc6`: calculates derived state.
- `8de1`, `8df1`, `8e01`: calls three seed setters.
- `8e09`: calls DTV_EnableEncryptionChipTSOutput.
- `8e0e`: only then clears control+30d60.

For revision11/controller type0f with control+15d8=1, the older setter and
revision16-only setter are inactive. The active ASV5606 setter at8a20 writes
controller registers 0x10..0x1f through DTV_WriteI2CEncData. On the observed non-MPU
route its one-byte path7fe0 reaches FUSBDTV_Cmd_I2CWriteEnc and ordinary I2C
request03. Absence of vendor requests12/13/18 is not the correct discriminator.

The output helper6440..656b builds controller 0x05 from fields selecting bits 0x10,
0x20 and, when control+1c70==1, bit 0x80. If the other selector fields remain unchanged and produced the observed
pre-seed 0x20, the corresponding prepared value would be 0xa0. Its electrical meaning and reception consequence
remain unverified; this is not a recommendation to write 0xa0 independently.

These operations coordinate a shared controller. For type0f/10, local1 skips
seed writes when local0 exists and its gate is already0 (8ae0..8afa). Local1's
controller 0x05 write requires local0 to exist and still have gate1 (650e..6525).
A per-lane replay without the application lifecycle could therefore be wrong.

Important failure-reporting limit: GenEncSeed does not check the four helper
returns before clearing the gate and returning1. There is also an early return1
at8ca8 which leaves the gate unchanged. Gate0 or API return1 alone is not proof
of successful hardware preparation. The one-byte writer at7ff9 also replaces
the lower-level result with1.

## What the RF trace actually contains

In [the sanitized RF trace](https://github.com/Khronos31/asicen-userland/blob/c0de090eed591bd50c5caa6cb28aacb513c8d034/docs/hardware-traces/2026-10-08-rf-diagnostic.usbmon.txt)
at the evidence checkpoint, controller slave4a request03 writes05=20 at lines469
and519. There are no controller 0x10..0x1f writes and no05=A0 write. Together with
the reported gate1, this supports absence of the identified preparation phase.
It does not prove that this is the physical TS-output blocker.

The trace contains 160 matched slave30/regb0 read transactions. Of them, 97 return
status byte01 plus A9. Other values occur during acquisition; the late00 needs
special care because of the actual interleaving described next.

## Windows cross-check: separate vendor preparation, plus resume replay

The original W3U3 v1.1 x64 BDA binary (SHA-256
`a032a28b28e5d239aa32b9c41e7d9f61c810ada4b120dcc214c58be770ea5b88`)
contains the analogous sequence. Addresses in this section are preferred-image
VAs with image base10000, not Linux member offsets.

- VA20ee4 is the 16-byte controller 0x10..0x1f writer.
- VA24b50 writes controller 0x05, adding bit 0x80 when control+1c54==1, alongside
  other selector bits. Shared local0/local1 guards apply here too.
- VA21e56 calls the seed writer,21e5e calls the output helper, then21e63 clears
  the Windows readiness field at control+30d3c. Init1cbcc sets that field to1.
- Fresh preparation is reached through the vendor KS property SET dispatcher
  at18778, property ID9. The 32-byte property payload supplies two 16-byte inputs;
  call18c14 reaches21a84. This is a separate vendor request, not proof that
  ordinary BDA stream start automatically establishes the input contract.
- The KSDEVICE_DISPATCH.SetPower callback at14d50 has a D0 path which replays
  stored seed state through20ee4 and24b50. Replay does not establish the fresh
  application inputs or prove that the readiness gate was cleared.

As in Linux, the Windows preparation caller does not check these helper returns
before clearing the gate. A reached call is attempted programming, not verified I/O.

This corroborates that the hardware programming phase is not merely a Linux
port artifact. It still does not supply a verified legitimate application caller
or justify constructing a property request with guessed inputs.

## Additional observation: last direct lock read splits a tuner transaction

At lines2277..2284, with timestamps in the trace's microsecond clock:

1. Polling traffic arms tuner register0d through FE/C6.
2. Request14 at308553195 triggers the FE/C7 read without STOP; it completes at
   308554611 (lines2279..2280).
3. A different URB's request02 reads demod b0 at308554649 and returns01 00 at
   308556163 (lines2281..2282).
4. The pending tuner request19 is submitted at308558671 and also returns01 00
   at308562863 (lines2283..2284).
5. Later b0 polls again return01 A9 (lines2297..2298 and2303..2304).

`TunerControl.o:TunerRegRead` source0 constructs these as separate calls:
FE/C6/register at1260..1285, FE/C7 with mode4 at1292..12a5, then mode2 read
at123b..1250. The no-stop trigger/read pair is therefore a multi-request logical
operation. The trace proves another same-demod transaction was inserted inside
that pair. URB tags themselves are not thread IDs; attribution to the diagnostic
and polling activity follows their distinct known call patterns.

This is a concrete concurrency confound, not proof that the00 was a false unlock,
or that the interleaving caused zero TS. It means the last direct poll must not
be treated as clean evidence of RF loss without controlling transaction ownership.
A mutex only around each USB request would not protect the full bridge sequence;
all participating I2C operations need a compatible sequence-level discipline.
No new synchronization patch or hardware trial is performed here.

## Remaining evidence needed

Both recovered Linux distributions reference DemoAP/apdtvdemo in their scripts
but omit those application files. A matching original DemoAP, vendor API contract,
or verified legitimate caller is needed to establish input provenance and lane
ordering. Do not fill this gap with arbitrary bytes, reconstructed table inputs,
a direct gate write, or isolated controller 0x05 mutation.

For an independently authorized reference trace, distinguish:
- real controller 0x10..0x1f programming and subsequent05 setting;
- USB completion and device/I2C status, not API return alone;
- successful public tune/lock calls that actually reach RF operations;
- isolated complete I2C bridge transactions, without competing reads;
- nonzero bulk completions before framing or decryption acceptance.

The earlier source statement about link conversion occurring after USB reads
still describes data processing. It does not rule out a separate pre-read
hardware initialization step associated with that link.
