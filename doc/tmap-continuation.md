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
| All permitted colour descriptions and raster paths | ICC LUTs and gray baselines are supported as documented; other non-RGB models, HLG viewing conditions and raster limits remain | Partial |
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

The additional matrix paths now honor mandatory bilinear chroma upsampling at
uniform coded depths by reusing the existing sampling pipeline with alpha's
original depth. Independent 5/6-pixel border expectations exposed an existing
4:2:0 sampler indexing error: its border loop's chroma coordinates were divided
by two again. The local correction covers every border, with odd/even width
and height checks. Native math passes 4,886 assertions in 27 cases, and the
complete 103-target suite has 97 passes, six capability/external-fixture skips
and no failures. Focused experimental ON/OFF ASan/UBSan and clang-tidy pass.
Fresh Mac 27 Apple consumer/producer, original Apple read-only samples and
pinned Google metadata checks retain their earlier pixel and byte results.

Mandatory bilinear sampling now also supports mixed Y/C depths. The 4:2:0 and
4:2:2 samplers interpolate only Cb/Cr through their actual storage type and copy
Y/alpha using their own bytes per sample. Their output state retains each
plane's depth. Tests cover Y=8/10/16 with C=8/12/16 at odd/even borders, plus
different-depth reversible YCgCo codewords in both range modes and input roles.
Native math passes 10,094 assertions in 27 cases; the complete suite and
experimental ON/OFF sanitizer checks (including mixed-chroma conversion) pass.
This removes the earlier mixed-depth mandatory-bilinear limitation.

Planar RGB and monochrome reconstruction now reads unsigned 1-64-bit and
full-range float32/float64 samples directly. This retains tiny values until
large gains are applied, above-white HDR baselines for reverse gain, and
negative extended-transfer values until offsets/colour conversion. It also
avoids an intermediate RGB allocation. Per-component typed normalization
supports mixed RGB formats and wide limited-range integers; floating
limited-range and signed/complex formats remain explicit errors. Original
opacity is used before final RGB16/alpha quantization, with finite/range checks.
Independent native math passes 10,433 assertions in 34 cases. Public uncompressed
writer tests serialize, reopen and decode typed baseline and float gain planes,
checking persisted component types and final pixels (212 additional assertions).
The complete suite remains 97 passes/six skips with no failures. Experimental
ON/OFF ASan/UBSan pass on Mac; its sanitizer runtime does not support leak
checking, which remains enabled in the Linux Actions jobs. Fresh Mac 27 Apple
consumer/producer and pinned Google Annex C checks pass without wider tolerances.
The preceding mixed-depth commit's four Linux gain-map jobs also passed;
hosted Mac 15 acceptance remains unresolved.

Integer YCbCr storage inversion now produces double-precision RGB signals at
its original dimensions. Defined linear matrices join the existing nonlinear
paths, removing the generic converter's 16-bit intermediate from the ISO gain
operation. H.273's YCgCo code-domain clips remain in their specified order;
TC 11/12 and non-identity TC 13 retain extended signals, while other transfers
retain their nominal signal range. TC 13's original non-identity description
also selects the signed sYCC RGB EOTF/OETF. Independent tests expose both the
former ST2085 amplification precision loss and negative values lost before
ISO offsets. A serialized uncompressed sYCC tmap verifies the public path.
Native math passes 10,859 assertions in 37 cases, writer 8,065 in 13 cases,
and the complete suite again has 97 passes/six skips with no failures.
Experimental ON/OFF ASan/UBSan, local tidy/lint, unchanged stable symbols and
fresh Mac 27 Apple/Google checks pass. The preceding typed-raster commit's
four Linux gain-map jobs passed; its hosted Apple job remained queued.

## Additional convergence work (2026-10-03)

Cb and Cr now use their own coded depths and storage widths during direct
centred bilinear sampling. Interleaved RGB/RGBA reads its actual integer/float
type, stride, embedded opacity and declared 16-bit byte order. Known-value
tests cover both input roles and preserve precision before the ISO equation.

RGB ICC v2/v4 LUT transforms now use optional Little CMS in production, with
independent oracle tests and a runtime-CMM ON/OFF Actions matrix. Missing optional
colourants permit PCS conversion when the other item defines the application
primaries; they never invent an application RGB space. Missing transform
directions remain errors. Each item's `ndwt` now controls its absolute transfer's
reference-white normalization, including unknown-version baseline fallback.

Pinned stock libultrahdr now verifies actual reconstructed HDR pixels in addition
to Annex C bytes. Mono/RGB, BT.709/BT.2020/Display P3, both application spaces
and three headrooms pass 16,416 assertions. The upstream RGBA input path avoids
its incomplete RGB888 classification; its historical private HEIF API still
needs a separate adapter for drop-in integration.

Canonical RGB uses its actual identity matrix/full range, independently of
the alternate item's storage description. Reconstructed nodes retain float32
samples through nested gains and requested root colour conversion, fixing
HDR clipping before a requested PQ encoding. ICC gamma/parametric equations
also retain extended positive values. The experimental float32 decode entry
point exposes relative HDR above one without changing the existing integer API.
Tests cover independent HDR expectations, ICC/NCLX, premultiplied opacity,
version fallback and the component datatype contract.

