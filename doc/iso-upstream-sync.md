# Automatic upstream integration

The `sync-iso-upstream` Actions workflow checks `strukturag/libheif:master`
hourly at minute 23 UTC. It can also be started manually from the Actions page;
`validate_current` exercises the complete validation path when there is no new
upstream commit. GitHub schedules may be delayed and combine several upstream
commits into one integration. This is polling, not an upstream push webhook.

The workflow merges upstream into the fork's `master`, then merges that candidate
baseline into `iso21496-gain-map-metadata-core`. It preserves the existing fork
fixes and ISO support using ordinary Git merges. It does not replay a stale patch
file, rewrite history or guess how to resolve conflicts.

Both candidates stay inside a Git bundle artifact until validation passes. Jobs
record the actual checked-out SHA and run with read-only repository permissions:

- The existing gain-map workflow: experimental ON/OFF by LCMS ON/OFF with
  ASan/UBSan, C headers and API symbols; the pinned Google byte/math oracle;
  Apple ImageIO/Core Image and Google HEIF/AVIF exchanges; the interop harness.
- The ordinary Linux test suite on both the fork baseline and ISO integration.
- Existing lint/license checks and the ordinary fuzzer on the ISO candidate.

Only the publication job has `contents: write`. It verifies the bundle and
ancestry, rechecks both remote heads, then performs one atomic, non-forced push.
The existing ISO PR updates with its maintained branch. There is no separate
bot PR or personal-token secret. Validation runs inside this workflow because
pushes made with `GITHUB_TOKEN` do not automatically run the usual push CI.

A merge conflict, failed gate or concurrent branch update leaves both remote
branches unchanged. The failed Actions run and retained source/conflict artifact
identify the problem. Resolve it on the affected maintained branch and rerun the
workflow. Repository rules or token permissions may also refuse publication;
that is a failed run, never a successful synchronization.

This maintenance gate is not a full ISO conformance certification. It does not
replace private image archives, physical-display/Photos validation, or the
separate complete compiler/platform CI matrix used for release review.
