<!-- markdownlint-disable MD060 -->
# Upstream watchers

Four workflows poll upstream, daily or weekly, and open a tracking issue when a
feature the fork is waiting for lands or when a Netflix/vmaf commit has no
recorded disposition. This
page is the operator-facing reference: what is watched, how the polling works
and how to add a watcher.

The fork depends on a handful of features that are not yet present in the
upstream projects it sits on top of (FFmpeg, Netflix/vmaf, ONNX Runtime, ...).
Rather than block work on them indefinitely or build a bespoke fork-side
replacement, the fork ships **placeholder adapters** that register today but
stay inactive until the upstream feature lands. Each is paired with an
**upstream watcher** that polls the relevant upstream tree on a regular cron and
opens a GitHub tracking issue the moment the feature appears.

## Currently watched

| Workflow | Cadence (UTC) | Upstream signal | Placeholder or deferral | ADR |
|---|---|---|---|---|
| `upstream-watcher.yml` (runs `scripts/upstream-watcher/check_ffmpeg_av1_videotoolbox.sh`) | Daily 08:00 | `AV_CODEC_ID_AV1` in `libavcodec/videotoolboxenc.c` on `git.ffmpeg.org/ffmpeg.git` `master` | `tools/vmaf-tune/src/vmaftune/codec_adapters/av1_videotoolbox.py` | [ADR-0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md) |
| `upstream-watcher.yml` (runs `scripts/upstream-watcher/check_netflix_unported.py`) | Daily 08:00 | Commits on `Netflix/vmaf` `master` after parity pin without a ledger entry | Netflix parity pin ([`known-upstream-bugs.md`](known-upstream-bugs.md)) | [ADR-3354](../adr/3354-netflix-unported-commit-watcher.md) |
| `upstream-ffmpeg-hip-hwdec-watcher.yml` | Sundays 06:30 | `hwcontext_hip.{c,h}` under `libavutil/`, or an `AV_HWDEVICE_TYPE_HIP` / `_ROCM` enum variant in `libavutil/hwcontext.h`, read through the contents API of the FFmpeg mirror | `T-FFMPEG-HIP-FILTER-DEFERRED` row in `docs/state.md` | [ADR-0448](../adr/0448-active-upstream-monitoring-discipline.md) |
| `upstream-netflix-645-hdr-model-watcher.yml` | Sundays 06:15 | Netflix/vmaf#645 activity, or a `vmaf_hdr_*.json` file under `Netflix/vmaf:model/` | the HDR-VMAF model port deferral | [ADR-0448](../adr/0448-active-upstream-monitoring-discipline.md) |
| `upstream-netflix-955-watcher.yml` | Sundays 06:00 | Netflix/vmaf#1494 merging (it closes #955) | the Netflix#955 `i4_adm_cm` deferral row | [ADR-0448](../adr/0448-active-upstream-monitoring-discipline.md) |

The two `upstream-watcher.yml` jobs run detection scripts under `scripts/upstream-watcher/`;
the other three are workflow-only (the poll is inline in the workflow, using
`gh api`).

## Netflix unported commits watcher

The `netflix-unported` job in `.github/workflows/upstream-watcher.yml` polls
`Netflix/vmaf` `master` daily and opens a deduplicated tracking issue for each
commit that has landed upstream after the fork's recorded parity pin without a
recorded disposition.

### What it reads

- **Parity pin**: the `## Upstream head the fork is at parity with` heading
  in [`known-upstream-bugs.md`](known-upstream-bugs.md), parsed by
  `scripts/ci/upstream_parity_pin.py`.
- **Upstream commit history**: `git rev-list --no-merges <pin>..<ref>`
  over `master` of `https://github.com/Netflix/vmaf.git`.
- **Dispositions ledger**: the tracked TSV file
  `scripts/upstream-watcher/netflix-dispositions.tsv`.

### How to dispose a commit

When the watcher files a tracking issue, or a local run reports a commit:

1. **Ported**: port the commit with `/port-upstream-commit` (or adapt it
   by hand) and add a `ported` row in the same pull request:
   `<full-40-hex-sha>`, tab, `ported`, tab, `#<fork-pr-number>`.
2. **In-fork**: the fork already implements the change; add an `in-fork`
   row naming the fork pull request or commit.
3. **Not applicable**: the change does not apply to the fork (another
   architecture, a dependency the fork does not use, a reverted change);
   add a `not-applicable` row with a one-line reason.
4. **Advance the parity head**: once every commit of an unbroken run from
   the pin has a row, move the pin heading in
   [`known-upstream-bugs.md`](known-upstream-bugs.md) forward.

### Exit codes

- `0`: at least one upstream commit has no row.
- `1`: every commit in the range has a row.
- `2`: infrastructure or ledger error (missing upstream ref, invalid ledger
  row, or a pin that is not an ancestor of the upstream ref). The workflow
  job fails on this status, and when an issue search or creation fails.

### Running locally

Run the detection script directly from the repository root:

