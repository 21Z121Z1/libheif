# Round 3 — test results (local)

Host: macOS 27.3 arm64, Apple clang 21.0.0, CMake 4.4.5, Ninja 1.13.2.
Build dirs: `build-eval` (testing preset, LCMS ON), `build-eval-asan`
(`-fsanitize=address,undefined`). All commands run from the eval worktree.

## Per-fix fail-before / pass-after

| Fix | Fail-before | Pass-after |
| --- | --- | --- |
| heif_error success contract | `return {}` → `REQUIRE(err.message != nullptr)` FAILED (5 assertions, 4 pass, 1 fail) | `return heif_error_success` → sequence test 1683 assertions / 8 cases pass |
| mapping serialization | pre-fix code: 6 new negative cases FAILED (7 assertions) | post-fix: gbt48347_metadata 113 assertions / 22 cases pass |
| re-read decode test | (defect: decoded write-context handle) | post-fix: 11 cases / 193 assertions pass, write ctx destroyed before decode |

## Unit / integration (build-eval, exit 0)

| Command | Result |
| --- | --- |
| `./build-eval/tests/gbt48347_metadata` | All tests passed (113 assertions / 22 cases) |
| `./build-eval/tests/gbt48347_heif` | All tests passed (57 / 3) |
| `./build-eval/tests/gbt48347_heif_integration` | All tests passed (193 / 11) |
| `./build-eval/tests/sequence_no_decoder_output` | All tests passed (1683 / 8) |
| `ctest --test-dir build-eval -R 'gbt48347_'` | 3/3 pass |

## ASan/UBSan (build-eval-asan, ASAN_OPTIONS=detect_leaks=0, UBSAN halt_on_error=1)

| Test | Result |
| --- | --- |
| gbt48347_metadata | pass, no report |
| gbt48347_heif | pass, no report |
| gbt48347_heif_integration | pass, no report |
| gain_map_metadata / gain_map_math / tmap_read / tmap_write / entity_groups | pass, no report |

macOS LSan unavailable (detect_leaks=0); Linux CI runs detect_leaks=1.

## Full CTest (build-eval)

`ctest --test-dir build-eval` → **134/134 pass, 0 failed, 8 skipped**, exit 0.
Skipped (by design; need codecs/fixtures/env): tmap_interop, encode,
jpeg2000_openjpeg_grid_limit, jpeg2000_signed_components,
derived_images_shared_input_threads, write_to_file, sequence_vvc_decoder_latency,
ffmpeg_decode_bitdepth. None counted as passed; none weakened.

## Independent oracles (current HEAD)

| Oracle | Result |
| --- | --- |
| gain_map_ultrahdr (pinned Google 66821e0a) | 16432 assertions / 3 cases pass |
| Apple ImageIO/CoreImage (check-tmap-apple.swift) | mono/rgb/ramp consumer PASS |
| Google container exchange (check-tmap-ultrahdr.py) | 14 PASS |
| tmap_interop (Apple+external+stock-ultrahdr) | 909522 assertions / 3 cases, 0 skipped |
| eval/verify_gbt_structure.py (non-libheif) | INDEPENDENT STRUCTURE OK |

## Static / ABI

| Command | Result |
| --- | --- |
| cpplint (repo config) on changed files | 0 errors |
| check-api-symbols.sh | 469 declared, all defined (no ABI added) |

## Not run locally (reason)

- licensecheck: tool not installed.
- Full remote CI matrix: see round3-ci.md (push + Actions).