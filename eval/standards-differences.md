# ISO vs GB/T 48347—2026 — clause-by-clause differences

Compares ISO 21496-1:2025, ISO/IEC 23008-12:2025 + Amd 1:2025 against
GB/T 48347—2026. Categories: **等价** (equivalent), **兼容扩展** (compatible
extension), **附加限制** (additional constraint), **冲突/歧义** (conflict or
ambiguity), **不同应用层** (different application layer), **缺失依赖** (missing
dependency). GB/T PDF physical pages cited where verified.

## 1. Equivalent / ISO-shared (等价)

| Topic | ISO | GB/T | Conclusion |
| --- | --- | --- | --- |
| HEIF tmap idat payload | Amd1 §6.6.2.4.2 ToneMapImage{version u8; GainMapMetadata} | §9.3.4 table 27 (PDF34): versiontmap u8 + version u32 + flag u8 + headrooms + channels | **Byte-identical.** GB/T `version u32` = ISO GainMapVersion (min u16 + writer u16). Reuse ISO parser; do not re-implement. |
| Per-channel metadata | C.2.2 GainMapChannel (min/max signed, gamma unsigned, offsets signed) | §8.2.1 table 14 (PDF22) / §9.2.3 table 25 (PDF31): ir(64)/ur(64) | Equivalent field order and signedness. |
| Gain-map application math | §6.2.1 Formula(1), §6.3 Formula(2)(3) | §10.2.5 a) + Annex C.4 (印43) | Same unnormalize → resample → apply; GB/T explicitly says "符合 ISO 21496-1". |
| dimg ordering | Amd1 §6.6.2.4.1: first=base, second=gain, count=2 | §9.3.4 d) (PDF34) | Same intent (base first, enhancement second). |
| tmap brand / colr / hidden gain | Amd1 §10.2.6, §6.6.2.4.1 | §9.3.2–9.3.4 | GB/T reuses the ISO HEIF graph; colr essential, gain hidden. |
| Resampling phase | §6.2.2 co-sited preferred | §7.2.2.11 (PDF21): co-sited, bilinear default | Equivalent; GB/T names bilinear as the default method. |

## 2. Compatible GB/T extensions (兼容扩展)

| Extension | GB/T clause | ISO status | Implementation |
| --- | --- | --- | --- |
| `it35` item carrying UWAInfo | §9.3.6 table 28 (PDF35) | Amd1 Annex A.6 defines the `it35` item type for ITU-T T.35, but no UWAInfo payload | Implemented: `gbt48347.cc` parse/serialize; `heif_context_add_item("it35")`. |
| T.35 identifiers | §9.2.2a/9.2.3a: country=0x26, provider=0x0004, oriented=0x0007 | not in ISO | Enforced by parser. |
| extended_type bit fields | §8.1, §9.2: file_type/meta_type/enhance_type; base=0x01000000, enhance=0x02010100/0x02010000 | not in ISO | Implemented accessors. |
| ExtendInfoExtension (BaseMapping/CombinedMapping) | §8.2.2 tables 15/16/17: 3×3 fp matrix + N³ LUT, fp16/fp32 | not in ISO | Parse/serialize implemented; pixel application not implemented. |
| Static/dynamic metadata in UWAInfo | §8.2.3 table 18, §8.2.4 table 19 | mdcv/clli exist in HEIF; dynamic metadata per GB/T 46269.1 | Static bytes carried; dynamic semantics 未验证. |
| JPEG UWA Info APP8 | §9.2 tables 21/23 | ISO C.4 uses MPF+APP2 only | Not implemented (JPEG container open). |

## 3. Additional GB/T constraints (附加限制)

