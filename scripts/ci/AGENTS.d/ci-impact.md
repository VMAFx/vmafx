---
paths:
  - scripts/ci/plan-ci-impact.py
  - .github/ci-impact.json
  - scripts/ci/tests/test_ci_*.py
  - .github/ci-tier.json
  - .github/workflows/ci-*.yml
  - scripts/ci/ci_*.py
invariant: Planner fails closed to `mode=full`; planner/work/gate; one tier file.
area: gates
---
<!-- markdownlint-disable MD013 MD060 -->
# CI impact planner (ADR-1140)

- `plan-ci-impact.py` + `.github/ci-impact.json` decide which surfaces change
  touches; every required job runs planner first, gates heavy steps on
  selectors. **Fail-closed**: unknown top-level paths, non-additive
  statuses (delete/rename/copy), CI-authority files (this directory included),
  missing merge-base, non-linear pushes and over-large diffs all yield
  `mode=full`.
- `tests/test_ci_impact.py` (stdlib `unittest`) pins map ↔ tree contract and
  no-path-filter invariant on required-context workflows. Run after
  adding top-level directory or required check.
- **Required contexts use planner -> work -> gate, never trigger filters
  (BUG-098).** workflow always starts. unconditional `impact` job exports
  one selector; distinctly named heavy `... work` jobs consume selector; and
  `if: always()` gate alone owns each exact required context name. gate
  accepts only `true:success` or `false:skipped`, fails when planning fails.
  Keep `(?m)` multiline anchor in no-path-filter regression: omitting anchor
  makes assertion inspect only beginning of YAML and silently miss
  every nested `paths:` key. GitHub creates no gate check until gate's
  `needs` chain completes, so required aggregator keeps polling while
  mapped planner/work proxy active and briefly after completion. Preserve
  complete `delayedStrictDependencies` map and paginated check-run fetch;
  `test_hiss_replay_contract.py` executes both failure modes.
- **Selectors `tester_image` and `windows_tester_zip` = former trigger
  path lists of `docker-publish-tester.yml` and `windows-tester-bundle.yml`
  (ADR-1687).** Change selectors together with inputs those workflows build;
  `tests/test_required_release_legs.py` pins lists. Both workflows =
  planner consumers and therefore in `full_patterns`.
- **`own_paths_only` = one exception to fail-closed (ADR-1700).** selector
  declaring flag = true in full plan only when known changed path matches selector's
  own patterns; with no change list (dispatch, schedule, unreadable diff) selector stays
  true, which keeps publish dispatch building. `load_config()` refuses
  flag on selector with `inherits` or no patterns, and `test_ci_impact.py`
  (`OwnPathsOnlyContract`) fails when selector outside `OWN_PATHS_ONLY` declares flag or when
  property stops working. Never widen by adding code paths; declare flag.
- **Inheritance resolved without recursion (HISS-01).** `inheritance_order()`
  sorts selectors topologically (and raises on cycle) and
  `impact_selectors()` resolves selectors in that order in one pass.

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
  keeps waiting (jobs behind `needs: tier`: no check run before tier completes). Never add `labeled` to aggregator
  types: skipped aggregator run on same SHA reads as pass.
- Only `pr-label-lanes.yml` listens for `labeled`; its `escalate` job (`if` = two labels) calls
  `ci-escalate.yml` (`workflow_call`, ADR-3314). Flow: cancel, await
  (bounded), re-run each latest PR run. Re-run reads live labels (`GH_TOKEN`, API): payload
  labels of re-run stale. Never put `labeled` in another workflow. Cancel answering 409 =
  run already completed = no error (ADR-3279). Run not re-run: printed with reason, later
  runs still started (`rerun_all()`), step exit 1 with count. Never let one refused run
  stop loop; `test_ci_escalate.py` (`RerunAll`) fails on that.
