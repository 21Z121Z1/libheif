# GB/T 48347—2026 independent conformance matrix

Standard: 《高动态范围（HDR）静态图像 双层格式》 GB/T 48347—2026
(2026-09-28 发布, 2027-04-01 实施, 推荐性国标). Audited **separately** from the
ISO conclusions in `eval/conformance-matrix.md`. ISO-shared capability is reused
but never auto-credited as GB/T conformance.

Evaluation HEAD: `ef2c7d850612740530699a15c9496fdefc63c230` + this evaluation's
GB/T increment (`libheif/gbt48347.{h,cc}`, `tests/gbt48347_metadata.cc`,
`tests/gbt48347_heif.cc`).

## Authority and method

- Normative authority is the original PDF `GBT+48347-2026.pdf`. The reading-version
  Markdown was cross-checked against the PDF for every syntax table used.
- PDF page numbers below are **physical PDF pages** (1-based, 51 pages total) for
  the tables actually verified; other clauses cite the printed page from the 目次.
- Capability levels: `ISO共用` (reused ISO 21496-1/HEIF code), `国标专有`
  (GB/T-specific, implemented here), `缺失` (not implemented), `库职责外`
  (outside a file codec API), `未验证` (cannot be verified here).
- GB/T 46269.1—2025 is a normative dependency for dynamic metadata / display
  adaptation; its full text is not part of this audit, so those rows are 未验证.

## Verified PDF syntax findings (drive the implementation)

| Item | PDF page | Finding |
| --- | --- | --- |
| Table 27 `ExtendInfoPart1` (HEIF tmap idat) | 34 | `versiontmap u(8)` + `version u(32)` + `flag u(8)` + headrooms + channels = **byte-identical to ISO ToneMapImage + GainMapMetadata**. Handled by the ISO parser, not re-implemented. |
| Table 28 `UWAInfo` (it35 idat) | 35 | country u(8)=0x26, provider u(16)=0x0004, oriented u(16)=0x0007, frame u(8), extended_type u(32); optional size-prefixed static/dynamic metadata (meta_type==1) and extend_info_extension (enhance_type==1). No APP framing inside HEIF idat. |
| Table 13 `extend_info_Part1` | 21–22 | flag byte laid out reserved:u(6), flag:u(1), numberofcomponents_flag:u(1) (**LSB-first**) — differs from the ISO MSB-first flag used by tables 24/27. See differences doc. |
| Table 14 / 25 per-channel | 22 / 31 | min/max/offsets ir(64) signed, gamma ur(64) unsigned — matches ISO GainMapChannel. |
| Table 24 (JPEG ISO APP2) | 31 | `version u(32)` + `flag u(8)` + headrooms + channels, ISO-compatible. |
| dimg order, §9.3.4 d) | 34 | first To_item_id = base (§9.3.2 b), second = enhancement (§9.3.3 a). PDF prints "第一个" twice — a drafting typo; intent matches ISO Amd1 (base first, gain second). |
| Channel loop | 21/31/34 | printed `i < number_of_components-1`; with count∈{1,3} this is a drafting inconsistency. File embeddings follow ISO (1 or 3 channels). Recorded as 标准解释待确认. |
| it35 optionality, §10.2.2 | 36 | "如存在 it35" → it35 is **optional**; tmap is the primary DRE-AIG indicator. |

## Clause → code → test → status

