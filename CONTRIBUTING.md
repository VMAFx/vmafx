<!-- markdownlint-disable MD007 MD013 MD059 -->
# Contributing to VMAFx

This fork adds SYCL / CUDA / HIP (AMD ROCm, partially live) GPU backends,
AVX2 / AVX-512 / NEON SIMD, full-precision CLI output, tiny-AI integration,
and embedded + standalone MCP servers. Below is the fork-specific
contribution guide. The **Netflix upstream contribution guide** follows
below — still authoritative for algorithmic contributions (new feature
extractors, quality runners, models).

## Quickstart

1. Install dev deps: `./scripts/setup/detect.sh` (auto-dispatches per distro).
2. Install pre-commit hooks: `make hooks-install` (runs `pre-commit install`).
   Required for fresh clones — missing hooks cause CI round-trips on
   trivial formatter failures.
3. Configure a build from the repository root: follow the
   [source-build guide](docs/getting-started/index.md#build-from-source-any-platform)
   (`meson setup build core ...`; the Meson source directory is `core/`).
4. Build: `ninja -C build`
5. Test: `python3 scripts/ci/run_meson_test.py -- -C build`
6. Format + lint before pushing: `make format && make lint`

## Your first contribution

New here? You do not need to understand the whole tree.

1. Pick an issue labelled
   [`good first issue`](https://github.com/VMAFx/vmafx/labels/good%20first%20issue)
   (small, self-contained, with the files named) or
   [`help wanted`](https://github.com/VMAFx/vmafx/labels/help%20wanted)
   (larger, but wanted). Comment on the issue that you are taking it, so two
   people do not do the same work.
2. Follow the [Quickstart](#quickstart) to build and run the tests.
3. Keep the change small and focused: one issue, one pull request.
4. Open the pull request and fill in the template. For a small change the
   [lighter deliverables track](#lighter-track-for-small-pull-requests)
   below applies.
5. Ask in the issue or the pull request if you are stuck; questions are
   welcome there. Open-ended questions go to
   [Discussions](https://github.com/VMAFx/vmafx/discussions).

Contributions that are not code count too: documentation fixes, bug
reproductions, [hardware reports](.github/ISSUE_TEMPLATE/hardware_report.yml)
from machines the project does not own, and
[accessibility reports](.github/ISSUE_TEMPLATE/accessibility.yml).

## Core rules

- **Conventional Commits** — every commit. Enforced by commit-msg hook and
  `conventional-pre-commit`. Allowed types: `feat`, `fix`, `perf`, `refactor`,
  `docs`, `test`, `build`, `ci`, `chore`, `revert`, `port` (upstream cherry-pick),
  `sycl`, `cuda`, `simd`. Use `!` or `BREAKING CHANGE:` for breaking changes.
- **Engineering principles** — see [`docs/principles.md`](docs/principles.md).
  NASA Power-of-10 + JPL-C-STD + SEI CERT C/C++ + MISRA-C:2012 (informative).
  `.clang-tidy` enforces the subset that can be machine-checked.
- **Netflix golden-data gate** — do **not** modify the hardcoded score
  assertions in `python/test/{quality_runner,feature_extractor,vmafexec,
  vmafexec_feature_extractor,result}_test.py`. They're the numerical source
  of truth for VMAF (see `CLAUDE.md` §8 and decision D24). Fork-added tests
  go in separate files.
- **Cross-backend numerical correctness** — any SIMD / GPU change must pass
  `/cross-backend-diff` with `--tolerance-ulp=2`. If you need a larger
  tolerance, open a CODEOWNERS-approved exception.
- **License headers** — every new C/C++/CUDA file starts with a header. Use
  `Copyright 2026 Lusoris` for wholly-new files
  and the Netflix header for files touched in upstream.
- **No `git push --force` to `master`**. No `--no-verify` skipping hooks.
  `master` is merge-via-squash-or-ff-only via branch protection.

## Developer Certificate of Origin

Every commit carries a sign-off: `git commit -s` appends
`Signed-off-by: Your Name <you@example.org>`. It states, under the
[Developer Certificate of Origin 1.1](https://developercertificate.org/), that
you wrote the change or have the right to submit it under the licence of the
files it touches. There is no contributor licence agreement. The required
check **DCO Sign-off** fails a pull request with an unsigned commit; fix a
branch with `git rebase --signoff origin/master`, then
`git push --force-with-lease`. Bot commits (Renovate, release automation) are
exempt. The check applies to pull requests created on or after
2026-10-09 (the cutoff in `scripts/ci/dco-cutoff.txt`); earlier pull requests
are grandfathered. Details, the check command and the exact exemptions:
[DCO sign-off](docs/development/dco.md).

## Branch naming

Use a short, kebab-case prefix that mirrors the Conventional-Commits
type of the work, followed by a slash and a descriptive slug:

- `feat/<slug>` — new user-facing capability.
- `fix/<slug>` — bug fix.
- `perf/<slug>` — performance improvement.
- `refactor/<slug>` — internal reshape with no behavior change.
- `docs/<slug>` — docs-only.
- `chore/<slug>` — tooling / housekeeping.
- `port/<upstream-sha>` — single-commit Netflix port (use
  `/port-upstream-commit`).
- `sync-upstream/<date>` — periodic upstream sync (use
  `/sync-upstream`).

Working branches MUST NOT be force-pushed onto `master`. Once a PR is
opened, force-push to the feature branch is fine — but re-run the
local lint + fast-test gate against the rewritten tip before pushing
(see `CLAUDE.md` §"Test locally before pushing").

## Deep-dive deliverables (ADR-0108)

Every **fork-local** PR ships the six deliverables in
[ADR-0108](docs/adr/0108-deep-dive-deliverables-rule.md):

1. **Research digest** under [`docs/research/`](docs/research/), OR
   "no digest needed: trivial".
2. **Decision matrix** in the accompanying ADR's `## Alternatives
   considered`, OR "no alternatives: only-one-way fix".
3. **`AGENTS.md` invariant note** in the relevant package, OR
   "no rebase-sensitive invariants".
4. **Reproducer / smoke-test command** in the PR description.
5. **`changelog.d/<section>/<topic>.md`** fragment file
   ([ADR-0221](docs/adr/0221-changelog-adr-fragment-pattern.md)).
   Never edit `CHANGELOG.md` directly — it is rendered from the
   fragment tree when pull requests land
   ([ADR-2197](docs/adr/2197-render-generated-docs-at-landing.md)).
6. **Rebase note** as one fragment file
   `docs/rebase-notes.d/<slug>.md`
   (see [`docs/rebase-notes.d/_README.md`](docs/rebase-notes.d/_README.md)),
   OR `no rebase impact: REASON`. Never edit `docs/rebase-notes.md`.

The PR template
([`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md))
carries the checklist. The CI gate parses it; prose bullets or
numbered lists fail the parser. See
[`docs/development/pr-body-sentinel-guide.md`](docs/development/pr-body-sentinel-guide.md)
for the exact sentinel forms.

**Upstream-port PRs are exempt** — pure `/port-upstream-commit` and
`/sync-upstream` PRs skip this gate.

### Lighter track for small pull requests

The six deliverables fit a feature. For a small change they are mostly
"no ... needed" lines. [ADR-2461](docs/adr/2461-small-pr-deliverables-track.md)
gives small pull requests a lighter track. A pull request is small when it
changes at most 100 lines in one subtree, adds no user-discoverable surface,
needs no ADR, and leaves SIMD/GPU twins, numeric results and the Netflix golden
assertions alone. Such a pull request puts `small PR (ADR-2461)` in its
description and waives the research digest, decision matrix, `AGENTS.md` note
and rebase note without a further reason. It still carries a reproducer, a
changelog fragment when users can see the change, and documentation for any
surface it touches. The checking script reads the diff and refuses the marker
when the pull request is larger than 100 lines, spans more than one top-level
directory, touches `core/src/`, `core/include/`, `core/tools/`,
`core/meson_options.txt`, `python/test/` or `ffmpeg-patches/`, or adds an ADR
([details](docs/development/pr-body-sentinel-guide.md#small-pull-requests-adr-2461)).

## Architecture Decision Records

Every non-trivial architectural, policy, or scope decision lands as
an ADR under [`docs/adr/`](docs/adr/) **before** the implementing
commit. Reserve the next number atomically with:

```bash
scripts/adr/next-free.sh --claim <kebab-slug>
```

This writes a `.stub` reservation visible to parallel agents (and
to sibling worktrees via the side-pointer in `.git/adr-claims/`).
Rename the stub to `.md` when committing. See
[ADR-0535](docs/adr/0535-adr-atomic-allocator.md) and
[ADR-0628](docs/adr/0628-adr-allocator-remote-aware.md) for the
allocator semantics.

ADRs follow the Nygard template
([`docs/adr/0000-template.md`](docs/adr/0000-template.md)). Once an
ADR's Status flips to **Accepted**, its body is immutable — a
superseding decision gets a new ADR that links back via
`Supersedes`.

## Governance

The fork follows the BDFL model documented in
[`GOVERNANCE.md`](GOVERNANCE.md). The current maintainer list is in
[`MAINTAINERS.md`](MAINTAINERS.md). PR review routing is in
[`.github/CODEOWNERS`](.github/CODEOWNERS).

## Reporting bugs / requesting features

Use the GitHub issue templates under `.github/ISSUE_TEMPLATE/`. Security
issues go via the coordinated-disclosure flow in [`SECURITY.md`](SECURITY.md) —
not a public issue.

## Review expectations

- Major new functionality must include automated tests in the same change.
  Cover the new behavior and its relevant failure paths, document how to run
  the tests, and preserve existing numerical assertions. Reviewers check this
  policy against the actual change; green CI alone does not prove new coverage.
- CI must be green (ci, lint, security workflows).
- PRs that touch a feature with SIMD / GPU twins must either touch every
  twin or explicitly call out the gap in the PR body so a follow-up can be
  opened.
- Performance-claiming PRs must include before/after numbers produced by
  `/profile-hotpath` or equivalent (no eyeballed claims).

## What to expect

VMAFx is maintained by one person alongside other work, so everything below is
best effort and carries no guaranteed response time.

- **Issues and pull requests** are read and triaged as time allows, and
  availability varies. A short wait, or a longer one, is not a judgement of
  your contribution. If a pull request has had no reply for a while, a polite
  comment on it is fine.
- **Review** checks the points under "Review expectations" above. You may be
  asked to change things, to split a pull request, or told that a change does
  not fit the project; the reason is given.
- **Security reports** are the exception: they follow the timelines in
  [SECURITY.md](SECURITY.md).
- **Conduct reports** follow the [Code of Conduct](CODE_OF_CONDUCT.md).
- **Support questions** are answered when someone can; see
  [SUPPORT.md](SUPPORT.md) for where to ask.

## Upstream sync

We periodically sync from `upstream/master` (Netflix/vmaf) via `/sync-upstream`.
If you're rebasing a branch that spans a sync, prefer `git rebase
--onto master <old-base>` over a merge commit.

---

## Netflix upstream contribution guide

The rest of this file is the upstream Netflix/vmaf contribution guide — still
the authoritative reference for adding new feature extractors, quality
runners, and VMAF models. Fork-specific overrides are the section above.

Please refer to this [slide deck](https://docs.google.com/presentation/d/1Gr4-MvOXu9HUiH4nnqLGWupJYMeh6nl2MNz6Qy9153c/edit#slide=id.p) for an overview contribution guide.

If you would like to contribute code to the VMAF repository, you can do so through GitHub by forking the repository and sending a pull request. When submitting code, please make every effort to follow existing conventions and style in order to keep the code as readable as possible.

## License

Which licence your contribution falls under depends on which file you touch, and
every file says so in its `SPDX-License-Identifier` tag. See
[ADR-1250](docs/adr/1250-eupl-fork-relicense.md) for the decision and
[the README](README.md#upstream-and-license) for what it means downstream.

- **A new file, or an existing fork-authored file** — [EUPL-1.2](LICENSES/EUPL-1.2.txt),
  a reciprocal licence. By contributing you agree to license that contribution
  under EUPL-1.2.
- **A file inherited from or derived from [Netflix/vmaf](https://github.com/Netflix/vmaf),
  or one that carries libjxl, Xiph or IQA code** — it keeps its existing terms and
  so does your change to it. Do not retag such a file, and leave its copyright
  notices alone.
- **A new helper header that replays reference arithmetic for a GPU or SIMD
  twin** — `EUPL-1.2 AND` the licences of exactly the code it reproduces, with
  that code's copyright notices above yours
  ([ADR-1474](docs/adr/1474-relicense-helper-headers-and-ci-check.md)). Do not
  pick the expression by hand: `scripts/dev/relicense_fork_files.py --write`
  adds the notices and licences of the file's family, and a
  `[ports."<path>"]` entry in `scripts/dev/relicense_provenance.toml` names the
  origins when the family lists more than the header holds. A header that holds
  none of the reference's code (an argument block, or macros and an include of
  a shared header that carries the notices) stays `EUPL-1.2` and gets a
  `[not_ports]` entry with the reason.

A tag describes the notices that are in the file, nothing more. A file whose only
notice is someone else's licence text carries that licence's identifier
(`BSD-2-Clause` for the two-condition text of the Daala and Xiph.Org files,
`BSD-3-Clause` for the three-condition text of the IQA files); a file with Netflix's
header and a quoted third-party notice names both (`BSD-2-Clause-Patent AND MIT` in
`core/src/feature/ciede.c`). The `spdx-tag-matches-notice` pre-commit hook compares
the two.

If you are unsure which case you are in, read the tag already in the file; if it
has none, ask in the pull request rather than guessing.
`scripts/dev/relicense_fork_files.py --check` answers the same question
mechanically, and the required check `Licence Provenance` runs it on every pull
request; [the guide](docs/development/licence-provenance-check.md) says how to
read a failure and how to run it locally.

New files start with this header, and the copyright line names you or your
employer, not the project:

```text
/*
 * Copyright <year> <the author or authors>
 *
 * SPDX-License-Identifier: EUPL-1.2
 */
```

Shell, Python and meson files use the same two lines with `#` comments, below any
shebang. The `check-copyright` pre-commit hook enforces the header on C, C++ and
CUDA sources; `reuse lint` checks that every identifier the tree uses resolves to
a licence text under `LICENSES/`.

## Ways to Contribute

There are many ways you can contribute, and no contribution is too small. To name a few:

- Submitting a bugfix
- Improving documentation
- Making the code run on a new platform
- Robustifying the build system
- Improving the CI loop
- Improving code coverage by adding tests
- Optimizing the speed
- Implementing a well-known quality metric
- Implementing a custom VMAF model for a specific use case

## Algorithmic Contribution

This section focuses on *algorithmic contribution*, which cover two main use cases:

  - Implementing a *well-known quality metric* that can be found in the literature
  - Implementing a *custom VMAF model*, using new elementary features and trained on a specific dataset

For both cases, one can follow the procedure below:

  - First, implement the feature extractor(s), by subclassing the `FeatureExtractor` Python class. A new `FeatureExtractor` class created could be either 1) native Python implementation, or 2) calling a subprocess implemented in a different language (in C or in Matlab, for example).
  - Second, implement the quality runner, by:
    - creating a new `QualityRunner` class as a thin wrapper around the new `FeatureExtractor` created, or
    - using the established `VmafQualityRunner` class but training a custom VMAF model.

For the concepts of `FeatureExtractor`, `QualityRunner` and `VmafQualityRunner`, please refer to the [Core Classes](docs/usage/python.md#core-classes) section of the VMAF Python library documentation.

For algorithmic contribution, for a clean organization of the repo, it is advised to submit new files under directory prefixed with `third_party/[organization]`. For example, for a new model trained, it should go under `model/third_party/[organization]/`. As another example, the [PSNR-HVS feature extractor](https://github.com/Netflix/vmaf/commit/ce2ad1af0b1ba8dd1fbae3e03da0329f078e6bc6) code sits under `core/src/feature/third_party/xiph/`.

### Creating a New `FeatureExtractor`

#### Native Python

To create a subclass of `FeatureExtractor` in native Python code, a minimalist example to follow is the `PypsnrFeatureExtractor` class ("Py-PSNR", see the [code diff](https://github.com/Netflix/vmaf/commit/e698b4d788fb3dcabdc4df2fd1bffe88dc0d3ecd)). The following steps discuss the implementation strategy.

  - Create a subclass of the `FeatureExtractor`. Make sure to specify the `TYPE`, `VERSION` and `ATOM_FEATURES`, which play a role in caching the features extracted in `ResultStore`. Optionally, one can specify a `DERIVED_ATOM_FEATURES` field (refer to the `_post_process_result()` method section for more details).
  - Implement the `_generate_result()` method, which is responsible for the heavy-lifting feature calculation. This method is called by the `_run_on_asset()` method in the parent `Executor` class, which preprocesses the reference and distorted videos to prepare them in a proper YUV format. It calls two `YuvReader` to read the video frame-by-frame, and run computations on them. The result is written to the `log_file_path` file path.
  - Implement the `_get_feature_scores()` method, which parses the result from the `log_file_path` and put it in a dictionary to prepare for a new `Result` object.
  - Optionally, implement the `_post_process_result()` method to compute the `DERIVED_ATOM_FEATURES` from the `ATOM_FEATURES`. Refer to the [Python Calling Matlab](#python-calling-matlab) section for a specific example.
  - Create [test cases](https://github.com/Netflix/vmaf/commit/e698b4d788fb3dcabdc4df2fd1bffe88dc0d3ecd#diff-5b58c2457df7e9b30b0a678d6f79b1caaad6c3f036edfadb6ca9fb0955bede33R630) to lock the numerical results.
  - Notice that `FeatureExtractor` allows one to pass in optional parameters that has an effect on the numerical result. This is demonstrated by the `max_db` parameter in `PypsnrFeatureExtractor` (see this [test case](https://github.com/Netflix/vmaf/commit/e698b4d788fb3dcabdc4df2fd1bffe88dc0d3ecd#diff-5b58c2457df7e9b30b0a678d6f79b1caaad6c3f036edfadb6ca9fb0955bede33R730) for an example use case). By default, there is a bit depth-dependent maximum PSNR value (see [here](FAQ.md#q-why-are-the-psnr-values-capped-at-60-db-for-8-bit-inputs-and-72-db-for-12-bit-inputs-in-the-packages-implementation) for the motivation behind), but the `max_db` parameter specified in the `optional_dict` input allows one to specify a maximum value.

#### Python Calling `libvmaf` in C

Very often the feature extractor implementation is in the C library `libvmaf`. In this case we simply create a thin Python `FeatureExtractor` subclass to call the `vmaf` command line executable. For more information on implementing a new feature extractor in `libvmaf`, refer to [this section](core/README.md#contributing-a-new-vmaffeatureextractor). An example to follow is the PSNR-HVS feature extractor (see the [code diff](https://github.com/Netflix/vmaf/commit/ce2ad1af0b1ba8dd1fbae3e03da0329f078e6bc6)). The following steps discuss the implementation strategy.

  - Add a new feature extractor implementation `vmaf_fex_psnr_hvs` in file `core/src/feature/third_party/xiph/psnr_hvs.c`. It is recommended to put the code under directory `third_party/[org]`.
  - In `core/src/feature/feature_extractor.c`:
    - Declare the new feature extractor as `extern`:

        ```c
        extern VmafFeatureExtractor vmaf_fex_psnr_hvs;
        ```

    - Add the new feature extractor to the `feature_extractor_list`:

        ```c
        static VmafFeatureExtractor *feature_extractor_list[] = {
        ...
        &vmaf_fex_psnr_hvs,
        ...
        };
        ```

  - In `core/src/meson.build`, add the new `psnr_hvs.c` file to the `libvmaf_feature_sources` list:

      ```c
        libvmaf_feature_sources = [
        ...
        feature_src_dir + 'third_party/xiph/psnr_hvs.c',
        ...
        ]
      ```

  - Create a Python wrapper class `PsnrhvsFeatureExtractor` in `python/vmaf/third_party/xiph/vmafexec_feature_extractor.py` (Note: you also need to make `vmaf.third_party.xiph` a Python package by adding the `__init__.py` files in corresponding directories.)
  - Add a test case for `PsnrhvsFeatureExtractor` in `python/test/third_party/xiph/vmafexec_feature_extractor_test.py` to lock the numerical values.

#### Python Calling Matlab

Oftentimes for a well-known quality metric, its Matlab implementation already exists. The VMAF Python library allows directly plugging in the Matlab code by creating a thin Python `MatlabFeatureExtractor` subclass to call the Matlab script. An example to follow is the STRRED feature extractor (see [implementation](https://github.com/Netflix/vmaf/blob/master/python/vmaf/core/matlab_feature_extractor.py#L24-L113) and [test case](https://github.com/Netflix/vmaf/blob/e698b4d788fb3dcabdc4df2fd1bffe88dc0d3ecd/python/test/extra/feature_extractor_extratest.py#L25-L44)). The following steps discuss the implementation strategy.

  - First, Matlab must be pre-installed and its path specified in the `MATLAB_PATH` field in the `python/vmaf/externals.py` file. If not, a user will be prompt with the installation instructions.
  - Create a subclass of the `MatlabFeatureExtractor`. Make sure to specify the `TYPE`, `VERSION` and `ATOM_FEATURES`, which play a role in caching the features extracted in `ResultStore`. Optionally, one can specify a `DERIVED_ATOM_FEATURES` field (refer to the `_post_process_result()` method section for more details).
  - Implement the `_generate_result()` method, which is responsible for calling the Matlab command line to output the result to the file at `log_file_path`.
  - Implement the `_get_feature_scores()` method, which parses the result from the `log_file_path` and put it in a dictionary to prepare for a new `Result` object. In the case of the `StrredFeatureExtractor`, the default method provided by the `FeatureExtractor` superclass can be directly used as the Matlab script's data format is compatible with it, hence the implementation is skipped. But in general, this methods needs to be implemented.
  - Optionally, implement the `_post_process_result()` method to compute the `DERIVED_ATOM_FEATURES` from the `ATOM_FEATURES`. In the case of STRRED, the `strred` feature can be derived from the `srred` and `trred` features via simple computation:

    ```python
    strred = srred * trred
    ```

    Therefore, we define the `strred` feature as "derived" and skip the caching process.
  - Create [test cases]((https://github.com/Netflix/vmaf/blob/e698b4d788fb3dcabdc4df2fd1bffe88dc0d3ecd/python/test/extra/feature_extractor_extratest.py#L25-L44)) to lock the numerical results.

### Creating a Thin `QualityRunner` Wrapper

For the use case of implementing a *well-known quality metric*, after the feature extractor is created, the job is almost done. But to run tests and scripts uniformly, we need to create a thin wrapper of the `QualityRunner` subclass around the new `FeatureExtractor` already created. A good example of this is the `SsimQualityRunner` class (see [code](https://github.com/Netflix/vmaf/blob/master/python/vmaf/core/quality_runner.py#L815)). One simply needs to create a subclass from `QualityRunnerFromFeatureExtractor`, and override the `_get_feature_extractor_class()` and `_get_feature_key_for_score()` methods.

### Implementing a Custom VMAF Model

For the use case of implementing a *custom VMAF model*, one basically follows the two-step approach of first extracting quality-inducing features, and then using a machine-learning regressor to fuse the features and align the final result with subjective scores. The first step is covered by the `FeatureExtractor` subclasses; the second step is done through the `TrainTestModel` subclasses.

#### Creating a New `TrainTestModel` Class

The default VMAF model has been using the `LibsvmNusvrTrainTestModel` class for training (via specifying the `model_type` field in the model parameter file, see the next section for more details). To use a different regressor, one needs to first create a new `TrainTestModel` class. One minimum example to follow is the `Logistic5PLRegressionTrainTestModel` (see [code diff](https://github.com/Netflix/vmaf/commit/bf607883f6bf37b9bdf6382ca191f3b1a4eb9102)). The following steps discuss the implementation strategy.

  - Create a new class by subclassing `TrainTestModel` and `RegressorMixin`. Specify the `TYPE` and `VERSION` fields. The `TYPE` is to be specified by the `model_type` field in the model parameter file.
  - Implement the `_train()` method, which fits the model with the input training data (preprocessed to be a 2D-array) and model parameters.
  - Implement the `_predict()` method, which takes the fitted model and the input data (preprocess to be a 2D-array) and generate the predicted score.
  - Optionally override housekeeping functions such as `_to_file()`, `_delete()`, `_from_info_loaded()` when needed.

#### Calling the `run_vmaf_training` Script

Once the `FeatureExtractor` and `TrainTestModel` classes are ready, the actually training of a VMAF model against a dataset of subjective scores can be initiated by calling the `run_vmaf_training` script. Detailed description of how to use the script can be found in the [Train a New Model](docs/usage/python.md#train-a-new-model) section of VMAF Python Library documentation.

Notice that the current `run_vmaf_training` implementation does not work with `FeatureExtractors` with a custom input parameter (e.g. the `max_db` of `PypsnrFeatureExtractor`). A workaround of this limitation is to create a subclass of the feature extractor with the hard-coded parameter (by overriding the `_custom_init()` method). Refer to [this code](https://github.com/Netflix/vmaf/commit/e698b4d788fb3dcabdc4df2fd1bffe88dc0d3ecd#diff-c5c651eeebd2949a31e059575be35f2018ec57ebdc09c0706cfc1187b9b32dbcR536) and [this test](https://github.com/Netflix/vmaf/commit/e698b4d788fb3dcabdc4df2fd1bffe88dc0d3ecd#diff-5b58c2457df7e9b30b0a678d6f79b1caaad6c3f036edfadb6ca9fb0955bede33R751) for an example.
