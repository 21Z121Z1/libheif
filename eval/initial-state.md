# Initial state — GB/T 48347 + ISO gain-map evaluation

Recorded at 2026-11-14 (UTC times below). All work happens on the evaluation
branch in an isolated worktree; no maintained branch is modified.

## Repository identities

| Item | Value |
| --- | --- |
| Fork | `21Z121Z1/libheif` (public fork of `strukturag/libheif`) |
| Upstream | `strukturag/libheif`, default branch `master` |
| Evaluation branch | `eval/gbt48347-iso-audit-20261114` (new, local only) |
| Evaluation worktree | `/Users/retriedstormtrooper/Documents/ChatGPT/libheif-eval-gbt48347` |
| Base checkout (untouched) | `/Users/retriedstormtrooper/Documents/ChatGPT/libheif` @ `5bfe52a3` |

### Full SHAs and fetch time

Fetched with `git fetch origin --prune` and `git fetch upstream master --prune`
on 2026-11-14; upstream fetch completed 2026-11-14T11:51:42Z, SHA readback
2026-11-14T11:52:02Z.

| Ref | Full SHA |
| --- | --- |
| Fork `master` (`origin/master`) | `3da8ba28db7ce9bb00f3a4c8a05d10b48bc89117` |
| ISO branch (`origin/iso21496-gain-map-metadata-core`) | `ef2c7d850612740530699a15c9496fdefc63c230` |
| Upstream `master` (`upstream/master`) | `12a259a5882dd99d45ec5fc77e296a3b620d1e2e` |
| Evaluation branch HEAD (= ISO branch HEAD) | `ef2c7d850612740530699a15c9496fdefc63c230` |
| Pre-existing local ISO checkout | `5bfe52a36f2fc74525ba50dc78aa40e4eb3b467c` (ancestor of remote ISO HEAD; left untouched) |

### Ancestry (checked with `git merge-base --is-ancestor`)

- `upstream/master` **is fully merged** into `origin/iso21496-gain-map-metadata-core`.
- `upstream/master` **is fully merged** into `origin/master`.
- Local `5bfe52a3` is an ancestor of `origin/iso21496-gain-map-metadata-core`.

Consequence: the upstream-sync task is the **no-op / validate_current** case.
No synthetic sync commit may be created; the existing `validate_current`
entry point must be exercised instead (see `eval/upstream-sync.md`).

## System environment

| Item | Value |
| --- | --- |
| OS | macOS 27.3 (build 26C5035r), arm64 |
| Compiler | Apple clang 21.0.0 (clang-2100.4.1.1) |
| CMake | 4.4.5 |
| Ninja | 1.13.2 |
| Python | 3.13.14 (pyenv) |
| git | homebrew |
| Codec deps (pkg-config) | libde265 1.1.3, x265 4.3, aom 3.15.3, dav1d 1.5.4, libavcodec 63.1.102, libavutil 61.1.102, lcms2 2.19, libjpeg 3.2.1 |

## Documents read before any change

- `README.md` (root) — project overview, C API usage.
- `AGENTS.md` — **does not exist** in the evaluation HEAD, in any local branch,
  or in any fetched remote ref (`git log --all -- AGENTS.md` empty). Recorded as
  absent, not silently skipped.
- `CMakeLists.txt`, `CMakePresets.json` — presets `develop/release/release-noplugins/testing/fuzzing`;
  `testing` = Debug, plugins OFF, experimental ON, warnings-as-errors ON, uncompressed codec ON.
- `.github/workflows/` — 14 workflows incl. `gain-map.yml` (now `workflow_call`-capable),
  `sync-iso-upstream.yml`, `lint.yml`, `test.yml`, `osx.yml`, `tmap-cifuzz.yml`.
- `scripts/sync-iso-upstream.py`, `scripts/test-sync-iso-upstream.py`,
  `.github/actions/load-iso-source/action.yml`, `doc/iso-upstream-sync.md`.
- `doc/tmap-conformance-matrix.md` — existing ISO acceptance matrix (E1–E12 evidence).
- `doc/tmap-support.md`, `doc/tmap-independent-review.md`, `doc/tmap-upstream-series.md`.
- Tests: `tests/gain_map_metadata.cc`, `gain_map_math.cc`, `gain_map_color.cc`,
  `tmap_read.cc`, `tmap_write.cc`, `tmap_interop.cc`, `entity_groups.cc`,
  `gain_map_ultrahdr.cc`.
