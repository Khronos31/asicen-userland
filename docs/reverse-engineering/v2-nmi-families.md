# V2 Newport Media terrestrial RF families

## Scope and provenance

`userland/src/v2_nmi.cpp` implements the portable terrestrial tuner path used by
PX-W3U3 V2, including all four chip-family branches actually accepted by the
vendor initializer. This is static source/disassembly work, with register-I/O
mocks. It does **not** establish successful reception on physical hardware or
activate a product, USB transport, power, firmware, GPIO, card, or LNB path.

Sources:

- Official V2 x64 SYS, SHA-256
  `5c7174d62eef7d704f44904ac1336261135a1edfccd2776e5e1c45ce7088f0f2`.
  Addresses below are RVAs, which equal file offsets in this image. Its displayed
  disassembly virtual addresses are RVA + `0x10000`.
- Public GPL reference
  [`nm131.c`, knight-rider/ptx](https://github.com/knight-rider/ptx/blob/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1/drivers/media/tuners/nm131.c),
  SHA-256 of the inspected source
  `fd35d5a07754627d8bea839c21cc041bcd389cbaf04f0be398faf6240a264adf`.
  Copyright Budi Rachmanto, AreMa Inc.; source declares `MODULE_LICENSE("GPL")`.
  The portable implementation preserves attribution and is GPL-2.0-only.

No vendor executable, encrypted initialization block, key material, or large
non-register lookup table is embedded in this implementation. The published
constants describe register programming or its arithmetic. The vendor binary
was not executed, and hardware was not accessed.

## Exact active configuration

`0x1f334`, even/internal terrestrial-source branch, constructs the tuner request:

| Field offset | Value |
| --- | --- |
| `+0x00` | Caller frequency + 143,000 Hz |
| `+0x04` | 0 |
| `+0x08` | 4,000,000 |
| `+0x0c` | 6 (standard selector) |
| `+0x10` | 2 (output selector) |
| `+0x14` | 0 |
| `+0x15` | 1 |
| `+0x18` | 0 |
| `+0x1c`, `+0x1d`, `+0x20`, `+0x21` | 0 |
| `+0x24` | 0 |
| `+0x28`, `+0x29`, `+0x2a` | 0 |

The callback route is `0x1a42c` → `0x1a388` → RF `0x1964c`, then digital
`0x17d3c`. The NM131 AFC/measurement/notch branch at `0x1a49d` requires output
selector 0 or 1. It is not used by this product's selector-2 ISDB-T path.
Similarly, analog standards, alternative IF/output modes, and diagnostic gain
controls are outside this implementation's scope.

`initialize_v2_nmi` receives an RF-only interface; demod initialization is owned
by the surrounding frontend. `tune_v2_nmi` receives the **already offset** Hz
value, so it does not apply a second 143 kHz offset. The surrounding product
frontend validates/remaps its RF input and obtains a fresh chip-ID read before
calling the tune function. A successful NMI operation means the checked
programming sequence completed; the frontend separately verifies demod lock.

## Initialization and IDs

`0x1b650` accepts exactly these predicates, in this order:

| Family | Predicate |
| --- | --- |
| NM131 | `(id & 0x000fff00) == 0x00013100` |
| NM120 | `(id & 0x000fff00) == 0x00012000` |
| NM130 | `(id & 0x00ffff00) == 0x00013000` |
| Extended 813000 | `(id & 0x00ffff00) == 0x00813000` |

The masks, including ignored upper/revision bits, are the vendor's actual
predicates rather than inferred model names. Unknown IDs return
`UnsupportedChip` before any tuner write.

The source sequence is:

1. Read 32-bit ID from `0x3fc` through `0x1b358`.
2. Write 30 common RF byte pairs from `0x28eb8`.
3. Read RF `0x36`, clear bit 7, and write it back. The product initializer's LDO
   bypass configuration is zero; the RF writer also clears this bit on every
   subsequent RF `0x36` write.
4. Write digital `0x164 = 0x800`, `0x1c0 = 0x2d8c19c7` from `0x28f20`.
5. Apply the family table: NM120 nine pairs at `0x28ef8`; NM130 seven at
   `0x28f10`; extended 813000 three at `0x28d84`; NM131 writes RF `0x28 = 0`.
6. Read RF `0x00`, `0x34`, `0x35` for the vendor's cache.
7. Initializer `0x1c1b8` invokes callback `+0x128 = 0x1a66c` with enable=1.
   Its zero-initialized gain selector yields RF `0x0a = 0xfb`.
8. Invoke callback `+8 = 0x1b358`, reading ID again.

There is no NMI calibration loop, sleep, or poll in this initialization path.
The six reads and write totals are NM120 43, NM130 41, NM131 35, extended 37.
All operations are checked, unlike several ignored transfer results in the GPL
reference. The first ID is the accepted/stored identity, matching the source;
the final reread is also checked for transport failure.

## RF tuning

The common PLL path in `0x1964c`:

- Uses the source's LO divider boundaries and VHF filter byte pairs.
- Uses a 24,000 kHz crystal parameter, modified by RF `0x21[1:0]`.
- Computes the 19-bit fractional divider with the original unsigned 32-bit
  multiply, shift, and truncation.
- Allows at most one +1,000 Hz retry for a nonzero fractional result.
- Computes divider byte/clamp and the digital clock offset with the source's
  unsigned 32-bit wrap behavior, rather than substituting wider arithmetic.
- Writes RF `0x01`–`0x04`, preserves `0x1d[4:0]`, and sets its divider bits.
- Keeps a per-call RF `0x05` cache, suppressing an unchanged write on the retry.

The RF `0x05 = 5` window is 120.1–120.4 MHz inclusive; standard 6 otherwise
uses `0x85`. The GPL simplification `frequency <= 120.4 MHz` is not used.

Source-backed differences include:

| Branch | Relevant behavior |
| --- | --- |
| NM120 (`0x19b1e`) | RF `25/27/29/2e` changes below/above 300 MHz; no fractional-dependent `1b` rewrite |
| NM130 (`0x196f2`, `0x19bf2`) | Exact-frequency crystal-divider exceptions; `0e` exceptions; `25/2e` exceptions at 115/123 MHz; `30/32` switch at 139 MHz |
| NM131 (`0x19faf`) | `0e=45`, `25=fa`, `2e=56`, `26=82`; gain-band settings; `34=78`, `35=54` for standard 6/output 2 |
| Extended (`0x19e50`) | `0e=45`, `25=fa`, `26=82`; same gain-band settings and `34/35`; `2e=56` is already set during its init |

NM131/extended gain transitions are 762, 786, and 818 MHz. Their existence in
the register algorithm does not expand the product frontend's accepted channel
range. The lower RF `0x36` threshold is **155 MHz**, not the 150 MHz in the
simplified GPL routine. Common `0x37 = 0x9c` applies at 155–300 MHz (upper bound
exclusive), otherwise `0x84`.

## Digital programming and acquisition

The implementation applies the complete first-tune mode-transition writes from
`0x17d3c`. The vendor initializes cached standard/output selectors to -1, so
these writes are source-backed. To keep this portable API independent of
vendor global cache lifetime, the same transition writes are reapplied on later
tunes. Read-modify-write masks are preserved. This is a deliberate stateless
adaptation; later-tune transfer counts are not claimed to match the cache-
optimized vendor implementation.

Important differences from a broad reuse of the GPL simplification:

- NM130 digital `0x164 = 0x300`; other families use `0x600` below 300 MHz and
  `0x500` above it.
- Digital `0x21c` computes `2 * (262144000 / ((clock >> 14) + 6750))`.
  Dividing a doubled numerator can differ by one; the implementation follows
  the binary's division-before-doubling order.
- Digital `0x210` applies mask `0xfe00fff3` after the wrapped unsigned
  multiplication/subtraction and division.
- Digital `0x104` uses mask `0x97ffffd1`; mode-2 family enables are `0x00a00000`
  for NM120/NM130 and `0x02600000` for NM131/extended with selector `+0x18=0`.
- After the two digital `0x104` reset writes, the source waits 1 ms, reads
  `0x328`, and conditionally performs one additional reset pulse. This is a
  single bounded check, not an unbounded calibration poll.
- Demod prepare at `0x169dc` writes `01=50,47=30,25=00,20=00,23=4d`.
- Acquire `0x16b70` waits **250 ms before** writing
  `23=4c,01=50,71=01,72=24`. The GPL reference omitted this wait.

## Verification

`userland/tests/v2_nmi_tests.cpp` uses a register-I/O mock with failure injection
at every init/tune operation, including delay and demod writes. An unhealthy
interface is checked before every operation. Transfer failure immediately stops
the sequence, including when the mock does not maintain a sticky error flag.
The surrounding frontend adapter supplies cancellation/deadline enforcement.

Checked fixtures include all four IDs, their exact default write counts,
unknown-ID no-write behavior, source ID masks, zero/overflow rejection,
90.143/120.143/473.143/770.143 MHz numeric results, NM130 integer-reference
exceptions, gain thresholds, and status-triggered reset. No vendor code is run
by these tests.

For 473.143 MHz and reset defaults, the one retry yields 473.144 MHz,
VCO=3,785,152, integer=157, fraction=`0x5b7a1`, divider=17,
clock offset=3,407,872. RF `01..04` is `4e 43 6f 1b`; digital
`230=000bbca4`, `21c=08012656`, `210=000037a3`. Final `104` is
`30a18821` for NM120/NM130 and `32618821` for NM131/extended.
The first-tune event totals are 66, 71, 74, and 72 respectively, including the
nine demod writes and two delays.

Standalone build:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iuserland/include \
  userland/src/v2_nmi.cpp userland/tests/v2_nmi_tests.cpp \
  -o /tmp/v2_nmi_tests
/tmp/v2_nmi_tests
```

Hardware receive quality, routing, power sequencing, model activation, and
shared-enclosure ownership remain separate integration and physical-QA gates.
