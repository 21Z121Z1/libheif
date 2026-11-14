# Round 3 — independent review, root causes, fixes

Branch `eval/gbt48347-iso-audit-20261114`. Round-3 start HEAD `0270817f` (remote
verified unchanged at round start). All findings below were re-derived from the
current tree and the original standard PDFs, not trusted from round 2.

## 1. HEIF re-read decode test defect (fixed)

- **Root cause**: `tests/gbt48347_heif_integration.cc` decoded `f.tmap` — a handle
  from the *write* context — after reopening the file. This proved nothing about
  the serialized file's decodability (production/test path inconsistency).
- **Severity**: high (false confidence in the read path).
- **Fix**: obtain fresh tmap/base handles from `read_ctx` via `aig->tmap_id` /
  `aig->base_id`; destroy the write context *before* decoding; compare HDR to an
  independent sRGB-EOTF × ISO-Formula(2) oracle (`2·srgb_eotf(127/255)`, tol 1e-4)
  and SDR base to its original codeword. Added no-it35 re-read decode and a
  malformed-`dimg`-target rejection case.
- **Evidence**: 7→11 cases; passes under ASan/UBSan. The loop now genuinely covers
  write → serialize → re-read → parse → HDR/SDR decode → independent pixels.

## 2. GB/T mapping metadata serialization (fixed)

Audited `serialize_mapping_block`, `serialize_extend_info_extension`,
`parse_mapping_block`, `parse_extend_info_extension`, `parse_uwa_info` against
GB/T 48347 tables 15/16/17 (PDF p23–24).

- **Confirmed defects**:
  1. `serialize_extend_info_extension` wrote `write16((uint16_t)block.size())`
     with **no overflow check** — a large LUT (N=32 → 131072 B) truncated the u16
     length prefix into an inconsistent value. Fixed: reject block > 0xFFFF before
     the cast.
  2. `serialize_mapping_block` performed **no validation** — it wrote any flag,
     any matrix length, any LUT length. Fixed: returns `Result`; validates flag ∈
     {0,16,32}, exactly 9 matrix elements for non-zero flag, LUT size == lut_order³,
     flag 0 carries no matrix/LUT.
  3. `parse_extend_info_extension` accepted unexplained **trailing bytes** after
     the two size-prefixed blocks. Fixed: reject (table 15 defines exactly two).
- **Spec-grounded non-changes** (avoid over-restriction): fp(M) is IEEE 754
  (GB/T 17966) which legitimately includes NaN/Inf bit patterns; the syntax does
  not forbid them, so no non-finite rejection was added. N=0/missing-LUT and the
  top-level UWAInfo trailing-byte tolerance are documented interpretations, not
  silently tightened.
- **Evidence**: six negative tests; **7 assertions fail against the pre-fix code,
  all pass after**, under ASan/UBSan.

## 3. HEIF irot / property association (verified correct, clarified + tested)

- GB/T 48347 §9.3.2d: "irot默认为0" → a **missing irot == explicit 0°**; §9.3.3c /
  §9.3.4c require gain/tmap irot to **match the base by value** (angle consistency,
  not property existence). The existing `item_rotation()` (missing→0, compare
  angles) is therefore **correct**; the feared "missing vs explicit-0" conflation
  is exactly what the spec mandates.
- `imir`/`clap` are **not** constrained by GB/T §9.3 → intentionally not compared
  (adding them would over-restrict legal ISO files).
- ipma essential=1 for colr/hvcC/irot (§9.3.2e/3d/4e) is a GB/T writer-side rule;
  not enforced by the reader to avoid rejecting legal ISO files (documented).
- **Evidence**: positive tests (missing default, explicit matching 90°, explicit 0°
  == missing) and negative (base/gain mismatch rejected). No ISO over-rejection.

## 4. Upstream PR #4 / heif_error contract (judgment)

- **Contract**: `heif_error.h:299` — message "is always defined, you do not have to
  check for NULL". `heif_track_decode_next_image` success path returned `{}`
  (message == nullptr) → **violates the public contract**.
