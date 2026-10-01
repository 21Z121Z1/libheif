# ISO 21496-1 / HEIF tone-map support (experimental)

This branch implements final-2025 Annex C metadata parsing/writing, first-class
`tmap` inspection, construction from existing base/gain items, and canonical
reconstruction for a conservative NCLX subset. It is not a complete implementation
of every colour profile allowed by HEIF.

## Reconstruction

Ordinary `heif_decode_image()` on a supported `tmap` applies the complete gain
map. Child images use normal item recursion, transformations and the shared
decode budget. Unknown minimum metadata versions return baseline pixels and
retain the baseline colour description. Unknown outer ToneMapImage versions
remain unsupported.

Supported primaries are BT.709, P3-D65 and BT.2020. Supported transfer curves
are linear, sRGB, BT.709/BT.601 and PQ. PQ uses the BT.2408 reference-white
convention of 203 cd/m2. This is an explicit implementation convention, not an
additional ISO 21496 metadata field.

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

## Remaining limitations

- ICC-only base/alternate reconstruction requires a real CMS, including a defined
  HDR reference-white interpretation. ICC data remain available for inspection.
- HLG requires an explicit viewing/system-gamma configuration.
- Unsupported YCbCr matrices, premultiplied baseline alpha and tile-only `tmap`
  decode return explicit errors.
- No root-specific display-headroom API is exposed yet.
- Generic hidden-item and `altr` writer infrastructure is still separate work;
  the existing writer core does not silently enforce those compatibility policies.
- No Apple legacy or vendor missing-version heuristics are part of the strict parser.

## Verification

`gain_map_math` checks independently known scalar/colour values, channel
reconciliation, limited-range endpoints and resampling order/phase. `tmap_read`
checks canonical decode, nested reconstruction, baseline fallback and existing
container error isolation. Existing `tmap_write` checks writer round trips.

The `gain-map-conformance` workflow runs experimental OFF and ON builds with
ASan/UBSan, leak detection, public C-header and stable API-symbol checks.

External sample checking avoids redistributing the user's images:

```sh
LIBHEIF_TMAP_INTEROP_DIR=/path/to/samples build/tests/tmap_interop
```

The supplied 4.93 archives contain 24 Adaptive HDR and 24 ordinary PQ images.
All 24 Adaptive HDR metadata/graphs parse and both inputs decode. Their base and
alternate profiles are ICC-only: canonical rendering explicitly returns
`Unsupported_color_conversion`. All 24 ordinary PQ primary images decode.
These outcomes are inspection/input-codec evidence, not ICC rendering conformance.
