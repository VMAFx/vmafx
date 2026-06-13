# Interactive SSH debugging on CI runners (tmate)

The `Builds` workflow includes an SSH debug step on all macOS
matrix legs. When a test step fails and the run was triggered manually via
`workflow_dispatch`, the step opens a tmate session that lets you SSH directly
into the GitHub-hosted macOS runner to run `lldb` on the crashing binary.

This capability was added after three speculative fix PRs (#1355, #1403, #1412)
failed to resolve the macOS SIGSEGV without direct access to the crash state.
See [ADR-0626](../adr/0626-macos-ci-tmate-debug-on-failure.md).

## How to trigger the SSH session

The step is gated on `github.event_name == 'workflow_dispatch'` — it does
**not** fire on regular PR pushes. You must trigger the workflow manually:

```bash
gh workflow run "Builds" \
  --ref feat/your-branch-name
```

Or from the GitHub UI: **Actions → Builds → Run workflow**.

## Finding the tmate URL in the logs

1. Open the Actions run that you triggered.
2. Select the failing macOS job (e.g. `Build — macOS clang (CPU)`).
3. Expand the **SSH debug session on test failure** step.
4. The step prints two lines:

   ```text
   SSH: ssh <session-id>@nyc1.tmate.io
   or: https://tmate.io/t/<session-id>
   ```

   Copy the `ssh` command and run it in your local terminal.

Access is restricted to the SSH public keys of the GitHub account that
triggered the workflow (`limit-access-to-actor: true`). Make sure your account
has at least one SSH public key registered at <https://github.com/settings/keys>.

## What to debug once connected

The source tree is at `/Users/runner/work/vmaf/vmaf`.
Test binaries are under `core/build/test/`.

### Typical session for a SIGSEGV in the test suite

```bash
# Find which test binary triggered the crash
ls /Users/runner/work/vmaf/vmaf/core/build/test/

# Attach lldb to the binary that crashed
lldb /Users/runner/work/vmaf/vmaf/core/build/test/test_output

# Inside lldb:
run
# When the SIGSEGV fires:
bt          # full backtrace
frame info  # current frame details
p <var>     # inspect variables
```

### Useful environment flags

Apple's `MallocScribble` and `MallocGuardEdges` surface heap corruption that
the standard allocator hides:

```bash
MallocScribble=1 MallocGuardEdges=1 \
  lldb /Users/runner/work/vmaf/vmaf/core/build/test/test_output
```

`MALLOC_PERTURB_=198` (the value used to expose the ADR-0606 off-by-one) is
also useful:

```bash
MALLOC_PERTURB_=198 \
  lldb /Users/runner/work/vmaf/vmaf/core/build/test/test_output
```

## Session limits

- The tmate step waits up to **30 minutes** (`connect-timeout-seconds: 1800`)
  for you to connect. If you do not connect within that window, the step exits
  and the job completes normally (with a failure status from the earlier test
  step).
- Once connected, the session remains open until you exit the shell or the
  GitHub-hosted runner's hard timeout fires (6 hours for macOS runners).
- Only the actor who triggered the `workflow_dispatch` can connect
  (`limit-access-to-actor: true`).
- Each session consumes roughly 30 min of macOS runner minutes at the
  GitHub-hosted rate (~$0.08/min for macOS = ~$2.40 per session at the cap).
  Avoid leaving sessions idle.

## When to remove this step

The `workflow_dispatch` gate makes the step a no-op on all regular PR pushes,
so it is safe to leave in place after the SIGSEGV is fixed. It has zero cost
on normal CI runs. Clean it up at your discretion once the macOS crash class is
fully resolved and you no longer need on-runner access.
