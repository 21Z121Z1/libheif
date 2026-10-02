# ISO gain-map continuation from PR #1

Baseline: `f4679fa14ad36e57aee60190a61650030c32659e` on
`iso21496-gain-map-metadata-core`. Work continues on that PR branch.

## Implementation and acceptance

1. Establish a macOS 27 / Xcode 27 build with the existing HEVC/AV1 codecs.
   Preserve warnings-as-errors; repair actual SDK failures narrowly.
2. Exercise real Apple ISO and XDRemux fixtures through first-class `tmap`
   inspection and canonical reconstruction. Identify legacy by its container
   graph, since ImageIO can synthesize ISO auxiliary metadata for legacy files.
3. Add reproducible synthetic writer fixtures and an Apple ImageIO/Core Image
   consumer check. Compare decoded HDR pixels against a separately calculated
   expectation, and check baseline fallback, auxiliary metadata and headroom.
4. Test final Annex C bytes with an independently built, pinned libultrahdr.
   Keep its PR #1503 API dependency distinct from container interoperability.
   Do not add the historical singleton API to the stable libheif headers.
5. Close evidence-backed reconstruction gaps and add numerical, malformed-input
   and nested-derived-image regressions. Run experimental OFF/ON, sanitizers,
   public-header/symbol gates and the latest-commit Actions checks.

The English ISO/IEC 23008-12:2025/Amd.1:2025 clause 6.6.2.4.1 uses **should**
for the `pixi` association and hidden gain input. The supplied Chinese
translation uses stronger wording for `pixi`; the English original governs.
The required colour associations and ordered two-input `dimg` remain **shall**.

## Boundaries

Original user images and ISO publications stay outside Git and Actions uploads.
No Photos library import or alteration is needed for the framework consumer
gate. Framework pixel checks prove the tested reconstruction, not iOS Photos
persistence or display presentation. Unsupported general ICC transforms and
other remaining limitations must remain explicit until a working colour
management strategy and independent output evidence exist.

## Local verification, 2026-10-02

Mac arm64, macOS 27.0 (26A428), Xcode 27, x265 4.0 / build 212,
libde265 1.0.15, AOM 3.13.1:

- Complete CTest: 99 targets, 93 passed, six codec/external-input skips, no failures.
- Experimental ON and OFF ASan/UBSan: seven and five focused targets passed.
  macOS does not support LeakSanitizer; Linux Actions retains leak detection.
- Public C headers: 108 compilations (gcc/clang). Stable symbols: all 469 found
  in both ON and OFF libraries. Changed C/C++ sources pass repository cpplint.
- Independently built libultrahdr: mono/RGB Annex C bytes match in both directions.
- Apple ImageIO reads both libheif-produced ISO gain maps. Maximum extended-linear
  sRGB HDR errors: 0.000465 mono, 0.004195 RGB with unequal gain samples. SDR
  fallback also matches the baseline. Core Image's synthetic producer is read
  by libheif and compared to ImageIO's pixels from the same compressed file.
- Two original Apple ISO tmap samples (`IMG_9301.HEIC`, `IMG_3265.HEIC`) pass
  metadata, both child decodes and canonical reconstruction. Originals are read
  through local symlinks only and are not part of the commit or CI artifacts.

The writer fixes are backed by controlled A/B files: changing only the colliding
`altr` group ID restored ImageIO ISO discovery. Default RGB gain identity with
4:2:0 caused a 0.322 green-channel error; choosing a matrix compatible with the
codec sampling resolved it. Explicit identity with subsampling is rejected.
The x265 build-212 output-picture fix also restores four existing HEVC encode
regressions that failed on this Mac before the correction.

## Conformance assessment and next bounded goal

Assessment baseline for this continuation: `c54744f5079b57eba041d58c4cada27a7b86a896`.
This includes subsequent upstream fixes and the corrected clause 4.4 requirement
of at least eight bits per gain component. Writer rejection of lower-depth input
and tolerant reader behaviour are distinct policies.

| Standard surface | Implementation and evidence | Assessment |
| --- | --- | --- |
| ISO 21496-1 Annex C syntax and semantics | Exact rational metadata, mono/RGB, semantic rejection and version handling; independent pinned libultrahdr byte comparison | Covered by current tests |
| ISO 21496-1 clauses 6.1-6.3 | Linearization, application primaries, inverse gamma, unnormalization, co-sited resampling and offset equation | Covered for supported colour subset |
| Clause 6.3 target-headroom weighting | New experimental root-only decode API; independent PQ pixel expectations in both directions, endpoint equality, invalid targets and nested PQ/HLG tests | Implemented in this continuation |
| HEIF Amd.1 clause 6.6.2.4 | Ordered two-input `dimg`, image geometry, required colour roles and ordinary derived-item traversal | Covered by structural and decode tests |
| HEIF writer and alternative selection | Gain NCLX, hidden-item policy, exact alternate ICC/NCLX preservation and noncolliding ordered `altr` groups | Tested with synthetic Apple consumers |
| All permitted colour descriptions and raster paths | Arbitrary ICC, other HLG viewing conditions, premultiplied baseline alpha, unsupported matrices and tile-only decode remain limited | Partial |
| External consumers | Apple public-framework pixels and producer round trip; libultrahdr metadata round trip | Tested cases only; no universal consumer claim |

There is no defensible “100% adapted” claim for the whole standards. HEIF
ISO/IEC 23008-12 contains much more than the `tmap` amendment, and passing the
current cases cannot establish all permitted ICC transforms or every producer.
The scope above identifies which operations are implemented and which remain
explicitly unsupported instead of assigning a misleading conformance percentage.

The bounded goal is to expose the already implemented Formula (3) through actual
root decoding while preserving canonical nested semantics and the stable API.
English ISO 21496-1 page 7 (PDF page 13), clause 6.3, was checked directly.
The existing scalar formula was correct; no change to its arithmetic is needed.
No new CMS, legacy-format heuristics or producer-specific matrix guesses are part
of this change. Hosted Linux sanitizer and macOS framework jobs exercise the
new tests through the existing workflow.

Local acceptance after this change: all 103 CTest targets completed with 97
passes and six capability/external-fixture skips. ASan/UBSan passed gain-map
math, all 15 tmap-read cases (301 assertions) and the newly merged uncompressed
sequence regression. That regression's diagnostic streamed a null success
message on macOS; its two log statements now guard the optional message.
All 108 C-header compilations, 469 stable symbols in both experimental modes
and changed-source cpplint pass. The new symbol is exported only in ON builds.
Apple's synthetic mono/RGB consumer and producer round trip pass again with
the existing pixel tolerances. Hosted CI status must be checked for the actual
published commit rather than inferred from these local results.
