- CI no longer stages gigabyte-scale downloads on the `ubuntu-26.04` runner's
  RAM-backed `/tmp`. Relocating the virtualenvs and the end-to-end image tar to
  `RUNNER_TEMP` was not enough, because two producers take their destination
  from `$TMPDIR`, which GitHub Actions leaves unset: pip unpacks every wheel it
  downloads under `$TMPDIR` before installing it, so the torch and CUDA wheel
  set still landed on the tmpfs and failed with `[Errno 122] Disk quota
  exceeded`; and `kind load docker-image` writes a single `images.tar` holding
  all three end-to-end images there, re-creating on the tmpfs the tar the
  `docker save` step had just been moved off. `TMPDIR` now points at
  `runner.temp` for the pip-bearing steps in `tests-and-quality-gates.yml` and
  for the kind load in `e2e-k8s.yml`.
