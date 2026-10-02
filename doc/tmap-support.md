# ISO 21496-1 / HEIF tone-map support (experimental)

This branch implements final-2025 Annex C metadata parsing/writing, first-class
`tmap` inspection, gain-raster encoding, construction from existing base/gain items, and canonical
reconstruction for a conservative colour subset. NCLX is handled directly, and
selected ICC profiles are mapped to equivalent CICP descriptions for the ISO
gain-map operation. It is not a complete implementation of every colour profile
allowed by HEIF.

## Reconstruction

Ordinary `heif_decode_image()` on a supported `tmap` applies the complete gain
map. Child images use normal item recursion, transformations and the shared
decode budget. Unknown minimum metadata versions return baseline pixels and
retain the baseline colour description. Unknown outer ToneMapImage versions
remain unsupported.

Supported primaries are BT.709, P3-D65 and BT.2020. Supported transfer curves
are linear, sRGB, BT.709/BT.601, PQ and HLG. PQ uses the BT.2408 reference-white
convention of 203 cd/m2. This is an explicit implementation convention, not an
additional ISO 21496 metadata field.

HLG uses the BT.2100 reference display (1000 cd/m2, zero black, system gamma
1.2), including its RGB-luminance-dependent OOTF, with the same 203 cd/m2
reference white. Other HLG viewing conditions are not configurable yet.

For ICC-described base or alternate items, reconstruction currently recognizes
an ICC `cicp` tag when it names the supported CICP subset, and conservative
RGB matrix/TRC profiles whose primaries and transfer curve can be matched to
that same subset. Arbitrary LUT/device-link ICC transforms are not approximated.
The original ICC property remains available on the decoded image.

Gain samples are clipped to the logical [0,1] range, inverse-gamma transformed,
then unnormalized to log2 gains before co-sited bilinear resampling with edge
extension. Mono pixels and mono metadata are independently replicated. The
headroom direction determines the sign of the full-application weight. Offsets
are applied in linear application primaries; gamut excursions are clipped at
output encoding. Results are quantized to planar 16-bit RGB in alternate colour
encoding. This decoded pixel depth is independent of the writer's caller-supplied
`pixi` hint. All raster allocations use existing pixel-image security accounting;
no full-resolution floating-point gain raster is allocated.

The default root `tmap` output preserves its alternate encoding instead of
silently tagging PQ pixels as sRGB. Explicit requested-output transfer/primaries
changes currently return unsupported. General HDR-to-SDR display tone mapping
is not part of this implementation.

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

ISO 21496-1 4.4 requires at least eight bits per gain component. The
standards-aware gain-map writer rejects lower-depth inputs. The reconstruction
path remains deliberately tolerant of lower-depth full-range samples when
reading a non-conforming file, while unsupported sample formats still return an
explicit error.

## Remaining limitations

- General ICC reconstruction still requires a real CMS, including a defined HDR
  reference-white interpretation. Only the explicitly recognized ICC-to-CICP
  subset described above is reconstructed without a CMS.
- HLG is limited to the reference viewing conditions described above.
- Unsupported YCbCr matrices, premultiplied baseline alpha and tile-only `tmap`
  decode return explicit errors.
- No root-specific display-headroom API is exposed yet.
- No Apple legacy or vendor missing-version heuristics are part of the strict parser.

## Verification

`gain_map_math` checks independently known scalar/colour values, channel
reconciliation, limited-range endpoints and resampling order/phase. `tmap_read`
checks canonical decode, nested reconstruction, baseline fallback and existing
container error isolation. Existing `tmap_write` checks writer round trips.

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
