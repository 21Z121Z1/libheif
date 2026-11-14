# Upstream synchronization verification

Scope: `scripts/sync-iso-upstream.py`, `scripts/test-sync-iso-upstream.py`,
`.github/workflows/sync-iso-upstream.yml`, `.github/workflows/gain-map.yml`
(`workflow_call`), `.github/actions/load-iso-source/action.yml`,
`doc/iso-upstream-sync.md`. All publication paths were exercised only inside
disposable local Git fixtures; **nothing was pushed to GitHub**.

## 1. Upstream vs fork difference — the no-op case

Full SHAs (fetched 2026-11-14T11:51:42Z, see `eval/initial-state.md`):

| Ref | SHA |
| --- | --- |
| upstream/master | `12a259a5882dd99d45ec5fc77e296a3b620d1e2e` |
| origin/master | `3da8ba28db7ce9bb00f3a4c8a05d10b48bc89117` |
| origin/iso21496-gain-map-metadata-core | `ef2c7d850612740530699a15c9496fdefc63c230` |

`git merge-base --is-ancestor` results:

- upstream/master **is an ancestor of** origin/master → fully merged.
- upstream/master **is an ancestor of** origin/iso21496-gain-map-metadata-core → fully merged.

**Conclusion: there is no new upstream commit to integrate.** Per the task rule,
no synthetic sync commit was created. The `validate_current` entry point was
exercised instead, and the full validation gates were run on the current source.

## 2. Real-SHA `validate_current` no-op evidence

A disposable fixture was seeded with the three real SHAs above (local bare
`origin.git` + `upstream.git`, no network publication). Running the actual CLI:

```
python3 scripts/sync-iso-upstream.py prepare --repo work \
  --upstream upstream.git --output artifact --validate-current
```

Result (exit 0): `changed=false`, `base_after==base_before==3da8ba28…`,
`iso_after==iso_before==ef2c7d85…`, `upstream=12a259a5…`. Because
`--validate-current` was set, a `source.bundle` (13,372,269 bytes) was produced
even though nothing changed — this is the forced-validation path.

```
python3 scripts/sync-iso-upstream.py publish --repo publisher --output artifact
```

Result (exit 0): `Everything up-to-date`; remote heads before == after
(`3da8ba28…`, `ef2c7d85…`). The no-op publication is a safe fast-forward of
identical refs, not a rewrite.

## 3. New-change end-to-end (prepare → merge candidate → validate → atomic publish)

In the same real-SHA fixture, upstream was advanced with a genuine new commit
`ac334e701c31936b04f94d91d5a9dd5e2457dd73` (adds `NEW_UPSTREAM.txt`). A **fresh
disposable checkout** was used (the script requires this; reusing a checkout
that already holds `codex/iso-sync-base` correctly fails with exit 128 — that is
the documented "never reset or reuse local branches" guard, not a defect).

- `prepare` (exit 0): `changed=true`,
  `base_after=12677cd5ea8a7a0427d16cd5209b3155ec356724`,
  `iso_after=19c0bf6fc90252a357bd1f0d1f20e795897dc4c9`.
  Candidate retains ISO source (`libheif/gain_map_metadata.h` present) and the
  new upstream file — an ordinary merge, not a patch replay.
- Validation gates: represented locally by the gain-map/tmap CTest suite run on
  the candidate source (see `eval/test-results.md`) plus the gate-integrity
  fixture test below. In CI these are the `gain-map`, `core-tests`,
  `source-checks` jobs.
- `publish` (exit 0): one `--atomic` non-forced push. Remote heads advanced to
  `12677cd5…` (master) and `19c0bf6f…` (ISO). Post-checks:
  `base_before→base_after` FF ok, `iso_before→iso_after` FF ok,
  `upstream→base_after` integrated ok.

## 4. Synchronization test matrix (isolated Git fixtures)

`python3 scripts/test-sync-iso-upstream.py -v` → **Ran 9 tests, OK** (exit 0).
Seven pre-existing tests plus two added by this evaluation.