- **PR #4** (`codex/fix-sequence-success-message`, ddd6a01e) changes
  `return {}` → `return heif_error_success` (message = `Error::kSuccess` = "Success")
  and adds `REQUIRE(err.message != nullptr)`. This is the **correct root-cause
  production fix**; its CI is green (gcc-10..16, fuzz, api-symbols, macOS).
- **Round-2 Qwen patch** (INFO null-guard) treated only the test symptom and left
  the API contract broken for all callers → **insufficient alone**.
- **Judgment**: **keep PR #4's production fix**; the INFO guard is at best
  complementary defensive hardening. The eval branch adopts the production fix
  (`return heif_error_success`) + the contract assertion. Fail-before:
  `REQUIRE(err.message != nullptr)` fails with `{}`; pass-after: 1683 assertions.
- No other uncontrolled refactor; only this one success-path `return {}` in
  heif_sequences.cc was changed (line 320 already used heif_error_success).
- **PR #4 not modified/closed/merged; no duplicate upstream PR created.**

## 5. GB/T 46269.1—2025 (present; round-2 "missing" corrected)

PDF exists (67 pp), read via rendered images (custom font encoding garbles text
extraction). §7 metadata syntax = **category A** (carried opaquely by UWAInfo /
mdcv/clli); §8 HDR / §9 SDR display adaptation = **category C** (application /
display terminal, per §6 "利用元数据和显示终端参数…在终端显示"). Not a libheif gap.
Cross-standard difference: 46269.1 表10 luminance u(16) (video ES) vs 48347 表18 /
HEIF mdcv u(32) — different carriers, not a defect. See standards-differences §8.

## 6. Second-pass full-diff review (ef2c7d85..HEAD)

- **ABI**: no new `LIBHEIF_API` symbols; gbt48347 is an internal namespace. Only
  heif_sequences.cc success return changed (existing symbol). 469 symbols defined.
- **Overflow / DoS**: metadata_size exact equality (u16 sum in size_t); LUT cube
  check in uint64; mapping block bounded by u16 prefix and rejected > 0xFFFF before
  cast; it35 read bounded by `max_memory_block_size`. No unbounded allocation.
- **Memory ownership**: parsers return owned `std::optional`/`std::vector`;
  serializers return `Result<vector>`; `set_item_data` copies. No leaks (ASan/LSan
  where available; macOS LSan unavailable, noted).
- **Item/reference consistency**: one tmap + one it35 enforced (multi rejected as
  ambiguous); self/duplicate/missing dimg targets rejected via ISO
  `get_input_item_ids`; base==pitm and gain hidden checked.
- **Cross-platform**: portable C++ (span/optional/cstdint, manual fp16); remote
  gcc-10..16 / macOS / Windows jobs added for the eval branch to verify.
- **Prod/test consistency**: the re-read test now decodes only from the re-read
  context (fix #1), removing the write/re-read path mismatch.
- **Independent evidence**: hand-assembled golden vectors, independent sRGB-EOTF
  numeric oracle, non-libheif ISOBMFF structure verifier, Google/Apple/LCMS
  oracles — not self-round-trip alone.

## 7. CI investigation (see round3-ci.md)

- gain-map-conformance / tmap-CIFuzz triggered only on ISO-branch push / master PR
  → eval SHA uncovered. Fixed by adding the eval branch to their push triggers.
- GB/T tests were not in the shared conformance ctest regex and exist only on the
  eval branch → added eval-only `gbt-eval.yml` (ASan/UBSan + structure check) so
  they enter remote CTest without breaking ISO-branch runs.
- sync-iso-upstream schedule: last run 2026-11-06T21:37Z; prior runs already
  throttled to ~8 h (not hourly). Workflow exists on master (default). Exact
  disable reason not inferable without the workflow-state API (network resets);
  GitHub scheduler is outside local control. No fabricated upstream commits.