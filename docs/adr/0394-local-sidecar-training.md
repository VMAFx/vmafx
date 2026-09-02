<!-- markdownlint-disable MD060 -->
# ADR-0394: Local sidecar training — on-host bias-correction model

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: Lusoris, Claude
- **Tags**: ai, vmaf-tune, sidecar, online-learning, privacy, fork-local

## Context

The fork ships a per-shot VMAF predictor
([`tools/vmaf-tune/src/vmaftune/predictor.py`](../../tools/vmaf-tune/src/vmaftune/predictor.py))
that estimates VMAF from cheap signals (probe-encode bitrate, frame
sizes, optional saliency, signalstats) without running VMAF on every
shot. The predictor is trained offline against a fixed corpus
(currently the Phase-A canonical-6 + BVI-DVC; ADR-0309, ADR-0310).

The user's "ChatGPT-vision" punch list (item 3, classified as
*partially scaffolded* in
[Research-0087](../research/0087-chatgpt-vision-and-claude-bias-audit.md))
asks for a complementary capability: **let an operator continuously
improve the predictor on their own content during real encodes**, so
that the prediction adapts to the operator's actual source mix
(anime vs live action, grain structure, encoder behaviour, bitrate
efficiency patterns) without waiting for the next shipped model
upgrade.

The fork already records the data substrate per encode
([`tools/vmaf-tune/src/vmaftune/cache.py`](../../tools/vmaf-tune/src/vmaftune/cache.py),
[`ai/scripts/extract_full_features.py`](../../ai/scripts/extract_full_features.py)).
What is missing is the adaptive loop: (1) capture
`(features, codec, crf, real_vmaf)` from the operator's own libvmaf
score after the encode; (2) feed it into a small online learner that
adjusts a sidecar-only correction term; (3) at inference time add the
correction to the shipped predictor's output. None of this should
mutate the shipped predictor — model upgrades stay deterministic and
reproducible across hosts.

There is no ADR for online / continual / sidecar fine-tune yet
(closest tangentially-relevant: ADR-0207 QAT, ADR-0287 corpus
expansion). This ADR opens that surface as a *scaffold*: contracts +
data layout + minimal online-ridge implementation. Production
training quality, drift detection thresholds, and the opt-in upload
to a community pool (item 4 of the ChatGPT-vision text) are explicit
follow-ups.

## Decision

We will ship a sidecar module at
`tools/vmaf-tune/src/vmaftune/sidecar.py` that adds a **bias-correction
term** to the shipped predictor's output at inference time. The
correction is fit by **online ridge regression** on
`(captured features, observed_vmaf − predicted_vmaf)` residuals; the
shipped predictor stays read-only and untouched. The sidecar persists
in `~/.cache/vmaf-tune/sidecar/<predictor-version>/<codec>/state.json`
keyed by an anonymised, random-per-install host UUID. The default
posture is **local-only**; the opt-in upload to a community pool is
out of scope for this PR and tracked under "Future work" below.

