# ISO conformance matrix — gain map + HEIF tmap

Evaluation HEAD: `ef2c7d850612740530699a15c9496fdefc63c230`
(`eval/gbt48347-iso-audit-20261114`, worktree `libheif-eval-gbt48347`).
Standards: ISO 21496-1:2025 (§4–6, normative Annex B/C; Annex A informative),
ISO/IEC 23008-12:2025 (§6.3/6.5/6.6) **merged with** Amd 1:2025 (§6.6.2.4,
§10.2.6, Annex A.6). The English 2025 publications govern `shall` vs `should`.

This matrix re-verifies the existing acceptance matrix
(`doc/tmap-conformance-matrix.md`) on the current HEAD with **fresh local
evidence** (labels V1–V5 below). `PASS` is evidence for the stated requirement
and tested configuration, not a whole-standard certification. GB/T 48347 is
audited separately in `eval/gbt48347-conformance-matrix.md`.

## Fresh evidence keys (this evaluation, all on ef2c7d85)

| Key | Command / oracle | Result |
| --- | --- | --- |
| V1 | `ctest -R 'gain_map_metadata\|gain_map_math\|gain_map_color\|tmap_read\|tmap_write\|entity_groups\|id_creator\|uncompressed_encode'` | 9/9 pass. Assertion counts: metadata 384/19 cases, math 27637/49, color 28529/31 (LCMS ON), tmap_read 3223/42, tmap_write 27002/28, entity_groups 192/9 |
| V2 | `gain_map_ultrahdr` linked against **pinned Google libultrahdr `66821e0a261aa3a06c0e7c889f52eced52850be1`** (independent raw metadata/rational/math) | 16432 assertions / 3 cases pass |
| V3 | `swift scripts/check-tmap-apple.swift build-eval/synthetic` — independent Apple ImageIO `DecodeToHDR` + Core Image consumer and Core Image producer | mono/rgb/ramp consumer PASS; partial headroom 1/2/4 PASS; Apple producer PASS (mono maxLinearError 0.0409, canonical decode checked separately) |
| V4 | `tmap_interop` with `LIBHEIF_TMAP_APPLE_FIXTURE` + `LIBHEIF_TMAP_INTEROP_DIR` | 24650 assertions, 2 passed / 1 skipped (stock-ultrahdr case needs V5 dir) |
| V5 | `scripts/check-tmap-ultrahdr.py` bidirectional Google HEIF/AVIF container exchange (stock ultrahdr_app, bundled HEIF workaround) | see `eval/test-results.md`; container exchange is compatibility evidence, not independent HEIF-graph proof |

Independent-oracle policy: V2 uses Google's own parser/math (never a copied
parser); V3 uses Apple frameworks; V1 color uses Little CMS. libheif
self-round-trips alone are **not** counted as independent proof.

## ISO 21496-1 — binary metadata (Annex C)

| Clause | Requirement (refined) | Code path | Test | Independent oracle | Result | Status |
| --- | --- | --- | --- | --- | --- | --- |
| C.2.1 | Big-endian fields; MSB-first sub-byte flags | `gain_map_metadata.cc` `GainMapByteReader::read_u16/u32`, flags `0x80/0x40` | V1 metadata mono/RGB golden vectors | V2 final Annex C bytes == Google | match | 符合 |
| C.2.1 | Stop after last recognized field; ignore padding/future optional fields | parser records `bytes_consumed`; trailing bytes ignored | V1 metadata future-writer/trailing-data cases | V2 known payload bytes | match | 符合 |
| C.2.2 | `channel_count` ∈ {1,3}; RGB order | flags derive 1/3; `validate_gain_map_metadata` rejects others | V1 metadata invalid-count rejection | V2 both counts | match | 符合 |
| C.2.3 | Unknown `minimum_version` → ignore gain metadata, display baseline | `parse_gain_map_metadata` early `unsupported_minimum_version`; `ImageItem_tmap::decode_compressed_image` baseline branch | V1 metadata unknown-min; V1 tmap_read baseline fallback | — | baseline decoded, gain not parsed | 符合 |
| C.2.3 / 5.2.8 | `minimum_version==0`; `writer_version>=minimum_version` | `validate_gain_map_metadata` | V1 metadata writer<min malformed | V2 version-0 bytes | match | 符合 |
| C.2.3 | Every rational denominator ≠ 0; gamma numerator ≠ 0 | `validate_gain_map_metadata` | V1 metadata every zero-denominator + zero-gamma-numerator rejection | — | rejected | 符合 |
| 5.2.5.3 / 5.2.5.6 | max(G) ≥ min(G); gamma > 0 | exact rational compare `signed_rational_less` | V1 metadata max<min rejection | V2 signed gains | match | 符合 |
| 5.2.7 | H_alternate ≠ H_baseline | `unsigned_rational_equal` exact compare | V1 metadata equal-headroom rejection; math endpoint rounding | V2 headroom | match | 符合 |
| C.2.3 | signed min/max/offsets; unsigned gamma/headroom | bounded i32/u32 rational types; INT32_MIN safe | V1 metadata INT32_MIN; math signed offsets | V2 negative offsets/gains both directions | match | 符合 |
| C.2.2 | reserved flag bits do not redefine known fields | parser masks known bits only | V1 metadata reserved-flag case | — | ignored | 符合 |
| C.3 | file-format identification / baseline / pixel / metadata / colour locations | HEIF `tmap` item + `dimg` + codec + `colr` | V1 tmap_read/write graph | V3/V5 Apple/Google containers | match | 符合 |
| C.4 | JPEG MPF/APP2 standalone embedding | outside HEIF container API | — | — | not claimed by this HEIF branch | 不适用 (standalone JPEG/MPF; see GB/T matrix CN06) |