```bash
python3 scripts/upstream-watcher/check_netflix_unported.py
```

Pass `--json` to output structured JSON data, or `--upstream-ref <ref>` to evaluate
against a specific branch or commit.

## How the pattern works

A script-based watcher has three moving parts:

1. **Detection script** under `scripts/upstream-watcher/`. A small
   shell script that runs `git ls-remote` to grab the upstream tip,
   does a partial clone (`--filter=blob:none` + sparse-checkout) of
   the file we care about, and greps for a sentinel string. Exit
   code 0 = feature present, 1 = feature absent, 2 =
   infrastructure failure (network, missing tools).
2. **Placeholder adapter / consumer** in the fork. Registers in
   whichever registry the surface uses (codec adapters, feature
   extractors, GPU backends, …) but raises a typed
   `*UnavailableError` when called until a runtime probe — usually
   running the upstream tool with a `--help` flag and inspecting
   the output — confirms the feature is reachable on the host.
   This makes the adapter **self-activate** the moment a fork
   sync pulls a recent enough upstream build, with no extra code
   change inside the adapter.
3. **CI watcher workflow** at `.github/workflows/upstream-watcher.yml`.
   Daily cron (08:00 UTC) that invokes every detection
   script and opens a GitHub tracking issue (de-duplicated by
   exact title) when one returns "feature present". The
   tracking issue carries the activation checklist.

The other three watchers skip the script: each is its own workflow file whose
`poll` job queries upstream with `gh api` and opens or updates its tracking
issue directly.

### Feature states

The upstream-blocked feature has three states a maintainer can observe:

| State | Meaning | Placeholder behaviour |
|---|---|---|
| Inactive | Upstream has not landed the feature. No tracking issue is open. | The placeholder adapter raises `*UnavailableError`. |
| Detected | The watcher has fired. A tracking issue is open with the `upstream-blocked` label and the activation checklist. | The placeholder still raises `*UnavailableError` until a fork sync pulls the upstream change. |
| Active | The fork has synced the upstream change. | The runtime probe inside the placeholder returns `True` and the adapter emits argv normally. The activation PR closes the tracking issue and updates the ADR status to Superseded. |

## Why polling, not a sync hook

A sync hook (run inside `/sync-upstream` and friends) would notice
the same change at sync time, but the fork's sync cadence is
manual and bursty — a feature could land upstream and sit
unnoticed for weeks until someone runs a sync. Polling on a fixed
cadence gives a bounded worst-case latency and runs even when no
one is actively syncing. Both can coexist; the polling watcher is
the safety net.

## Adding a new watcher

1. **Pick a sentinel.** It must be a string that is present in
   the upstream tree exactly when the feature is, and absent
   otherwise. For codec encoders the convention is the
   `AV_CODEC_ID_*` reference inside the encoder source file (every
   encoder file matches its codec ID into the encoder struct).
   For feature extractors the convention is the public symbol
   name. Avoid sentinels that match build-flag-gated code paths
   that haven't been compiled yet.

2. **Write the detection script.** Copy
   `scripts/upstream-watcher/check_ffmpeg_av1_videotoolbox.sh` as
   the template. Replace the `REMOTE`, `REF`, `ENCODER_FILE`,
   `SENTINEL`, and the YES/NO note text. Keep the
   `set -euo pipefail` / exit-code contract (0 = found, 1 = not
   yet, 2 = infra fail).

3. **Wire the watcher into a workflow.** Two patterns are live:
   add a job to `.github/workflows/upstream-watcher.yml` that runs
   the script, or, for an upstream signal that `gh api` can read
   directly (an issue, a pull request, a file listing), add a
   separate workflow file as the three newer watchers do (weekly
   cron, `workflow_dispatch`, `issues: write`). Either way each
   watcher opens its own tracking issue with a unique title; the
   dedup-on-title check must be exact-match.

4. **Write the ADR.** New `docs/adr/NNNN-*.md` covering: what
   upstream feature, what placeholder adapter, what runtime
   probe, what argv shape (or other behaviour) the placeholder
   commits to today vs. defers to the activation PR. Cite
   ADR-0339 as the pattern source.

5. **Update the table at the top of this file.**

## Failure modes the watcher tolerates

| Failure | What happens | Cost |
|---|---|---|
| Network failure during `git ls-remote` or partial clone (exit 2 from the script) | The FFmpeg job logs a `::warning::` and continues without opening a tracking issue; the Netflix commit job fails. The next daily run retries. | One missed day. |
| Sentinel false-positive (upstream adds the sentinel string in a comment, doc, or an unrelated context) | The tracking issue opens, a maintainer triages, and the script's sentinel is tightened in a follow-up PR. | One false-positive issue, not a silent miss. |
| Sentinel false-negative (upstream lands the feature using a different identifier) | The activation PR notices when a developer checks manually before the watcher does. The tracking ADR's "Activation checklist" item that asks "verify the watcher fired" exists for this case; close the loop by updating the sentinel in the activation PR. | A delayed detection. |