| Constraint | GB/T | ISO | Note |
| --- | --- | --- | --- |
| irot consistency | §9.3.3c/9.3.4c: enhancement & tmap irot must equal base irot exactly | Amd1 requires orientation match (4.5) but not byte-equal irot | GB/T is stricter; tested graph uses irot=0. |
| colr types | §9.3.2d: base colr has nclx and rICC; gain colr must be nclx | Amd1: base colr, gain nclx CP=2/TC=2 | Compatible; GB/T names rICC explicitly. |
| HDR colour gate | §10.1.2 a): ColourPrimaries must be BT.2020/BT.2100, transfer HLG/PQ else "非本文件HDR图像" | ISO has no such gate | GB/T-specific classification; a display/application concern, not enforced in the generic ISO reader (must not relax ISO tmap). |

## 4. Conflicts / ambiguities (冲突/歧义) — recorded, not silently resolved

| # | Location | Issue | Disposition |
| --- | --- | --- | --- |
| D1 | table 13 (PDF21–22) vs tables 24/27 (PDF31/34) + Annex C.2 | table 13 flag byte = reserved:u(6), flag:u(1), numberofcomponents_flag:u(1) (**LSB-first**); tables 24/27 + C.2 use ISO **MSB-first** (bit7 multichannel, bit6 use_base). Internal GB/T inconsistency. | The HEIF/JPEG **file embeddings** (tables 24/27) follow the ISO MSB layout, which is what libheif parses. The abstract table-13 byte is not used for the file path. 标准解释待确认. |
| D2 | tables 13/24/27 channel loop | printed `for(i=0;i<number_of_components-1;i++)`; with count∈{1,3} yields 0 or 2 iterations, contradicting the per-channel table and ISO (1 or 3). | Drafting inconsistency. File embeddings follow ISO channel_count. 标准解释待确认; no golden vector invented from the `-1`. |
| D3 | §9.3.4 d) (PDF34) | both To_item_id described as "第一个"; second should be "第二个". | Typo; intent (base first, enhancement second) matches ISO Amd1. Implemented per intent. |
| D4 | §9.2.3a metadata_size | "动态和静态元数据总长度" — unclear whether it includes the two 16-bit size fields. | Parser records it and accepts the inclusive range [payloads, payloads+4]; payloads located by the explicit size fields. 标准解释待确认. |
| D5 | tables 16/17 3D LUT | `fp(M)×N×N×N` but N is not given in the syntax. | N derived from the size-prefixed block length; must be an exact cube. 标准解释待确认. |
| D6 | Annex C.2 (资料性, 印40) | calls headroom numerator/denominator "16位无符号数", while tables 24/27 (normative) use u(32) and ISO uses u32. | Informative annex inconsistency; normative u(32) governs. Not used to change the ISO parser. |

## 5. Different application layer (不同应用层)

- GB/T §10.1.3 / §10.2.6 / §11.1 **display adaptation** follows GB/T 46269.1—2025
  (tone-mapping curve from target display luminance). ISO 21496-1 stops at
  reconstructing the alternate representation; it does not define display adaptation.
  This is outside a file codec's API → 库职责外 / 未验证. libheif exposes reconstructed
  pixels + metadata; the application performs display adaptation.

## 6. Missing dependency (缺失依赖)

- **GB/T 46269.1—2025** (dynamic metadata + display adaptation) is normatively
  referenced by GB/T 48347 (§7.1.2/7.1.3, §8.2.4, §10, §11). Its full text is not
  part of this audit. All dynamic-metadata-semantics and full-display-adaptation
  conclusions are **未验证**, not PASS.

## 7. Version-reference rules

GB/T §2 uses dated references where given (e.g. GB/T 46269.1—2025, ITU-R
BT.2100-3:2025) and undated otherwise (ISO 21496-1, ISO/IEC 23008-12, 14496-12,
23091-2 → latest edition incl. amendments). ISO 21496-1:2025 and HEIF 2025+Amd1
are the editions audited here. Conclusions are scoped to these editions; a future
ISO amendment is not automatically covered by the GB/T undated reference.

## Engineering rule honored

No generic ISO `tmap` validation was relaxed for a GB/T special case. The GB/T
increment is additive (`gbt48347` module + tests); the ISO parser/math/graph are
unchanged, and the full ISO suite still passes (see `eval/test-results.md`).