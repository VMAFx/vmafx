**python package health** (`python/`, `compat/python-vmaf/`):

- Remove stale `run_vmaf_in_batch` console-script entry point (script deleted upstream at Netflix/vmaf `014718b746`; installing the wheel would fail at invocation time).
- Add `__main__.py` so `python -m vmaf` works without `python -m vmaf.script.run_vmaf`.
- Add `py.typed` PEP 561 marker so downstream type checkers recognise the package as typed.
- Add `[project]` table to `python/pyproject.toml` (was build-system-only; `pip install .` lacked metadata).
- Sync `setup.py install_requires` with `requirements.txt` (was missing `PyWavelets`, `python-slugify`, `libsvm-official`).
- Loosen over-pinned `numpy>=2.4.6,<2.4.7` to `numpy>=2.4.6` and `libsvm-official>=3.37.0,<=3.37` to `libsvm-official>=3.37` in `requirements.txt`.
