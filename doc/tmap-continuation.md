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
This includes subsequent upstream fixes and the writer's eight-bit minimum
policy. Direct inspection of English clause 4.4 (page 3, PDF page 9) confirms
that at least eight bits is recommended, not mandatory. Writer rejection of
lower-depth input is a supported-subset policy; lower-depth reader inputs are
not labelled non-conforming solely for that reason.

| Standard surface | Implementation and evidence | Assessment |
| --- | --- | --- |
| ISO 21496-1 Annex C syntax and semantics | Exact rational metadata, mono/RGB, semantic rejection and version handling; independent pinned libultrahdr byte comparison | Covered by current tests |
| ISO 21496-1 clauses 6.1-6.3 | Linearization, application primaries, inverse gamma, unnormalization, co-sited resampling and offset equation | Covered for supported colour subset |
| Clause 6.3 target-headroom weighting | New experimental root-only decode API; independent PQ pixel expectations in both directions, endpoint equality, invalid targets and nested PQ/HLG tests | Implemented in this continuation |
| HEIF Amd.1 clause 6.6.2.4 | Ordered two-input `dimg`, image geometry, required colour roles and ordinary derived-item traversal | Covered by structural and decode tests |
| HEIF writer and alternative selection | Gain NCLX, hidden-item policy, exact alternate ICC/NCLX preservation and noncolliding ordered `altr` groups | Tested with synthetic Apple consumers |
| Alpha and requested output colour | Encoded RGB unpremultiplication, straight-colour ISO reconstruction, restored alpha, and root EOTF/matrix/OETF output conversion | Implemented and tested, including a real `prem` container graph |
| ICC colour pipeline | Exact RGB matrix/TRC colourants, per-channel parametric/sampled curves and D50 PCS conversion; independent Little CMS oracle in ON/OFF CI | Implemented for this profile model |
| All permitted colour descriptions and raster paths | ICC LUT/device-link/non-RGB, other HLG viewing conditions and unsupported matrices remain limited | Partial |
| External consumers | Apple public-framework pixels and producer round trip; libultrahdr metadata round trip | Tested cases only; no universal consumer claim |

There is no defensible “100% adapted” claim for the whole standards. HEIF
ISO/IEC 23008-12 contains much more than the `tmap` amendment, and passing the
current cases cannot establish all permitted ICC transforms or every producer.
The scope above identifies which operations are implemented and which remain
explicitly unsupported instead of assigning a misleading conformance percentage.

The bounded goal is to expose the already implemented Formula (3) through actual
root decoding while preserving canonical nested semantics and the stable API.
English ISO 21496-1 page 7 (PDF page 13), clause 6.3, was checked directly.
The existing scalar formula was correct. A subsequent endpoint regression found
that subtracting separately rounded rational headrooms could erase a nonzero
interval. The algebraically equivalent implementation now uses exact integer
cross-product differences and a fused multiply-add for the target distance.
Forward and reverse intervals near `UINT32_MAX`, including finite extreme
targets, pass native and experimental ON/OFF ASan/UBSan checks.
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

The hosted macOS 15 framework run at `37e0fb07` returned SDR-range pixels from
the synthetic files even after making `contentHeadroom` diagnostic. The exact
downloaded files reconstruct correctly on macOS 27. Framework-version
interoperability therefore remains unproven until the hosted pixel gate passes.
The harness now explicitly allows floating-point decode and disables generated
image-specific display scaling while checking the full alternate. It logs
range, error, bit depth and colour space before asserting, and retains producer
fixtures on consumer pixel failures. The 0.025 HDR error gate is unchanged.

## Continued implementation while Actions run

Premultiplied baseline alpha and explicit output NCLX conversion now have
independent sRGB/PQ and coloured BT.709-to-BT.2020 expectations. Container tests
exercise an actual alpha `auxl`/`prem` graph, unknown-version fallback, and
requested linear output from nested PQ/HLG nodes with a root target weight.
The alpha graph exposed a monochrome-to-YCbCr conversion detour that changed
neutral RGB values; direct monochrome expansion removes that bias. Neither
change alters the original raster or adds an external colour-management library.

The macOS 15 run at `d543ed21` still failed the strict framework pixel gate with
SDR-range values (maximum about 0.394), whereas its C++ tmap read tests passed.
Its own Apple-produced fixture has baseline NCLX CP=2/TC=2/MC=6 and no ICC
description, so strict libheif reconstruction cannot infer its baseline EOTF.
That missing colour description is a separate observed limitation from the
ImageIO consumer result; neither is counted as a compatibility pass.