## ISO 21496-1 — reconstruction & colour (§4–6, Annex B)

| Clause | Requirement | Code path | Test | Independent oracle | Result | Status |
| --- | --- | --- | --- | --- | --- | --- |
| 4.2 / 5.2.2 | gain dims may differ; declared; resample to baseline | codec/ispe dims; `reconstruct_tone_map` resample | V1 math resampling; tmap_write subsampled tiles | V2 unequal-size (constant-gain cross-check) | match | 符合 |
| 4.3 / 5.2.4 | raster mono or RGB | `prepare_rgb` / `RGBPlanes` | V1 math mono/RGB mismatch | V2/V3 mono+RGB | match | 符合 |
| 4.4 / 5.2.3 | ≥8 bits recommended; declare actual depth | gain encoder expands low depth; decode reads codec depth | V1 math below-8-bit full-range | V2/V3 8-bit gains | recommendation, not reader rejection | 符合 (should) |
| 4.5 | gain orientation matches baseline (display space) | writer compares effective irot/imir in ipma order | V1 tmap_write 64 orientation pairs + order-sensitive cases | — | writer invariant enforced | 符合 |
| 5.2.5.1 | mono metadata broadcasts independent of raster channels | per-channel metadata/raster mapping | V1 math mono-pixels/RGB-metadata + reverse | V2 all count combos | match | 符合 |
| 5.2.5.2–.6 / 6.2.1 | Formula (1) inverse-gamma unnormalize | `gain_map_unnormalize` before sampling | V1 math hand-calculated signed gains | V2 HDR pixel oracle | match | 符合 |
| 6.2.2 | resample to baseline size; after unnormalize; co-sited preferred | sample unnormalized log gain, edge extension, phase 0 (co-sited) / 0.5 (centered compat) | V1 math co-sited phase; tmap_read lattice phase | limited (Google IDW differs) | match scalar ref | 符合 (phase note) |
| 6.3 | Formula (2) offsets in linear RGB | `gain_map_apply`, colour decode→gain→encode | V1 math signed gains/offsets | V2 constructed HDR intentions | match | 符合 |
| 6.3 | Formula (3) clamp+sign, SDR→HDR and reverse | `gain_map_target_weight` (fma, exact rational direction) | V1 math both directions + clamps + near-equal endpoints | V2 1x/2x/4x headroom | match | 符合 |
| 5.3.2 | baseline encoding explicitly described | `resolve_tone_map_colour` ICC/CICP | V1 color CICP/matrix-TRC/LUT; tmap_read dual ICC | V3 explicit-ICC Apple producer | supported profiles; unresolved fails closed | 符合 |
| 5.3.3 | absent alternate colour inherits baseline | HEIF Amd1 requires alternate `colr`; absent HEIF colr invalid | V1 math same-space | — | generic fallback superseded by HEIF rule | 不适用 (HEIF) |
| 5.3.4 / B.2 | application primaries follow `use_base_colour_space` | `GainMapColour::matrix_to`, application selection | V1 color adapted white/reference red; custom primaries | V2 BT.709/BT.2020/P3 both spaces | match | 符合 |
| B.1 | linearize baseline / encode alternate with actual transfer | H.273 transfers, matrix/TRC, optional LCMS PCS | V1 color independent sRGB/PQ + LCMS both directions | V1 LCMS; V3 Apple pixels | match within documented limits | 符合 |
| B.2 | gamut mapping when alternate gamut smaller (should) | float output preserves out-of-gamut; integer clips | V1 math signed extended samples | — | no perceptual mapping algorithm specified | 局限 (should, 未实现感知映射) |
| H.273 code 18 | HLG normalization viewing-condition dependent | reference 1000-nit / gamma 1.2 OOTF | V1 math luminance OOTF | — | non-reference conditions not configurable | 局限 |
| Annex A | automatic gain-map computation (informative) | writer accepts supplied gain pixels/metadata | V1 tmap_write supplied-gain | Apple/Google synthesize own gains | not a reader requirement | 不适用 (资料性) |

## HEIF 2025 / Amd 1 — storage & derived-image semantics

