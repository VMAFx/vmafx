- **chore(container):** Add `apt-mark showmanual` post-install checks for the
  NEO compute-runtime, ROCm, and mesa-vulkan-drivers/intel-media-va-driver-non-free
  package sets in `dev/Containerfile`; a failed `apt-get install` now surfaces
  as a fatal build-time error rather than a silent run-time failure. (ADR-0549)
- **chore(cuda,sycl):** Add upstream-mirror-filename comment to 8 CUDA/SYCL
  translation units whose `integer_` filename prefix belies their `float_*`
  symbols; the comment explains the Netflix upstream naming convention and cites
  ADR-0549 so future maintainers do not rename the files. (ADR-0549)
- **chore(docs):** Correct stale 2026-05-15 Updated note in `docs/state.md`
  that described T-VK-T7-29-PART-2-IMPORT-NOT-IMPL and T-CAMBI-HIP-NOT-STARTED
  as newly added to Open; both are now in Recently-closed. (ADR-0549)
- **chore(test):** Remove stale `# 88.032956` inline baseline comment from
  `python/test/vmafexec_test.py` line 1294; the assertion itself is unchanged.
  (ADR-0549)
- **chore(gitignore):** Add `.claude/worktrees/` to `.gitignore` so parallel-agent
  isolation worktrees do not appear as untracked files in `git status`. (ADR-0549)
