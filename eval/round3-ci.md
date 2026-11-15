# Round 3 — remote CI evidence for the final SHA

Final evaluation SHA: `0b932b601d768ad0d30161eeb50c32b7b1733c3a`
(branch `eval/gbt48347-iso-audit-20261114`). Push history this round:
`0270817f..fa66cfd3` (code + fixes + CI config) then `fa66cfd3..0b932b60`
(eval documentation only; **no code change**).

**All four workflows completed `success` on the final SHA `0b932b60`** (headSha
verified, event=push): gbt-eval-conformance 43673615228, gain-map-conformance
43673615153, tmap-CIFuzz 43673615213, gcc-versions 43673615147. The detailed job
breakdown below was captured on the code-identical `fa66cfd3` and re-confirmed on
`0b932b60`.

## Why earlier eval pushes lacked coverage (root cause)

- `gain-map.yml` and `tmap-cifuzz.yml` triggered only on
  `push: [iso21496-gain-map-metadata-core]` / `pull_request: [master]`. The eval
  branch matched neither, so round-2's `0270817f` push ran only `gcc-versions`
  (run 43605842466). **Fixed** by adding the eval branch to both push triggers.
- GB/T tests exist only on the eval branch and were not in the shared conformance
  ctest regex; adding them there would break ISO-branch runs. **Fixed** with an
  eval-only `gbt-eval.yml`.

## Runs on fa66cfd3 (all success)

| Workflow | Run | Jobs (all success) |
| --- | --- | --- |
| gbt-eval-conformance | [43614441498](https://github.com/21Z121Z1/libheif/actions/runs/43614441498) | gbt-tests: build ASan/UBSan; GB/T ctest (`gbt48347_`); independent ISOBMFF structure check |
| gain-map-conformance | [43614441615](https://github.com/21Z121Z1/libheif/actions/runs/43614441615) | conformance (OFF,OFF)/(OFF,ON)/(ON,OFF)/(ON,ON) [ASan/UBSan, exp×LCMS]; ultrahdr-metadata (pinned Google); apple-frameworks (macos-15, macos-15-intel); windows-native; interop-binary |
| tmap-CIFuzz | [43614441503](https://github.com/21Z121Z1/libheif/actions/runs/43614441503) | Fuzzing x86_64 address/undefined/memory, i386 address |
| gcc-versions | [43614441481](https://github.com/21Z121Z1/libheif/actions/runs/43614441481) | gcc-10..16 (7 jobs) |

### Coverage answers to the round-3 questions

1. **gain-map-conformance on eval push**: now triggers (was ISO-branch-only). Ran
   and passed on fa66cfd3.
2. **tmap-CIFuzz on latest SHA**: now triggers; 4 fuzz jobs passed on fa66cfd3.
3. **Linux/macOS/Windows**: conformance+ultrahdr+interop on ubuntu-24.04;
   apple-frameworks on macos-15 and macos-15-intel; windows-native on windows-2022.
   All actually ran and passed.
4. **GB/T tests in remote CTest**: yes, via gbt-eval-conformance
   (`ctest -R 'gbt48347_'` = metadata + heif + integration), under ASan/UBSan, plus
   the non-libheif structure verifier. (Kept out of the shared gain-map job so
   ISO-branch runs are unaffected.)
5. **Independent oracles on latest code**: ultrahdr-metadata (pinned Google
   66821e0a) and apple-frameworks (ImageIO/CoreImage) ran on fa66cfd3; LCMS via the
   conformance lcms=ON matrix. All passed.
6. **ASan/UBSan**: conformance matrix builds with
   `-fsanitize=address,undefined`, `ASAN_OPTIONS=detect_leaks=1`,
   `UBSAN_OPTIONS=halt_on_error=1`; gbt-eval likewise. No skipped/ignored sanitizer
   checks; no requirement lowered.
7. **Every job on the final SHA**: headSha of all four runs = fa66cfd3 (verified).

No failures; no skips among the triggered jobs. (Local full CTest has 8
design-skips needing codecs/fixtures; the remote jobs build the needed codecs.)

## sync-iso-upstream schedule investigation

- Workflow exists on `master` (default branch); triggers `schedule: '23 * * * *'`
  + `workflow_dispatch`.
- Run history: last scheduled run **2026-11-06T21:37:35Z** (success); prior runs
  spaced ~8 h apart (21:37/16:00/07:59/00:25/…), i.e. already **throttled** well
  below the hourly cron before stopping.
- Evidence-based conclusion: the schedule stopped after Nov 6 and was throttled
  before that. The exact disable reason (GitHub inactivity/throttling policy vs.
  manual disable) could not be confirmed because the workflow-state API returned
  repeated connection resets; **not inferred without evidence**. GitHub's
  scheduler is outside local control.
- No fabricated upstream commits; no maintained-branch modification. The sync
  logic itself was re-verified locally (9 fixture tests; real-SHA no-op
  prepare/publish leaves heads unchanged; see round-2/round-3 review).
- Safe, reproducible local verification of the sync path is provided by
  `scripts/test-sync-iso-upstream.py` (fixtures), independent of GitHub scheduling.