| Clause | Requirement | Code path | Test | Independent oracle | Result | Status |
| --- | --- | --- | --- | --- | --- | --- |
| 6.6.2.4.1 | one ordered `dimg` entry, exactly {base,gain}, distinct, existing | `ImageItem_tmap::get_input_item_ids` | V1 tmap_read ordered/multiple/self/dup/missing rejection | V3/V5 Apple/Google files | match | 符合 |
| 6.6.2.4.1 | base associated `colr` | `validate_tone_map_inputs` / `require_colour_property` | V1 tmap_read required-colour; tmap_write invalid roles | V3 baseline descriptions | match | 符合 |
| 6.6.2.4.1 | gain NCLX CP=2/TC=2, actual matrix/range; limited clip to [0,1] | associated-property presence; storage inversion then normalize+clip | V1 math limited-range endpoints; tmap_read gain NCLX role | V3/V5 gain roles | match | 符合 |
| 6.6.2.4.1 | tmap `colr` = fully-applied alternate | `resolve_tone_map_colour(*this)` + tagged canonical RGB | V1 math output transform; tmap_read canonical RGB | V3 PQ alternate pixel gates | match | 符合 |
| 6.6.2.4.1 | nested tmap fully applies gain, retains alternate encoding | internal float reconstruction; root target/output only | V1 tmap_read nested relative HDR | — | match | 符合 |
| 6.6.2.4.1 | independent raster/metadata channel counts | broadcast raster or metadata independently | V1 math mismatch; tmap_write mono/RGB | V2 all combos | match | 符合 |
| 6.6.2.4.1 | clli on base/alternate (should) | optional CLLI; writer preserves base | V1 tmap_write graph colour/brand round trip | V3 Apple clli dump | optional recommendation | 符合 (should) |
| 6.6.2.4.1 / 6.5.6 | pixi hint (should); reconstructed components/depth | optional caller hint; preflight 0 or 3 components | V1 tmap_write mono-gain/RGB8/RGB16 + 12/12/12 PIXI; 1/2/4 rejected | libavif/CrabbyAvif retain original hints | match; no gray alternate | 符合 (should) |
| 6.6.2.4.1 | gain hidden (should); preserve valid primary | gain writer defaults hidden; visibility rules | V1 entity_groups hidden-primary; tmap_write hidden mono | V3 Apple hidden gain | match | 符合 (should) |
| 6.6.2.4.2–.3 | outer ToneMapImage version 0; unknown not processed | outer parser distinct from Annex C version | V1 metadata unknown outer version; tmap_read outer/inner distinction | — | match | 符合 |
| 10.2.6.1 | tmap item ⇒ `tmap` compatible brand | `context.cc:605` writer adds brand | V1 tmap_write round trip | V3/V5 ftyp | writer invariant | 符合 |
| 10.2.6.2 | tmap brand ⇒ ≥1 tmap item | `context.cc:607-609` write check | V1 tmap_write brand-without-item rejected | — | writer invariant | 符合 |
| 10.2.6.3 / 6.10 | altr reader support; ordering/visibility | ordered group reader + typed writer | V1 entity_groups ordered/visibility/overlap; tmap_write reopened {tmap,base} | V3 Apple altr | item alternatives | 符合 |
| 6.3 | transformed geometry & graph safety | shared `verify_decodable`; child transforms before root; ispe validation | V1 tmap_read malformed + two-node cycles; tmap_write transformed extent | libavif rotated wrong-extent rejected | match | 符合 |
| 6.5.5 | ICC + CP=2/TC=2 storage NCLX | ICC resolves colourimetry; NCLX storage | V1 color storage NCLX; tmap_read dual ICC | V3 explicit-ICC Apple producer | match | 符合 |
| Amd1 Annex A.6 | `it35` ITU-T T.35 metadata item type | **not implemented** (see GB/T matrix CN09) | — | — | ISO Amd1 defines the item type; GB/T uses it | 未实现 (GB/T 任务) |

## Security / publication gates (summary)

| Gate | Evidence | Status |
| --- | --- | --- |
| Truncated payloads / rational overflow / finite arithmetic | V1 metadata every truncated prefix + INT32_MIN; math NaN/Inf/exponent overflow | 符合 (exercised scope) |
| Malformed graph / memory budget | V1 tmap_read cycles; upstream compressed-tile backport regressions | 符合 (exercised scope) |
| exp ON/OFF × LCMS ON/OFF ASan/UBSan | local focused runs in `eval/test-results.md`; full matrix is CI | 部分 (local subset; CI not re-run, push unauthorized) |
| C headers / exported symbols | `scripts/check-c-headers.sh`, `check-api-symbols.sh` in `eval/test-results.md` | see test-results |
| Real-device archives (G08/G09) | not present in this environment | 未验证 (no licensed samples) |

## Unverified / out-of-scope (explicit)

- Standalone JPEG/MPF ISO 21496-1 C.4 container (this is a HEIF branch).
- Perceptual gamut mapping (B.2 `should`, no algorithm specified).
- Non-reference HLG viewing conditions.
- Real-device HDR archives; physical-display / Photos acceptance.
- Full CI matrix on a new SHA (push not authorized); latest published exact-SHA
  CI remains E7/E10 in `doc/tmap-conformance-matrix.md`.
- `it35` / T.35 item (Amd1 Annex A.6) — carried into the GB/T task.

No claim of "fully conforms to all standards" is made; every declared-scope
`shall` above has the cited evidence, and all gaps are listed.