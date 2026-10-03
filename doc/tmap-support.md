# ISO 21496-1 / HEIF tone-map support (experimental)

This branch implements final-2025 Annex C metadata parsing/writing, first-class
`tmap` inspection, gain-raster encoding, construction from existing base/gain items, and canonical
reconstruction with direct NCLX and RGB ICC colour descriptions. ICC matrix/TRC
profiles use their actual colourants and per-channel curves for the ISO gain-map
operation. RGB LUT profiles with explicit colourants use optional Little CMS.
It is not a complete implementation of every colour profile
allowed by HEIF.

## Reconstruction

Ordinary `heif_decode_image()` on a supported `tmap` applies the complete gain
map. Child images use normal item recursion, transformations and the shared
decode budget. Unknown minimum metadata versions return baseline pixels and
retain the baseline colour description. Unknown outer ToneMapImage versions
remain unsupported.

Experimental `heif_decode_tone_map_image()` applies the root gain map with the
target-headroom weight of ISO 21496-1 clause 6.3, Formula (3). The target is
finite, nonnegative **log2 headroom in stops**, matching the metadata: a 4:1
HDR-to-reference-white luminance ratio is two stops. The weight clamps between
the baseline and alternate endpoints in either direction. Only the root consumes
the target; nested `tmap` inputs reconstruct fully. Output retains the alternate
colour encoding, even at zero weight, and Formula (2)'s offsets still apply.
This API does not perform a general HDR display tone curve. Explicit requested
output NCLX conversion happens after the root reconstruction, as described below.

Supported CICP primaries are all the defined H.273 Table 2 code points:
1, 4-12 and 22. Native-white RGB-to-XYZ matrices use their declared
chromaticities; conversions between different whites use Bradford adaptation.
Code point 10 uses the XYZ identity basis and equal-energy centre white.
All defined H.273 transfer codes are supported (1 and 4-18): linear,
display gamma 2.2/2.8, sRGB, BT.709/BT.601/BT.2020 SDR, SMPTE 240,
logarithmic 100:1 and 100√10:1, IEC 61966-2-4, BT.1361 extended gamut,
SMPTE ST 428, PQ and HLG. Signed extended-gamut branches are evaluated
before the unsigned output raster clips to its representable range.
For a logarithmic zero signal, the non-invertible low interval is decoded as
black; levels below its cutoff cannot be recovered from that signal.
The four equivalent SDR code points (1, 6, 14, 15) use H.273's continuous
alpha/beta constants. PQ uses the BT.2408 reference-white
convention of 203 cd/m2. This is an explicit implementation convention, not an
additional ISO 21496 metadata field.

SMPTE ST 428's display-referred intensity uses the same physical 203 cd/m2
reference normalization: a unit encoded signal decodes to 52.37/203 and the
nominal 48 cd/m2 white decodes to 48/203, following H.273 Table 3.

HLG uses the BT.2100 reference display (1000 cd/m2, zero black, system gamma
1.2), including its RGB-luminance-dependent OOTF, with the same 203 cd/m2
reference white. Other HLG viewing conditions are not configurable yet.

For ICC-described base or alternate items, an ICC `cicp` tag uses the supported
CICP subset and HDR reference-white convention above. RGB input/display
matrix/TRC profiles use their stored D50 PCS colourants and each channel's
monotone `curveType` or `parametricCurveType` (types 0-4), without matching them
to a CICP approximation. Sampled curves use linear interpolation; inverse
plateaus follow ICC.1:2022 Annex F.1. Cross-profile conversions use relative
colourimetry through the D50 PCS, with a double-precision Bradford white adaptation
for the supported NCLX spaces.

When Little CMS 2.10 or newer is detected (`WITH_LCMS2=ON`, the default), RGB
input, display and output LUT profiles use their relative-colorimetric CMM
transforms through XYZ PCS. The actual `rXYZ`/`gXYZ`/`bXYZ` colourants define
the linear RGB application space. Both directions must be available, and LUT
tags take precedence over coexisting shaper tags. The CMM uses per-decode
contexts and uncached transforms. Without this dependency, these profiles
return unsupported; the matrix/TRC and CICP paths remain available.

When ICC and NCLX are both associated, HEIF 6.5.5's CP=2/TC=2 storage NCLX
does not replace the ICC colourimetry. Codec matrix/range handling remains on
the decoded raster. A matrix/TRC alternate retains its original ICC and uses
CP=2/TC=2 for the RGB raster instead of inventing a CICP encoding. Unknown-version
baseline fallback also honours ICC during requested output conversion.
LUT profiles without explicit RGB colourants, device-link and non-RGB transforms
are not approximated. A LUT tag does not silently fall back to the matrix/TRC
model. The original ICC property remains
available on the canonical decoded image.

