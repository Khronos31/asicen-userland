# Source provenance

The shared `dtv-userland-core` dependency is pinned to commit
`0c136e2b5513524e31ab317945244c599c261e65` in
`cmake/dtv-userland-core.revision`. It is fetched from the public
`Khronos31/dtv-userland-core` repository for normal builds and included in
corresponding-source archives from that exact Git commit. The dependency's
`LICENSE` and `PROVENANCE.json` accompany the bundled source. The root MIT
license covers the `dtv::core`, `dtv::wire`, `dtv::identity`, `dtv::tuning`,
`dtv::json`, `dtv::control`, `dtv::receiver`, and `dtv::card` library targets
at this pin. The product enables receiver and card support and links these
targets where needed. The optional `dtv::host_ctl` frontend is a separate
GPL-2.0-only adapter, enabled for `asicenctl`; it is not part of the MIT
subset. The MIT notice is reproduced in `THIRD_PARTY_NOTICES.md`.

The C1 card ownership increment was reviewed against common commit
`121071c3458e6a03d69f05794c066b704996b13a`; the active product lock is the
combined dependency commit `0c136e2b5513524e31ab317945244c599c261e65` above.
Its optional `dtv::card` target provides the generic card ownership engine.
The GPL ASICEN facade retains Native-session exclusion, active receiver/profile
routing, product hardware adapters, the card operation guard, and both stop
notifications. The corresponding-source archive includes the common card
engine templates and its audit rejects an archive missing
`card_ownership_engine.tpp`.

This dependency record does not change the product's GPL license, existing
third-party notices, or the separate vendor firmware rights status.
