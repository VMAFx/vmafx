<!-- markdownlint-disable MD013 MD060 -->
# ADR-3390: The required check `GStreamer Element` runs the element's tests

- **Status**: Accepted
- **Date**: 2026-10-30
- **Deciders**: lusoris
- **Tags**: `ci`, `gstreamer`

## Context

Issue #2474 asks that the GStreamer element's property list equal the API definition `core/api/vmafx.toml` and that a planted extra property fail CI. `gstreamer/test/test_gst_vmafx_properties.py` holds that comparison and `gstreamer/test/test_gst_vmafx_parity.py` compares the element's scores with the CLI, but `gstreamer/test/run.sh`, which runs both, ran in no workflow. The `tooling` suite of `.github/test-suites.json` did collect both files, so `Tooling Tests` ran the table half and skipped every test that needs the element. The `Upstream Consumers` job builds the unpatched upstream plug-in, not this element, and is not a required check.

Both test files skip when a fixture or the `gi` module is absent, so wiring the script in as it stood could report green having compared nothing.

## Decision

We will add the required job `GStreamer Element` to `lint-and-format.yml`, shaped like `Tidy Lane (<lane>)` ([ADR-2796](2796-hosted-tidy-all-lanes.md)): it always starts on the light tier, and the `own_paths_only` selector `gstreamer_element` decides whether it builds. A job that does no work says so in a notice.

When selected it builds the CPU library, fetches the 576x324 and checkerboard pairs, and runs `gstreamer/test/run.sh` with `GST_VMAFX_STRICT=1`, which turns a missing CPU fixture, `gi` module, `glupload` element or test input into a failure. The 4K pair stays optional, as do the CUDA, VA and Vulkan lanes, which need devices the hosted runner lacks; those tests report as skipped by name. `GST_VMAFX_PYTHON` selects the system interpreter, because the distribution's `python3-gi` is built for it and not for the interpreter from `setup-python`. `gstreamer/test/` moves from the `tooling` suite to a suite of its own, because a test runs once in CI ([ADR-1568](1568-tests-run-once-in-ci.md)); the selector covers every input of the table half. `glupload` needs a GL context and the runner has no display, so the job installs Mesa's software driver and selects a surfaceless EGL window. The job generates the `de_DE.UTF-8` locale and fails if it is absent, because the decimal-comma tests pass trivially without it.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| :--- | :--- | :--- | :--- |
| Own job, always starts, own selector (chosen) | Gates a pull request at the light tier; absence means broken registration | One more required context | Picked: a change to the element is caught before it lands |
| Steps in the `Upstream Consumers` job | No new context | Full tier only and not required, so a light-tier pull request skips it and the failure first shows on `master` | The failure [ADR-3338](3338-dev-container-own-input-lane.md) describes for a full-only lane |
| Steps in `Tidy Lane (integrations)` | Dependencies already in the container | Runs in the dev container for a different purpose, and mixes a test result into a lint ratchet | A failure there would read as a tidy finding |

## Consequences

- A pull request touching `gstreamer/`, `core/`, `model/`, `scripts/codegen/`, the generated option table, the pooling helper or the user page builds and tests the element on a hosted runner.
- Reproduce with `GST_VMAFX_STRICT=1 gstreamer/test/run.sh "$PWD/build-cpu"`.
- The 0.2.0 option entries of #2453 land against this check.

## References

- Issue #2474, implementing #2155.
- [ADR-2796](2796-hosted-tidy-all-lanes.md), [ADR-3338](3338-dev-container-own-input-lane.md), [ADR-3379](3379-tidy-integrations-lane.md).
