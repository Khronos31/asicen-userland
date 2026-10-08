# Synthetic transport-transform oracle

This offline oracle compares the portable implementation with synthetic
inputs passed through the inspected official routines. It contains no capture,
device-derived key, license table contents, or vendor data tables. The wrapper
source is deliberately separate from product code and substitutes only the
archive's multi-block DES entry point with repeated calls to its single-block
DES primitive; this avoids the observed nondeterminism in the x86-64
multi-block routine. The oracle checks 32 synthetic packets, seed derivation
for a fixed numeric vector plus 64 generated seeds and 128 one-bit seeds, and
the DES reference vectors.

Build in the offline analysis environment with the official archive objects
and the wrapper. Paths below assume the source checkout is at
`/config/GitHub/asicen-userland` and the extracted reference objects are in
`/config/.tools/asicen-work`:

```sh
g++ -std=c++17 -O0 -Wall -Wextra -Wpedantic -Werror \
  -I/config/GitHub/asicen-userland/userland/include \
  /config/GitHub/asicen-userland/docs/hardware-traces/transport-transform-oracle.cpp \
  /config/GitHub/asicen-userland/userland/src/transport_transform.cpp \
  /config/.tools/asicen-work/DTV_Lib.o \
  /config/.tools/asicen-work/DTV_Device.o \
  /config/.tools/asicen-work/des.o \
  -Wl,--wrap=des_crypt_ecb_Multi -Wl,--unresolved-symbols=ignore-all \
  -no-pie -pthread -o /config/.tools/asicen-work/transport_oracle
/config/.tools/asicen-work/transport_oracle
```

The archive objects are required only for this local differential check; they
are not shipped or linked by the product. The run validates implementation
agreement for the exercised synthetic paths, not hardware reception or the
full official receive lifecycle.
