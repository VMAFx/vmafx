- **The hosted `Tooling Tests` job installs its lock again, and the `vmaf-tune` suite no
  longer assumes a GPU host or a `vmaf` on `PATH`.** `reuse==6.2.0` has no wheel for
  Python 3.14, so pip builds it from source with `poetry-core`, which the locked build
  backend set lacked (`poetry-core==2.5.0` is now in `requirements/locks/package-build.in` and the
  locks that include it). The NVENC probe test needs a GPU the driver lists, and the two
  bisect cap tests stub the scoring step the way their docstring says.
