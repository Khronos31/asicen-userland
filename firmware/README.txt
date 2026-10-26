asicen-loader.bin is the common Windows BDA x64 loader image used by the
model-omitted default daemon startup.
Size: 16384 bytes.
SHA-256: 5db39facda140bdcb3a238dad10da5aac752a1ba3aaf17e2239f8d79a0ad6a2a

This file is checked out so GitHub-hosted runners can copy it into every binary
distribution archive. Corresponding-source archives omit this directory
(export-ignore, and the source snapshot skips firmware/*). The image was
extracted from the verified Windows BDA x64 Loader.sys:
source artifact SHA-256: 071469e57215774366eaab14055d23a134452248be3b7caf1d85990b3f961ff2
Loader.sys SHA-256: edd9badb7588f59c6438e1ceaa0e0f3f33b2ce75667e541e1d75c45e2c485e5
Load address: 0x55d6.

Redistribution rights remain unresolved. Inclusion does not mean those rights
are cleared, and the file is not covered by the project GPL.