Gain samples are clipped to the logical [0,1] range, inverse-gamma transformed,
then unnormalized to log2 gains before co-sited bilinear resampling with edge
extension. Mono pixels and mono metadata are independently replicated. The
headroom direction determines the sign of the full-application weight. Offsets
are applied in linear application primaries; gamut excursions are clipped at
output encoding. Results are quantized to planar 16-bit RGB in alternate colour
encoding. This decoded pixel depth is independent of the writer's caller-supplied
`pixi` hint. All raster allocations use existing pixel-image security accounting;
no upscaled full-resolution floating-point gain raster is allocated.
Planar RGB and monochrome rasters are read at their actual component types:
unsigned integers of 1-64 bits and full-range IEEE float32/float64. Integer
limited-range normalization uses each component's own depth and requires at
least eight bits. Floating baseline values above reference white and signed
extended-transfer samples are retained through linear gain application.
Floating gain values undergo the same logical normalization and inverse gamma
as integer gains. Opacity must be finite and within [0,1]; its original value
is used before final output quantization. No intermediate RGB raster is needed
for these layouts. Integer YCbCr inversions use double-precision RGB planes
at the original raster dimensions rather than quantizing before gain. H.273's
explicit YCgCo code-domain clipping remains in its required order; extended
TC 11/12 and non-identity TC 13 signals retain their sign and range until the
ISO linear operation and final output. Other transfers retain their nominal
signal range. The original non-identity TC 13 description selects the signed
sYCC EOTF/OETF rather than unsigned sRGB. Floating limited-range, signed/complex component formats,
and floating or greater-than-16-bit YCbCr/interleaved rasters remain explicit
unsupported cases.

The default root `tmap` output preserves its alternate encoding instead of
silently tagging PQ pixels as sRGB. Explicit requested-output NCLX uses the
supported EOTF, linear primaries matrix and requested OETF before the ordinary
chroma/range conversion. Unspecified primaries or transfer inherit the input.
This conversion also applies to an unknown-version baseline fallback, and never
changes a nested `tmap` operation. A changed encoding drops the original ICC
property, which would describe different samples. General HDR-to-SDR display
tone mapping is not part of this implementation; integer output clips excursions
outside its representable range.

For a premultiplied baseline, reconstruction divides the decoded RGB sample
values by normalized alpha before linearization, applies the gain and offsets
to straight colours, and premultiplies the alternate encoded output again.
Alpha values are retained (with depth expansion if needed), zero-alpha output
is black, and the source pixels are unchanged. Requested output conversion
likewise operates on straight colours before restoring premultiplication.
Monochrome baseline samples expand directly to neutral RGB, including when
alpha is present, without introducing a rounded YCbCr neutral-chroma bias.

## Writer

`heif_context_encode_gain_map_image()` copies a logical gain raster with the
existing pixel memory accounting, encodes it through the ordinary codec path,
and associates mandatory CP=2/TC=2 NCLX signalling. It defaults to a hidden
gain item and never selects a primary. RGB/mono input defaults to full range;
YCbCr input requires explicit matrix/range signalling. For RGB input, a codec
requiring subsampled YCbCr defaults to BT.601; otherwise it uses identity.
Explicit identity requires a codec configured for 4:4:4. The source image and its
colour properties are unchanged. Encoder compatibility switches cannot suppress
the required gain NCLX property.

`heif_tone_map_options_alloc()` returns version-2 options. The caller supplies
either alternate NCLX or exact `prof`/`rICC` bytes. Defaults hide the existing gain
and create an ordered `{tmap, base}` alternative group through the generic writer.
Primary selection is preserved, and attempting to hide a primary gain is an
error. Version-1 options retain their original behaviour and read no version-2
fields. ICC preservation is independent of whether reconstruction supports the
profile's colour transform.

For several tmap nodes sharing a base, disable automatic group creation while
constructing the nodes and create one ordered group with
`heif_context_add_alternative_entity_group()`. An item cannot belong to multiple
alternative groups. The generic writer and visibility setter reject mixed
hidden/visible alternative membership (HEIF 6.4.2). Other ordinary files do not
receive the `tmap` brand.

Entity-group IDs are reserved across item/track/group namespaces even without
the optional `unif` brand. This avoids an ImageIO discovery failure when an
`altr` group aliases an image item, including items added after the group.

The generic visibility and entity-group writer prerequisite is imported with
its history from upstream PR #1893, commit
`a30c8fbc0be21f2807260a8da5d3e779f8555363`. The integration adds visibility
consistency checks and C-boundary exception guards.

ISO 21496-1 4.4 recommends at least eight bits per gain component ("should",
not "shall"). Full-range monochrome and RGB (planar or byte interleaved) inputs below eight bits
are now expanded by their normalized values to a common unsigned depth of at
least eight bits before codec encoding. Each level rounds to the nearest
representable value; the caller's raster remains unchanged. Mixed planar
depths use the largest depth, with a minimum of eight. Low-depth YCbCr
or limited-range inputs are still explicitly rejected rather than
guessing their zero point or layout. Reconstruction supports lower-depth
full-range samples; bit depth alone does not make a file non-conforming.

