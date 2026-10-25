asicen-loader.bin is the Linux loader image for PX-S3U, PX-S3U2, PX-W3U2, and PX-W3U3.
Size: 16384 bytes.
SHA-256: b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8

asicen-loader-common.bin is the common Windows BDA x64 loader image used by
the model-omitted default daemon startup.
Size: 16384 bytes.
SHA-256: 5db39facda140bdcb3a238dad10da5aac752a1ba3aaf17e2239f8d79a0ad6a2a

Both files are checked out so GitHub-hosted runners can copy them into every
binary distribution archive. The Linux image remains available for explicitly
selected compatibility use; the common image is the default. Corresponding-
source archives omit this directory (export-ignore, and the source snapshot
skips firmware/*). Redistribution rights remain unresolved. Inclusion does not
mean those rights are cleared, and neither file is covered by the project GPL.