A bumped predictor version (sidecar's recorded
`predictor_version != Predictor.version`) invalidates the sidecar to
zero, falling back to a clean cold-start. Cold-start
(no captures yet) returns zero correction, so `SidecarPredictor`
degenerates exactly to the shipped predictor.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Sidecar bias-correction on top of read-only predictor (chosen) | Predictor stays deterministic and shippable. Sidecar is local-only by default — no privacy surface to defend in this PR. Online ridge fits in <100 LOC, no new ML dep. Cold-start is trivially safe (zero correction → pass-through). Sidecar invalidates cleanly on predictor version bump. | Linear correction can't model curvature the shipped MLP missed. Fixed-dim feature vector means changing the predictor's inputs requires also bumping `SIDECAR_SCHEMA_VERSION`. | — |
| Replace shipped predictor entirely with online-trained model | Maximises personalisation; one model, not two. | Predictor scores become non-reproducible across hosts — every CI run, every collaborator, every fresh install diverges. Loses the offline-training corpus's coverage of codecs the operator has never run. The whole `vmaf-tune` cache layer assumes deterministic predictions. | Reproducibility is a load-bearing project invariant (Netflix-golden-style determinism on the predictor surface). Disqualifies the option. |
| Server-side training of personalised models | Heaviest per-feature lift; centralises drift detection. | Requires a server; conflicts with the fork's local-only data posture (`docs/ai/training-data.md`); adds operational cost the project doesn't carry. The user explicitly asked for an *on-host* loop in the ChatGPT-vision text. | Out of scope for an open-source fork. |
| Federated learning (FedAvg / FedProx) on community uploads | Privacy-preserving aggregation; community-of-the-future story. | Heavy implementation (secure aggregation protocol, client/server, scheduling); tiny payoff at the fork's current adoption. The ChatGPT-vision text frames community uploads as a *separate* item (item 4, "opt-in upload"). | Defer. Local-only sidecar is the prerequisite; community pooling is a separate ADR / PR once the local surface is stable. |
| Gradient-boosted residual trees (XGBoost / LightGBM) | More expressive than linear; well-understood drift behaviour. | New dependency; pickle-format trees have well-known supply-chain footguns when shared (which we're not doing here, but the contract should still permit a future opt-in upload without changing the on-disk format). | Overshoots scaffold scope. Re-evaluate once the linear baseline shows residual structure beyond what ridge can absorb. |
| SGD-trained tiny MLP (PyTorch) | Smooth path to non-linearities; reuses the `ai/` toolchain. | torch is already in `ai/pyproject.toml` but **not** in `tools/vmaf-tune/pyproject.toml`. Adds a heavy import to the harness's hot path. SGD over a streaming corpus needs careful tuning (LR schedule, replay buffer) — premature for a scaffold PR. | Defer behind the linear baseline. The scaffold's `SidecarModel` interface is small enough to swap implementations under it later. |

## Consequences

- **Positive**:
  - Operators can adapt the shipped predictor to their own content
    distribution without waiting for the next model upgrade.
  - The shipped predictor stays fully deterministic and reproducible
    — sidecar effects are scoped to the host that trained them.
  - Sidecar invalidation is automatic on predictor version bumps;
    operators don't have to remember to `rm -rf ~/.cache/vmaf-tune/`.
  - Privacy posture is the safe default: nothing leaves the host.
- **Negative**:
  - Adds a per-host state-management surface that has to be tested
    on every release (sidecar load / save round-trip; predictor
    version change semantics; concurrent-encode write contention).
  - The sidecar's correction can mask the shipped predictor's
    gradual degradation on out-of-distribution content — operators
    may not notice the predictor is genuinely stale because the
    sidecar absorbs the residual. Drift-detection follow-up (see
    [Research-0086 §Drift](../research/0086-local-sidecar-feasibility.md))
    is the mitigation.
  - On-disk JSON state ties the sidecar's portability to host
    environment (cache_dir defaults follow `XDG_CACHE_HOME`, which
    differs Linux vs macOS vs Windows). Cross-host migration is
    explicitly **not** supported; on a new host the operator
    starts fresh.
- **Neutral / follow-ups**:
  - **Privacy posture**. Sidecar persists `(features, residual,
    timestamp)` rows under `~/.cache/vmaf-tune/sidecar/<predictor-version>/<codec>/state.json`.
    The host UUID is a random 128-bit token generated at first
    install and persisted under the cache dir; **never** derived
    from MAC, hostname, machine-id, or any machine-identifying
    info. Anonymisation is a load-bearing precondition for the
    opt-in upload follow-up.
  - **Retraining cadence**. The scaffold updates the model
    incrementally per capture (online ridge has a closed-form
    rank-1 update). A future PR can switch to per-session or
    scheduled retraining once we measure the per-capture cost on
    real workloads.
  - **Interaction with model upgrades**. When the shipped
    predictor's version string changes, the sidecar's recorded
    `predictor_version` no longer matches and `SidecarModel.load`
    discards the old state, falling back to cold-start. This is
    documented in
    [`docs/ai/local-sidecar-training.md`](../ai/local-sidecar-training.md)
    so operators understand why a model upgrade resets their
    personalised correction.
  - **Drift detection**. The sidecar tracks the rolling residual
    norm; a follow-up PR can surface a warning when the norm
    crosses a threshold (proxy for "the shipped predictor has
    drifted on your content; consider running the offline retrain
    with your corpus shard").
  - **Tests**. Five contract tests under
    `tools/vmaf-tune/tests/test_sidecar.py` (cold-start
    pass-through, update-then-predict residual reduction, save /
    load round-trip, anonymised UUID stability, predictor-version
    invalidation). All run CPU-only, sub-second.

### Future work

- Opt-in upload of anonymised captures to a community pool
  (item 4 of the ChatGPT-vision text). Separate ADR; requires a
  signing surface, an upload protocol, and explicit consent UX.
- Drift detection + operator-facing warning when the rolling
  residual norm exceeds threshold. Separate PR.
- Gradient-boosted residual trees as an alternative to online
  ridge once we have empirical residual data showing the linear
  model is insufficient. Separate PR.

## References

- [Research-0086 — local sidecar feasibility](../research/0086-local-sidecar-feasibility.md)
  — the digest this ADR cites for online-learning algorithm
  choice, privacy surface, cold-start posture, and drift detection.
- [Research-0087 — ChatGPT-vision + Claude-bias audit](../research/0087-chatgpt-vision-and-claude-bias-audit.md)
  — Section 1 item 3 classifies on-host continual learning as
  *partially scaffolded* (data substrate present, adaptive loop
  absent). This ADR opens that surface.
- [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) —
  the offline retrain workflow the sidecar complements (offline
  retrains the shipped predictor; sidecar adapts on-host between
  retrains).
- [ADR-0324](0324-ensemble-training-kit.md) — portable retrain
  kit for collaborators; the sidecar is the *on-host* analogue
  (no collaborator handoff, no tarball).
- [`tools/vmaf-tune/src/vmaftune/predictor.py`](../../tools/vmaf-tune/src/vmaftune/predictor.py)
  — the Predictor surface the sidecar wraps.
- [`docs/ai/local-sidecar-training.md`](../ai/local-sidecar-training.md)
  — operator-facing usage doc.
- Source: `req` — direct user request to scaffold "local sidecar
  training" from the ChatGPT-vision item 3, with explicit scope
  exclusion of the community-pool upload (item 4).
