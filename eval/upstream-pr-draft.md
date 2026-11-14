# Upstream PR draft — sequence_no_decoder_output null-message crash

Target: `strukturag/libheif` (master). **Draft only — not submitted.**
Patch file: `eval/0001-Fix-null-error-message-crash-in-sequence_no_decoder_.patch`
(based on upstream `12a259a5882dd99d45ec5fc77e296a3b620d1e2e`, single file, 2 lines).

## Suggested title

`Fix SIGSEGV in sequence_no_decoder_output when err.message is null`

## Suggested description

### What

`tests/sequence_no_decoder_output.cc` crashes with SIGSEGV in the test case
"decoder latency may exceed the length of the repeated segment". The crash is
in the test's own `INFO` diagnostic, not in library decoding.

### Root cause

Catch2 builds an `INFO` message **eagerly**. The macro expands to
`Catch::MessageBuilder(...) << log`, and `MessageBuilder::operator<<`
(`catch_amalgamated.hpp`) immediately executes `m_stream << value`. Streaming a
`const char*` calls `std::ostream::operator<<(const char*)` → `strlen`.

`INFO("error: " << err.message << ...)` therefore calls `strlen(err.message)`
on **every** loop iteration, whether or not any assertion fails. On the success
path `heif_error.message == nullptr`, so `strlen(NULL)` segfaults.

The failing case expects 200 successful frames (`start_mock_decoder(24)`,
latency 24 > repeated segment length 1); every successful decode returns
`message == nullptr`, so the first iteration crashes.

### Fix

Guard the null message at both `INFO` sites, matching the convention already
used in the suite (e.g. `tests/tmap_write.cc`: `INFO((error.message ? error.message : ""))`):

```cpp
INFO("error: " << (err.message ? err.message : "(null)") << ", subcode: " ...);
```

### Necessity / minimality analysis (second pass)

- **Line 390** (failing case): **necessary** — this is the observed crash; the
  success path always yields `message == nullptr`.
- **Line 300** (sibling case "repeated samples without decoder output..."):
  **defensive hardening of the identical pattern**, not strictly required today.
  That case expects `heif_suberror_Security_limit_exceeded`, whose error is built
  by `sequence_decode_work_limit_error()` (`libheif/sequences/track_visual.cc:58`)
  with a non-null message, so it does not currently crash. It is guarded to remove
  the same unsafe idiom; a maintainer preferring the absolute minimum may keep
  only the line-390 change.

No library code is changed; the decode path is correct.

## Reproduction evidence (pristine upstream, independent second pass)

Environment: macOS 27.3 arm64, Apple clang 21.0.0, CMake 4.4.5, Ninja 1.13.2.
Upstream worktree detached at `12a259a5` (no fork changes). Built with the
`testing` preset; `CMAKE_COMPILE_WARNING_AS_ERROR=OFF` was required only because
upstream `grid.cc:247` triggers a `-Wunused-result` error under clang 21 (an
unrelated toolchain issue) — this does not affect the runtime bug or sanitizers.

| Step | Command | Result |
| --- | --- | --- |
| Fail-before | `./build-up/tests/sequence_no_decoder_output "decoder latency may exceed*"` | SIGSEGV; `test cases: 1 \| 1 failed`, `assertions: 5 \| 4 passed \| 1 failed` |
| Crash stack (ASan) | `_platform_strlen` ← `std::ostream<<(char const*)` ← `Catch::MessageBuilder::operator<<` ← `sequence_no_decoder_output.cc:390` | `SEGV on unknown address 0x0` (READ) |
| Pass-after | apply 2-line patch, rebuild, run full binary | `All tests passed (1483 assertions in 8 test cases)` |
| ASan/UBSan | `-fsanitize=address,undefined`, `ASAN_OPTIONS=detect_leaks=0` | `All tests passed (1483 assertions in 8 test cases)`, no sanitizer report |

Library-correctness isolation: guarding **only** the INFO (no other change) makes
the case pass all 404 assertions — all 200 frames decode successfully. This proves
the defect is the test diagnostic, not the sequence decoder.

## Scope guard

This patch contains **only** `tests/sequence_no_decoder_output.cc` (2 lines). No
GB/T 48347 feature code, no eval artifacts, no other files. It is independent of
the evaluation branch's GB/T work.