# ISO gain-map and HEIF tmap acceptance matrix

This is the acceptance and task queue for integration PR #1. Take implementation
tasks from `OPEN` rows; record a newly demonstrated gap here before changing code.
The English 2025 publications govern `shall` versus `should`. `PASS` is evidence
for the stated requirement and tested configuration, not a complete-conformance
claim. `OPEN` means required work or evidence is missing. `N/A` includes its reason.

| Source | Edition and applicable clauses |
| --- | --- |
| ISO 21496-1 | 2025, clauses 4-6, normative Annexes B/C; Annex A is informative |
| ISO/IEC 23008-12 | 2025, image-item properties, derivation, visibility and alternative groups |
| ISO/IEC 23008-12/Amd.1 | 2025, 6.6.2.4 and 10.2.6; Figure J.5 is an example |
| H.273 / ICC | H.273:2024 and ICC.1:2022 where referenced by the colour descriptions |

| Test key | Source / executable |
| --- | --- |
| M | [gain_map_metadata](../tests/gain_map_metadata.cc) |
| N | [gain_map_math](../tests/gain_map_math.cc) |
| C | [gain_map_color](../tests/gain_map_color.cc) |
| R | [tmap_read](../tests/tmap_read.cc) |
| W | [tmap_write](../tests/tmap_write.cc) |
| E | [entity_groups](../tests/entity_groups.cc) |
| Q | [gain_map_ultrahdr](../tests/gain_map_ultrahdr.cc), pinned Google raw oracle |
| I | [tmap_interop](../tests/tmap_interop.cc), external container/pixel oracles |

Test names below identify existing Catch cases; abbreviated names retain their
distinctive words. A dash means no independent evidence is claimed for that cell.