## Remaining limitations

- ICC LUT profiles without explicit RGB colourants, device-link, non-RGB and
  non-monotone shaper transforms remain unsupported. ISO Annex B requires a
  resolved linear RGB application space. Supported RGB LUTs use the optional
  CMM described above; matrix/TRC and CICP profiles need no CMM dependency.
- HLG is limited to the reference viewing conditions described above.
- YCbCr raster conversion supports explicit linear matrices 0, 1, 4, 5, 6, 7
  and 9, plus matrix 12 when the raster's primaries are defined. YCgCo (8),
  YCgCo-R, -Re (16), -Ro (17), and ST 2085 (11) use their H.273 inverse
  equations, actual RGB depth and separate luma/chroma normalization.
  Constant-luminance matrices 10/13 use their declared transfer to restore
  R'/B', solve linear G and reapply the transfer; matrix 13 also needs defined
  storage primaries. This uses HLG before its display OOTF and the signed sYCC
  transfer where applicable. A gain raster with unspecified TC cannot resolve
  such a matrix from the alternate image's colour description. ICtCp (14)
  and IPT-C2 (15) invert the specified chroma-to-LMS and linear LMS-to-RGB
  matrices around the normalized transfer. The HLG ICtCp matrix differs from
  its PQ matrix; display linearization follows storage inversion. These
  additional paths use nearest-neighbor sampling by default and support
  mandatory bilinear upsampling with separate Y, Cb and Cr coded depths and
  storage widths. Chroma is sampled directly at its own depth with centred
  phase, edge extension and one rounding; Y/alpha codewords are unchanged.
  Reversible YCgCo retains its matching chroma-depth requirement. Unknown
  algorithms remain explicit errors. Both
  baseline and gain inputs reject unsupported or
  unresolved matrices. BT.2020 constant luminance is not substituted with
  non-constant luminance. Defined matrix equations are covered subject to
  these raster depth, range and sampling limits.
- The tile API exposes `tmap` as one logical tile covering the complete coded
  image. Decoding `(0,0)` reconstructs both whole inputs, including their own
  transformations. It does not expose the baseline or gain raster's internal
  tiles as independently reconstructed regions. Root clean-aperture offsets
  follow the ordinary tile API contract.
- No Apple legacy or vendor missing-version heuristics are part of the strict parser.

## Verification

`gain_map_math` checks independently known scalar/colour values, channel
reconciliation, limited-range endpoints and resampling order/phase. `tmap_read`
checks canonical decode, nested reconstruction, baseline fallback and existing
container error isolation. Existing `tmap_write` checks writer round trips.

`gain_map_color` also checks custom ICC colourants, unequal channel curves,
sampled-curve interpolation and plateau inverses, and actual reconstruction
with ICC alternates. Optional `LIBHEIF_TEST_LCMS=ON` compares serialized profiles
in both directions against independent Little CMS. Actions enables this oracle
for both experimental modes, with the runtime CMM independently enabled and
disabled. Serialized ICC v2 LUT16 and v4 LUT profiles also have independently
known gamma, gain and offset expectations, including LUT precedence.

The `gain-map-conformance` workflow runs experimental OFF and ON builds with
ASan/UBSan, leak detection, public C-header and stable API-symbol checks. It
installs AOM and includes writer and generic entity-group tests, so codec-backed
writer regressions cannot pass merely because the encoder is unavailable.

The ordinary fuzzer workflow also runs a focused Annex C/ToneMapImage fuzzer
with synthetic mono/RGB seeds. It checks canonical serializer round trips and
exercises inverse gamma, headroom weighting and finite-checked gain arithmetic.

External sample checking avoids redistributing the user's images:

```sh
LIBHEIF_TMAP_INTEROP_DIR=/path/to/samples build/tests/tmap_interop
```

The interoperability test walks HEIC/AVIF files recursively and classifies them
by their actual item graph. Every `tmap` must parse its final Annex C metadata,
decode both ordered inputs and complete canonical reconstruction. Files without
`tmap` must decode their primary image; at least one actual `tmap` is required.
Because these fixtures are external and are not redistributed, they are an
opt-in interoperability gate rather than part of the repository test corpus.

The workflow independently builds libultrahdr at
`66821e0a261aa3a06c0e7c889f52eced52850be1` and compares bidirectional mono/RGB
Annex C bytes, including signed offsets and both application colour spaces.
Its HEIF integration still targets the historical PR #1503 API: this proves
metadata interoperability, not drop-in compatibility with that private API.
The published `gainmap_hevc_16x16.heic` fixture declares a 64x64 tmap but crops
its base to 16x16; canonical decode correctly rejects this inconsistent geometry.

The macOS workflow writes synthetic HEVC gain maps, checks Apple ImageIO ISO
auxiliary recognition and HDR pixels against independent linear expectations,
then reads an Apple Core Image-produced ISO tmap through libheif. It does not
upload user camera originals or prove Photos-library persistence.
