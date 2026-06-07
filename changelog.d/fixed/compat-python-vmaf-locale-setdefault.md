# fix(compat): force C locale unconditionally in ProcessRunner and _run_matlab

`ProcessRunner.run` in `compat/python-vmaf/__init__.py` used
`kwargs.setdefault("env", env)` to inject `LC_ALL=C`/`LANG=C`, but
`setdefault` is a no-op when the caller already supplies an `env=` kwarg
(e.g. the ffmpeg path at `executor.py:786`). The host locale leaked through
and could produce non-English subprocess error messages on non-English dev hosts.

The fix builds a merged env: start from the caller's `env=` dict (or
`os.environ` if none), then unconditionally stamp `LC_ALL=C`/`LANG=C` on top.
Caller-specific env entries (paths, tokens) are preserved.

Same bug fixed in `_run_matlab` in `core/matlab_feature_extractor.py` where
`env.setdefault("LC_ALL", "C")` also failed to override a host-set locale.

Stale `python/vmaf/workspace` and `python/vmaf/resource` references in
`config.py` docstrings updated to `compat/python-vmaf/` (post-ADR-0700).