- Private local standards references (read-only, never committed): ISO 21496-1:2025,
  ISO/IEC 23008-12:2025 + Amd 1:2025 Markdown, GB/T 48347—2026 PDF + reading-version
  Markdown, GB/T 46269.1—2025 PDF, and the GB/T audit checklist.

## Baseline configuration

```
cmake --preset testing -G Ninja -S . -B build-eval \
  -DENABLE_EXPERIMENTAL_FEATURES=ON -DENABLE_PLUGIN_LOADING=OFF \
  -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF \
  -DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF \
  -DWITH_X265=ON -DWITH_X265_PLUGIN=OFF \
  -DWITH_AOM_DECODER=ON -DWITH_AOM_ENCODER=ON \
  -DWITH_DAV1D=ON -DWITH_DAV1D_PLUGIN=OFF \
  -DWITH_FFMPEG_DECODER=ON -DWITH_FFMPEG_DECODER_PLUGIN=OFF \
  -DWITH_AOM_DECODER_PLUGIN=OFF -DWITH_AOM_ENCODER_PLUGIN=OFF \
  -DWITH_JPEG_DECODER=ON -DWITH_JPEG_ENCODER=ON
```

Configure exit 0. Built-in backends: x265, libde265, dav1d, aomdec, aomenc,
jpegdec, jpegenc, ffmpegdec, x264, libsharpyuv. LCMS2 found (2.19).

## Baseline test results (before any source change)

Command: `ctest --test-dir build-eval -R 'gain_map_metadata|gain_map_math|gain_map_color|tmap_read|tmap_write|entity_groups|id_creator|uncompressed_encode' --output-on-failure`

First run: 8/9 passed; `uncompressed_encode_multicomponent` was **Not Run**
(executable not yet built — a build-target omission, not a code failure).
After building that target: **9/9 passed, 0 failed, 0 skipped**, exit 0.

| Test | Result | Catch2 cases |
| --- | --- | --- |
| gain_map_metadata | Passed | 19 |
| gain_map_math | Passed | 51 |
| gain_map_color | Passed | 18 |
| tmap_read | Passed | 52 |
| tmap_write | Passed | 33 |
| entity_groups | Passed | 9 |
| id_creator | Passed | — |
| uncompressed_encode | Passed | — |
| uncompressed_encode_multicomponent | Passed | — |

Sync fixture suite: `python3 scripts/test-sync-iso-upstream.py -v` →
**Ran 7 tests, OK**, exit 0 (no_change, manual_validation, merge+publish,
conflict, concurrent_update, atomic_rejection, manifest_mismatch).

## Known blockers / boundaries at baseline

- `AGENTS.md` absent (see above).
- GB/T 46269.1—2025 full text is referenced by GB/T 48347 for dynamic metadata /
  display adaptation; only its PDF is present locally and it is a separate
  standard. Full display-adaptation conformance is **unverified** by design.
- No licensed real-device HDR archives are available in this environment; the
  two archives named in the existing matrix (G08/G09) remain OPEN.
- Apple ImageIO/Core Image cross-checks are available on this macOS host;
  pinned libultrahdr/libavif oracles must be built from fixed revisions.
- Push / PR / merge / publish are **not authorized**; all publication paths are
  exercised only inside disposable local Git fixtures.
---

## Round-2 continuation state

- Round-2 starting HEAD: `a3786f03998bc412c268a52cfdc1ebfdaa883081` (round-1 final, pushed).
- Round-2 final HEAD: `02e83f0947b8f7429256db02b21e3aac97736494`.
- Branch / worktree unchanged: `eval/gbt48347-iso-audit-20261114` in
  `/Users/retriedstormtrooper/Documents/ChatGPT/libheif-eval-gbt48347`.
- Round-2 commits: `f5013691` (metadata_size exact-equality fix), `02e83f09` (GB/T HEIF
  production integration). Round-1 work, tests, logs and eval/ evidence preserved.
- Upstream `strukturag/libheif` master still `12a259a5882dd99d45ec5fc77e296a3b620d1e2e`,
  fully merged into both fork branches → sync remains no-op.
- Pristine upstream worktree used for the independent bug second pass:
  `/tmp/libheif-upstream-pristine` (detached at upstream master).
- Full CTest at round-2 HEAD: 134/134 pass, 0 fail, 8 skipped.
