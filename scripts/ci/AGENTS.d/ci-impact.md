---
paths:
  - scripts/ci/plan-ci-impact.py
  - .github/ci-impact.json
  - scripts/ci/tests/test_ci_*.py
  - .github/ci-tier.json
  - .github/workflows/ci-*.yml
  - scripts/ci/ci_*.py
invariant: Planner fails closed to `mode=full`; planner/work/gate; one tier file.
---
<!-- markdownlint-disable MD013 MD060 -->
# CI impact planner (ADR-1140)

- `plan-ci-impact.py` + `.github/ci-impact.json` decide which surfaces change
  touches; every required job runs it first, gates heavy steps on
  selectors. **Fail-closed**: unknown top-level paths, non-additive
  statuses (delete/rename/copy), CI-authority files (this directory included),
  missing merge-base, non-linear pushes and over-large diffs all yield
  `mode=full`.
- `tests/test_ci_impact.py` (stdlib `unittest`) pins map ↔ tree contract and
  no-path-filter invariant on required-context workflows. Run it after
  adding top-level directory or required check.
- **Required contexts use planner -> work -> gate, never trigger filters
  (BUG-098).** workflow always starts. unconditional `impact` job exports
  one selector; distinctly named heavy `... work` jobs consume it; and
  `if: always()` gate alone owns each exact required context name. gate may
  accept only `true:success` or `false:skipped` and must fail when planning fails.
  Keep `(?m)` multiline anchor in no-path-filter regression: omitting it
  makes assertion inspect only beginning of YAML and silently miss
  every nested `paths:` key. GitHub does not create gate check until its
  `needs` chain completes, so required aggregator must keep polling while
  mapped planner/work proxy is active and briefly after it completes. Preserve
  complete `delayedStrictDependencies` map and paginated check-run fetch;
  `test_hiss_replay_contract.py` executes both failure modes.
- **Selectors `tester_image` and `windows_tester_zip` are former trigger
  path lists of `docker-publish-tester.yml` and `windows-tester-bundle.yml`
  (ADR-1687).** Change them together with inputs those workflows build;
  `tests/test_required_release_legs.py` pins lists. Both workflows are
  planner consumers and therefore in `full_patterns`.
- **`own_paths_only` is one exception to fail-closed (ADR-1700).** selector
  declaring it is true in full plan only when known changed path matches its
  own patterns; with no change list (dispatch, schedule, unreadable diff) it stays
  true, which is what keeps publish dispatch building. `load_config()` refuses
  it on selector with `inherits` or no patterns, and `test_ci_impact.py`
  (`OwnPathsOnlyContract`) fails when third selector declares it or when
  property stops working. Do not widen it by adding code paths; declare it.
- **Inheritance is resolved without recursion (HISS-01).** `inheritance_order()`
  sorts selectors topologically (and raises on cycle) and
  `impact_selectors()` resolves them in that order in one pass.

- Tier = `ci_tier.py` decision. Fork PR, `ci: full`, master push, dispatch, schedule, release PR
  with `autorelease: cut` = full. Own PR (Renovate incl.) = light. Release PR without cut label
  = release-light (`always` contexts only). Release PR = bot author, or maintainer account with
  release-only diff (`release-pr-exempt.sh`, reused): head ref alone never trusted.
- Required context neither in `always` nor `full_only` = light. New required context: aggregator
  `required`, marker comment, and `full_only` or `always` when not light. Routing contract fails
  when workflows and file disagree.
- Aggregator: `CI_TIER`, `CI_TIER_ALWAYS`, `CI_TIER_FULL_ONLY` from `ci_tier.py` outputs; no tier
  env = full = old behaviour. Not-owed context: absent or skipped OK, ran and failed = fail.
  Failed `Decide the CI tier` check = aggregator failure; queued or running one = aggregator
  keeps waiting (jobs behind `needs: tier` have no check run before it completes). Never add `labeled` to aggregator
  types: skipped aggregator run on same SHA reads as pass.
- Only `ci-escalate.yml` listens for `labeled` (job `if` = two labels). Flow: cancel, await
  (bounded), re-run each latest PR run. Re-run reads live labels (`GH_TOKEN`, API): payload
  labels of re-run stale. Never put `labeled` in another workflow.
- Light-tier job: `needs: tier`, `if: needs.tier.outputs.light == 'true'`. Full: `.full`. Gate of
  planner workflow: `needs: [tier, impact, <work>]`, `if: always() && needs.tier.outputs.<t> == 'true'`
  Pinned by `test_ci_impact.py`, `test_required_release_legs.py`, `test_rust_ci_workflow_contract.py`.
  `libvmaf-build-matrix.yml` legs: matrix key `tier` = `light`/`full` per `full_only`; step
  `leg` skips work; checkout stays unconditional (checkout-ordering gate), shallow when skipped.
- Untiered job or non-master push trigger = entry in `ci-tier.json` with reason and expiry; test
  fails after expiry. `praetor-api.yml`, `praetor-docs.yml` byte-locked by `praetorctl audit`:
  fix upstream, never edit here.
- `gha_expressions.py` evaluates `if:` per Actions docs (null == false, case-insensitive strings,
  `&&`/`||` return operands); unsupported syntax raises. `workflow_router.py` simulates routing
  only, not steps. `CI_ROUTING_WORKFLOWS_DIR` points contract at another tree (proof vs master).
- ci-tier.json = CI-authority file: planner plans `mode=full` on change.
- **Own-input lanes (ADR-2198).** `ci-tier.json` `own_input_lanes` names a full-only
  context whose lane still plans and gates in the light tier (`needs.tier.outputs.light`).
  Today that is `Windows Tester Zip`: selector `windows_tester_zip_sycl` must stay a
  superset of `windows_tester_zip` (the gate reads either), and
  `test_ci_routing_contract.py` plants the old full-tier gate as a defect. Do not move
  the lane's `impact` or gate back to `outputs.full`.
- **Cut check (ADR-2198).** `scripts/release/candidate-legs.json` lists the tester legs
  a cut needs green on the exact commit; a leg added to the Windows matrix, the tester
  image or the macOS bundle goes in the list (`test_check_candidate_legs.py` pins the
  names). The three tester workflows keep a `run-name` that carries the dispatched
  source: the check accepts a dispatch only when the title holds the full SHA.
