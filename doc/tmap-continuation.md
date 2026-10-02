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
