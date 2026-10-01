# ISO 21496-1 / HEIF tone-map support (experimental)

This branch implements final-2025 Annex C metadata parsing/writing, first-class
`tmap` inspection, construction from existing base/gain items, and canonical
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
are linear, sRGB, BT.709/BT.601 and PQ. PQ uses the BT.2408 reference-white
convention of 203 cd/m2. This is an explicit implementation convention, not an
additional ISO 21496 metadata field.

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

## Remaining limitations

- General ICC reconstruction still requires a real CMS, including a defined HDR
  reference-white interpretation. Only the explicitly recognized ICC-to-CICP
  subset described above is reconstructed without a CMS.
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
The interoperability test walks the sample directory recursively, so the two
archives may be extracted into separate subdirectories under one parent. Each
Adaptive HDR file is required to expose exactly one `tmap`, parse its final
Annex C metadata, decode both ordered inputs, and complete canonical
reconstruction. Each ordinary PQ file is required to decode its primary image.
Because these fixtures are external and are not redistributed, they are an
opt-in interoperability gate rather than part of the repository test corpus.
