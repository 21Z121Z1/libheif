# ISO gain-map review series for v1.24.x

`upstream/iso21496-tmap-v1.24` starts at upstream `v1.24.x` commit
`31d1d85ccbd33b9671df1fff30ddb0d6debb2fab`. That branch still reports version
1.23.5 in CMake; this is not a released v1.24 tag. PR #1 remains the integration
branch. The series carries its owned implementation through `f796f17b`, rather
than copying intervening upstream-master changes onto the v1.24.x tree.

| Commit | Review scope and dependencies | Applicable local check |
| --- | --- | --- |
| 1 / `be6da8ec` | Annex C metadata and ToneMapImage envelopes, bounded rational validation, versions and serialization | `gain_map_metadata` |
| 2 / `8786c1e0` | Equations (1)-(3), application primaries and supported NCLX/ICC transforms; preserve numerical samples and chroma phase while decoding | Metadata, math and colour tests, including independent Little CMS |
| 3 / `c7f44515` | First-class tmap reader, ordered dimg validation, child reconstruction, baseline fallback and root output boundary | Library build and core tests; container tests arrive with commit 4 |
| 4 / `c12d52da` | Experimental POD inspection, target headroom and floating output APIs; reader/container regressions | `tmap_read`, public C headers and exported symbols |
| 5 / `80b35b61` | Typed altr/stereo writers, ID allocation and primary/visibility invariants | Entity-group, ID and reader tests |
| 6 / `be5b3b51` | Gain-raster producer, tmap writer and colour/light/pixel hints; reopen graphs and pixels | Read/write and uncompressed-encode tests |
| 7 / branch tip | Pinned Google and public Apple oracles, fuzzing, CI and support/acceptance documentation | Consolidated core/oracle checks and fresh branch-specific Actions |

Each implementation commit was built from its actual cumulative source tree;
later files were absent. A saved three-way target patch keeps the migration
reviewable. The only required reader adaptation was a tmap-local check rejecting
nonzero tile coordinates: this baseline lacks the integration base's newer shared
tile-coordinate guard, while tmap exposes one whole-image tile. Its existing
regression failed before the adaptation and passed afterwards. No per-region
reconstructed tile API is introduced.

Upstream [#1893](https://github.com/strukturag/libheif/pull/1893), observed at
`717f61510c1b6c0cc0f129f07e0fe49082234acf`, is unmerged. Its corresponding
alternative/stereo/visibility function names match this dependency's typed
interfaces. Commit 5 isolates that prerequisite for reuse when an upstream
implementation is available; it does not expose an arbitrary internal group
graph. The public metadata remains a POD in `heif_experimental.h`, pending
maintainer review of a possible HDR header. The series does not freeze an HDR ABI.

The integration-only null-safe diagnostics in an absent upstream-master sequence
test and the research continuation diary are omitted. Existing CI job bodies are
retained; branch filters include this exact review branch and v1.24.x PRs so that
the same compiler, sanitizer, license, API, fuzz and independent-oracle gates can
exercise the new baseline.

The completed seven-commit tip `49ea0cbf` passed nine native core/raw-oracle
executables, five exp OFF/CMM OFF executables, 112 C-header compilations and
479 stable-symbol checks. The owned C/C++ scope passed cpplint and repository
clang-tidy; unchanged upstream/Catch/Google diagnostics from the unfiltered tidy
run remain in local evidence. Public Apple and pinned Google exchanges passed
909,433 joint assertions; Google HEIF/AVIF depends on its bundled libheif and
is compatibility evidence, not a separate HEIF graph implementation.

The [conformance matrix](tmap-conformance-matrix.md) is the remaining task queue.
Complete standards conformance remains unclaimed while the requested archives,
final audit and revision-specific acceptance evidence are incomplete. The separate
Apple Rec.709 serialized-ICC/native-pixel discrepancy remains documented without a
producer heuristic or relaxed threshold. OPPO private ProXDR remains outside scope.
