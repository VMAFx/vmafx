<!-- markdownlint-disable MD013 MD060 -->
# ADR-0270: libFuzzer scaffold for parser surfaces (OSSF Scorecard remediation)

- **Status**: Accepted
- **Date**: 2026-05-03
- **Deciders**: lusoris, Claude
- **Tags**: security, build, ci, docs

## Context

The fork's [OSSF Scorecard](https://securityscorecards.dev/) report
flags `Fuzzing` at 0/10. Scorecard expects at least one fuzz target
in the repository — registered with OSS-Fuzz, or a standalone
[libFuzzer](https://llvm.org/docs/LibFuzzer.html) /
[AFL++](https://github.com/AFLplusplus/AFLplusplus) harness committed
to the tree and exercised by CI. PR #337 enumerated five active
remediation items for the supply-chain surface; this ADR addresses
one of them.

The libvmaf attack surface that fuzzes well is parser-shaped: it
takes attacker-controlled bytes, performs `sscanf` / `memcpy` /
size-derived `malloc`, and emits a structured object. The
vendored Daala Y4M parser (`core/tools/y4m_input.c`) is the
closest match in-tree — it ingests headers from disk, drives
chroma-conversion callbacks selected by the parsed format string,
and was originally written for offline transcoding rather than
hostile inputs. Code quality varies (the file ships under
`// NOLINTBEGIN(bugprone-unchecked-string-to-number-conversion,
cert-err34-c)` precisely because the upstream sscanf returns are
not checked at the granularity SEI CERT requires).

This is the right wedge target for the first harness because
(1) it has no GPU / DNN dependency so the build is hermetic,
(2) the input contract is well-bounded (text header + binary
frame body), and (3) any heap or arithmetic crash there is a
real CVE-class bug — `vmaf` ships as a CLI that callers point
at filesystem paths.

## Decision

We will land an opt-in libFuzzer scaffold under
`core/test/fuzz/`, gated by a new `-Dfuzz=true` Meson option,
with one initial harness (`fuzz_y4m_input`) wrapping the public
`video_input_open` / `video_input_fetch_frame` /
`video_input_close` surface. The scaffold ships with a small
hand-crafted seed corpus, a `README.md` operator runbook, and a
nightly GitHub Actions workflow (`.github/workflows/fuzz.yml`)
that runs the harness for 5 minutes per night and uploads any
`crash-*` / `oom-*` / `timeout-*` artefacts. New harnesses follow
the same pattern; the README documents the steps.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **Standalone libFuzzer in-tree (chosen)** | Zero external dependency; clang-only; CI runtime fully under our control; works on every host with clang. | We carry the seed corpus and CI minutes ourselves. | Picks the lowest-friction path that satisfies the Scorecard `Fuzzing` check immediately; we can graduate to OSS-Fuzz later without dropping this harness. |
| OSS-Fuzz integration (Google-hosted) | 24/7 fuzzing on Google's infrastructure; bug bounty visibility; CIFuzz integration. | Onboarding requires a project review by the OSS-Fuzz team and a `project.yaml` upstream PR — multi-week lead time before a single Scorecard-visible fuzz target lands. | Rejected as the *first* step. Worth revisiting once the in-tree scaffold has caught a few bugs and we have a clean baseline to onboard. |
| AFL++ standalone | Generally finds bugs faster than libFuzzer on parser-shaped inputs; deterministic forkserver. | Heavier toolchain (afl-clang-fast, persistent-mode shims); the scaffold idiom is less widely understood by drive-by contributors. | Rejected for now; nothing in this ADR prevents an `AFL_INSTRUMENT=1` companion build later. |
| Driver-only fuzzing of `feature_psnr_y` | Smaller blast radius; bit-exactness gate is well-studied. | Harness has to fabricate a valid `VmafPicture` shaped state; any crash there is more likely to be a fuzzer-harness bug than a libvmaf bug. The reachable attack surface from the public CLI does not start there. | The Y4M parser is the actual attacker-reachable surface; psnr_y is downstream of a successful parse. |
| Defer until OSS-Fuzz onboards | Frees up engineering time. | Scorecard stays at 0/10 indefinitely; the reachable Y4M-parser bug remains undetected. | Rejected — the scaffold is a few hundred lines of code and already paid for itself by surfacing the 411-chroma OOB write below. |

## Consequences

- **Positive**:
  - OSSF Scorecard `Fuzzing` check moves from 0/10 toward the
    "≥ 1 fuzz target present" tier the moment this PR merges.
  - The 60-second smoke run on the seed corpus already
    surfaced a heap-buffer-overflow in
    `y4m_convert_411_422jpeg` (`core/tools/y4m_input.c:507`)
    when `c_w == 1` and the destination chroma width
    `dst_c_w == 1` — the first sub-loop unconditionally writes
    `_dst[1]` without the `(x << 1 | 1) < dst_c_w` guard the
    third sub-loop carries. Tracked in `docs/state.md` as a new
    Open bug; reproducer parked under
    `core/test/fuzz/y4m_input_known_crashes/`. Per the
    project workflow, the fix lands as a follow-up PR; this PR
    only ships the harness + the bug report.
  - Future parser bugs (Y4M extensions, future raw-YUV format
    additions) are caught locally before they reach a release.
- **Negative**:
  - One additional opt-in build configuration to keep alive
    (`-Dfuzz=true`).
  - Nightly CI minutes (~5 min × `n_targets`) — currently 5 min
    total.
  - Requires clang on the fuzz-build host; gcc-only environments
    (e.g. some Cray HPC images) cannot drive `-Dfuzz=true`. The
    Meson option errors cleanly when the compiler is wrong.
- **Neutral / follow-ups**:
  - Bug-fix PR for the 411-chroma OOB write (separate ADR; will
    cite this one).
  - Add a `fuzz_<feature>` harness for the next attacker-reachable
    surface (likely the JSON model loader,
    `core/src/read_json_model.c`) once the Y4M fix lands.
  - Once the harness has soaked for ~1 month with no spurious
    failures, file an OSS-Fuzz onboarding PR — keep the in-tree
    scaffold as the local reproducibility surface.

## References

- [OSSF Scorecard `Fuzzing` check definition](https://github.com/ossf/scorecard/blob/main/docs/checks.md#fuzzing).
- [libFuzzer documentation (LLVM)](https://llvm.org/docs/LibFuzzer.html).
- [SEI CERT C, FIO45-C: Avoid TOCTOU race conditions while accessing files](https://wiki.sei.cmu.edu/confluence/display/c/FIO45-C.+Avoid+TOCTOU+race+conditions+while+accessing+files) — the Y4M parser predates the rule and the harness surfaces it where it matters.
- ADR-0108 (deep-dive deliverables); ADR-0141 (touched-file lint cleanup);
  ADR-0165 (state.md bug tracking).
- PR #337 — OSSF Scorecard remediation tracker (Fuzzing was the open
  remediation item this ADR addresses).
- Source: `req` — user direction in 2026-05-03 task brief
  ("Stand up a minimal libFuzzer harness for ONE feature extractor
  (or the YUV-input parsing path) to address the OSSF Scorecard
  Fuzzing check").

### Status update 2026-05-08: Accepted

Audited as part of the 2026-05-08 ADR `Proposed` sweep
([Research-0086](../research/0086-adr-proposed-status-sweep-2026-05-08.md)).

Acceptance criteria verified in tree at HEAD `0a8b539e`:

- `core/test/fuzz/fuzz_y4m_input.c` — present (initial harness).
- `core/test/fuzz/fuzz_yuv_input.c` /
  `fuzz_cli_parse.c` — present (added by ADR-0311 follow-up).
- Seed corpora (`y4m_input_corpus/`,
  `y4m_input_known_crashes/`, `yuv_input_corpus/`,
  `cli_parse_corpus/`) — present.
- `core/meson_options.txt` declares `option('fuzz', ...)`.
- `.github/workflows/fuzz.yml` — present (nightly 5-min run per
  harness with crash artefact upload).
- Operator runbook `docs/development/fuzzing.md` exists.
- Verification command:
  `ls core/test/fuzz/ .github/workflows/fuzz.yml;
  grep "option('fuzz'" core/meson_options.txt`.