| Evidence ID | Authoritative result |
| --- | --- |
| E1 | `7d0503b15701530aece9a30af7d0ba5fcab87e8a`: [six Linux conformance/oracle jobs passed](https://github.com/21Z121Z1/libheif/actions/runs/37130954715); Hosted Apple failed. |
| E2 | `0f33cdf1329ef8dbcacdeb8d2ddaca878487b86f`: local `tmap_read`/`tmap_write`, Apple producer/consumer and Google container gates passed; Apple producer max linear error 0.0188146 at unchanged 0.025 limit; Google 884,844 assertions, max 0.0015862. |
| E3 | `0f33cdf1`: [lint, licenses, C headers and API symbols passed](https://github.com/21Z121Z1/libheif/actions/runs/37139297821). All six Linux jobs in the [conformance run](https://github.com/21Z121Z1/libheif/actions/runs/37139297756) passed; Hosted Apple remains pending. |

## ISO 21496-1 binary metadata

| ID / normative requirement | Implementation | Unit test | Container test | Independent implementation | Status |
| --- | --- | --- | --- | --- | --- |
| C01 / C.2.1 big-endian fields and MSB-first flags | `gain_map_metadata.cc` readers/writers | M: mono v0 golden vector; RGB channel order | M: ToneMapImage version 0 wraps Annex C metadata; W: graph metadata colour and brand | Q: Final Annex C bytes interoperate with upstream libultrahdr (E1) | PASS |
| C02 / C.2.1 stop after known fields; accept future optional fields and padding | Parser records `bytes_consumed`, ignores trailing bytes | M: accepts future writer and trailing data; ToneMapImage accepts trailing data | ToneMapImage envelope tested by M; no independent file-level future-field oracle | Q verifies known payload bytes, not future-field handling | PASS |
| C03 / C.2.2 only 1 or 3 metadata channels, RGB order | Flag derives 1/3; serializer validates count | M: RGB channel order; serializer rejects invalid metadata | W: mono/RGB synthetic HEVC files | Q: both metadata counts and application spaces (E1) | PASS |
| C04 / C.2.3 unknown minimum version ignores gain metadata and displays baseline | `ImageItem_tmap::decode_compressed_image` baseline branch | M: unknown minimum version is not parsed | R: Unknown minimum metadata version decodes baseline without alternate colour tagging; Float32 baseline fallback | — | PASS |
| C05 / 5.2.8, C.2.3 known minimum=0; writer >= minimum | Metadata version validation | M: writer version below minimum is malformed; unknown minimum version is not parsed | R: reports unsupported outer and inner versions | Q: version-0 bytes (E1) | PASS |
| C06 / C.2.3 every rational denominator nonzero; gamma numerator nonzero | `validate_gain_map_metadata` | M: rejects every zero denominator; rejects zero gamma numerator | M: ToneMapImage wrapper shares parser | — | PASS |
| C07 / 5.2.5.3 max >= min; 5.2.5.6 gamma > 0 | Exact rational validation | M: rejects maximum below minimum; zero gamma | W: writer rejects invalid roles | — | PASS |
| C08 / 5.2.7 distinct baseline and alternate headroom | Exact rational comparison | M: rejects equal HDR headrooms; N: rational endpoints round identically | R: target headroom in both directions | Q: target headroom pixel oracle (E1) | PASS |
| C09 / C.2.3 signed min/max/offsets, unsigned gamma/headroom | Bounded signed/unsigned 32-bit rational types | M: INT32_MIN safely; N: signed gains and offsets | W: graph metadata round trip | Q: negative offsets and gains, both byte directions (E1) | PASS |
| C10 / C.2.2 reserved flags do not redefine known fields | Parser masks known bits | M: ignores reserved flag bits | M: ToneMapImage payload parser | — | PASS |
| C11 / C.3 file-format identification, metadata and pixel locations | HEIF `tmap` item, `dimg`, codec descriptions and `colr` | M: ToneMapImage wrapper | R/W: first-class item and serialized graph | I: Apple/Google HEIF containers (E1/E2) | PASS |
| C12 / C.4 JPEG MPF/APP2 standalone-file embedding | Outside HEIF container APIs; HEIF JPEG-coded items still use `tmap` | — | — | Google JPEG support is not claimed by this HEIF branch | N/A: standalone JPEG/MPF container |

## ISO 21496-1 reconstruction and colour

| ID / requirement | Implementation | Unit test | Container test | Independent implementation | Status |
| --- | --- | --- | --- | --- | --- |
| R01 / 4.2, 5.2.2 gain dimensions may differ; declare dimensions | Codec/ispe dimensions; resample to baseline | N: Resampling interpolates unnormalized log gain at co-sited phase | R: Float32 public decoding forwards lattice phase; W: subsampled uncompressed tiles | I: tested containers; unequal-size oracle coverage is not claimed | PASS |
| R02 / 4.3, 5.2.4 raster is mono or RGB | `prepare_rgb` / `RGBPlanes` | N: mono pixels with RGB metadata and the reverse | W: mono/RGB synthetic HEVC files | I/Q: mono and RGB (E1/E2) | PASS |
| R03 / 4.4 recommended >=8 bits; 5.2.3 declare actual depth | Gain encoder expands low-depth inputs; decode reads codec depth | N: full-range gain maps below recommended 8 bits | W: lower-depth monochrome and planar RGB; byte-interleaved normalization | Q/I: 8-bit gains (E1/E2) | PASS: recommendation, not a mandatory reader rejection |
| R04 / 4.5 gain orientation matches baseline in display space | Child transformations before resampling; root transformations afterwards | — | W: cropped and oriented HEVC baseline; R: child transforms on fallback | Spatially varying, jointly oriented gain/base pixel oracle missing | OPEN: demonstrate both input orientations with nonuniform gain |
| R05 / 5.2.5.1 mono metadata broadcasts independently of raster channels | Per-channel metadata/raster mapping | N: mono pixels with RGB metadata and the reverse | W: mono/RGB generated containers | Q: 1/3 metadata x mono/RGB rasters (E1) | PASS |
| R06 / 5.2.5.2-.6, 6.2.1 Formula (1) inverse gamma and unnormalization | `gain_map_unnormalize` before gain sampling | N: hand-calculated signed gains and offsets; Floating gain samples are unnormalized before interpolation | R: public floating phase test | Q: HDR pixel oracle (E1) | PASS |
| R07 / 6.2.2 match baseline size; recommended resample after unnormalization | Sample unnormalized log gains directly, edge extension | N: co-sited phase and explicit gain-map phases | R: public lattice phase; W: odd/tiled geometry | Google IDW resampling differs; equal-size Google oracle does not prove this row | PASS: independent scalar references; external resampling oracle remains limited |
| R08 / 6.3 Formula (2), offsets in linear RGB | `reconstruct_tone_map`, colour decode -> gain -> colour encode | N: signed gains/offsets; straight premultiplied colours | R: canonical/public float decode; nested relative HDR | Q: constructed HDR pixel intentions (E1) | PASS |
| R09 / 6.3 Formula (3), clamp and sign for SDR->HDR and reverse | `gain_map_target_weight`, root-only target request | N: HDR-to-SDR direction and clamps; nearly equal rational endpoints | R: independent ISO/PQ values in both directions; root-only target for nested tmap | Q: 1x/2x/4x display headroom (E1) | PASS |
| R10 / 5.3.2 baseline encoding is explicitly described | `resolve_tone_map_colour`, ICC/CICP | C: CICP, matrix/TRC, supported LUT transforms | R: dual ICC and storage NCLX; undefined baseline application colour rejected | I: E2 explicit-ICC Apple producer | PASS for supported declared profiles; unresolved metadata fails closed |
| R11 / 5.3.3 absent alternate colour inherits baseline | HEIF Amd.1 requires alternate `colr`, so absent HEIF `colr` is invalid; same-space alternate can be explicitly associated | N: same-space linear reconstruction | W: explicit alternate colour | — | N/A for missing HEIF alternate `colr`; mandatory HEIF rule supersedes this generic fallback |
| R12 / 5.3.4, B.2 selected application primaries follow use_base_colour_space | `GainMapColour::matrix_to`, application selection | N: adapted white/reference red; C: custom primaries, unequal TRCs | R: Gray baseline uses declared alternate RGB primaries | Q: BT.709/BT.2020/P3, both selected spaces (E1) | PASS for resolved linear RGB spaces |
| R13 / B.1 linearize baseline and encode alternate using actual transfer | H.273 transfers, matrix/TRC or optional LCMS PCS transforms | N: independent sRGB/PQ, remaining H.273 curves; C: independent LCMS in both directions | R: requested output, ICC, nested PQ/HLG | I: Apple/Core Image/Google HDR pixel comparison (E1/E2) | PASS within documented colour-transform limits |
| R14 / B.2 recommended gamut mapping when alternate gamut is smaller | Floating canonical output preserves out-of-gamut values; integer output clips | N: requested primary conversion; signed extended samples | R: Floating linear tmap output retains out-of-gamut values | — | OPEN: record and review the gamut-mapping recommendation before final conformance wording |
| R15 / 5.3.2, B.1 present but unsupported ICC must not be treated as absent | Strict ICC/CMM error paths, no guessed fallback | C: LUT is not guessed as CICP; malformed bounds; CMM OFF | R: preflight ICC errors / dual-profile behaviour | Apple named Rec.709 producer probe exposes a decreasing parametric branch join | OPEN: audit this concrete profile against ICC curve requirements; do not expand arbitrary ICC variants |
| R16 / H.273 HLG normalization depends on viewing conditions | Reference 1000-nit / gamma 1.2 OOTF | N: luminance-dependent OOTF; C: reference normalizations | R: nested HLG canonical operations | — | OPEN: final audit must justify or disclose the supported reference-viewing boundary |
| R17 / Annex A automatic gain-map computation | Writer accepts supplied gain pixels/metadata; no automatic two-intention generator | — | W: supplied gain-map writer | Apple/Google synthesize their own gains for oracle files | N/A: informative algorithm, not a reader requirement |

## HEIF 2025 / Amd.1 storage and derived-image semantics

| ID / requirement | Implementation | Unit test | Container test | Independent implementation | Status |
| --- | --- | --- | --- | --- | --- |
| H01 / 6.6.2.4.1 one ordered dimg entry with exactly {base,gain} | `ImageItem_tmap::get_input_item_ids` | — | R: ordered inputs; multiple dimg entries rejected; W: reopen graph ordering | I: Apple and Google files (E1/E2) | PASS |
| H02 / 6.6.2.4.1 baseline associated with colr | `validate_tone_map_inputs` | — | R: required colour checks; W: invalid roles | I: generated baseline descriptions (E2) | PASS |
| H03 / 6.6.2.4.1 gain NCLX CP=2/TC=2, actual matrix/range; limited samples clip logically | Associated-property presence, storage inversion then normalization | N: Mono limited-range gain endpoints are normalized then clipped | R: gain-map NCLX role; W: preserves encoder colour policy | I: Apple/Google gain roles (E1/E2) | PASS |
| H04 / 6.6.2.4.1 tmap colr describes fully applied alternate | `resolve_tone_map_colour(*this)` and tagged canonical RGB | N: output signal transform | R: canonical RGB independent of alternate storage range | I: PQ alternate pixel gates (E1/E2) | PASS |
| H05 / 6.6.2.4.1 nested tmap fully applies gain and retains alternate encoding | Internal float reconstruction; root target/output conversion only | N: relative HDR before reverse gain | R: nested relative HDR; root requested output leaves nested operations canonical | — | PASS |
| H06 / 6.6.2.4.1 independent raster/metadata channel counts | Broadcast raster or metadata independently | N: mono/RGB mismatch case | W: mono/RGB synthetic files | Q: all metadata/raster count combinations (E1) | PASS |
| H07 / 6.6.2.4.1 recommended clli on base/alternate | Optional CLLI property; writer preserves base | — | W: graph metadata colour and brand round trip | I: actual Apple clli dump | PASS: optional recommendation |
| H08 / 6.6.2.4.1 recommended pixi; 6.5.6 reconstructed components/depth | Caller-supplied reconstructed-resolution hint, independent of output storage | — | W: graph round trip; shared base does not invent PIXI | Apple file declares alternate 10/10/10 | OPEN: final audit of hint semantics and channel-count validation |
| H09 / 6.6.2.4.1 recommended hidden gain, preserve valid primary | Gain writer defaults hidden; public visibility rules | E: hidden-primary rejection/promotion | W: hidden mono without selecting primary; preflight visibility errors | I: Apple hidden gain graph (E2) | PASS |
| H10 / 6.6.2.4.2-.3 outer ToneMapImage version 0; unknown version not processed | Outer parser status distinct from Annex C version | M: unknown outer version not processed | R: outer/inner version distinction and unaffected base access | — | PASS |
| H11 / 10.2.6.1 tmap item implies tmap compatible brand | Writer adds compatible brand | — | W: round-trips graph metadata colour and brand | I: Apple/Google ftyp (E1/E2) | PASS: writer invariant |
| H12 / 10.2.6.2 tmap brand implies at least one tmap item | No explicit branded-file validation found in current reader | — | Missing dedicated negative file test | — | OPEN: audit and test the branded-file invariant |
| H13 / 10.2.6.3 altr reader support; 6.10 alternative ordering/visibility | Ordered group reader and typed writer; application chooses alternatives | E: ordered groups, visibility, invalid inputs, overlapping alternatives | W: reopened {tmap,baseline}, baseline primary preserved | I: Apple altr graph (E2) | PASS for item alternative groups |
| H14 / 6.3 transformed geometry and graph safety | Shared `verify_decodable`; child transformations before root | Shared graph cycle regressions | R: nested tmap and fallback child transforms; W: cropped/oriented HEVC | — | OPEN: dedicated tmap cycle/self/missing-input matrix and unknown-minimum fallback interaction |
| H15 / Figure J.5 two tiled inputs, transforms, colr/clli/pixi, altr | Existing grid/unci/codec paths feed a logical whole-image tmap | — | W: subsampled tiled component origins, cropped/oriented baseline; R: logical tile API | — | OPEN: verify the complete two-tiled-input example; per-region reconstructed tile API is not claimed |
| H16 / HEIF 6.5.5 ICC plus CP=2/TC=2 storage NCLX | ICC resolves colourimetry; NCLX handles storage | C: storage NCLX preserves relative RGB before transfer | R: dual ICC / storage NCLX and fallback | I: explicit-ICC Apple producer (E2) | PASS |

## Security, interoperability and publication gates

| ID / gate | Implementation / required action | Local evidence | Actions evidence | Independent oracle / artifact | Status |
| --- | --- | --- | --- | --- | --- |
| G01 / truncated payloads, rational overflow, finite arithmetic | Bounded reads, exact rational checks, allocation limits, finite output checks | M: every truncated prefix; INT32_MIN; N: NaN/Inf/exponent overflow | E1 ASan/UBSan matrix | Q byte/pixel oracle | PASS for exercised payload and arithmetic; G02 covers graph audit |
| G02 / final independent malformed graph / memory-limit audit | C.2, tmap/dimg, nested paths and security budgets | Dedicated complete checklist not yet executed | Existing CIFuzz is evidence, not a substitute for the checklist | Review every normative OPEN row | OPEN |
| G03 / exp ON/OFF x LCMS ON/OFF, Linux ASan/UBSan | Four blocking jobs; main LSan enabled with x265 OFF; separate x265 ASan/UBSan step has LSan OFF | Mac LSan unavailable; prior focused ASan/UBSan passed | All four E3 sanitizer configurations passed | CMM ON independent LCMS and OFF explicit unsupported results | PASS on 0f33cdf1 |
| G04 / compiler / fuzzer / lint / C headers / exported symbols | Existing repository workflows, no license ignore expansion | Changed-file cpplint; clang-tidy source diagnostics; diff --check | E3 lint/license/headers/symbols passed; ordinary workflows pending | License failure was only check-tmap-ultrahdr.py, fixed with full MIT grant | OPEN until all latest-head workflows terminal |
| G05 / Apple producer -> libheif consumer, Intel and ARM | Public Core Image generic linear RGB producer, explicit ICC; strict unresolved-colour negative regression | E2: 24,635 assertions; max linear error 0.0188146 < 0.025 | Current Hosted Mac job pending | Original 7d artifact has no ICC and CP/TC=2 in both colr/VUI; reader correctly rejects it | OPEN until Hosted result and generated colour dump |
| G06 / libheif producer -> Apple consumer | DecodeToHDR, ISO aux discovery, ImageIO and Core Image pixels | E2 mono/RGB passed, unchanged 0.025 pixel limit | Original E1 consumer passed; latest pending | Independently calculated baseline/gain expectations | OPEN until latest Hosted result |
| G07 / pinned Google HEIC/AVIF bidirectional, bytes and float pixels | Pin 66821e0a261aa3a06c0e7c889f52eced52850be1 and its bundled HEIF workaround | E2 six containers / 884,844 assertions passed | E1 raw oracle passed; latest raw oracle passed, Hosted containers pending | Historical PR #1503 ABI is not imported into stable API | OPEN until latest Hosted container gate |
| G08 / 1-ISO_22028-5_HDR_PQ_4-93.zip | Locate originals, classify every file, record graph/profile/metadata/dimensions/decode/hash/error | Exact archive not found in Downloads/XDRemux; path requested | No user-original upload authorized to public Actions | Machine-readable per-file report missing | OPEN |
| G09 / 2-ISO_21496-1_Adaptive_HDR_4-93.zip | Same per-file report; distinguish ISO tmap from legacy auxiliary maps | Exact archive not found in Downloads/XDRemux; path requested | No user-original upload authorized to public Actions | Existing IMG_3265/9301 probes do not prove entire archive coverage | OPEN |
| G10 / upstream-ready v1.24.x baseline and logical commits | Keep PR #1 integration branch; create upstream/iso21496-tmap-v1.24 after convergence | Fetched baseline 31d1d85ccbd33b9671df1fff30ddb0d6debb2fab; branch not created; 5-7 dependency-ordered commits required | Branch is v1.24.x; its CMake version still reads 1.23.5, so this is not a released v1.24 tag | Upstream #1893 is OPEN/unmerged and typed writers are absent from this baseline; reuse if merged, otherwise isolate its dependency | OPEN |
| G11 / experimental ABI review, typed entity-group reuse | POD remains experimental; do not freeze new HDR ABI; reuse upstream typed group writers when available | Existing experimental POD and typed group APIs | Public C-header/symbol gates E3 passed | #1121 maintainer POD/HDR-header direction; #1893 pending | OPEN: upstream-ready branch review |
| G12 / OPPO private ProXDR excluded | No vendor missing-version/colour heuristics in ISO reader | Strict parser and unresolved-colour errors | Same strict path in E1/E3 | Private vendor formats are not a success oracle | PASS: outside requested support |
| G13 / completion wording | No complete-standard-conformance claim while required OPEN rows lack proof | This matrix and tmap-support limitations | A green job does not replace clause-level evidence | Final independent audit after prior stages | OPEN |