ICC matrix/TRC reconstruction now uses the actual profile model rather than
classifying its primaries and transfer as a nearby CICP code. The tests include
custom primaries, different channel curves, sampled curves with flat intervals,
both application-space directions, and a real dual ICC/NCLX container through
ordinary and unknown-version decoding. The independent Little CMS test found
that reusing a quantized `chad` matrix amplified inverse-curve errors; deriving
the Bradford adaptation in double precision resolves that regression without
loosening the 0.0001 comparison tolerance. The existing Apple ImageIO 0.025 pixel
gate also stays unchanged; a direct Core Image `expandToHDR` load now provides
additional hosted diagnostics.

The public tile API now decodes the single logical `tmap` tile it reports. Both
inputs are fully decoded and transformed, including unknown-version baseline
fallback. Tile regressions cover requested colour, ICC, premultiplied alpha,
nested PQ/HLG and transformed children; out-of-range tile coordinates retain
the generic API error. This adds no partial-region reconstruction algorithm.

CICP decoding now covers all defined H.273 primary code points, including
Illuminant C, DCI white and the XYZ basis. Relative colour conversion uses
Bradford adaptation between the declared whites. All primary pairs preserve
adapted white; the RGB cases and cross-white ICC conversions also have an
independent Little CMS oracle. BT.2020 SDR transfer codes 14/15 are supported
alongside their equivalent 1/6 codes, using H.273's continuous alpha/beta
constants rather than rounded thresholds. The remaining defined transfer codes
now have their actual gamma, SMPTE 240, logarithmic, signed extended-gamut and
ST 428 equations, with independent high-precision reference values. Logarithmic
zero signals decode as black because their lower interval cannot be inverted
uniquely; ST 428 retains its physical 48 cd/m2 white in the shared 203 cd/m2
normalization. Unsupported raster matrices and profile models remain explicit.

FCC and SMPTE 240 raster matrices now work for baseline and gain inputs, with
independent full/limited-range RGB and gain expectations. The matrix guard is
shared by both inputs; a baseline constant-luminance or unspecified matrix
cannot silently take the generic converter's non-constant/default path.

The writer now accepts lower-depth full-range monochrome and planar RGB gain
inputs, expanding normalized levels to the nearest value in a common depth of
at least eight bits. AV1 lossless tests serialize and reread 1-7-bit mono and
mixed-depth RGB data; source pixels and primary selection remain unchanged.
Low-depth limited-range/other-layout input stays explicit, and out-of-range
sample values fail before adding an encoded item. This closes the former blanket
minimum-depth writer policy without inventing lower-depth YCbCr normalization.
The byte-interleaved RGB layout also expands each component independently;
lossless AV1 checks cover its unequal component values and padded image rows.

YCgCo, its reversible -R/-Re/-Ro forms, and ST 2085 now use the actual H.273
inverse equations for both baseline and gain rasters. Reversible codewords are
normalized at their RGB depth rather than their larger coded depth; negative
odd differences use floor division, and B is clipped before deriving R as
required by equations 64-65. Independent codewords, full/limited range, 4:4:4,
4:2:2 and 4:2:0 nearest-neighbor sampling, mixed Y/C depths and alpha preservation
pass native and experimental ON/OFF sanitizer checks. The focused math target
has 3,716 assertions in 20 cases. No bitstream colour-description override or
producer-specific inference is introduced.

Constant-luminance matrices 10 and 13 now restore their transfer-dependent
R'/B' signals and solve green in linear light using H.273 equations 66-75.
Independent 70-digit Decimal references cover both chroma signs, full/limited
range, SDR, PQ, ST 428 and HLG, plus negative sYCC blue that affects green.
Matrix transfer signals are distinct from physical HDR display linearization;
in particular, HLG matrix inversion precedes the RGB display OOTF. Unspecified
transfer or chromaticity-derived storage primaries remain explicit errors.
Native math passes 4,094 assertions in 23 cases. Experimental ON sanitizer
checks pass math, colour, reader and writer; OFF passes math and colour (the
public experimental tmap targets are not built in OFF).

ICtCp (14) and IPT-C2 (15) now invert the H.273 chroma/LMS and linear LMS/RGB
matrices around the normalized transfer signal. PQ and HLG use their distinct
ICtCp coefficient sets. Independent 70-digit Gaussian-elimination references
cover coloured signals, mixed Y/C depths, range and chroma signs. Unequal RGB
gains 1/2/4 also verify that the ISO gain equation and HLG display OOTF follow
storage inversion. Native math passes 4,532 assertions in 26 cases; the same
ON/OFF sanitizer targets and local clang-tidy pass. This covers the defined
matrix equations within the documented integer raster and sampling subset,
not general floating-point/ICC-LUT or unspecified-signalling compatibility.