The complete Mac suite has 97 passes and six capability/external-fixture skips.
Targeted experimental ON/OFF ASan/UBSan, C headers, 469 stable API symbols,
lint and LLVM 22 tidy pass. Fresh Mac 27 ImageIO/SDR consumers and the Apple
producer round trip retain their strict pixel gates. Read-only comparisons
with two original Apple ISO files exposed the relative-HDR clipping above;
remaining whole-framework/codec pixel differences are still under investigation.
Originals and their derived pixel probes stay local and are not redistributed.
Hosted Mac 15 acceptance is still required; no complete-standards claim is made.

ICC-described YCbCr now retains RGB excursions until its actual ICC transfer;
the required CP=2/TC=2 storage NCLX no longer clips them early. A known-value
test covers full and limited range, gamma, offsets and relative HDR above one.
The experimental floating decode accepts an explicit co-sited/centered phase,
including nested nodes; ordinary decode retains ISO's preferred co-sited phase.
Tests use nonuniform gains, inverse gamma before interpolation and both down-
and upsampling lattice geometry. C-header checks now also enable the experimental
declarations.

Using the same Apple-decoded baseline and ISO auxiliary raster isolates the
reconstruction equation: centered phase yields maximum linear-sRGB differences
of 0.000227 and 0.006473 on 589 sample positions in each of two originals.
The public libheif decoder with that phase yields maxima of 0.016445 and
0.130829, respectively; these complete-decoder differences are not presented as
pixel identity or as a passed 100% acceptance gate. The originals and full
auxiliary buffers remain private local evidence.

Hosted runs `37091358325` and `37104332250` passed all six Linux jobs. The
second run isolated the Mac 15 ImageIO failure: the luma-scaling option must be
inside `kCGImageSourceDecodeRequestOptions`. With that request, the same hosted
files decode above reference white with errors of 0.000465 (mono) and 0.015871
(RGB), below the unchanged 0.025 gate. The corrected script removes the temporary
request A/B. A fresh local Mac 27 consumer/producer round trip passes 24,635
assertions; the corrected hosted producer round trip still needs a completed run.

The reconstruction path now observes declared 4:2:0 chroma locations 0-5
according to H.273 Table 8. A known two-dimensional chroma ramp failed ten
input-role cases before this correction; it now passes for baseline and gain
inputs. Serialization through an uncompressed baseline/tmap graph passes 132
assertions. Location 6 remains explicit unsupported, requiring a resolved
component-specific phase instead of guessing one. YCbCr opacity also uses its
own integer/float datatype independently of the colour-depth restriction;
eight added typed-alpha cases failed before the correction and now pass,
including premultiplication, offsets, zero opacity and invalid floating values.

Stock libultrahdr's bundled HEIF workaround now provides a separate container
oracle, without changing its source or this branch's API. Google-produced
mono/RGB HEIC and AVIF files and libheif-produced mono/RGB HEIC files pass
884,844 assertions against Google's half-float decode and their independently
constructed HDR intentions. The maximum linear difference is 0.001586; the
existing LUT/half-precision tolerance is unchanged. The Mac Actions job includes
the same six synthetic-file comparisons and archives only synthetic evidence.

Requested linear RGB now retains negative out-of-gamut floating components.
The former transfer-code-8 identity clipped them to zero after the primaries
conversion. Independent BT.2020-to-BT.709 values reproduce that defect through
the public floating API, while ordinary integer output retains its final range
clipping. The scalar identity and public-container regressions fail before the
two-return correction and pass afterwards.

For the second original Apple file, the public centered-phase linear comparison
now has a maximum difference of 0.024271 instead of 0.130829; the first remains
0.016445. These are 589-position comparisons, not full-image equality. Using
the existing FFmpeg HEVC plugin independently gives 0.021310 on the second
file. Native math/colour/reader/writer and experimental ON/OFF ASan/UBSan pass.
Fresh Apple consumer/producer checks pass 24,635 assertions with unchanged
pixel tolerances, and stock Google container checks pass 884,844 assertions.

The old installed libde265 1.0.15 also changes the second original's Cr plane
when its baseline is decoded first. An isolated libde265 1.1.3 build
(`ba62bf4cfb3242f3bf0a45617ff09e35236e4d82`) has identical gain-plane checksums
in both decode orders, matching independent FFmpeg, and gives the same 0.021310
sampled maximum difference. This is a runtime dependency comparison; no installed
codec or libheif decoder implementation was replaced. Hosted Mac checks use
the standard `macos-15-intel` runner alongside the local arm64 evidence and log
the actual OS, compiler and codec versions.

Gray ICC baselines now use their actual `kTRC` and XYZ or Lab PCS, with the
alternate RGB description supplying the ISO application primaries. The shaper
uses ICC.1:2022 Annex F.2 without a new dependency; optional Little CMS handles
gray input LUTs with normal tag precedence. Gray descriptions cannot supply
RGB application primaries or describe the canonical RGB output, and unequal
RGB input is rejected instead of silently reading one channel.

The gray regressions have 372 assertions covering independent PCS values,
serialized profiles against Little CMS, input-only LUT precedence, unequal RGB
rejection and public integer/float reconstruction with unknown-version baseline
fallback. The first gray cases fail before the change. Native math/colour/reader/
writer and experimental ON/OFF ASan/UBSan pass, including runtime CMM ON/OFF.
Fresh local Apple consumer/producer checks again pass 24,635 assertions, and
stock Google container checks pass 884,844 assertions with unchanged tolerances.
At preceding head `f77db169`, all six Linux Actions jobs passed while the Intel
Mac job remained queued. Hosted acceptance is still pending actual completion.
