# Source provenance

The shared `dtv-userland-core` dependency is pinned to commit
`80ccc7b7e9ccad7c9b3656f02a28d12f003c45a0` in
`cmake/dtv-userland-core.revision`. It is fetched from the private
`Khronos31/dtv-userland-core` repository for normal builds and included in
corresponding-source archives from that exact Git commit. The dependency's
`LICENSE` and `PROVENANCE.json` accompany the bundled source. Its MIT notice
is reproduced in `THIRD_PARTY_NOTICES.md`. This revision adds the optional
`dtv::receiver` lifecycle engine with exact-Hz strict receiver admission,
cancellation, deadline accounting, and guarded result publication; immutable
profile preparation remains in the ASICEN GPL facade, which also retains
receiver validation, active profile count, adapters, and stop notifications.

The C1 card ownership increment was reviewed against common commit
`121071c3458e6a03d69f05794c066b704996b13a`; the active product lock selects the pushed combined dependency
commit `80ccc7b7e9ccad7c9b3656f02a28d12f003c45a0` above. Its optional `dtv::card` engine owns generic card
ownership and session lifecycle only. The GPL ASICEN facade retains Native-session exclusion, active
receiver/profile routing, product hardware adapters, the card operation guard, and both stop notifications.
The corresponding-source archive includes the common card engine templates and its audit rejects an archive
missing `card_ownership_engine.tpp`.

This dependency record does not change the product's GPL license, existing
third-party notices, or the separate vendor firmware rights status.