| ID | Clause (PDF/printed页) | Requirement (refined) | Code path | Test | Level | Status |
| --- | --- | --- | --- | --- | --- | --- |
| CN01 | §6 (印10) | DRE-AIG dual-layer model; SDR-base→HDR and HDR-base→SDR directions | `ImageItem_tmap` + `reconstruct_tone_map` (ISO共用); GB/T graph in `gbt48347_heif.cc` | gbt48347_heif dual-layer + HDR/SDR oracle | ISO共用 | 符合 (SDR-base direction tested; HDR-base direction uses same ISO reverse-gain path, see CN11) |
| CN02 | §7.1–7.2 (印11–14) | PicInfo colour/chroma/bit-depth; static/dynamic metadata; enhancement pixels & headroom | `gain_map_metadata` headroom/channels; nclx/ICC colour; `Box_mdcv`/`Box_clli` | gain_map_metadata; gbt48347_heif | ISO共用 | 符合 for ISO-shared fields; dynamic metadata 未验证 (GB/T 46269.1) |
| CN03 | §7.2.2.9–.11 (印13–14) | BaseMapping/CombinedMapping 3×3 matrix + 3D LUT; co-sited resampling, bilinear default | `gbt48347.cc` `parse_mapping_block` (matrix fp16/fp32, LUT order from size); resampling ISO共用 | gbt48347_metadata fp32/fp16/LUT cases | 国标专有 | 部分符合: matrix+LUT parse/serialize tested; LUT **application** to pixels not implemented (缺失); N derived from size (标准解释待确认) |
| CN04 | §8.1–8.2.1 (印15–16, PDF21–22) | ExtendInfoPart1 byte order/fields/channel flags/fractions | ISO `parse_tone_map_image` (table 27 = ISO); abstract table-13 LSB layout NOT used | gain_map_metadata golden vectors; gbt48347_heif | ISO共用 | 符合 for the HEIF/JPEG embedding (ISO layout); table-13 abstract layout discrepancy recorded |
| CN05 | §8.2.2–8.2.4 (印16–18) | ExtendInfoExtension, static & dynamic metadata; truncation/length/T.35 bounds | `gbt48347.cc` `parse_uwa_info`/`parse_extend_info_extension` | gbt48347_metadata truncation/length/flag cases | 国标专有 | 符合 for framing/static bytes; dynamic-metadata **semantics** 未验证 (GB/T 46269.1) |
| CN06 | §9.1–9.2 (印20–25, PDF26–31) | JPEG dual-layer: MPF, ISO APP2, UWA APP8, two JPEG sub-formats | UWAInfo payload parser reused; **JPEG MPF/APP2/APP8 container not implemented** | gbt48347_metadata (payload only) | 缺失 | 未实现 (JPEG container); payload layer tested. Minimal plan: add MPF/APP8 framing writer+reader |
| CN07 | §9.3.1–9.3.3 (印26–27) | HEIF base/enhance items: pitm/iinf/iloc/ipco/ipma/colr/irot/hvcC; hidden enhance | `ImageItem_tmap`, `HeifFile` item/property API; `heif_item_set_item_hidden` | gbt48347_heif graph (hidden gain, ordered dimg) | ISO共用 | 符合 (tested with uncompressed codec; hvcC path covered by ISO tmap_write HEVC cases) |
| CN08 | §9.3.4 (印28, PDF34) | tmap + 2 ordered dimg + idat main metadata + colr/irot + optional altr | `ImageItem_tmap::get_input_item_ids`/`add_new_tone_map_item`; ISO payload | gbt48347_heif; tmap_read/write | ISO共用 | 符合 (ordered inputs, brand, colr; altr covered by entity_groups) |
| CN09 | §9.3.5–9.3.6 (印29, PDF35) | mdcv/clli static; it35 + UWAInfo; enhance extension & dynamic metadata | `Box_mdcv`/`Box_clli` (ISO共用); `gbt48347.cc` UWAInfo; `heif_context_add_item("it35")` | gbt48347_heif it35 discovery+parse; gbt48347_metadata | 国标专有 | 符合 for it35/UWAInfo + mdcv/clli presence; dynamic-metadata semantics 未验证 |
| CN10 | §10.1–10.2 (印30–34) | HDR from single/dual layer: colour model/EOTF/gamut/mapping/resample/gain math | `reconstruct_tone_map`, `gain_map_color`, `gain_map_math` (ISO共用) | gbt48347_heif independent sRGB-EOTF×Formula(2) oracle; gain_map_math; Google/Apple oracles | ISO共用 | 符合 for the gain-map reconstruction path (independent numeric oracle matches to 1e-4); BaseMapping/CombinedMapping pixel application 缺失 |
| CN11 | §11 (印35–36) | SDR from HDR or from dual-layer base | base-as-SDR (§11.2) tested; HDR→SDR reverse gain ISO共用 | gbt48347_heif SDR codeword preservation; gain_map_math reverse | ISO共用 | 符合 for §11.2 base-as-SDR; §11.1 HDR→SDR via GB/T 46269.1 display adaptation 未验证 |
| CN12 | Annex A/B/C (资料性, 印37–44) | generation flow, ICC advice, ISO封装 & HEIF example | referenced only; C.5 example matches implemented graph | gbt48347_heif mirrors C.5 topology | 参考 | 资料性 — not treated as normative; C.2/C.3 flag semantics confirm ISO MSB layout |

## Display adaptation boundary (explicit)

