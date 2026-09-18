# Pre-Migration Epic: VMAFx/vmafx

- **Target Framework**: `github.com/golusoris/golusoris v0.8.0`
- **Current Readiness Score**: `43.4%`
- **Third-Party Dependencies**: `76` total (33 covered, 43 gaps)

## Pre-Migration Tasks

- [ ] **Task 1**: [TASK 1/5] Invariant & Complexity Hygiene: VMAFx/vmafx
- [ ] **Task 2**: [TASK 2/5] Decoupling & Config Externalization: VMAFx/vmafx
  - *Prerequisites*: Depends-On: VMAFx/vmafx#1
- [ ] **Task 3**: [TASK 3/5] Framework Dependency Substitution: VMAFx/vmafx
  - *Prerequisites*: Depends-On: VMAFx/vmafx#2
- [ ] **Task 4**: [TASK 4/5] Gated Verification & Ed25519 Receipt: VMAFx/vmafx
  - *Prerequisites*: Depends-On: VMAFx/vmafx#3
- [ ] **Task 5**: [TASK 5/5] Full Praetor Activation & Governance Lockdown:
  VMAFx/vmafx
  - *Prerequisites*: Depends-On: VMAFx/vmafx#4

## Execution Directives

1. All changes must pass `make verify-all` with zero warnings.
2. Direct commits to `main` are prohibited; changes must traverse `standardsctl
   gate run`.

---

## Decomposed Sub-Issue Definitions

### Issue 1: [TASK 1/5] Invariant & Complexity Hygiene: VMAFx/vmafx

**Labels**: `task, hiss, hygiene`

#### Scope (task 1)

- Enforce NASA JPL Rule 4: refactor all functions to <= 60 LOC.
- Eliminate unhandled panics, unwrap(), and raw fatal exits.
- Add 3D unit tests (positive, negative, boundary) with race detector.

### Issue 2: [TASK 2/5] Decoupling & Config Externalization: VMAFx/vmafx

**Labels**: `task, architecture, decoupling`
**Depends-On**: `VMAFx/vmafx#1`

#### Scope (task 2)

- Eliminate in-cluster DNS and hardcoded localhost URLs.
- Externalize secrets and tokens behind environment variables / HashiCorp Vault.
- Decouple monorepo circular import dependencies.

### Issue 3: [TASK 3/5] Framework Dependency Substitution: VMAFx/vmafx

**Labels**: `task, dependencies, migration`
**Depends-On**: `VMAFx/vmafx#2`

#### Scope (task 3)

- Swap 213 external dependencies for github.com/golusoris/golusoris v0.8.0
  builder kits.
- Apply verified import substitutions.
- Reconcile .needs.yaml capability declarations.

### Issue 4: [TASK 4/5] Gated Verification & Ed25519 Receipt: VMAFx/vmafx

**Labels**: `task, verification, gating`
**Depends-On**: `VMAFx/vmafx#3`

#### Scope (task 4)

- Run `standardsctl gate run --target=.` in isolated worktree.
- Verify all 5 gates (prefetch, SCA, HISS-16, tests, receipts).
- Sign Ed25519 Exit-0 receipt and submit fast-forward PR.

### Issue 5: [TASK 5/5] Full Praetor Activation & Governance Lockdown: VMAFx/vmafx

**Labels**: `task, activation, governance`
**Depends-On**: `VMAFx/vmafx#4`

#### Scope (task 5)

- Reconcile and lock branch protection rulesets via `standardsctl sync`.
- Transition .standards.yaml enforcement level to `strict-zero-debt`.
- Synthesize Paperclip agent harness (`standardsctl paperclip harness`).
- Configure ARC/fleet runner routing policy and enroll into bot gating webhook.
