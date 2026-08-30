- **chore(ci):** Add `concurrency:` block + `cancel-in-progress: true` to
  `ffmpeg-integration.yml` (deferred from PR #301). Force-push / PR-rebase now
  cancels superseded matrix runs across the gcc / clang / SYCL legs instead of
  queueing duplicates. (ADR-0890)
- **chore(ci):** Wrap libvmaf + FFmpeg builds in `ffmpeg-integration.yml` with
  ccache (`actions/cache@v5` + `--max-size=400M`) and switch the FFmpeg clone
  to `--depth=1`. Mirrors the established `libvmaf-build-matrix.yml` /
  `build.yml` pattern; saves 3–5 min per leg after warm-up plus 20–40 s on the
  shallow clone. (ADR-0890)
- **chore(ci):** Wrap the ASan + UBSan (PR-gate) and TSan (master-push) jobs
  in `sanitizers.yml` with ccache, so the clang-18 debug-instrumentation
  rebuild benefits from the same 60–85 % hit rate as the matrix build legs.
  (ADR-0890)
- **chore(ci):** Add the conservative `paths-ignore` deny-list
  (`docs/**`, `**/*.md`, `changelog.d/**`, `CHANGELOG.md`, `.workingdir2/**`)
  to `security-scans.yml` on the PR trigger. Doc-only PRs no longer fire
  CodeQL C++ (~35 min build + analyze), CodeQL Python, CodeQL Actions,
  Semgrep, Gitleaks, or Dependency Review. The weekly `0 6 * * 1` schedule
  still provides full master-branch coverage. (ADR-0890)
- **chore(ci):** Add an early `Detect C/C++ changes (early skip)` probe to
  `lint-and-format.yml::clang-tidy` and gate the `Install deps`,
  `Generate compile_commands.json`, and `Run clang-tidy on changed files`
  steps on its output. Doc-only / Python-only PRs skip ~5 min of
  apt-install + meson-setup + meson-compile that previously ran unconditionally
  before the existing inner short-circuit. (ADR-0890)