GB/T §10.1.3 / §10.2.6 / §11.1 require display adaptation **per GB/T 46269.1—2025**.
That standard's full text is not in this audit and its tone-mapping/display pipeline
is outside a file codec's API. **Full display adaptation is 未验证 / 库职责外.**
libheif provides the reconstructed HDR/SDR pixels and metadata; the display-adaptation
step is an application responsibility. No claim of end-to-end display conformance.

## What was implemented this evaluation (国标专有 increment)

- `libheif/gbt48347.{h,cc}`: UWAInfo (table 28) + extend_info_extension (tables
  15/16/17) parse/serialize, big-endian, bounded reads, fp16/fp32, LUT order derived
  from the size prefix (exact-cube check). No stable C ABI added; uses the existing
  experimental `heif_context_add_item` / item-inspection API.
- `tests/gbt48347_metadata.cc`: 15 cases / 90 assertions — hand-assembled golden
  vectors, wrong T.35 identifiers, every truncated header prefix, metadata_size
  bounds, invalid fp flag, non-cube LUT, fp16 IEEE encodings, round-trips.
- `tests/gbt48347_heif.cc`: 3 cases / 57 assertions — dual-layer HEIF with it35,
  it35 discovery + UWAInfo parse, ordered dimg + hidden gain graph, independent
  sRGB-EOTF×ISO-Formula(2) HDR oracle, SDR codeword preservation, invalid it35.

## Remaining gaps (not claimed as done)

- JPEG MPF/APP2/UWA APP8 container (CN06) — 未实现.
- BaseMapping/CombinedMapping **pixel application** (CN03/CN10) — parse only; 缺失.
- Dynamic-metadata semantics + full display adaptation (GB/T 46269.1) — 未验证.
- Real-device GB/T samples — none licensed/available here; synthetic + independent
  numeric oracle only.
- 3D LUT order N is not pinned by the syntax; derived from block size (标准解释待确认).

No "完全符合国标" claim is made. HEIF dual-layer read/write/it35/graph/reconstruction
is implemented and tested; JPEG and display adaptation are explicitly open.

## Round-2 update (HEAD 02e83f09)

Status changes since round 1:

- **CN05 / metadata_size — defect found and fixed.** Round 1 accepted the inclusive
  range `[payloads, payloads+4]` and serialized `payloads+4`. Re-checking the original
  PDF (p30, §9.2.3a) shows metadata_size = static_metadata_size + dynamic_metadata_size
  (payloads only) — a single discrete value. Parser now requires exact equality and the
  serializer emits the payload total (commit `f5013691`). Regression tests cover the
  conformant value and every adjacent illegal value (payloads−1, +1, +2, +3, +4) plus
  serializer wire bytes: **6 assertions failed before the fix, all pass after**.
- **CN07/CN08/CN09 — production integration added.** New `libheif/gbt48347_heif.{h,cc}`:
  `inspect_dre_aig()` discovers and validates the DRE-AIG through the production context
  (one tmap + valid ISO structure, base-as-primary §9.3.2a, hidden enhancement §9.3.3a,
  irot consistency §9.3.3c/9.3.4c, optional it35 with conformant UWAInfo §9.3.6/10.2.2);
  `add_it35_uwa_info()` attaches a hidden it35 on the write path. This moves GB/T support
  from a test-side standalone parser into libheif itself (commit `02e83f09`).
- **New tests:** `tests/gbt48347_heif_integration.cc` — 6 cases / 95 assertions exercising
  the production read/write/inspect/validate/decode path with negative cases (visible
  enhancement, bad it35 UWAInfo, missing tmap, it35-without-tmap). Pass under ASan/UBSan.
- **Independent binary structure check:** `eval/verify_gbt_structure.py` (non-libheif
  Python ISOBMFF walker) confirms a generated GB/T file has items {base=1, gain=2, tmap=3,
  it35=4}, one `dimg` from tmap count=2 to=[base,gain] (ordered), `tmap` compatible brand,
  base==pitm, idat present.
- **Test counts (round 2):** gbt48347_metadata 104/16, gbt48347_heif 57/3,
  gbt48347_heif_integration 95/6. Full CTest **134/134 pass, 0 fail, 8 skipped**.

Still open (unchanged): JPEG container (CN06), BaseMapping/CombinedMapping pixel
application (CN03/CN10, parse-only; 3D LUT order N not pinned by syntax), GB/T 46269.1
dynamic-metadata semantics + display adaptation (未验证), real-device samples (none).