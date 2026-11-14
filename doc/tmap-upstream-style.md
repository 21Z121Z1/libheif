# Upstream style and simplification review

Review baseline: PR #1 at `1f3c71731e56765e7d5f692a79fbeb5214d77d0d`.
The fork merge-base is `9585110d405ae24ba258295d4ba7542dfc59035d`;
the upstream merge-base is `364905fc9841bdda229e0ae2c5fda13b5fc98d1a`.
Only PR additions and directly affected code are in scope.

The review used `git log`, `git log --first-parent`, `git show`, `git blame`,
`git diff`, and `git merge-base`. It examined the patches below and adjacent
image-item, C API, color-conversion, context, parser, and test code.

## Historical evidence

| Upstream commit | Code examined | Practice relevant to this PR |
| --- | --- | --- |
| [24cd2a63](https://github.com/strukturag/libheif/commit/24cd2a63caaf3a8f6efc562d92371dc866f3befa) | C API color/auxiliary functions; `error.{cc,h}` | Reuse the shared null-argument error; omit redundant C++ type keywords. |
| [c1b3d163](https://github.com/strukturag/libheif/commit/c1b3d163a12b068dcc179aa4dc693aa2d95a8987) | `error.h` | Move values into `Result<T>` without adding ownership wrappers. |
| [3a81c0ec](https://github.com/strukturag/libheif/commit/3a81c0ecc04ae079ff5f4d84ca865c1718ef20a5) | `image-items/mask_image.cc` | Check required properties before reading them; avoid overflowing size products. |
| [f01870c1](https://github.com/strukturag/libheif/commit/f01870c1d7323a3003796d58eba7fff502be994c) | `context.cc`, `box.cc`, `image_item.cc` | Apply transforms at their owning layer; retain independent boundary checks. |
| [748656fb](https://github.com/strukturag/libheif/commit/748656fb74e3ed60e2a1a429574af9b2cb15e63c) | `error.h`, experimental API, image items | Test `Result` success explicitly and extract its value with `*`. |
| [bfa2e835](https://github.com/strukturag/libheif/commit/bfa2e8354959984495903b04fdce8afdd4810575) | `image-items/grid.cc` | Remove the unsafe fast path and keep the synchronization reason. |
| [86a8b43d](https://github.com/strukturag/libheif/commit/86a8b43db4f636f2183a4707b642b0b8a4d0a8d6) | Grid/image decoding | Return the original failure before it becomes an opaque downstream error. |
| [542ccdfb](https://github.com/strukturag/libheif/commit/542ccdfbc322cdb5b543e25083e038d17df63728) | Property writers, `box`, `context`, `file`, image items | Preserve property numbering and ownership; propagate write failures. |
| [f313930b](https://github.com/strukturag/libheif/commit/f313930be3980a2b92f79179a99cabda176a84ca) | `image-items/overlay.cc` | Use direct wire-width conversions and explain non-obvious signed semantics. |
| [d34a288c](https://github.com/strukturag/libheif/commit/d34a288c469c469b479b74cc879fbe3ff28c6f4c) | Color state, alpha, HDR/SDR conversion | Name the real invariant: uniform depth and maximum depth are different. |
| [1b22ef41](https://github.com/strukturag/libheif/commit/1b22ef41d62c76cc32230f4c4d4ea8f57745e268) | Chroma sampling, conversion planning, YUV/RGB conversion | Remove a blanket restriction only after each consumer declares its constraint. |
| [aa0714f6](https://github.com/strukturag/libheif/commit/aa0714f6999e394fac394842186d8e8975c88748) | C API file writer; `tests/write_to_file.cc` | Preserve failure and error-string lifetime semantics; test observable outcomes. |
| [ef1a201f](https://github.com/strukturag/libheif/commit/ef1a201f78982eb3303334292dd53d9ced1fb178) | `color-conversion/chroma_sampling.cc` | Extract one per-plane helper to remove duplicate index arithmetic. |
| [a1645b6f](https://github.com/strukturag/libheif/commit/a1645b6f13dad2fd9132532fa890f4fd11de38d0) | `api/libheif/heif_error.h` | Public lifetime contracts require documentation. |
| [c92bd851](https://github.com/strukturag/libheif/commit/c92bd8514886c8b7fad311848a0c0e0bcb6ef998) | `api/libheif/heif_cxx.h` | Thin public wrappers can be intentional API; copied comments still need review. |

## Rules applied to new code

- Use two-space indentation, function/class braces on the next line, control
  braces on the condition line, and a separate `else` line. Keep short header
  accessors compact, but write algorithmic error paths as multiline guards.
- Follow adjacent snake_case names, `m_` members, and `ImageItem_*` types.
  Historical camelCase names and alternative brace layouts do not justify
  reformatting unrelated code.
- `Result<T>` is true for a value; `Error` is true for an error. Check before
  dereferencing, propagate the original error, and use the correct error-buffer
  owner when crossing the C API boundary.
- Reuse existing library primitives. Add a helper when it removes repeated work
  or an independently maintained invariant, not merely to forward arguments.
- Prefer explicit loops for pixel arithmetic and ordinary library algorithms
  for copying and searching. Keep bounds and rounding visible.
- Preserve licenses, API contracts, standard-specific semantics, numerical
  precision, ownership, and synchronization explanations. Remove comments that
  repeat the code; do not use comment count as a quality target.
- Keep tests focused on observable behavior. Establish prerequisites with
  `REQUIRE`, then use `CHECK` for independent outcomes. Do not delete upstream
  tests as part of cleanup.

## Simplification decisions

The legacy internal `gain_map_nclx_from_icc()` projection had only test callers.
Production uses `GainMapColour::from_icc()` to preserve actual ICC colourants and
curves. Removing the old projection also removes its matrix/transfer classifiers
and the curve evaluator's classifier-only mode. Its tests must exercise the real
consumer instead, including malformed profiles and custom primaries. The CICP
reader remains because production still uses it.

The private metadata byte writer duplicates `StreamWriter`. Reusing the existing
writer removes that parallel implementation. Signed numerators must retain their
exact unsigned wire representation, including `INT32_MIN`. The bounded span
reader remains: exact truncation reporting and unknown-version prefix handling
are part of its contract.

The review retained parse-status distinctions, root-only target headroom,
distinct raster/output ICC descriptions, per-decode CMM ownership, and exact
rational endpoint arithmetic. Each has a production consumer or a required
failure behavior. Replacing these with fewer fields would change semantics.

The pstack no-comments/unslop and DeepSeek simplification guidance informed this
review. Their product-specific commands and architecture were not imported.
