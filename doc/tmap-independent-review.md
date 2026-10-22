# Independent ISO gain-map implementation review

## 2026-10-23 colour-association correction

The integration PR at `88b50610` omitted the colour-association fix already
present in local review commit `355de68c`. Replaying its seven focused reader
cases produced six failures. A grid with an explicit gamma-2 ICC inherited its
tile's sRGB NCLX and reconstructed RGB16 as 27817 instead of approximately
32511. Reading identical profiles at separate property indices also appended
associations, and conflicting ICC/NCLX pairs were accepted.

The integration fix loads descriptions without registering properties, retains
explicit grid colour, and validates every original colour association. The
existing unknown-minimum-version baseline fallback remains unchanged. Reader
and writer regressions cover the pixel result, ownership, malformed pairs and
preflight failure without item mutation.

The pinned Google HEIF workaround also emits a conflicting baseline ICC/NCLX
pair. Its four original HEIC/AVIF files are now negative tests. Separately named
single-NCLX projections remove one non-essential ICC association byte, retain
all coded payloads and gain metadata, and must produce exactly the same Google
HDR reference pixels. Positive container comparisons use these projections;
the raw Google metadata/math oracle is unchanged. Earlier container results
below are historical and do not prove that the original producer files comply
with HEIF 6.5.5.

## Earlier independent comparisons

The following revisions were read directly from their official repositories on
2026-10-04. The English ISO 2025 publications decide differences between
implementations. This review supplies cross-checks, not a general conformance
claim for any of the projects.

