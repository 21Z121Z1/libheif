# GB/T 48347—2026 interoperability & numeric verification

Reports GB/T HEIF (and JPEG) encapsulation + bidirectional HDR/SDR reconstruction
by capability level, with sample sources and independent oracles. ISO-shared
interoperability is reported in `eval/conformance-matrix.md` (V2–V5) and is not
re-credited here as GB/T-specific proof.

## Capability levels

| Level | Scope | Status |
| --- | --- | --- |
| L1 HEIF dual-layer read/write | base + hidden enhancement + tmap(2 ordered dimg) + it35/UWAInfo + colr/irot + mdcv/clli | **implemented & tested** |
| L2 HEIF HDR/SDR reconstruction | gain-map math + colour (ISO-shared) | **tested with independent numeric oracle** |
| L3 ExtendInfoExtension | BaseMapping/CombinedMapping parse/serialize | **parse tested; pixel application not implemented** |
| L4 JPEG MPF/APP2/UWA APP8 | dual-layer JPEG container | **not implemented** (payload layer only) |
| L5 dynamic metadata + display adaptation | GB/T 46269.1 | **unverified / out of codec scope** |

## Samples (all synthetic, generated here; no licensed device samples)

| Sample | How produced | Source authority |
| --- | --- | --- |
| GB/T dual-layer HEIF (it35) | `tests/gbt48347_heif.cc` builds base(RGB sRGB) + hidden mono gain + tmap + it35 via experimental API, uncompressed codec | GB/T §9.3 graph; Annex C.5 example topology (资料性) |
| UWAInfo golden vectors | hand-assembled big-endian bytes in `tests/gbt48347_metadata.cc` | GB/T tables 28/15/16/17 (PDF35/22) |
| ISO synthetic tmap HEIC | `tmap_write 'Generate synthetic*'` → `build-eval/synthetic/libheif-{mono,rgb,ramp}-pq.heic` | ISO 21496-1 / Amd1 |

No private device archives are used or redistributed. Fixtures are synthetic and
safe to keep in-tree (no standard text, no secrets).

## L1 — HEIF encapsulation verification

`gbt48347_heif` "dual-layer file carries it35 UWAInfo and a valid tmap graph":
- write → reopen → discover `it35` via `heif_context_get_list_of_item_IDs` +
  `heif_item_get_item_type` → `heif_item_get_item_data` → `parse_uwa_info`.
- checks: extended_type=0x02010100, static/dynamic metadata bytes preserved,
  extend_info_extension fp32 identity base matrix preserved.
- graph: tmap is a tone-map derived item; base/gain handles resolve to the original
  item IDs in order; gain item is hidden (GB/T §9.3.3a).
- Result: PASS (57 assertions across the 3 HEIF cases).

Box-level structure (pitm/iinf/iloc/ipco/ipma/iref/idat/grpl) is exercised through
the writer + reopen path; the ordered `dimg` and hidden-gain invariants are asserted.
`mdcv`/`clli` boxes are supported by the existing `Box_mdcv`/`Box_clli` (ISO-shared).

## L2 — bidirectional HDR/SDR numeric oracle

`gbt48347_heif` "HDR reconstruction matches an independent ISO numeric oracle":
- base sRGB codeword 127; mono gain normalized 1.0; metadata min=-1, max=1, gamma=1,
  zero offsets, base headroom 0, alternate headroom 2.
- **Independent oracle** (not the library's colour code): IEC 61966-2-1 sRGB EOTF
  `base_linear = ((127/255+0.055)/1.055)^2.4 = 0.212231`, then ISO 21496-1 Formula (2)
  with W=1, log2 gain=1 → `expected_hdr = 2 × base_linear = 0.424462`.
- HDR direction: decode tmap to requested linear RGB → `hdr = 0.424460`,
  `|hdr − expected_hdr| < 1e-4`. **PASS.**
- SDR direction (GB/T §11.2 base-as-SDR): decode base in native profile → returns
  codeword 127 unchanged. **PASS.**
- Tolerance source: 16-bit output quantization (1/65535 ≈ 1.5e-5) plus the sRGB
  EOTF rounding; 1e-4 absolute bound is ~6 quantization steps.

Note: an earlier attempt to read the SDR baseline by decoding the standalone base
with a *requested linear* output returned the raw normalized codeword (0.498) rather
than the sRGB-EOTF value; the tmap input path applies the EOTF correctly. The SDR
check therefore uses the native-profile decode. This is an existing ISO/base decode
observation, not changed by the GB/T increment.

## L3 — ExtendInfoExtension

`gbt48347_metadata`: fp32 identity matrix, fp16 IEEE binary16 encodings
(1.0=0x3C00, 0.5=0x3800, −2.0=0xC000), N=2 LUT derived from block length, invalid
fp flag and non-cube LUT rejected, empty/absent blocks, round-trips. **Parse/serialize
PASS.** Applying the matrix/LUT to pixels is **not implemented** (L3 gap).

## L4 — JPEG (not implemented)

The UWAInfo **payload** parser is shared with the future JPEG UWA APP8 path, but the
JPEG MPF / ISO APP2 / UWA APP8 container framing (tables 20/21/22/23/24) is **not
implemented**. No JPEG dual-layer read/write is claimed. Minimal follow-up: add an
MPF + APP2 + APP8 framing writer/reader reusing `parse_uwa_info`/`serialize_uwa_info`
and the ISO Annex C JPEG metadata, then add offset/APP-segment positive/negative tests.

## L5 — dynamic metadata / display adaptation (unverified)

Dynamic-metadata semantics and the GB/T 46269.1 display-adaptation pipeline are
**unverified** (dependency full text not in this audit; outside a file codec API).
Static metadata bytes are carried opaquely; no tone-mapping/display conformance is
claimed.

## Independent oracle summary (GB/T-relevant)

| Oracle | What it proves | Result |
| --- | --- | --- |
| Hand-assembled big-endian golden vectors | UWAInfo/extension byte layout vs GB/T tables | PASS |
| IEEE 754 binary16 fixed encodings | fp16 matrix correctness | PASS |
| IEC 61966-2-1 sRGB EOTF + ISO Formula (2) | HDR reconstruction numeric intent | PASS (1e-4) |
| ISO suite Google/Apple/LCMS (V2–V5) | shared reconstruction/colour/container | PASS (see conformance-matrix) |

Self-round-trips alone are not counted as independent proof; the numeric HDR oracle
and the hand-assembled vectors are independent of the parser under test.