| State class | Test | Result |
| --- | --- | --- |
| No change (upstream already integrated) | `test_no_change` | PASS — `changed=false`, no bundle, refs untouched |
| Already-updated branch, forced validation | `test_manual_validation_can_bundle_unchanged_source` | PASS — bundles unchanged source, publish is no-op |
| Real merge + atomic publish both branches | `test_merge_retains_iso_and_publishes_both_branches` | PASS — ISO content retained, master does not gain ISO files |
| Conflict | `test_conflict_does_not_publish` | PASS — `conflicts.txt` written, refs untouched |
| Concurrent update during validation | `test_concurrent_update_does_not_overwrite_either_branch` | PASS — `changed during validation`, refs untouched |
| Atomic rejection (one branch refused) | `test_atomic_rejection_preserves_both_branches` | PASS — update hook rejects ISO, **both** branches unchanged |
| Manifest/bundle mismatch | `test_manifest_must_match_bundle` | PASS — `do not match`, refs untouched |
| **Non-fast-forward candidate** (added) | `test_non_fast_forward_candidate_is_rejected` | PASS — manifest+bundle agree but ISO candidate is not descended from ISO head → ancestry gate rejects, refs untouched |
| **Validation-gate integrity** (added) | `test_publish_requires_all_validation_gates` | PASS — publish `needs` = {prepare, gain-map, core-tests, source-checks}; no `always()`/`failure()` bypass; no `continue-on-error` on any gate |

The two added tests close evidence gaps for the task's explicit "快进"
(fast-forward) and "候选验证失败 / 禁止弱化门禁" requirements. They are
regression coverage; **no production sync code required a change** — the
prepare/publish logic was verified correct, including:

- bundle verify + ref/manifest match before any push;
- four ancestry gates (`base_before→base_after`, `iso_before→iso_after`,
  `upstream→base_after`, `base_after→iso_after`);
- `ls-remote` concurrency recheck, backed by non-forced push (a concurrent
  advance makes the push non-fast-forward and thus rejected even in the
  TOCTOU window);
- `--atomic` both-or-neither publication;
- read-only `GITHUB_TOKEN` everywhere; only the publish job holds the
  `ISO_SYNC_SSH_KEY` deploy key, and no candidate code runs in that job.

## 5. Workflow behavior review

- `sync-iso-upstream.yml` triggers hourly (`23 * * * *`) and on
  `workflow_dispatch` with boolean `validate_current`. Gate jobs run when
  `changed == 'true' || inputs.validate_current`. `publish` additionally
  `needs` all three gate jobs, so a failed gate blocks publication.
- `gain-map.yml` is `workflow_call`-capable with `source_ref` / `source_bundle`
  / `source_sha` inputs; `load-iso-source` verifies the bundle, fetches
  `codex/iso-sync-*`, detaches at the exact SHA and asserts
  `rev-parse HEAD == SOURCE_SHA`. Gates therefore run on the **candidate**, not
  on a stale branch tip.
- The reusable gate workflow is referenced at the maintained ISO branch
  (`@iso21496-gain-map-metadata-core`); gate definitions come from the
  maintained branch while the tested source comes from the immutable bundle.

## 6. CI status for this evaluation

Push to the evaluation branch is **not authorized**, so no new GitHub Actions
run was triggered by this evaluation. The latest published exact-SHA evidence
remains that recorded in `doc/tmap-conformance-matrix.md` (E7/E10 on
`2d3ae1e0…` / `e28e36ee…`). The local gates in `eval/test-results.md` were run
on the evaluation HEAD `ef2c7d85…`. Any claim about Actions green state in this
report refers to those previously published runs, not to a new run.

## 7. Artifacts

- Fixture root (disposable, local only): `/tmp/iso-sync-real-fixture/`
  - `artifact/manifest.json` — no-op validate_current manifest
  - `artifact2/manifest.json` — new-change manifest
- Added tests: `scripts/test-sync-iso-upstream.py`
  (`test_non_fast_forward_candidate_is_rejected`,
  `test_publish_requires_all_validation_gates`).