| Implementation / revision | Problem and relevant design | libheif comparison and decision |
| --- | --- | --- |
| [libavif e4db889c](https://github.com/AOMediaCodec/libavif/tree/e4db889c6b574bf221a380e9bddde0273c942023) | Rational metadata, original alternate colour/precision hints, ordered tmap inputs, compatible brand, typed altr, gain grids and invalid-file tests. Writer disallows transforms on supplied gain pixels and copies baseline transforms onto the encoded gain item. | Supplied libheif items may already have transforms; compare their effective ordered irot/imir composition instead. Preserve optional caller precision hints; do not infer reconstructed components from the mono gain raster or decoded storage. H08's before-fix test demonstrates rejection of inconsistent hints. |
| [CrabbyAvif 7d1cb2d1](https://github.com/webmproject/CrabbyAvif/tree/7d1cb2d1ccc6f5dfd369e51232ec2e4a41fe5892) | Independent Rust Annex C and ToneMapImage parser/writer, typed fractions, ordered inputs, tmap branding and alternate PIXI interpretation. | Byte differential uses the actual Rust parser/writer without modifying their production code. Its supported layouts and error API are implementation choices; libheif retains generic derived inputs and distinct unknown outer/minimum-version results. |
| [libultrahdr 66821e0a](https://github.com/google/libultrahdr/tree/66821e0a261aa3a06c0e7c889f52eced52850be1) | Independent rational/metadata/math and JPEG gain-map implementation; HEIF/AVIF workaround delegates to libheif. | Keep this revision as the blocking raw oracle. Its six HEIF/AVIF exchanges establish compatibility only. Do not import its resampling policy, JPEG-specific layout or vendor heuristics into the HEIF reader. |
| [Skia 1b95d35b](https://github.com/google/skia/blob/1b95d35ba26d4bd3761ddb1120ce112359a00df7/src/codec/SkGainmapInfo.cpp) | Big-endian ISO parsing/serialization, malformed-rational tests and float rendering parameters. Serialization approximates rationals from floats; colour discovery belongs to the image codec consumer. | Preserve exact public POD rationals and explicit application primaries. An intermediate sRGB placeholder in a rendering model does not authorize an ISO colour fallback. Existing denominator/gamma and round-trip regressions cover the useful parser boundaries. Source review only; no Skia runtime oracle is claimed. |
| [libjxl 5f92f524](https://github.com/libjxl/libjxl/blob/5f92f524f1407de323c409c360ab38cae1d97b27/lib/include/jxl/gain_map.h) | Bounded bundle API transports ISO metadata as opaque bytes separately from alternate colour and encoded gain pixels; unpacked pointers borrow the original buffer. | This is a transport/ownership oracle, not an ISO parser. libheif already exposes generic item payloads and an experimental POD; retain those boundaries rather than adding a second opaque metadata API or exposing internal GainMapMetadata. Source review only. |

## Executed byte differential

Native libavif, codec-free Rust CrabbyAvif and the review branch were given the
same 141 ToneMapImage payloads. Test-only harnesses call each project's actual
parser/writer; production parser/writer source is unchanged in the two oracles.
The corpus varies mono/RGB metadata, application-space selection, reversed
headrooms and signed rational parameters.

| Cases | Result | English 2025 decision |
| --- | --- | --- |
| 64 valid payloads | All three parse and reserialize to identical canonical bytes | Annex C field order, signedness, flags and rational representation agree |
| 62 truncated prefixes, seven zero denominators, zero gamma, maximum below minimum | All three reject | Retain bounded reads and exact validation |
| Unknown minimum or outer version | All three report unsupported; libheif distinguishes the two | Baseline fallback applies to unknown minimum metadata version only |
| Reserved flag bits | All three accept and normalize the known fields | Reserved flags do not redefine current fields |
| Trailing bytes with writer_version=1 | All three accept | Future fields are permitted |
| Trailing padding with writer_version=0 | libheif accepts; libavif and CrabbyAvif reject | C.2.1 permits padding after known fields regardless of the current writer version; retain libheif's existing regression |
| Equal baseline and alternate headroom | libheif rejects; libavif and CrabbyAvif accept | 5.2.7 requires distinct headrooms; retain libheif's exact-rational regression |

The version/padding/headroom differences do not justify weakening the parser.
Skia's separate malformed-input and round-trip tests support the same bounded
parsing concerns, but were reviewed as source rather than executed here.

## Executed independent AVIF container and pixel cross-check

libavif generated lossless mono/RGB gain files with a 16x12 baseline, 8x6 gain,
linear alternate RGB, ordered dimg, tmap brand, altr and 12/12/12 alternate PIXI.
Its independent decoder and `avifImageApplyGainMap` produced linear RGB16 pixels.
libheif's float reconstruction of the two unrotated files agreed at maximum
linear errors 0.0000140998 (mono) and 0.0000379400 (RGB), below the fixed
3/65535 quantization/conversion allowance. CrabbyAvif independently parsed all
four generated containers, including their metadata and alternate precision.
No Rust codec or Rust pixel-math oracle is claimed.

Two additional files rotate both rectangular inputs by 90 degrees. Their box
dumps associate irot with the baseline and gain, while tmap shares the baseline's
untransformed 16x12 ispe and has no own transform. libavif and CrabbyAvif accept
that geometry; libheif rejects it as an invalid image size. Under HEIF 6.3 and
6.5.3 the derived reconstruction consumes the input output images, so its
reconstructed extent is 12x16. The correct comparison is against transformed
input geometry, not coded baseline dimensions. A libheif regression preserves
both valid transformed output and rejection of the incorrect declaration.

This is a container finding with a structural explanation, not grounds for a
producer-specific relaxation. The separate public Apple producer/consumer and
pinned Google gates remain required. The subsequently supplied user archives
now have their own [per-file validation](tmap-sample-validation.md); these
synthetic files do not substitute for that evidence.

## 2026-10-11 continuation

Current libavif source at
[`fa78ed71`](https://github.com/AOMediaCodec/libavif/tree/fa78ed71de0bcd35cc314393ea491e57c0eba66f)
was checked again for ToneMapImage parsing, dimg ordering, output geometry,
colour-property association and compatible-brand writing. Its narrower
container/transform model is not copied into libheif's nested derived-image
support. The English final standards still decide padding, rational headroom
and transformed geometry. This is a fresh source comparison; the executed
libavif/CrabbyAvif differential above remains tied to its original pins.

The raw Google oracle was rebuilt and executed at the same `66821e0a` pin and
passed. Final parser, reconstruction, colour, graph and writer regressions also
passed locally with ASan/UBSan. The second semantic pass retained exact public
POD rationals, unsupported-colour errors, baseline fallback only for unknown
minimum metadata versions, shared graph limits and RAII raster ownership.
No private rendering fallback or resampling rule was imported.

That pass found a writer invariant outside the typed tmap helper: generic item
creation could emit a valid hidden or visible tmap without its required brand.
The regression failed for both cases. The context writer now scans actual item
types once to enforce both brand implications, preserving rejection of a tmap
brand without an item. Writing contexts read from files remains unsupported.
No other broad simplification has a demonstrated benefit sufficient to change
the existing normative or security boundaries.

## Evidence boundary

Local raw source pins, harnesses, corpus manifest, canonical hashes, complete
logs and container dumps are retained under the ignored
`build-mac/evidence/upstream-series/independent-review` evidence directory in the
integration checkout. Existing regression tests and the acceptance matrix remain
the maintained gates. No additional production dependency, public ABI, colour
fallback or private vendor support results from this review.
