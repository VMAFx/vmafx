<!-- markdownlint-disable MD060 -->
# Upstream watchers

The fork depends on a handful of features that are not yet present
in the upstream projects we sit on top of (FFmpeg, Netflix/vmaf,
ONNX Runtime, …). Rather than block work on these features
indefinitely or build a bespoke fork-side replacement, we ship
**placeholder adapters** that register today but stay inactive
until the upstream feature lands, paired with an **upstream watcher**
that polls the relevant upstream tree on a weekly cron and opens a
GitHub tracking issue the moment the feature appears.

This document is the operator-facing reference for the watcher
pattern: what we currently watch, how the polling works, and how to
add a new watcher.

## Currently watched

| Watcher | Upstream | Sentinel | Placeholder | ADR |
|---|---|---|---|---|
| `check_ffmpeg_av1_videotoolbox.sh` | `git.ffmpeg.org/ffmpeg.git` `master` | `AV_CODEC_ID_AV1` in `libavcodec/videotoolboxenc.c` | `tools/vmaf-tune/src/vmaftune/codec_adapters/av1_videotoolbox.py` | [ADR-0339](../adr/0339-av1-videotoolbox-placeholder-adapter.md) |

## How the pattern works

A watcher has three moving parts:

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
   Weekly cron (Mondays 08:00 UTC) that invokes every detection
   script and opens a GitHub tracking issue (de-duplicated by
   exact title) when one returns "feature present". The
   tracking issue carries the activation checklist.

The upstream-blocked feature thus has three states a maintainer
can observe:

- **Inactive**: upstream has not landed the feature; the
  placeholder adapter raises `*UnavailableError`; no tracking
  issue is open.
- **Detected**: the watcher has fired; a tracking issue is open
  with `upstream-blocked` label and the activation checklist;
  the placeholder adapter still raises `*UnavailableError` until
  a fork sync pulls the upstream change.
- **Active**: the fork has synced the upstream change; the
  runtime probe inside the placeholder returns `True`; the
  adapter starts emitting argv normally; the activation PR
  closes the tracking issue and updates the ADR status to
  Superseded.

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

3. **Add a watcher job to `.github/workflows/upstream-watcher.yml`.**
   One job per watcher; each opens its own tracking issue with a
   unique title. The dedup-on-title check must be exact-match.

4. **Write the ADR.** New `docs/adr/NNNN-*.md` covering: what
   upstream feature, what placeholder adapter, what runtime
   probe, what argv shape (or other behaviour) the placeholder
   commits to today vs. defers to the activation PR. Cite
   ADR-0339 as the pattern source.

5. **Update the table at the top of this file.**

## Failure modes the watcher tolerates

- Network failure during `git ls-remote` or partial clone
  (exit 2 from the script): the workflow logs a `::warning::`
  and continues without opening a tracking issue. A subsequent
  weekly run will retry.
- Sentinel false-positive (upstream adds the sentinel string in
  a comment, doc, or an unrelated context): the tracking issue
  opens, a maintainer triages, the script's sentinel is tightened
  in a follow-up PR. Cost is one false-positive issue, not a
  false-negative silent miss.
- Sentinel false-negative (upstream lands the feature using a
  different identifier): the activation PR notices when a
  developer manually checks before the watcher does. The
  tracking ADR's "Activation checklist" item that asks "verify
  the watcher fired" exists exactly for this case — close the
  loop by updating the sentinel in the activation PR.
