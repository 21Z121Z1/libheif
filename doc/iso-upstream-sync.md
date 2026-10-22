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

Only the publication job receives the repository-scoped SSH deploy key from
`ISO_SYNC_SSH_KEY`; all `GITHUB_TOKEN` permissions remain read-only. The job
verifies the bundle and ancestry, rechecks both remote heads, then performs one
atomic, non-forced push. The existing ISO PR updates with its maintained branch.
There is no separate bot PR. Validation runs before publication inside this
workflow; an SSH push can also start the ordinary repository CI. Manual
validation exercises publication of unchanged refs as well.

A merge conflict, failed gate or concurrent branch update leaves both remote
branches unchanged. The failed Actions run and retained source/conflict artifact
identify the problem. Resolve it on the affected maintained branch and rerun the
workflow. Repository rules or token permissions may also refuse publication;
that is a failed run, never a successful synchronization.

The default token cannot introduce new workflow-file content. The dedicated
write deploy key also handles upstream changes to workflow files, without a
personal account token. Its public key is named `ISO upstream sync publisher`
in this repository's Deploy keys settings. It can be revoked there and rotated
together with the `ISO_SYNC_SSH_KEY` Actions secret. See GitHub's
[deploy key documentation](https://docs.github.com/en/authentication/connecting-to-github-with-ssh/managing-deploy-keys).

This maintenance gate is not a full ISO conformance certification. It does not
replace private image archives, physical-display/Photos validation, or the
separate complete compiler/platform CI matrix used for release review.