- Light-tier job: `needs: tier`, `if: needs.tier.outputs.light == 'true'`. Full: `.full`. Gate of
  planner workflow: `needs: [tier, impact, <work>]`, `if: always() && needs.tier.outputs.<t> == 'true'`
  Pinned by `test_ci_impact.py`, `test_required_release_legs.py`, `test_rust_ci_workflow_contract.py`.
  `libvmaf-build-matrix.yml` legs: matrix key `tier` = `light`/`full` per `full_only`; step
  `leg` skips work; checkout stays unconditional (checkout-ordering gate), shallow when skipped.
- Untiered job or non-master push trigger = entry in `ci-tier.json` with reason and expiry; test
  fails after expiry. `praetor-api.yml`, `praetor-docs.yml` byte-locked by `praetorctl audit`:
  regenerate with engine, never edit here. Push only on `master` (praetor#815), so
  `push_branch_exceptions` empty. Draft: first step fails closed, later steps skipped; job still
  starts, so both stay in `untiered_jobs`. `test_praetor_managed_jobs_stop_on_a_draft_before_any_work`
  pins step shape. `reuse.yml` (ADR-2784): praetor-written, not locked; edited only to pin
  actions to commits (praetor#899). Same draft shape, so `untiered_jobs` + `PRAETOR_MANAGED`;
  `REUSE lint` in `always`.
- `gha_expressions.py` evaluates `if:` per Actions docs (null == false, case-insensitive strings,
  `&&`/`||` return operands); unsupported syntax raises. `workflow_router.py` simulates routing
  only, not steps. `CI_ROUTING_WORKFLOWS_DIR` points contract at another tree (proof vs master).
- ci-tier.json = CI-authority file: planner plans `mode=full` on change.
- **Own-input lanes (ADR-2198).** `ci-tier.json` `own_input_lanes` names full-only
  context whose lane still plans and gates in light tier (`needs.tier.outputs.light`).
  Today: `Windows Tester Zip`. Selector `windows_tester_zip_sycl` stays superset of
  `windows_tester_zip` (gate reads either). `test_ci_routing_contract.py` plants old full-tier
  gate as defect. Never move lane's `impact` or gate back to `outputs.full`.
- **Second own-input lane: `Windows MSVC+SYCL` (ADR-3279).** Context = matrix leg, no
  gate job. `libvmaf-build-matrix.yml` job `impact` plans on light tier; `windows-gpu-build`
  `if` = not cancelled and (full tier, or `impact` failed, or `impact` selected
  `windows_msvc_sycl`). Failed planner starts leg (fail closed). Rows: SYCL `tier: light`,
  CUDA `tier: full`; step `leg` decides; next step = checkout, unconditional, `fetch-depth: 1`
  (checkout-ordering gate); every step after checkout reads `steps.leg.outputs.run`.
  CUDA row on light tier = notice, green check, no build. New step in job: add that `if`.
  Selector = C, C++, headers, Meson under `core/` plus leg inputs; outside: `.asm`, Python
  helpers, `vmafx.toml`, symbol lists, Rust, `.cu`. `test_ci_routing_contract.py` plants
  ten defects; `test_ci_impact.py` (`MsvcLegRouting`) pins selector. Never mark SYCL row
  `full`, CUDA row `light`, or drop `needs.impact.result == 'failure'`.
- **`build_work` = `c_core` + `python/**`, `compat/**`, build / pytest-timeout / tox
  locks, prune script.** `build.yml` work job (`Linux Intel LLVM`, `macOS Clang+Metal`,
  `Windows MSVC+CUDA (full)`) runs `compat/vmaf/tests/test_decorator_extended.py`
  (Windows leg = only Windows run of it); macOS leg runs `tox -c python`. Not whole
  `python` selector: tools, mcp-server, scripts would start heavy legs. New `-r` lock,
  `tox -c` config or `python -m pytest` target in `build.yml` -> add path;
  `BuildWorkRouting` tests fail otherwise. Never point `build.yml` back at `c_core`:
  compat-only diff then skips its own test.
- **Cut check (ADR-2198).** `scripts/release/candidate-legs.json` lists tester legs
  cut needs green on exact commit. Leg added to Windows matrix, tester image or macOS bundle
  goes in list (`test_check_candidate_legs.py` pins names). Three tester workflows keep
  `run-name` carrying dispatched source: check accepts dispatch only when title holds full SHA.
