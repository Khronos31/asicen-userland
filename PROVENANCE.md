# Source provenance

The shared `dtv-userland-core` dependency is pinned to commit
`35249f85347093ca3b431cbe4a730b2366de7ada` in
`cmake/dtv-userland-core.revision`. Normal builds fetch that exact revision from
`Khronos31/dtv-userland-core`; corresponding-source archives include a Git
archive of the same commit, including its root `LICENSE`, `PROVENANCE.json`,
adapter-specific `COPYING` files, and adapter README/provenance records. The
revision and file inventory are checked against the product lock by the source
archive audit.

The root MIT license applies only to the independently licensed engine and
other destinations enumerated in the dependency's `PROVENANCE.json`. Optional
product-facing adapters under `adapters/host_daemon`, `host_facade`,
`host_runtime`, `host_ts`, and `windows_winscard` retain their individual
GPL-2.0-only notices in each directory's `COPYING`; they are not covered by the
root MIT license. ASICEN's product source and GPL facade remain under this
repository's GPL-2.0-only license. Bundling a shared MIT engine does not
relicense the product, and bundling a GPL adapter does not expand the MIT
license scope.

The source archive includes the current host daemon, facade, runtime, TS, and
Windows adapter sources because the product build uses those interfaces and
implementations at the pinned revision. Their source and adapter provenance
remain distinct from the MIT engine provenance.

This dependency record does not change the product's GPL license, existing
third-party notices, or the separate vendor firmware rights status.
